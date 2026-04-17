#include "search.h"
#include "bitboard.h"
#include "eval.h"
#include "movegen.h"
#include "see.h"
#include "tt.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

static constexpr int MAX_HISTORY = 16384;
static constexpr int MAX_CONT_HISTORY = 16384;
static constexpr int MAX_PAWN_CORR = 16384;
static constexpr int PAWN_CORR_SIZE = 16384;
// Internal eval divisor; with the rescaled material units (~228 per pawn) a
// saturated entry contributes ~56 cp, well inside the ~60 cp design cap.
static constexpr int PAWN_CORR_SCALE = 128;
static constexpr int MAX_LMR_MOVES = 256;

static int lmrReductions[MAX_PLY][MAX_LMR_MOVES];

static TranspositionTable tt(16);

enum BoundType { BOUND_EXACT, BOUND_LOWER, BOUND_UPPER };

// Pairs a move with its ordering score so the sort comparator does not
// re-evaluate scoreMove on every comparison. The quiet history portion is
// captured alongside so the LMR reduction term can read it directly.
struct ScoredMove {
    int score;
    int historyScore;
    Move move;
};

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
static constexpr int CONT_HIST_OFFSETS[3] = {1, 2, 4};

// Sum the continuation-history contribution across all tiers for (currPt, currTo)
// at the given ply. Tiers whose offset exceeds ply are skipped.
static int contHistoryScore(const SearchState &state, int ply, PieceType currPt, int currTo) {
    int score = 0;
    for (int t = 0; t < 3; t++) {
        int off = CONT_HIST_OFFSETS[t];
        if (ply >= off) {
            PieceType prevPt = state.movedPiece[ply - off];
            int prevTo = state.moveStack[ply - off].to;
            score += state.historyTables->contHistory[t][prevPt][prevTo][currPt][currTo];
        }
    }
    return score;
}

// Adjust a raw static eval by the pawn-structure correction for the side to
// move. Mate-range scores are returned untouched so the correction cannot
// accidentally push a non-mate eval into mate territory.
static int correctedEval(int staticEval, const Board &board, const SearchState &state) {
    if (staticEval <= -MATE_SCORE + MAX_PLY || staticEval >= MATE_SCORE - MAX_PLY) {
        return staticEval;
    }
    int idx = static_cast<int>(board.pawnKey % PAWN_CORR_SIZE);
    int correction = state.historyTables->pawnCorrHist[board.sideToMove][idx] / PAWN_CORR_SCALE;
    return staticEval + correction;
}

