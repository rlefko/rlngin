#include "search.h"
#include "bitboard.h"
#include "eval.h"
#include "move_picker.h"
#include "movegen.h"
#include "search_params.h"
#include "see.h"
#include "tt.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

static constexpr int MAX_HISTORY = 16384;
static constexpr int MAX_CONT_HISTORY = 16384;
// Every correction-history table shares the same clamp magnitude and modulo
// size. Per-table weighting is handled by SearchParams.*CorrWeight so the
// relative contribution of each signal is tunable without changing storage.
static constexpr int MAX_CORR_HIST = 16384;
static constexpr int CORR_HIST_SIZE = 16384;
static constexpr int MAX_LMR_MOVES = 256;

static int lmrReductions[MAX_PLY][MAX_LMR_MOVES];

static TranspositionTable tt(16);

enum BoundType { BOUND_EXACT, BOUND_LOWER, BOUND_UPPER };

// Saturating gravity update used by every history table. The gravity term
// pulls `entry` toward zero in proportion to its current magnitude; clamping
// into `[-max, max]` prevents transient overshoot from runaway bonuses.
template <typename T> static void applyHistoryBonus(T &entry, int bonus, int max) {
    int updated = static_cast<int>(entry) + bonus - static_cast<int>(entry) * std::abs(bonus) / max;
    if (updated > max)
        updated = max;
    else if (updated < -max)
        updated = -max;
    entry = static_cast<T>(updated);
}

// Tier offsets for multi-ply continuation history: 1-ply, 2-ply, and 4-ply back.
// Kept in sync with the definition in move_picker.cpp; both files must index
// the same entries because scoring and updating share the table.
static constexpr int CONT_HIST_OFFSETS[3] = {1, 2, 4};

// Adjust a raw static eval by the accumulated correction history for the side
// to move. Every table contributes via a tunable weight, divided by a shared
// grain so SPSA can trade correction magnitude between signals cleanly.
// Mate-range scores are returned untouched so the correction cannot
// accidentally push a non-mate eval into mate territory.
static int correctedEval(int staticEval, const Board &board, const SearchState &state, int ply) {
    if (staticEval <= -MATE_SCORE + MAX_PLY || staticEval >= MATE_SCORE - MAX_PLY) {
        return staticEval;
    }
    int stm = board.sideToMove;
    const auto &h = *state.historyTables;

    int pawnIdx = static_cast<int>(board.pawnKey % CORR_HIST_SIZE);
    int whiteIdx = static_cast<int>(board.nonPawnKey[White] % CORR_HIST_SIZE);
    int blackIdx = static_cast<int>(board.nonPawnKey[Black] % CORR_HIST_SIZE);
    int minorIdx = static_cast<int>(board.minorKey % CORR_HIST_SIZE);

    long weighted = 0;
    weighted += static_cast<long>(searchParams.PawnCorrWeight) * h.pawnCorrHist[stm][pawnIdx];
    weighted +=
        static_cast<long>(searchParams.NonPawnCorrWeight) * h.nonPawnCorrHist[stm][White][whiteIdx];
    weighted +=
        static_cast<long>(searchParams.NonPawnCorrWeight) * h.nonPawnCorrHist[stm][Black][blackIdx];
    weighted += static_cast<long>(searchParams.MinorCorrWeight) * h.minorCorrHist[stm][minorIdx];
    if (ply >= 2) {
        PieceType prev2Pt = state.movedPiece[ply - 2];
        PieceType prev1Pt = state.movedPiece[ply - 1];
        // A null parent at either slot leaves movedPiece as None; with no
        // meaningful move there is nothing to key a continuation correction
        // on, so skip the term.
        if (prev2Pt != None && prev1Pt != None) {
            int prev2To = state.moveStack[ply - 2].to;
            int prev1To = state.moveStack[ply - 1].to;
            weighted += static_cast<long>(searchParams.ContCorrWeight) *
                        h.contCorrHist[prev2Pt][prev2To][prev1Pt][prev1To];
        }
    }

    int correction = static_cast<int>(weighted / searchParams.CorrHistGrain);
    return staticEval + correction;
}

// Shared scaled bonus used by every correction-history table: the residual
// gap between the node's search result and its ALREADY-CORRECTED eval,
// scaled by depth and clamped so a single update cannot saturate the entry.
// Depth is capped at 32 so very deep lines cannot single-handedly drive the
// entry to saturation in one update.
static int corrHistBonus(int baseEval, int bestValue, int depth, int max) {
    int diff = bestValue - baseEval;
    int cappedDepth = depth > 32 ? 32 : depth;
    int bonus = diff * cappedDepth / 8;
    int cap = max / 4;
    if (bonus > cap)
        bonus = cap;
    else if (bonus < -cap)
        bonus = -cap;
    return bonus;
}