// Fold the gap between the node's search result and its raw static eval into
// the correction table, scaled by depth. Gated by the caller to fire only on
// quiet bestMove cutoffs/exact bounds outside singular exclusion.
static void updatePawnCorrHist(const Board &board, SearchState &state, int staticEval,
                               int bestValue, int depth) {
    if (staticEval == -INF_SCORE) return;
    int idx = static_cast<int>(board.pawnKey % PAWN_CORR_SIZE);
    int diff = bestValue - staticEval;
    int bonus = diff * depth / 8;
    int cap = MAX_PAWN_CORR / 4;
    if (bonus > cap)
        bonus = cap;
    else if (bonus < -cap)
        bonus = -cap;
    applyHistoryBonus(state.historyTables->pawnCorrHist[board.sideToMove][idx], bonus,
                      MAX_PAWN_CORR);
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

static bool isCapture(const Board &board, const Move &m) {
    if (board.squares[m.to].type != None) return true;
    if (board.squares[m.from].type == Pawn && m.to == board.enPassantSquare &&
        board.enPassantSquare != -1)
        return true;
    return false;
}

static PieceType capturedType(const Board &board, const Move &m) {
    if (board.squares[m.to].type != None) return board.squares[m.to].type;
    if (board.squares[m.from].type == Pawn && m.to == board.enPassantSquare &&
        board.enPassantSquare != -1)
        return Pawn;
    return None;
}

static int scoreMove(const Move &m, const Board &board, const Move &ttMove, int ply,
                     const SearchState &state, int *outQuietHistory = nullptr) {
    PieceType pt = board.squares[m.from].type;
    bool capture = isCapture(board, m);
    bool quietCandidate = !capture && m.promotion == None;

    // Compute the quiet history once and expose it to the caller; LMR reads
    // this instead of recomputing the same lookups after move ordering.
    int quietHist = 0;
    if (quietCandidate) {
        quietHist = state.historyTables->mainHistory[board.sideToMove][m.from][m.to];
        if (ply >= 0) {
            quietHist += contHistoryScore(state, ply, pt, m.to);
        }
        if (outQuietHistory) *outQuietHistory = quietHist;
    }

    // TT move gets highest priority
    if (m.from == ttMove.from && m.to == ttMove.to && m.promotion == ttMove.promotion) {
        return 10000000;
    }

    // Promotions
    if (m.promotion != None) {
        if (m.promotion == Queen) return 9000000;
        return -5000000;
    }

    // Captures: use SEE to separate good from bad (skip SEE in quiescence, ply == -1)
    if (capture) {
        PieceType ct = capturedType(board, m);
        int capHist = state.captureHistory[pt][m.to][ct];
        if (ply < 0) {
            // Quiescence: use MVV-LVA + capture history (no SEE for speed)
            return 5000000 + PieceValue[ct] * 100 - PieceValue[pt] + capHist / 32;
        }
        int seeVal = see(board, m);
        if (seeVal >= 0) {
            return 5000000 + seeVal + capHist / 32;
        } else {
            return -2000000 + seeVal + capHist / 32;
        }
    }

    // Killer moves
    if (ply >= 0) {
        for (int k = 0; k < 2; k++) {
            const Move &killer = state.killers[ply][k];
            if (m.from == killer.from && m.to == killer.to && m.promotion == killer.promotion) {
                return 4000000 - k;
            }
        }
    }

    // Counter-move: the quiet reply that previously refuted the prior move
    if (ply >= 1) {
        Color prevColor = (board.sideToMove == White) ? Black : White;
        PieceType prevPt = state.movedPiece[ply - 1];
        int prevTo = state.moveStack[ply - 1].to;
        const Move &counter = state.historyTables->counterMoves[prevColor][prevPt][prevTo];
        if (m.from == counter.from && m.to == counter.to && m.promotion == counter.promotion) {
            return 3500000;
        }
    }

    // Plain quiet move: fall back to the precomputed history score.
    return quietHist;
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

static int quiescence(Board &board, int alpha, int beta, int ply, SearchState &state) {
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
        standPat = correctedEval(rawStandPat, board, state);
        bestScore = standPat;
        if (standPat >= beta) return standPat;
        if (standPat > alpha) alpha = standPat;
    }

    // When in check, search all legal moves (must escape check).
    // Otherwise, search only captures.
    std::vector<Move> moves = inCheck ? generateLegalMoves(board) : generateLegalCaptures(board);

    if (inCheck && moves.empty()) return -(MATE_SCORE - ply);

    // Ordering: promote the TT move when present, otherwise fall back to the
    // MVV-LVA + capture-history score used at leaves.
    std::vector<ScoredMove> scored;
    scored.reserve(moves.size());
    for (const Move &m : moves) {
        scored.push_back({scoreMove(m, board, ttMove, -1, state), 0, m});
    }
    std::sort(scored.begin(), scored.end(),
              [](const ScoredMove &a, const ScoredMove &b) { return a.score > b.score; });

    Move bestMove = {0, 0, None};

    for (const ScoredMove &sm : scored) {
        const Move &m = sm.move;
        if (!inCheck && isCapture(board, m)) {
            // Prune losing captures
            if (see(board, m) < 0) continue;
            // Delta pruning: skip captures that cannot raise alpha even on a
            // full recapture with a positional bonus. With a wider eval
            // distribution, standPat often sits well below alpha, so qsearch
            // fans out captures that have no chance of moving the score.
            if (m.promotion == None &&
                standPat + PieceValue[capturedType(board, m)] + 724 <= alpha) {
                continue;
            }
        }

        UndoInfo undo = board.makeMove(m);
        tt.prefetch(board.key);
        int score = -quiescence(board, -beta, -alpha, ply + 1, state);
        board.unmakeMove(m, undo);
        if (state.stopped) return 0;
        if (score > bestScore) {
            bestScore = score;
            bestMove = m;
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }

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
                   bool allowNullMove = true, Move excludedMove = {0, 0, None},
                   bool cutNode = false) {
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
    bool hasExcludedMove = (excludedMove.from != 0 || excludedMove.to != 0);

    // Mate distance pruning: once we know the best achievable score at this ply
    // is bounded by a mate in (MATE_SCORE - ply) and the worst is bounded by a
    // mated in (MATE_SCORE - ply - 1), clamping the window collapses any search
    // line that cannot improve on a mate we have already found closer to the
    // root. Singular exclusion searches pass a deliberately narrow window, so
    // we leave those untouched.
    if (ply > 0 && !hasExcludedMove) {
        int mateAlpha = std::max(alpha, -MATE_SCORE + ply);
        int mateBeta = std::min(beta, MATE_SCORE - ply - 1);
        if (mateAlpha >= mateBeta) return mateAlpha;
        alpha = mateAlpha;
        beta = mateBeta;
    }

    // TT probe
    TTEntry ttEntry;
    Move ttMove = {0, 0, None};
    bool ttHit = tt.probe(board.key, ttEntry, ply);
    if (ttHit) {
        ttMove = ttEntry.best_move;
        if (!hasExcludedMove && ttEntry.depth >= depth) {
            if (ttEntry.flag == TT_EXACT) {
                return ttEntry.score;
            }
            // In PV nodes, only cut on exact matches to preserve the full PV line
            if (!pvNode) {
                if (ttEntry.flag == TT_LOWER_BOUND && ttEntry.score >= beta) {
                    return ttEntry.score;
                }
                if (ttEntry.flag == TT_UPPER_BOUND && ttEntry.score <= alpha) {
                    return ttEntry.score;
                }
            }
        }
    }

    std::vector<Move> moves = generateLegalMoves(board);

    if (moves.empty()) {
        if (isInCheck(board)) return -(MATE_SCORE - ply);
        return 0;
    }

    if (depth <= 0) return quiescence(board, alpha, beta, ply, state);

    // Internal iterative reduction: if we lack a TT move at a node deep enough
    // to justify it, spend one ply less so the sibling search produces a TT
    // move for the eventual re-visit. Applies at both PV and non-PV nodes.
    if (depth >= 4 && ttMove.from == 0 && ttMove.to == 0) {
        depth -= 1;
    }

    // Move ordering: TT move first, then MVV-LVA for captures, then quiet moves.
    // Capture the quiet history for each move in the same pass so LMR does not
    // repeat the lookup.
    std::vector<ScoredMove> scored;
    scored.reserve(moves.size());
    for (const Move &m : moves) {
        int hist = 0;
        int s = scoreMove(m, board, ttMove, ply, state, &hist);
        scored.push_back({s, hist, m});
    }
    std::sort(scored.begin(), scored.end(),
              [](const ScoredMove &a, const ScoredMove &b) { return a.score > b.score; });

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
    state.staticEvals[ply] = staticEval;

    // Pawn-structure correction folded on top of the raw static eval. The raw
    // value still drives improving detection and is what we store in TT.
    int corrEval = inCheck ? -INF_SCORE : correctedEval(staticEval, board, state);

    // Determine if the position is improving (eval better than 2 plies ago)
    bool improving = false;
    if (inCheck) {
        improving = false;
    } else if (ply >= 2 && state.staticEvals[ply - 2] != -INF_SCORE) {
        improving = staticEval > state.staticEvals[ply - 2];
    } else if (ply >= 4 && state.staticEvals[ply - 4] != -INF_SCORE) {
        improving = staticEval > state.staticEvals[ply - 4];
    } else {
        improving = true;
    }

    // Razoring: at shallow non-PV depths, if the corrected eval plus a generous
    // margin still cannot reach alpha, the position is losing badly enough that
    // a full search is unlikely to recover. Drop straight to qsearch instead.
    if (!pvNode && !inCheck && depth <= 2 && alpha > -MATE_SCORE + MAX_PLY) {
        int razorMargin = 300 + 250 * depth;
        if (corrEval + razorMargin <= alpha) {
            return quiescence(board, alpha, beta, ply, state);
        }
    }

    // Reverse futility pruning: if static eval is far above beta at shallow depth,
    // assume this node will fail high
    if (!inCheck && depth <= 3 && beta - alpha == 1 && beta > -MATE_SCORE + MAX_PLY) {
        int rfpMargin = (290 - 145 * improving) * depth;
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
            int R = 3 + depth / 3 + std::clamp((corrEval - beta) / 483, 0, 3);
            R = std::min(R, depth - 1);
            UndoInfo nullUndo = board.makeNullMove();
            state.searchKeys[ply + 1] = board.key;
            int nullScore = -negamax(board, depth - 1 - R, ply + 1, -beta, -beta + 1, state, true,
                                     {0, 0, None}, !cutNode);
            board.unmakeNullMove(nullUndo);
            if (state.stopped) return 0;
            if (nullScore >= beta) {
                // Verification search at high depths to guard against zugzwang
                if (depth >= 8) {
                    int verifyScore = negamax(board, depth - 1 - R, ply, alpha, beta, state, false,
                                              {0, 0, None}, false);
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
            pcScored.push_back({scoreMove(m, board, noTT, -1, state), 0, m});
        }
        std::sort(pcScored.begin(), pcScored.end(),
                  [](const ScoredMove &a, const ScoredMove &b) { return a.score > b.score; });

        for (const ScoredMove &sm : pcScored) {
            const Move &pcMove = sm.move;
            if (!seeGE(board, pcMove, probcutBeta - corrEval)) continue;

            state.moveStack[ply] = pcMove;
            state.movedPiece[ply] = board.squares[pcMove.from].type;
            UndoInfo pcUndo = board.makeMove(pcMove);

            int pcScore = -negamax(board, probcutDepth, ply + 1, -probcutBeta, -probcutBeta + 1,
                                   state, true, {0, 0, None}, !cutNode);

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
                                    state, false, ttMove, cutNode);

        if (singularScore < singularBeta) {
            singularExtension = 1;
        }
    }

    int bestScore = -INF_SCORE;
    Move bestMove = scored[0].move;

    Move searchedCaptures[64];
    Move searchedQuiets[64];
    int numSearchedCaptures = 0;
    int numSearchedQuiets = 0;
    int bonus = std::min(depth * depth, 400);
    int movesSearched = 0;

    for (int moveIndex = 0; moveIndex < static_cast<int>(scored.size()); moveIndex++) {
        const Move &m = scored[moveIndex].move;

        // Skip the excluded move during singular extension searches
        if (hasExcludedMove && m.from == excludedMove.from && m.to == excludedMove.to &&
            m.promotion == excludedMove.promotion) {
            continue;
        }

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
            if (!seeGE(board, m, -48 * depth * depth)) {
                continue;
            }
        }

        // SEE pruning: skip quiet moves to heavily attacked squares at shallow depth
        if (!pvNode && !inCheck && moveIndex > 0 && !capture && !isPromotion && depth <= 8 &&
            bestScore > -MATE_SCORE + MAX_PLY) {
            if (!seeGE(board, m, -121 * depth)) {
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
            int fpMargin = 241 + 193 * depth;
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

        // History leaf pruning: late in the quiet list at shallow depth, a
        // move with badly negative combined butterfly + continuation history
        // is almost certainly noise. The threshold scales with depth so deep
        // searches tolerate more mediocre moves before pruning. Skipped near
        // mate windows so a low-history mating move cannot be discarded, and
        // gated past the first few quiets so a freshly-cleared history table
        // does not prune the engine's best move out from under itself.
        if (!pvNode && !inCheck && moveIndex >= 4 && !capture && !isPromotion && !givesCheck &&
            depth <= 5 && alpha > -MATE_SCORE + MAX_PLY && beta < MATE_SCORE - MAX_PLY) {
            int histThreshold = -3000 * depth;
            if (scored[moveIndex].historyScore < histThreshold) {
                board.unmakeMove(m, undo);
                continue;
            }
        }

        int newDepth = depth - 1 + moveExtension;

        int score;
        // Cut-node semantics for the children: the PV child of a PV node is a
        // PV node itself (cut-node does not apply, pass false). Every other
        // child is searched with a null window, so we alternate expected
        // node-type by parity: an expected cut-node's null-window child is an
        // expected all-node, and vice versa. Non-first PV children are always
        // expected cut-nodes since they are being tested against the PV move.
        bool pvChild = pvNode && moveIndex == 0;
        bool nullWindowCutNode = pvNode ? true : !cutNode;
        if (moveIndex == 0) {
            score = -negamax(board, newDepth, ply + 1, -beta, -alpha, state, true, {0, 0, None},
                             pvChild ? false : !cutNode);
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
                reduction -= scored[moveIndex].historyScore / 24576;

                // Reduce less when position is improving
                reduction -= improving;

                // Reduce harder at expected cut-nodes. These are nodes where
                // the parent search is betting on a beta cutoff, so a late
                // move that failed the earlier pruning checks is unlikely to
                // overturn that bet; saving a ply here is almost free.
                if (cutNode) reduction += 1;

                reduction = std::max(0, std::min(reduction, newDepth - 1));
            }

            // Reduced null-window search
            score = -negamax(board, newDepth - reduction, ply + 1, -alpha - 1, -alpha, state, true,
                             {0, 0, None}, nullWindowCutNode);

            // Re-search at full depth if reduced search beats alpha
            if (reduction > 0 && score > alpha) {
                score = -negamax(board, newDepth, ply + 1, -alpha - 1, -alpha, state, true,
                                 {0, 0, None}, nullWindowCutNode);
            }

            // PVS: re-search with full window if null-window search beats alpha
            if (score > alpha && score < beta) {
                score = -negamax(board, newDepth, ply + 1, -beta, -alpha, state, true, {0, 0, None},
                                 false);
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
    }

    // If all moves were pruned, fail low to avoid erroneous stalemate scores
    if (movesSearched == 0) return alpha;

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

        // Fold the search result into the pawn correction table whenever we have
        // a quiet best move and a bound that actually informs the correction:
        // exact, a passing lower bound, or a failing upper bound. Captures and
        // promotions skew the correction target so we omit them.
        if (!inCheck && depth >= 2 && bestMove.from != bestMove.to && !isCapture(board, bestMove) &&
            bestMove.promotion == None) {
            bool boundUseful = (flag == TT_EXACT) ||
                               (flag == TT_LOWER_BOUND && bestScore >= beta) ||
                               (flag == TT_UPPER_BOUND && bestScore <= origAlpha);
            if (boundUseful) {
                updatePawnCorrHist(board, state, staticEval, bestScore, depth);
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
                            BoundType bound) {
    int64_t nps = (timeMs > 0) ? (state.nodes * 1000 / timeMs) : state.nodes;

    std::cout << "info depth " << depth << " seldepth " << state.seldepth;

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

    std::cout << " nodes " << state.nodes << " nps " << nps << " time " << timeMs << " hashfull "
              << getHashfull() << " pv";
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
    state.staticEvals[0] = evaluate(board);
    state.startTime = std::chrono::steady_clock::now();
    state.allocatedTimeMs = computeTimeAllocation(board, limits);

    int maxDepth = (limits.depth > 0) ? limits.depth : 100;

    Board pos = board;
    std::vector<Move> rootMoves = generateLegalMoves(pos);
    if (rootMoves.empty()) return;

    int prevScore = 0;

    for (int depth = 1; depth <= maxDepth; depth++) {
        state.rootDepth = depth;
        state.extensionsOnPath[0] = 0;
        // Internal eval grain is ~228 per pawn, so 60 is roughly a 25 cp
        // window; tighter than that costs more in re-searches than it saves.
        int delta = 60;
        int alpha, beta;

        // Use aspiration windows at depth >= 4 when the previous score is not
        // a mate score. Earlier than depth 4 the score from the prior
        // iteration is too noisy to anchor a window without paying for it in
        // re-searches.
        if (depth >= 4 && std::abs(prevScore) < MATE_SCORE - 100) {
            alpha = std::max(prevScore - delta, -INF_SCORE);
            beta = std::min(prevScore + delta, INF_SCORE);
        } else {
            alpha = -INF_SCORE;
            beta = INF_SCORE;
        }

        Move currentBest = rootMoves[0];
        int currentBestScore = -INF_SCORE;

        // Aspiration window loop: re-search with wider windows on fail-low/fail-high
        while (true) {
            state.seldepth = 0;
            currentBest = rootMoves[0];
            currentBestScore = -INF_SCORE;
            int localAlpha = alpha;

            state.pvLength[0] = 0;

            for (size_t mi = 0; mi < rootMoves.size(); mi++) {
                const Move &m = rootMoves[mi];

                if (depth >= 2) {
                    std::cout << "info depth " << depth << " currmove " << moveToString(m)
                              << " currmovenumber " << (mi + 1) << std::endl;
                }

                state.moveStack[0] = m;
                state.movedPiece[0] = pos.squares[m.from].type;

                UndoInfo undo = pos.makeMove(m);

                int score;
                if (mi == 0) {
                    // Root PV child: always a PV node.
                    score = -negamax(pos, depth - 1, 1, -beta, -localAlpha, state, true,
                                     {0, 0, None}, false);
                } else {
                    // PVS: null-window search for non-first moves. Non-first
                    // root children are expected cut nodes; if they beat alpha
                    // we re-search as a PV node.
                    score = -negamax(pos, depth - 1, 1, -localAlpha - 1, -localAlpha, state, true,
                                     {0, 0, None}, true);
                    if (score > localAlpha && score < beta) {
                        score = -negamax(pos, depth - 1, 1, -beta, -localAlpha, state, true,
                                         {0, 0, None}, false);
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
                // Fail-low: score is below the window. Re-center beta and
                // widen alpha downward by a doubled delta. The beta re-center
                // keeps the window biased around the true score on the next
                // attempt rather than abandoning it.
                if (state.pvLength[0] == 0) {
                    state.pv[0][0] = currentBest;
                    state.pvLength[0] = 1;
                }
                printSearchInfo(depth, state, currentBestScore, timeMs, BOUND_UPPER);
                beta = (alpha + beta) / 2;
                alpha = std::max(currentBestScore - delta, -INF_SCORE);
                delta *= 2;
            } else if (currentBestScore >= beta) {
                // Fail-high: score is above the window. Re-center alpha and
                // widen beta upward by a doubled delta so a ricocheting score
                // does not force a full-window rewind on the next iteration.
                state.bestMove = currentBest;
                printSearchInfo(depth, state, currentBestScore, timeMs, BOUND_LOWER);
                alpha = (alpha + beta) / 2;
                beta = std::min(currentBestScore + delta, INF_SCORE);
                delta *= 2;
            } else {
                // Exact: score is within the aspiration window
                break;
            }

            // Fall back to full window after enough widening
            if (alpha <= -INF_SCORE && beta >= INF_SCORE) break;
        }

        if (state.stopped) break;

        prevScore = currentBestScore;
        state.bestMove = currentBest;

        if (state.pvLength[0] >= 2) {
            state.ponderMove = state.pv[0][1];
        } else {
            state.ponderMove = {0, 0, None};
        }

        // Move the best move to the front for the next iteration
        for (size_t i = 0; i < rootMoves.size(); i++) {
            if (rootMoves[i].from == currentBest.from && rootMoves[i].to == currentBest.to &&
                rootMoves[i].promotion == currentBest.promotion) {
                std::swap(rootMoves[0], rootMoves[i]);
                break;
            }
        }

        auto now = std::chrono::steady_clock::now();
        int64_t timeMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - state.startTime).count();

        printSearchInfo(depth, state, currentBestScore, timeMs, BOUND_EXACT);

        if (std::abs(currentBestScore) >= MATE_SCORE - 100) break;
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

void clearHistory(SearchState &state) {
    memset(state.captureHistory, 0, sizeof(state.captureHistory));
    if (state.historyTables) {
        std::memset(state.historyTables.get(), 0, sizeof(SearchState::HistoryTables));
    }
}

void initSearch() {
    for (int d = 0; d < MAX_PLY; d++) {
        for (int m = 0; m < MAX_LMR_MOVES; m++) {
            if (d == 0 || m == 0) {
                lmrReductions[d][m] = 0;
            } else {
                lmrReductions[d][m] = static_cast<int>(0.75 + std::log(d) * std::log(m) / 2.25);
            }
        }
    }
}