// Fold the residual error between the node's search result and its corrected
// eval into every correction table. Gated by the caller to fire only on
// quiet bestMove cutoffs/exact bounds outside singular exclusion. Passing the
// corrected eval lets the bonus self-regulate: as the correction converges
// the residual shrinks and updates stop accumulating. Every table shares the
// same storage bound, so the per-table bonus is also shared.
//
// The pawn, non-pawn, and minor tables are keyed purely by position features.
// At ply 1 and 2 the `cappedDepth`-scaled bonus is at its largest and the
// feature keys overlap heavily with keys that sibling root moves will visit
// in their own subtrees, so writes accumulated while the first root move is
// searched would otherwise bias the correction every later-searched root
// move reads. Rather than dropping shallow writes outright, we taper them:
// `ply == 1` lands a quarter of the bonus, `ply == 2` lands half, and every
// ply from 3 onward lands the full bonus. That keeps the learning signal
// Stockfish-style engines rely on at shallow ply, while shrinking the per
// update magnitude below the threshold where sibling pollution flips the
// root best move between iterations. The continuation table is keyed on
// the two moves leading into the node, so sibling root moves write to
// disjoint cells by construction; its update fires at every ply where both
// preceding moves are available and does not need tapering.
static void updateCorrectionHistories(const Board &board, SearchState &state, int ply, int baseEval,
                                      int bestValue, int depth) {
    if (baseEval == -INF_SCORE) return;
    int stm = board.sideToMove;
    int bonus = corrHistBonus(baseEval, bestValue, depth, MAX_CORR_HIST);
    auto &h = *state.historyTables;

    int posBonus = bonus;
    if (ply == 1) {
        posBonus = bonus / 4;
    } else if (ply == 2) {
        posBonus = bonus / 2;
    }
    {
        int pawnIdx = static_cast<int>(board.pawnKey % CORR_HIST_SIZE);
        applyHistoryBonus(h.pawnCorrHist[stm][pawnIdx], posBonus, MAX_CORR_HIST);

        int whiteIdx = static_cast<int>(board.nonPawnKey[White] % CORR_HIST_SIZE);
        int blackIdx = static_cast<int>(board.nonPawnKey[Black] % CORR_HIST_SIZE);
        applyHistoryBonus(h.nonPawnCorrHist[stm][White][whiteIdx], posBonus, MAX_CORR_HIST);
        applyHistoryBonus(h.nonPawnCorrHist[stm][Black][blackIdx], posBonus, MAX_CORR_HIST);

        int minorIdx = static_cast<int>(board.minorKey % CORR_HIST_SIZE);
        applyHistoryBonus(h.minorCorrHist[stm][minorIdx], posBonus, MAX_CORR_HIST);
    }

    if (ply >= 2) {
        PieceType prev2Pt = state.movedPiece[ply - 2];
        PieceType prev1Pt = state.movedPiece[ply - 1];
        if (prev2Pt != None && prev1Pt != None) {
            int prev2To = state.moveStack[ply - 2].to;
            int prev1To = state.moveStack[ply - 1].to;
            applyHistoryBonus(h.contCorrHist[prev2Pt][prev2To][prev1Pt][prev1To], bonus,
                              MAX_CORR_HIST);
        }
    }
}

// Apply the same bonus to every tier of continuation history for this move.
static void updateContHistoryAll(SearchState &state, int ply, PieceType currPt, int currTo,
                                 int bonus) {
    for (int t = 0; t < 3; t++) {
        int off = CONT_HIST_OFFSETS[t];
        if (ply >= off) {
            PieceType prevPt = state.movedPiece[ply - off];
            int prevTo = state.moveStack[ply - off].to;
            applyHistoryBonus(state.historyTables->contHistory[t][prevPt][prevTo][currPt][currTo],
                              bonus, MAX_CONT_HISTORY);
        }
    }
}

static void checkTime(SearchState &state) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state.startTime).count();
    if (elapsed >= state.allocatedTimeMs) {
        state.stopped = true;
    }
}

static bool isInCheck(const Board &board) {
    Color side = board.sideToMove;
    Color opponent = (side == White) ? Black : White;
    Bitboard kingBB = board.byPiece[King] & board.byColor[side];
    if (!kingBB) return false;
    return isSquareAttacked(board, lsb(kingBB), opponent);
}

int quiescence(Board &board, int alpha, int beta, int ply, SearchState &state) {
    state.nodes++;
    if (ply > state.seldepth) state.seldepth = ply;

    if (ply >= MAX_PLY) return evaluate(board);

    state.pvLength[ply] = ply;

    if (state.nodes % 1024 == 0) checkTime(state);
    if (state.stopped) return 0;

    int origAlpha = alpha;

    // TT probe: qsearch entries are written at depth 0, so any entry provides a
    // usable bound for the leaf-level search. A TT cut here is a free save.
    TTEntry ttEntry;
    Move ttMove = {0, 0, None};
    bool ttHit = tt.probe(board.key, ttEntry, ply);
    if (ttHit) {
        ttMove = ttEntry.best_move;
        if (ttEntry.flag == TT_EXACT) return ttEntry.score;
        if (ttEntry.flag == TT_LOWER_BOUND && ttEntry.score >= beta) return ttEntry.score;
        if (ttEntry.flag == TT_UPPER_BOUND && ttEntry.score <= alpha) return ttEntry.score;
    }

    bool inCheck = isInCheck(board);

    int rawStandPat = -INF_SCORE;
    int standPat = -INF_SCORE;
    int bestScore = -INF_SCORE;
    if (!inCheck) {
        rawStandPat = (ttHit && ttEntry.eval != TT_NO_EVAL) ? ttEntry.eval : evaluate(board);
        // Read-only correction: qsearch consumes the corrected eval for its
        // stand-pat and delta pruning decisions but does not update the table,
        // since qsearch returns are the signal the correction is tracking.
        standPat = correctedEval(rawStandPat, board, state, ply);
        bestScore = standPat;
        if (standPat >= beta) return standPat;
        if (standPat > alpha) alpha = standPat;

        // Node-level delta shortcut: the per-move delta prune below will
        // reject every capture when even the most valuable enemy piece
        // cannot lift stand-pat into the alpha window. Detect that up
        // front so we can skip move generation and ordering entirely.
        // Promotions are handled separately because the per-move prune
        // deliberately ignores them; whenever a push-to-promote is on
        // the board we fall through to the normal search path. Pawn
        // captures must be included in the max-gain calculation here so
        // that pure-pawn endgames, where the per-move prune still lets
        // pawn captures through at PieceValue[Pawn] + margin, are not
        // silently pruned by a shortcut that thinks no capture exists.
        Color them = (board.sideToMove == White) ? Black : White;
        int bestTargetValue = 0;
        for (int pt = Queen; pt >= Pawn; pt--) {
            if (board.byPiece[pt] & board.byColor[them]) {
                bestTargetValue = PieceValue[pt];
                break;
            }
        }
        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[board.sideToMove];
        Bitboard promoReady = ourPawns & ((board.sideToMove == White) ? Rank7BB : Rank2BB);
        if (!promoReady && standPat + bestTargetValue + searchParams.QsDeltaMargin <= alpha) {
            // Mirror the no-move-tried store path below so future probes
            // can cut immediately instead of recomputing stand-pat from
            // scratch. The shortcut fires only when standPat < alpha, so
            // the bound is always an upper bound.
            Move noMove = {0, 0, None};
            tt.store(board.key, standPat, rawStandPat, 0, TT_UPPER_BOUND, noMove, ply);
            return standPat;
        }
    }

    Move bestMove = {0, 0, None};
    MovePicker picker(board, state, ply, ttMove, inCheck, /*qsearchTag=*/true);
    PickedMove pm;
    int movesSearched = 0;
    while (picker.next(pm)) {
        const Move &m = pm.move;
        if (!inCheck && isCapture(board, m)) {
            // Prune losing captures
            if (see(board, m) < 0) continue;
            // Delta pruning: skip captures that cannot raise alpha even on a
            // full recapture with a positional bonus. With a wider eval
            // distribution, standPat often sits well below alpha, so qsearch
            // fans out captures that have no chance of moving the score.
            if (m.promotion == None &&
                standPat + PieceValue[capturedType(board, m)] + searchParams.QsDeltaMargin <=
                    alpha) {
                continue;
            }
        }

        // Record the move on the search stack before recursing so the child's
        // correction-history read sees the real previous move instead of stale
        // data left over from an earlier branch.
        state.moveStack[ply] = m;
        state.movedPiece[ply] = board.squares[m.from].type;
        UndoInfo undo = board.makeMove(m);
        tt.prefetch(board.key);
        int score = -quiescence(board, -beta, -alpha, ply + 1, state);
        board.unmakeMove(m, undo);
        if (state.stopped) return 0;
        movesSearched++;
        if (score > bestScore) {
            bestScore = score;
            bestMove = m;
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }

    // In-check mate detection: if we never made it past move generation with
    // a legal evasion, the side to move is mated at this ply. Stand-pat was
    // suppressed above for in-check positions, so bestScore would otherwise
    // stay at -INF_SCORE here.
    if (inCheck && movesSearched == 0) return -(MATE_SCORE - ply);

    // Store the leaf result so future visits can cut immediately.
    TTFlag flag;
    if (bestScore <= origAlpha) {
        flag = TT_UPPER_BOUND;
    } else if (bestScore >= beta) {
        flag = TT_LOWER_BOUND;
    } else {
        flag = TT_EXACT;
    }
    // Skip the store when nothing was learned beyond the stand-pat: no move
    // improved on it and the resulting bound is exact. That entry would be
    // redundant with a probe that recomputes stand-pat from the cached eval.
    bool tried = bestMove.from != 0 || bestMove.to != 0;
    if (tried || bestScore != standPat || flag != TT_EXACT) {
        int storedEval = inCheck ? TT_NO_EVAL : rawStandPat;
        tt.store(board.key, bestScore, storedEval, 0, flag, bestMove, ply);
    }

    return bestScore;
}

int qsearchScore(const Board &board) {
    // Quiet-target helper for the Texel tuner. We run qsearch with a
    // fresh state and an effectively unlimited time budget, then return
    // the side-to-move POV leaf score. Using qsearch (instead of raw
    // evaluate()) ensures every labeled position is scored on its quiet
    // continuation, which is the Texel premise. The shared TT is read
    // and written during the qsearch; when the tuner calls this from
    // multiple threads, rare TT races are acceptable noise and are
    // mitigated by clearing the TT between loss computations.
    SearchState state;
    state.startTime = std::chrono::steady_clock::now();
    state.allocatedTimeMs = std::numeric_limits<int64_t>::max();
    Board copy = board;
    return quiescence(copy, -INF_SCORE, INF_SCORE, 0, state);
}

Board qsearchLeafBoard(const Board &root) {
    // Clear TT so the post-search walk only follows entries that this
    // call wrote. Not thread-safe: the tuner must run this step
    // sequentially before the multi-threaded loss loop starts.
    tt.clear();

    SearchState state;
    state.startTime = std::chrono::steady_clock::now();
    state.allocatedTimeMs = std::numeric_limits<int64_t>::max();
    Board copy = root;
    quiescence(copy, -INF_SCORE, INF_SCORE, 0, state);

    // Walk the best-move chain. Qsearch writes a zero move for pure
    // stand-pat returns; walking stops when no move, a non-capture, or
    // a TT miss is seen. The cap on iterations guards against any
    // pathological cycle we might observe under perturbation.
    Board cur = root;
    for (int iter = 0; iter < 32; iter++) {
        TTEntry entry;
        if (!tt.probe(cur.key, entry, 0)) break;
        Move m = entry.best_move;
        if (m.from == 0 && m.to == 0 && m.promotion == None) break;
        if (!isCapture(cur, m)) break;
        cur.makeMove(m);
    }
    return cur;
}

static bool isRepetition(const Board &board, const SearchState &state, int ply) {
    uint64_t key = board.key;
    int hmc = board.halfmoveClock;

    // Only positions within the halfmove clock window can possibly repeat,
    // since captures and pawn moves make earlier positions unreachable.

    // Check positions during the current search (step by 2 for same side to move)
    int searchBack = std::min(ply, hmc);
    for (int i = 2; i <= searchBack; i += 2) {
        if (state.searchKeys[ply - i] == key) return true;
    }

    // Check game history (positions before search started)
    int gameBack = hmc - ply;
    if (gameBack > 0) {
        int histSize = static_cast<int>(state.positionHistory.size());
        int end = std::max(0, histSize - gameBack);
        for (int i = histSize - 1; i >= end; i--) {
            if (state.positionHistory[i] == key) return true;
        }
    }

    return false;
}

static int negamax(Board &board, int depth, int ply, int alpha, int beta, SearchState &state,
                   bool allowNullMove = true, Move excludedMove = {0, 0, None}) {
    state.nodes++;
    if (ply > state.seldepth) state.seldepth = ply;
    state.pvLength[ply] = ply;
    state.searchKeys[ply] = board.key;

    if (ply >= MAX_PLY - 1) return evaluate(board);

    if (state.nodes % 1024 == 0) checkTime(state);
    if (state.stopped) return 0;

    // Draw detection: repetition and 50-move rule
    if (ply > 0) {
        if (board.halfmoveClock >= 100) return 0;
        if (isRepetition(board, state, ply)) return 0;
    }

    int origAlpha = alpha;
    bool pvNode = (beta - alpha > 1);

    // TT probe
    TTEntry ttEntry;
    Move ttMove = {0, 0, None};
    bool hasExcludedMove = (excludedMove.from != 0 || excludedMove.to != 0);
    bool ttHit = tt.probe(board.key, ttEntry, ply);
    if (ttHit) {
        ttMove = ttEntry.best_move;
        // PV nodes never take a TT score cutoff. A stale exact bound from an
        // earlier root search, or even from an earlier iteration of the
        // current search, can otherwise short-circuit the fresh verdict we
        // are trying to compute and pin the engine on a move the deeper
        // search would have rejected. The TT move above is still consumed
        // for ordering, which is where the hint is actually useful.
        if (!hasExcludedMove && !pvNode && ttEntry.depth >= depth) {
            if (ttEntry.flag == TT_EXACT) {
                return ttEntry.score;
            }
            if (ttEntry.flag == TT_LOWER_BOUND && ttEntry.score >= beta) {
                return ttEntry.score;
            }
            if (ttEntry.flag == TT_UPPER_BOUND && ttEntry.score <= alpha) {
                return ttEntry.score;
            }
        }
    }

    if (depth <= 0) return quiescence(board, alpha, beta, ply, state);

    // Internal iterative reduction: if we lack a TT move at a node deep enough
    // to justify it, spend one ply less so the sibling search produces a TT
    // move for the eventual re-visit. Applies at both PV and non-PV nodes.
    if (depth >= 4 && ttMove.from == 0 && ttMove.to == 0) {
        depth -= 1;
    }

    bool inCheck = isInCheck(board);

    // Static eval for pruning decisions (unreliable when in check). Prefer the
    // cached eval from the TT when available to avoid a redundant evaluate call.
    int staticEval;
    if (inCheck) {
        staticEval = -INF_SCORE;
    } else if (ttHit && ttEntry.eval != TT_NO_EVAL) {
        staticEval = ttEntry.eval;
    } else {
        staticEval = evaluate(board);
    }
    // Correction history folded onto the raw static eval. The raw value is
    // still what we store in TT; the corrected value is what every eval-gated
    // decision in this node reads, and it is also what we stash in the search
    // stack so the improving comparison stays in the same units.
    int corrEval = inCheck ? -INF_SCORE : correctedEval(staticEval, board, state, ply);
    // Only the outer entry at this ply owns state.staticEvals[ply]. Singular
    // extension and NMP verification re-enter the same ply (both pass
    // allowNullMove=false); if we let them overwrite, their corrEval would be
    // computed AFTER NMP or ProbCut children already updated the correction
    // tables, producing a value that disagrees with what outer wrote and
    // destabilizing the improving flag seen by descendants at ply+2.
    bool writeStaticEval = allowNullMove && !hasExcludedMove;
    if (writeStaticEval) {
        state.staticEvals[ply] = inCheck ? -INF_SCORE : corrEval;
    }

    // Determine if the position is improving (corrected eval better than 2
    // plies ago). Using corrected on both sides keeps improving aligned with
    // the pruning gates that also read corrEval.
    bool improving = false;
    if (inCheck) {
        improving = false;
    } else if (ply >= 2 && state.staticEvals[ply - 2] != -INF_SCORE) {
        improving = corrEval > state.staticEvals[ply - 2];
    } else if (ply >= 4 && state.staticEvals[ply - 4] != -INF_SCORE) {
        improving = corrEval > state.staticEvals[ply - 4];
    } else {
        improving = true;
    }

    // Razoring: at shallow non-PV depths, if the corrected eval plus a generous
    // margin still cannot reach alpha, the position is losing badly enough that
    // a full search is unlikely to recover. Drop straight to qsearch instead.
    if (!pvNode && !inCheck && depth <= 2 && alpha > -MATE_SCORE + MAX_PLY) {
        int razorMargin = searchParams.RazorBase + searchParams.RazorDepth * depth;
        if (corrEval + razorMargin <= alpha) {
            return quiescence(board, alpha, beta, ply, state);
        }
    }

    // Reverse futility pruning: if static eval is far above beta at shallow depth,
    // assume this node will fail high
    if (!inCheck && depth <= 3 && beta - alpha == 1 && beta > -MATE_SCORE + MAX_PLY) {
        int rfpMargin = (searchParams.RfpBase - searchParams.RfpImproving * improving) * depth;
        if (corrEval - rfpMargin >= beta) {
            return corrEval - rfpMargin;
        }
    }

    // Null move pruning: skip the move if the opponent can't beat beta even
    // with a free move, which indicates this position is too good for us
    if (allowNullMove && !inCheck && depth >= 3 && beta - alpha == 1 &&
        beta > -MATE_SCORE + MAX_PLY) {
        Color us = board.sideToMove;
        Bitboard nonPawnMaterial = board.byColor[us] & ~board.byPiece[Pawn] & ~board.byPiece[King];
        if (nonPawnMaterial) {
            // Dynamic reduction: base depth component + eval-based bonus
            int R = searchParams.NmpBase + depth / 3 +
                    std::clamp((corrEval - beta) / searchParams.NmpEvalDiv, 0, 3);
            R = std::min(R, depth - 1);
            // Sentinel so the child's correction-history lookup can detect a
            // null parent and skip the continuation term, rather than indexing
            // into a stale move's slot.
            state.moveStack[ply] = {0, 0, None};
            state.movedPiece[ply] = None;
            UndoInfo nullUndo = board.makeNullMove();
            state.searchKeys[ply + 1] = board.key;
            int nullScore = -negamax(board, depth - 1 - R, ply + 1, -beta, -beta + 1, state);
            board.unmakeNullMove(nullUndo);
            if (state.stopped) return 0;
            if (nullScore >= beta) {
                // Verification search at high depths to guard against zugzwang
                if (depth >= 8) {
                    int verifyScore = negamax(board, depth - 1 - R, ply, alpha, beta, state, false);
                    if (state.stopped) return 0;
                    if (verifyScore >= beta) return beta;
                } else {
                    return beta;
                }
            }
        }
    }

    // ProbCut: use a shallow search of captures to predict whether a full-depth
    // search would produce a beta cutoff
    if (!pvNode && !inCheck && depth >= 5 && beta > -MATE_SCORE + MAX_PLY &&
        std::abs(beta) < MATE_SCORE - MAX_PLY) {
        int probcutBeta = beta + 483 - 145 * improving;
        int probcutDepth = depth - 4;

        std::vector<Move> pcMoves = generateLegalCaptures(board);

        Move noTT = {0, 0, None};
        std::vector<ScoredMove> pcScored;
        pcScored.reserve(pcMoves.size());
        for (const Move &m : pcMoves) {
            pcScored.push_back({scoreMove(m, board, noTT, -1, state, nullptr), 0, m});
        }
        std::sort(pcScored.begin(), pcScored.end(),
                  [](const ScoredMove &a, const ScoredMove &b) { return a.score > b.score; });

        for (const ScoredMove &sm : pcScored) {
            const Move &pcMove = sm.move;
            if (!seeGE(board, pcMove, probcutBeta - corrEval)) continue;

            state.moveStack[ply] = pcMove;
            state.movedPiece[ply] = board.squares[pcMove.from].type;
            UndoInfo pcUndo = board.makeMove(pcMove);

            int pcScore =
                -negamax(board, probcutDepth, ply + 1, -probcutBeta, -probcutBeta + 1, state);

            board.unmakeMove(pcMove, pcUndo);
            if (state.stopped) return 0;

            if (pcScore >= probcutBeta) {
                int storedEval = inCheck ? TT_NO_EVAL : staticEval;
                tt.store(board.key, pcScore, storedEval, depth, TT_LOWER_BOUND, pcMove, ply);
                return probcutBeta;
            }
        }
    }

    // Restricted singular extensions: if the TT move is much better than all
    // alternatives, extend its search by one ply to resolve critical lines
    int singularExtension = 0;
    if (depth >= 8 && ply > 0 && !inCheck && !hasExcludedMove && ttMove.from != 0 &&
        ttEntry.depth >= depth - 3 &&
        (ttEntry.flag == TT_LOWER_BOUND || ttEntry.flag == TT_EXACT) &&
        std::abs(ttEntry.score) < MATE_SCORE - MAX_PLY) {

        int singularBeta = ttEntry.score - depth * 2;
        int singularDepth = (depth - 1) / 2;

        int singularScore = negamax(board, singularDepth, ply, singularBeta - 1, singularBeta,
                                    state, false, ttMove);

        if (singularScore < singularBeta) {
            singularExtension = 1;
        }
    }

    int bestScore = -INF_SCORE;
    Move bestMove = {0, 0, None};

    Move searchedCaptures[64];
    Move searchedQuiets[64];
    int numSearchedCaptures = 0;
    int numSearchedQuiets = 0;
    int bonus = std::min(depth * depth, 400);
    int movesSearched = 0;

    // Staged move picker: the excluded move is threaded through as `skipMove`
    // so the singular-extension path never sees the excluded candidate in any
    // phase, including from the TT slot.
    MovePicker picker(board, state, ply, ttMove, inCheck);
    PickedMove pm;
    int moveIndex = 0;
    while (picker.next(pm, excludedMove)) {
        const Move &m = pm.move;

        state.moveStack[ply] = m;
        state.movedPiece[ply] = board.squares[m.from].type;

        bool capture = isCapture(board, m);
        bool isPromotion = (m.promotion != None);

        // Compute extensions before makeMove so piece types are still on the board
        int moveExtension = 0;
        if (singularExtension > 0 && m.from == ttMove.from && m.to == ttMove.to &&
            m.promotion == ttMove.promotion) {
            moveExtension = singularExtension;
        }

        // Modern capture extension: extend high-value recaptures in PV nodes
        if (moveExtension == 0 && pvNode && capture && ply >= 1) {
            int prevTo = state.moveStack[ply - 1].to;
            if (m.to == prevTo) {
                PieceType pt = board.squares[m.from].type;
                PieceType ct = capturedType(board, m);
                int capHistScore = state.captureHistory[pt][m.to][ct];
                if (capHistScore >= 1000) {
                    moveExtension = 1;
                }
            }
        }

        // SEE pruning: skip captures with very negative SEE at non-PV nodes
        if (!pvNode && !inCheck && moveIndex > 0 && capture && !isPromotion &&
            bestScore > -MATE_SCORE + MAX_PLY) {
            if (!seeGE(board, m, -searchParams.SeeCaptureCoef * depth * depth)) {
                continue;
            }
        }

        // SEE pruning: skip quiet moves to heavily attacked squares at shallow depth
        if (!pvNode && !inCheck && moveIndex > 0 && !capture && !isPromotion && depth <= 8 &&
            bestScore > -MATE_SCORE + MAX_PLY) {
            if (!seeGE(board, m, -searchParams.SeeQuietCoef * depth)) {
                continue;
            }
        }

        // Late-history pruning: once the picker enters the Quiets phase, the
        // remaining moves are strictly ordered by history. At shallow
        // non-PV depths, skip any quiet whose history score sits below a
        // depth-scaled threshold. Gating on phase == Quiets keeps TT move,
        // good captures, killers, and the counter move unpruned.
        if (!pvNode && !inCheck && moveIndex > 0 && pm.phase == PickPhase::Quiets && depth <= 5 &&
            bestScore > -MATE_SCORE + MAX_PLY) {
            int threshold = -searchParams.HistoryPruningCoef * depth;
            if (pm.histScore < threshold) {
                continue;
            }
        }

        // Check extension gate. seeGE operates on the pre-move board, so we
        // compute it here, and only for captures/promotions where the SEE
        // answer is meaningful. Quiet non-captures extend without the SEE
        // gate, sparing the per-move SEE cost on the common case where the
        // move never gives check at all.
        bool checkExtPass = pvNode && depth >= 6;
        if (checkExtPass && (capture || isPromotion)) {
            checkExtPass = seeGE(board, m, 0);
        }

        UndoInfo undo = board.makeMove(m);
        tt.prefetch(board.key);
        bool givesCheck = isInCheck(board);

        // Check extension: extend checking PV moves. Take the max rather than
        // sum with singular to avoid stacking extensions on the same move. A
        // per-path cap keeps forcing lines from exploding.
        if (givesCheck && checkExtPass) {
            moveExtension = std::max(moveExtension, 1);
        }
        int extBudget = 2 * state.rootDepth - state.extensionsOnPath[ply];
        if (extBudget <= 0) {
            moveExtension = 0;
        } else if (moveExtension > extBudget) {
            moveExtension = extBudget;
        }
        state.extensionsOnPath[ply + 1] = state.extensionsOnPath[ply] + moveExtension;

        // Futility pruning: skip quiet moves at shallow depth when static eval + margin <= alpha
        if (!inCheck && depth <= 3 && moveIndex > 0 && !capture && !isPromotion && !givesCheck &&
            alpha > -MATE_SCORE + MAX_PLY && beta < MATE_SCORE - MAX_PLY) {
            int fpMargin = searchParams.FpBase + searchParams.FpDepth * depth;
            if (corrEval + fpMargin <= alpha) {
                board.unmakeMove(m, undo);
                continue;
            }
        }

        // Move count based pruning: at shallow depths, skip late quiet moves
        // beyond a threshold, as they are unlikely to beat alpha
        if (!inCheck && depth <= 5 && moveIndex > 0 && !capture && !isPromotion && !givesCheck &&
            alpha > -MATE_SCORE + MAX_PLY && beta < MATE_SCORE - MAX_PLY) {
            int moveCountThreshold = (3 + depth * depth) / (2 - improving);
            if (moveIndex >= moveCountThreshold) {
                board.unmakeMove(m, undo);
                continue;
            }
        }

        int newDepth = depth - 1 + moveExtension;

        int score;
        if (moveIndex == 0) {
            score = -negamax(board, newDepth, ply + 1, -beta, -alpha, state);
        } else {
            // Late move reductions
            int reduction = 0;
            if (depth >= 3 && moveIndex >= 2 && !capture && !isPromotion && !inCheck &&
                !givesCheck) {
                reduction = lmrReductions[std::min(depth, MAX_PLY - 1)]
                                         [std::min(moveIndex, MAX_LMR_MOVES - 1)];

                // Reuse the butterfly + multi-ply continuation history captured
                // during move ordering. The divisor scales with the added tiers
                // so the reduction magnitude stays comparable to the single-tier
                // configuration.
                reduction -= pm.histScore / 24576;

                // Reduce less when position is improving
                reduction -= improving;

                reduction = std::max(0, std::min(reduction, newDepth - 1));
            }

            // Reduced null-window search
            score = -negamax(board, newDepth - reduction, ply + 1, -alpha - 1, -alpha, state);

            // Re-search at full depth if reduced search beats alpha
            if (reduction > 0 && score > alpha) {
                score = -negamax(board, newDepth, ply + 1, -alpha - 1, -alpha, state);
            }

            // PVS: re-search with full window if null-window search beats alpha
            if (score > alpha && score < beta) {
                score = -negamax(board, newDepth, ply + 1, -beta, -alpha, state);
            }
        }

        board.unmakeMove(m, undo);
        movesSearched++;
        if (state.stopped) return 0;
        if (score > bestScore) {
            bestScore = score;
            bestMove = m;
        }
        if (score > alpha) {
            alpha = score;
            state.pv[ply][ply] = m;
            for (int i = ply + 1; i < state.pvLength[ply + 1]; i++) {
                state.pv[ply][i] = state.pv[ply + 1][i];
            }
            state.pvLength[ply] = state.pvLength[ply + 1];
        }
        if (alpha >= beta) {
            bool cap = isCapture(board, m);
            if (cap) {
                // Reward the capture that caused the cutoff
                PieceType pt = board.squares[m.from].type;
                PieceType ct = capturedType(board, m);
                applyHistoryBonus(state.captureHistory[pt][m.to][ct], bonus, MAX_HISTORY);
                // Penalize previously searched captures
                for (int i = 0; i < numSearchedCaptures; i++) {
                    const Move &prev = searchedCaptures[i];
                    PieceType prevPt = board.squares[prev.from].type;
                    PieceType prevCt = capturedType(board, prev);
                    applyHistoryBonus(state.captureHistory[prevPt][prev.to][prevCt], -bonus,
                                      MAX_HISTORY);
                }
            } else {
                // Killer move update
                state.killers[ply][1] = state.killers[ply][0];
                state.killers[ply][0] = m;
                // Butterfly history reward and continuation history reward
                Color us = board.sideToMove;
                applyHistoryBonus(state.historyTables->mainHistory[us][m.from][m.to], bonus,
                                  MAX_HISTORY);
                for (int i = 0; i < numSearchedQuiets; i++) {
                    const Move &prev = searchedQuiets[i];
                    applyHistoryBonus(state.historyTables->mainHistory[us][prev.from][prev.to],
                                      -bonus, MAX_HISTORY);
                }
                if (ply >= 1) {
                    PieceType prevPt = state.movedPiece[ply - 1];
                    int prevTo = state.moveStack[ply - 1].to;
                    PieceType currPt = board.squares[m.from].type;
                    Color prevColor = (us == White) ? Black : White;
                    state.historyTables->counterMoves[prevColor][prevPt][prevTo] = m;
                    updateContHistoryAll(state, ply, currPt, m.to, bonus);
                    // Penalize previously searched quiets across every tier
                    for (int i = 0; i < numSearchedQuiets; i++) {
                        const Move &prev = searchedQuiets[i];
                        PieceType qPt = board.squares[prev.from].type;
                        updateContHistoryAll(state, ply, qPt, prev.to, -bonus);
                    }
                }
            }
            break;
        }

        // Track searched moves for malus
        if (isCapture(board, m)) {
            if (numSearchedCaptures < 64) searchedCaptures[numSearchedCaptures++] = m;
        } else {
            if (numSearchedQuiets < 64) searchedQuiets[numSearchedQuiets++] = m;
        }

        moveIndex++;
    }

    // Terminal detection: the eager generateLegalMoves gate was removed when
    // the picker took over, so mate / stalemate is recognized here by the
    // picker having yielded nothing. During singular extension searches a
    // zero-move result means only the excluded candidate was legal, and we
    // fall through to the alpha return so the outer search marks singular.
    if (movesSearched == 0) {
        if (hasExcludedMove) return alpha;
        return inCheck ? -(MATE_SCORE - ply) : 0;
    }

    // TT store (skip during singular searches to avoid overwriting deeper entries)
    if (!hasExcludedMove) {
        TTFlag flag;
        if (bestScore <= origAlpha) {
            flag = TT_UPPER_BOUND;
        } else if (bestScore >= beta) {
            flag = TT_LOWER_BOUND;
        } else {
            flag = TT_EXACT;
        }
        int storedEval = inCheck ? TT_NO_EVAL : staticEval;
        tt.store(board.key, bestScore, storedEval, depth, flag, bestMove, ply);

        // Fold the search result into every correction-history table whenever
        // we have a quiet best move and a bound that actually informs the
        // correction: exact, a passing lower bound, or a failing upper bound.
        // Captures and promotions skew the correction target so we omit them.
        // The ply gate for the position-keyed tables is applied inside
        // `updateCorrectionHistories` so the continuation-keyed term can still
        // update at ply 2 where it remains safe.
        if (!inCheck && depth >= 2 && bestMove.from != bestMove.to && !isCapture(board, bestMove) &&
            bestMove.promotion == None) {
            bool boundUseful = (flag == TT_EXACT) ||
                               (flag == TT_LOWER_BOUND && bestScore >= beta) ||
                               (flag == TT_UPPER_BOUND && bestScore <= origAlpha);
            if (boundUseful) {
                // Pass the CORRECTED eval as the bonus baseline so the update
                // measures the residual error after the current correction is
                // applied. Once the correction converges to the right value,
                // bestScore - corrEval goes to zero and the update stops
                // strengthening the entries further.
                updateCorrectionHistories(board, state, ply, corrEval, bestScore, depth);
            }
        }
    }

    return bestScore;
}

static int64_t computeTimeAllocation(const Board &board, const SearchLimits &limits) {
    if (limits.infinite) return std::numeric_limits<int64_t>::max();
    if (limits.depth > 0) return std::numeric_limits<int64_t>::max();
    if (limits.movetime > 0) return limits.movetime;

    int64_t timeLeft = (board.sideToMove == White) ? limits.wtime : limits.btime;
    int64_t increment = (board.sideToMove == White) ? limits.winc : limits.binc;
    int movesToGo = (limits.movestogo > 0) ? limits.movestogo : 25;

    int64_t allocated = timeLeft / movesToGo + increment;
    allocated = std::min(allocated, timeLeft * 4 / 5);
    allocated = std::max(allocated, static_cast<int64_t>(10));

    return allocated;
}

static void printSearchInfo(int depth, const SearchState &state, int score, int64_t timeMs,
                            BoundType bound, int multiPV) {
    // Floor the reported time at 1 ms so shallow iterations do not emit
    // "time 0" or a degenerate nps derived from zero elapsed time.
    int64_t reportedTimeMs = std::max<int64_t>(timeMs, 1);
    int64_t nps = state.nodes * 1000 / reportedTimeMs;

    std::cout << "info depth " << depth << " seldepth " << state.seldepth << " multipv " << multiPV;

    if (std::abs(score) >= MATE_SCORE - 100) {
        int matePly = MATE_SCORE - std::abs(score);
        int mateInMoves = (matePly + 1) / 2;
        if (score < 0) mateInMoves = -mateInMoves;
        std::cout << " score mate " << mateInMoves;
    } else {
        // Convert internal eval grain (~228 per pawn after material rescale)
        // into standard UCI centipawns so GUI-reported scores stay intuitive.
        constexpr int NormalizePawn = 228;
        int cp = score * 100 / NormalizePawn;
        std::cout << " score cp " << cp;
    }

    if (bound == BOUND_LOWER) {
        std::cout << " lowerbound";
    } else if (bound == BOUND_UPPER) {
        std::cout << " upperbound";
    }

    // Field order mirrors Stockfish so GUIs regression-tested against it see
    // a byte-identical shape. tbhits is stubbed at 0 since we never probe
    // tablebases.
    std::cout << " nodes " << state.nodes << " nps " << nps << " hashfull " << getHashfull()
              << " tbhits 0 time " << reportedTimeMs << " pv";
    for (int i = 0; i < state.pvLength[0]; i++) {
        std::cout << " " << moveToString(state.pv[0][i]);
    }
    std::cout << std::endl;
}

void startSearch(const Board &board, const SearchLimits &limits, SearchState &state,
                 const std::vector<uint64_t> &positionHistory) {
    state.stopped = false;
    state.nodes = 0;
    state.bestMove = {0, 0, None};
    memset(state.killers, 0, sizeof(state.killers));
    state.positionHistory = positionHistory;

    // Tag every store from this root search with a fresh generation so the
    // aging replacement rule can discount entries from prior searches.
    tt.new_search();
    state.searchKeys[0] = board.key;
    // Raw root eval is position-only and safe to compute once; the corrected
    // value is refreshed per iteration below so the ply=2 improving baseline
    // stays in the same units as the live correction tables.
    int rootRawEval = evaluate(board);
    state.staticEvals[0] = correctedEval(rootRawEval, board, state, 0);
    state.startTime = std::chrono::steady_clock::now();
    state.allocatedTimeMs = computeTimeAllocation(board, limits);

    int maxDepth = (limits.depth > 0) ? limits.depth : 100;

    Board pos = board;
    std::vector<Move> rootMoves = generateLegalMoves(pos);
    if (rootMoves.empty()) return;

    // Snapshot the MultiPV setting for the whole search. The UCI loop drains
    // any in-flight search before accepting another setoption, but the
    // snapshot keeps per-search state self-consistent regardless.
    const int multiPVRequested = getMultiPV();
    std::vector<int> prevSlotScores(multiPVRequested, 0);

    auto isSameMove = [](const Move &a, const Move &b) {
        return a.from == b.from && a.to == b.to && a.promotion == b.promotion;
    };

    for (int depth = 1; depth <= maxDepth; depth++) {
        state.rootDepth = depth;
        state.extensionsOnPath[0] = 0;
        // Refresh the root corrected eval against the live correction tables.
        // Interior nodes read this via improving at ply 2; letting it drift
        // against a frozen seed produces mismatched pruning decisions as the
        // correction tables evolve during the iteration.
        state.staticEvals[0] = correctedEval(rootRawEval, board, state, 0);

        const int numSlots = std::min<int>(multiPVRequested, static_cast<int>(rootMoves.size()));
        std::vector<Move> excludedMoves;
        excludedMoves.reserve(numSlots);

        Move slotZeroBest = rootMoves[0];
        bool mateFound = false;

        for (int slot = 0; slot < numSlots; slot++) {
            int delta = 60;
            int alpha, beta;
            int slotPrev = prevSlotScores[slot];

            // Aspiration windows only seed slot 0 and only once the previous
            // score is reliable; other slots start at full width because
            // their score is unconstrained relative to slot 0.
            if (slot == 0 && depth >= 4 && std::abs(slotPrev) < MATE_SCORE - 100) {
                alpha = std::max(slotPrev - delta, -INF_SCORE);
                beta = std::min(slotPrev + delta, INF_SCORE);
            } else {
                alpha = -INF_SCORE;
                beta = INF_SCORE;
            }

            // Seed currentBest with the first non-excluded move so we always
            // have a valid move to emit even on full fail-low.
            Move currentBest = {0, 0, None};
            for (const Move &m : rootMoves) {
                bool excluded = false;
                for (const Move &ex : excludedMoves) {
                    if (isSameMove(ex, m)) {
                        excluded = true;
                        break;
                    }
                }
                if (!excluded) {
                    currentBest = m;
                    break;
                }
            }
            int currentBestScore = -INF_SCORE;

            while (true) {
                state.seldepth = 0;
                currentBestScore = -INF_SCORE;
                int localAlpha = alpha;
                state.pvLength[0] = 0;
                bool firstSearched = false;

                for (size_t mi = 0; mi < rootMoves.size(); mi++) {
                    const Move &m = rootMoves[mi];

                    bool excluded = false;
                    for (const Move &ex : excludedMoves) {
                        if (isSameMove(ex, m)) {
                            excluded = true;
                            break;
                        }
                    }
                    if (excluded) continue;

                    // Only print per-move progress once the search has been
                    // running long enough for a GUI update to matter. Below
                    // the threshold, shallow iterations just show the PV in
                    // printSearchInfo so the output stays focused.
                    if (depth >= 2) {
                        auto nowForInfo = std::chrono::steady_clock::now();
                        int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                nowForInfo - state.startTime)
                                                .count();
                        if (elapsedMs >= 3000) {
                            std::cout << "info depth " << depth << " currmove " << moveToString(m)
                                      << " currmovenumber " << (mi + 1) << std::endl;
                        }
                    }

                    state.moveStack[0] = m;
                    state.movedPiece[0] = pos.squares[m.from].type;

                    UndoInfo undo = pos.makeMove(m);

                    int score;
                    if (!firstSearched) {
                        score = -negamax(pos, depth - 1, 1, -beta, -localAlpha, state);
                        firstSearched = true;
                    } else {
                        // PVS: null-window search for non-first moves
                        score = -negamax(pos, depth - 1, 1, -localAlpha - 1, -localAlpha, state);
                        if (score > localAlpha && score < beta) {
                            score = -negamax(pos, depth - 1, 1, -beta, -localAlpha, state);
                        }
                    }

                    pos.unmakeMove(m, undo);
                    if (state.stopped) break;
                    if (score > currentBestScore) {
                        currentBestScore = score;
                        currentBest = m;
                    }
                    if (score > localAlpha) {
                        localAlpha = score;
                        state.pv[0][0] = m;
                        for (int i = 1; i < state.pvLength[1]; i++) {
                            state.pv[0][i] = state.pv[1][i];
                        }
                        state.pvLength[0] = state.pvLength[1];
                    }
                }

                if (state.stopped) break;

                auto now = std::chrono::steady_clock::now();
                int64_t timeMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - state.startTime)
                        .count();

                if (currentBestScore <= alpha) {
                    // Fail-low: score is below the window, widen downward
                    // Ensure the PV contains at least the best move for UCI output
                    if (state.pvLength[0] == 0) {
                        state.pv[0][0] = currentBest;
                        state.pvLength[0] = 1;
                    }
                    printSearchInfo(depth, state, currentBestScore, timeMs, BOUND_UPPER, slot + 1);
                    beta = (alpha + beta) / 2;
                    alpha = std::max(currentBestScore - delta, -INF_SCORE);
                    delta *= 4;
                } else if (currentBestScore >= beta) {
                    // Fail-high: score is above the window, widen upward
                    if (slot == 0) state.bestMove = currentBest;
                    printSearchInfo(depth, state, currentBestScore, timeMs, BOUND_LOWER, slot + 1);
                    beta = std::min(currentBestScore + delta, INF_SCORE);
                    delta *= 4;
                } else {
                    // Exact: score is within the aspiration window
                    break;
                }

                // Fall back to full window after enough widening
                if (alpha <= -INF_SCORE && beta >= INF_SCORE) break;
            }

            if (state.stopped) break;

            prevSlotScores[slot] = currentBestScore;

            auto now = std::chrono::steady_clock::now();
            int64_t timeMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - state.startTime)
                    .count();

            if (slot == 0) {
                slotZeroBest = currentBest;
                state.bestMove = currentBest;
                if (state.pvLength[0] >= 2) {
                    state.ponderMove = state.pv[0][1];
                } else {
                    state.ponderMove = {0, 0, None};
                }
                if (std::abs(currentBestScore) >= MATE_SCORE - 100) mateFound = true;
            }

            printSearchInfo(depth, state, currentBestScore, timeMs, BOUND_EXACT, slot + 1);

            excludedMoves.push_back(currentBest);
        }

        if (state.stopped) break;

        // Move the slot-0 best move to the front for the next iteration
        for (size_t i = 0; i < rootMoves.size(); i++) {
            if (isSameMove(rootMoves[i], slotZeroBest)) {
                std::swap(rootMoves[0], rootMoves[i]);
                break;
            }
        }

        if (mateFound) break;
    }

    if (state.bestMove.from == 0 && state.bestMove.to == 0 && !rootMoves.empty()) {
        state.bestMove = rootMoves[0];
    }
}

Move findBestMove(const Board &board, int depth) {
    SearchLimits limits;
    limits.depth = depth;
    SearchState state;
    startSearch(board, limits, state);
    return state.bestMove;
}

void setHashSize(size_t mb) {
    tt.resize(mb);
}

void clearTT() {
    tt.clear();
}

int getHashfull() {
    return tt.hashfull();
}

static int g_multiPV = 1;

void setMultiPV(int n) {
    if (n < 1) n = 1;
    if (n > 256) n = 256;
    g_multiPV = n;
}

int getMultiPV() {
    return g_multiPV;
}

void clearHistory(SearchState &state) {
    memset(state.captureHistory, 0, sizeof(state.captureHistory));
    if (state.historyTables) {
        std::memset(state.historyTables.get(), 0, sizeof(SearchState::HistoryTables));
    }
}

void rebuildLmrTable() {
    double base = searchParams.LmrBase / 100.0;
    double div = searchParams.LmrDivisor / 100.0;
    for (int d = 0; d < MAX_PLY; d++) {
        for (int m = 0; m < MAX_LMR_MOVES; m++) {
            if (d == 0 || m == 0) {
                lmrReductions[d][m] = 0;
            } else {
                lmrReductions[d][m] = static_cast<int>(base + std::log(d) * std::log(m) / div);
            }
        }
    }
}

void initSearch() {
    rebuildLmrTable();
}
