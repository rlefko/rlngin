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
// Pawn history shares the saturation magnitude of the other quiet-history
// tables so the bonus and malus loops can reuse the same `applyHistoryBonus`
// gravity update without rescaling.
static constexpr int MAX_PAWN_HISTORY = 16384;
// Every correction-history table shares the same clamp magnitude and modulo
// size. Per-table weighting is handled by SearchParams.*CorrWeight so the
// relative contribution of each signal is tunable without changing storage.
static constexpr int MAX_CORR_HIST = 16384;
static constexpr int CORR_HIST_SIZE = 16384;
static constexpr int MAX_LMR_MOVES = 256;

static int lmrReductions[MAX_PLY][MAX_LMR_MOVES];

// Per-thread transposition table. The engine's UCI search runs on a
// single thread, so behavior there is identical to the prior global
// TT (the main thread instance is sized via setHashSize). The tuner's
// leaf precompute now spawns worker threads that each touch
// `qsearchLeafBoard`; with thread_local each worker gets its own 16 MB
// TT on first use, no inter-thread races on tt.clear() / tt.store().
// Loss-eval workers never call qsearch and so never default-construct
// their copies (thread_local with a non-trivial constructor only runs
// on first access in that thread).
static thread_local TranspositionTable tt(16);

enum BoundType { BOUND_EXACT, BOUND_LOWER, BOUND_UPPER };

// Root move record used only inside startSearch. Carrying a score per move
// lets iterative deepening reorder the root by the latest search verdict
// instead of leaving the movegen order frozen behind a single swap-to-front.
struct RootMove {
    Move move;
    int score = -INF_SCORE;
    int previousScore = -INF_SCORE;
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

// Tuner-leaf mode flag. While `qsearchLeafBoard` is running, delta and
// SEE pruning are turned off so every plausible capture gets resolved
// and the leaf the tuner labels is genuinely quiet. Real search reads
// this as `false`. Thread-local so the parallelized leaf precompute
// can set/reset per worker without races; default-false on every
// thread that hasn't entered tuner-leaf mode (real search).
static thread_local bool g_tunerLeafMode = false;

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
        if (!g_tunerLeafMode && !promoReady &&
            standPat + bestTargetValue + searchParams.QsDeltaMargin <= alpha) {
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
            // Prune losing captures. Disabled under tuner-leaf mode so
            // SEE-losing tactical captures still feed into the leaf
            // resolution.
            if (!g_tunerLeafMode && see(board, m) < 0) continue;
            // Delta pruning: skip captures that cannot raise alpha even on a
            // full recapture with a positional bonus. With a wider eval
            // distribution, standPat often sits well below alpha, so qsearch
            // fans out captures that have no chance of moving the score.
            // Also disabled under tuner-leaf mode.
            if (!g_tunerLeafMode && m.promotion == None &&
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

// Aggregate counters describing how `qsearchLeafBoard` exited across the
// entire precompute. The tuner reads these via `qsearchLeafCounters()`
// after the precompute finishes so a non-zero in-check count, a wave
// of TT-miss exits, or hitting the iteration cap shows up explicitly
// instead of silently producing noisy labels. Stored as atomics so
// the parallelized leaf precompute can increment from worker threads
// without racing.
static std::atomic<uint64_t> g_qsearchLeafTotal{0};
static std::atomic<uint64_t> g_qsearchLeafInCheck{0};
static std::atomic<uint64_t> g_qsearchLeafTtMiss{0};
static std::atomic<uint64_t> g_qsearchLeafCapped{0};

Board qsearchLeafBoard(const Board &root) {
    // Clear TT so the post-search walk only follows entries that this
    // call wrote. Not thread-safe: the tuner must run this step
    // sequentially before the multi-threaded loss loop starts.
    tt.clear();

    // Switch qsearch into tuner-leaf mode for the duration of this
    // call. Both delta pruning (against searchParams.QsDeltaMargin)
    // and SEE-loss capture pruning are disabled so every plausible
    // capture is resolved before the static eval is fitted to the
    // returned leaf. Real search keeps both prunes for speed; the
    // SPSA-tuned QsDeltaMargin is unchanged.
    g_tunerLeafMode = true;

    SearchState state;
    state.startTime = std::chrono::steady_clock::now();
    state.allocatedTimeMs = std::numeric_limits<int64_t>::max();
    Board copy = root;
    quiescence(copy, -INF_SCORE, INF_SCORE, 0, state);

    // Walk the best-move chain. Qsearch writes a zero move for pure
    // stand-pat returns; walking stops when no move or a TT miss is
    // seen. While the current position is in check we keep walking
    // along legal evasions even when they are non-captures, because
    // returning a still-in-check leaf to the static evaluator produces
    // meaningless label noise. Outside of check we still stop at the
    // first non-capture so the leaf reflects a quiet position. The
    // 32-iteration cap remains as a guard against pathological cycles.
    Board cur = root;
    int iter = 0;
    bool ttMissed = false;
    bool capped = false;
    for (; iter < 32; iter++) {
        TTEntry entry;
        if (!tt.probe(cur.key, entry, 0)) {
            ttMissed = true;
            break;
        }
        Move m = entry.best_move;
        if (m.from == 0 && m.to == 0 && m.promotion == None) break;
        bool inCheckNow = isInCheck(cur);
        if (!inCheckNow && !isCapture(cur, m)) break;
        cur.makeMove(m);
    }
    if (iter == 32) capped = true;

    g_qsearchLeafTotal.fetch_add(1, std::memory_order_relaxed);
    if (isInCheck(cur)) g_qsearchLeafInCheck.fetch_add(1, std::memory_order_relaxed);
    if (ttMissed) g_qsearchLeafTtMiss.fetch_add(1, std::memory_order_relaxed);
    if (capped) g_qsearchLeafCapped.fetch_add(1, std::memory_order_relaxed);

    g_tunerLeafMode = false;
    return cur;
}

QsearchLeafStats qsearchLeafCounters() {
    QsearchLeafStats s;
    s.total = g_qsearchLeafTotal.load(std::memory_order_relaxed);
    s.inCheck = g_qsearchLeafInCheck.load(std::memory_order_relaxed);
    s.ttMiss = g_qsearchLeafTtMiss.load(std::memory_order_relaxed);
    s.cappedIterations = g_qsearchLeafCapped.load(std::memory_order_relaxed);
    return s;
}
void resetQsearchLeafCounters() {
    g_qsearchLeafTotal.store(0, std::memory_order_relaxed);
    g_qsearchLeafInCheck.store(0, std::memory_order_relaxed);
    g_qsearchLeafTtMiss.store(0, std::memory_order_relaxed);
    g_qsearchLeafCapped.store(0, std::memory_order_relaxed);
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

// `cutNode` carries the Stockfish-style node-type signal: true marks a
// non-PV node where the parent expects a fail-high (children of a cut-node
// are searched with a null window and we are looking for any move that
// proves >= beta). False marks both PV nodes and all-nodes (where the
// parent expects fail-low). The flag flips between cut and all on every
// null-window recursion so descendants stay correctly classified, and
// resets to false on PV re-searches. Pruning and reduction sites read it
// to decide how aggressively to trim children of an expected cut.
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

    // TT probe
    TTEntry ttEntry;
    Move ttMove = {0, 0, None};
    bool hasExcludedMove = (excludedMove.from != 0 || excludedMove.to != 0);
    bool ttHit = tt.probe(board.key, ttEntry, ply);
    bool ttCapture = false;
    if (ttHit) {
        ttMove = ttEntry.best_move;
        // Cache whether the TT move is a capture so the singular block can
        // gate its multi-cut shortcut on it. A TT capture means quiet
        // alternatives are likely tactically dominated; we should not
        // shortcut past a singular search just because it happened to
        // surface another capture at shallow depth.
        if (ttMove.from != 0 || ttMove.to != 0) {
            ttCapture = isCapture(board, ttMove);
        }
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
    // move for the eventual re-visit. Restricted to PV and cut nodes,
    // matching modern Stockfish: all-nodes already prune aggressively, so
    // dropping a ply there only adds noise without buying a useful TT hint
    // for the next visit.
    if ((pvNode || cutNode) && depth >= 4 && ttMove.from == 0 && ttMove.to == 0) {
        depth -= 1;
    }
    // Cut-node IIR: at deep cut-nodes the search is already expected to fail
    // high, so an extra ply less at this visit is comparatively cheap when
    // the TT either has no best move or a much shallower record than the
    // current iteration. Stacks with the original rule when both apply, so a
    // depth-8 cut-node with no TT move loses two plies instead of one (the
    // original IIR drops 8 to 7, then this rule drops it to 6).
    if (cutNode && depth >= searchParams.IirCutNodeDepth &&
        (!ttHit || (ttMove.from == 0 && ttMove.to == 0) || ttEntry.depth + 4 <= depth)) {
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
            // The null-window child of a fail-high prediction is itself an
            // expected fail-low (all-node), so the flag flips for the recurse.
            int nullScore = -negamax(board, depth - 1 - R, ply + 1, -beta, -beta + 1, state, true,
                                     Move{0, 0, None}, !cutNode);
            board.unmakeNullMove(nullUndo);
            if (state.stopped) return 0;
            if (nullScore >= beta) {
                // Verification search at high depths to guard against zugzwang
                if (depth >= 8) {
                    // Verification re-enters with the original window at the
                    // same ply; treat it as a non-cut search so descendants
                    // do not inherit the original cut-node prediction.
                    int verifyScore = negamax(board, depth - 1 - R, ply, alpha, beta, state, false,
                                              Move{0, 0, None}, false);
                    if (state.stopped) return 0;
                    if (verifyScore >= beta) return beta;
                } else {
                    return beta;
                }
            }
        }
    }

    // ProbCut: use a shallow search of captures to predict whether a full-depth
    // search would produce a beta cutoff. Skipped inside a singular search:
    // ProbCut iterates legal captures including the excluded TT move, and
    // returning early would let the TT capture itself "verify" that another
    // move reaches singularBeta, defeating the singular check.
    if (!pvNode && !inCheck && !hasExcludedMove && depth >= 5 && beta > -MATE_SCORE + MAX_PLY &&
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

            // Null-window child of a node that just predicted fail-high; flip
            // the cut flag so the recurse sees itself as an all-node.
            int pcScore = -negamax(board, probcutDepth, ply + 1, -probcutBeta, -probcutBeta + 1,
                                   state, true, Move{0, 0, None}, !cutNode);

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
    if (depth >= 8 && ply > 0 && !inCheck && !hasExcludedMove &&
        (ttMove.from != 0 || ttMove.to != 0) && ttEntry.depth >= depth - 3 &&
        (ttEntry.flag == TT_LOWER_BOUND || ttEntry.flag == TT_EXACT) &&
        std::abs(ttEntry.score) < MATE_SCORE - MAX_PLY) {

        int singularBeta = ttEntry.score - searchParams.SingularBetaMul * depth;
        int singularDepth = (depth - 1) / searchParams.SingularDepthDiv;

        // Singular search asks "does any non-TT move at shallow depth reach
        // singularBeta". Pass cutNode unchanged so the underlying position's
        // node-type signal is preserved.
        int singularScore = negamax(board, singularDepth, ply, singularBeta - 1, singularBeta,
                                    state, false, ttMove, cutNode);

        if (singularScore < singularBeta) {
            singularExtension = 1;
            // Double extension: when alternatives fall well below
            // singularBeta the TT move is sharply better than every other
            // candidate. Gated to non-PV nodes; the existing per-path
            // extension budget keeps double extensions from stacking
            // indefinitely down forcing lines.
            if (!pvNode && singularScore < singularBeta - searchParams.SingularDoubleMargin) {
                singularExtension = 2;
            }
        } else if (singularBeta >= beta && !ttCapture) {
            // Multi-cut: the singular search proved a non-TT move also
            // reaches the parent's beta at shallow depth, so this node is
            // very likely to fail high too. Returning singularBeta is a
            // shallow lower bound; we deliberately do NOT store it in TT
            // (matches Stockfish), since the deeper search may still tighten
            // it. Gated on `!ttCapture` so a tactically loaded TT move does
            // not let us shortcut past the alternatives' real evaluation.
            return singularBeta;
        } else if (ttEntry.score >= beta) {
            // Strong negative extension: the TT score itself already proves
            // a fail-high, so the singular search not finding alternatives
            // is consistent with the TT verdict. Drop two plies because the
            // outcome is over-determined and any further work is wasted.
            singularExtension = -2;
        } else if (cutNode) {
            // Negative extension: TT score is below beta but we are at a
            // cut-node already expecting fail-high. Searching shallower
            // saves work because the TT verdict is likely correct.
            singularExtension = -1;
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

    // Enemy attack map, layered by attacker class. Built once per node and
    // shared between the move picker and the LMR block so both speak the
    // same language when they ask "is this square attacked by a lesser
    // enemy piece".
    ThreatMap threats;
    buildThreatMap(board, threats);

    // Staged move picker: the excluded move is threaded through as `skipMove`
    // so the singular-extension path never sees the excluded candidate in any
    // phase, including from the TT slot.
    MovePicker picker(board, state, ply, ttMove, inCheck, &threats);
    PickedMove pm;
    int moveIndex = 0;
    while (picker.next(pm, excludedMove)) {
        const Move &m = pm.move;

        state.moveStack[ply] = m;
        state.movedPiece[ply] = board.squares[m.from].type;

        bool capture = isCapture(board, m);
        bool isPromotion = (m.promotion != None);

        // Compute extensions before makeMove so piece types are still on the board.
        // Apply the singular verdict whether positive (extend) or negative
        // (search shallower) so the negative-extension path actually shaves
        // a ply on the TT move at cut-nodes.
        int moveExtension = 0;
        if (singularExtension != 0 && m.from == ttMove.from && m.to == ttMove.to &&
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
            moveExtension = std::min(moveExtension, 0);
        } else if (moveExtension > extBudget) {
            moveExtension = extBudget;
        }
        // Only positive extensions consume the per-path budget. Letting a
        // negative extension decrement the tracker would credit forcing
        // lines with extra room for later +1 / +2 extensions, which is the
        // opposite of the budget's purpose. Stockfish accumulates only
        // positive extensions for the same reason.
        state.extensionsOnPath[ply + 1] = state.extensionsOnPath[ply] + std::max(0, moveExtension);

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

        // Negative singular extensions only fire from the singular block
        // when remaining depth is at least 8, so the natural `depth - 1` step
        // here cannot push newDepth below 1 from a negative extension. We
        // deliberately do NOT clamp to 1 because doing so would block the
        // depth-1 -> qsearch transition that turns the leaves of the tree
        // into the quiescence search.
        int newDepth = depth - 1 + moveExtension;

        int score;
        if (moveIndex == 0) {
            // First move: full window. PV nodes start with cutNode=false; at
            // a non-PV node the first child of a cut prediction is the
            // candidate that should fail high, so its own children are then
            // expected to fail low, hence the flip.
            bool firstChildCutNode = pvNode ? false : !cutNode;
            score = -negamax(board, newDepth, ply + 1, -beta, -alpha, state, true, Move{0, 0, None},
                             firstChildCutNode);
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

                // Threat-aware adjustment. The tier sampled by the piece's own
                // value class answers "is the move entering or leaving a
                // square attacked by a strictly less-valuable enemy piece".
                // Evacuating a threatened piece onto a safe square deserves
                // a deeper look; walking a quiet piece into the same kind of
                // attack deserves a shallower one. Gated to the Quiets phase
                // so killer / counter / bad-capture moves (which have their
                // own priority signals) do not get reshuffled in depth.
                if (pm.phase == PickPhase::Quiets) {
                    PieceType movedPt = state.movedPiece[ply];
                    Bitboard tier = lesserAttackerTier(threats, movedPt);
                    if (tier) {
                        Bitboard fromBB = squareBB(m.from);
                        Bitboard toBB = squareBB(m.to);
                        bool fromThreatened = (tier & fromBB) != 0;
                        bool toThreatened = (tier & toBB) != 0;
                        if (fromThreatened && !toThreatened) {
                            reduction -= searchParams.LmrThreatEscape;
                        } else if (!fromThreatened && toThreatened) {
                            reduction += searchParams.LmrThreatWalkIn;
                        }
                    }
                }

                // Cut-node bonus: null-window nodes are expected to fail
                // high, so later quiets past the first few candidates
                // should be sampled shallower. Matches the Stockfish
                // cutNode heuristic; the moveIndex gate keeps the first
                // couple of candidate refutations searched at current
                // depth in case the TT or killer slots already pointed
                // at the right move.
                if (!pvNode && moveIndex >= 3) {
                    reduction += 1;
                }

                // TT-PV discount: a prior iteration resolved a PV node to
                // an exact bound, so quiet moves below it are likely
                // close to the true PV and benefit from a shallower
                // reduction. Gated on pvNode so the discount only fires
                // in the subtree that actually carries a PV signal,
                // which avoids polluting deep null-window subtrees with
                // TT entries that happen to be exact for unrelated
                // reasons.
                if (pvNode && ttHit && ttEntry.flag == TT_EXACT) {
                    reduction -= 1;
                }

                reduction = std::max(0, std::min(reduction, newDepth - 1));
            }

            // Reduced null-window search. Null-window children of a node are
            // expected to fail high relative to alpha+1, so they are cut-nodes.
            score = -negamax(board, newDepth - reduction, ply + 1, -alpha - 1, -alpha, state, true,
                             Move{0, 0, None}, true);

            // Re-search at full depth if reduced search beats alpha. The
            // earlier reduced search expected a cut and did not deliver one,
            // so flip the prediction: this re-search now treats the child as
            // an all-node relative to the parent's expectation.
            if (reduction > 0 && score > alpha) {
                score = -negamax(board, newDepth, ply + 1, -alpha - 1, -alpha, state, true,
                                 Move{0, 0, None}, !cutNode);
            }

            // PVS: re-search with full window if null-window search beats alpha.
            // PV re-searches are PV nodes, never cut-nodes.
            if (score > alpha && score < beta) {
                score = -negamax(board, newDepth, ply + 1, -beta, -alpha, state, true,
                                 Move{0, 0, None}, false);
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
                // Pawn-keyed history reward and malus. Pawn key is shared by
                // every move searched at this node (we make and unmake from
                // the same root position), so the index is computed once.
                int pawnIdx = static_cast<int>(board.pawnKey & (PAWN_HIST_SIZE - 1));
                PieceType cutPt = board.squares[m.from].type;
                applyHistoryBonus(state.historyTables->pawnHistory[us][pawnIdx][cutPt][m.to], bonus,
                                  MAX_PAWN_HISTORY);
                for (int i = 0; i < numSearchedQuiets; i++) {
                    const Move &prev = searchedQuiets[i];
                    PieceType prevPt = board.squares[prev.from].type;
                    applyHistoryBonus(
                        state.historyTables->pawnHistory[us][pawnIdx][prevPt][prev.to], -bonus,
                        MAX_PAWN_HISTORY);
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
    std::vector<Move> legalMoves = generateLegalMoves(pos);
    if (legalMoves.empty()) return;
    std::vector<RootMove> rootMoves;
    rootMoves.reserve(legalMoves.size());
    for (const Move &m : legalMoves)
        rootMoves.push_back({m});

    // Seed the PV with a legal move so that if the iter-deepening loop exits
    // before any aspiration iteration completes, the fallback info line at
    // the bottom of this function still has a move to report.
    state.pv[0][0] = rootMoves[0].move;
    state.pvLength[0] = 1;

    // Snapshot the MultiPV setting for the whole search. The UCI loop drains
    // any in-flight search before accepting another setoption, but the
    // snapshot keeps per-search state self-consistent regardless.
    const int multiPVRequested = getMultiPV();
    std::vector<int> prevSlotScores(multiPVRequested, 0);

    // Tracks whether any scored info line has been emitted during this
    // search. If the search is stopped before iteration 1 completes (for
    // example under tight time controls on complex positions) we fall back
    // to a single scored info line before returning so that UCI consumers
    // walking stdout for the engine's last score always find one.
    bool anyInfoPrinted = false;

    auto isSameMove = [](const Move &a, const Move &b) {
        return a.from == b.from && a.to == b.to && a.promotion == b.promotion;
    };

    // Cross-iteration stability signal. searchAgainCounter grows when the
    // previous iteration's aspiration saw a fail-low land after a fail-high,
    // which is the conventional "root score oscillated within a single
    // window" indicator that the search is still hunting for the true
    // verdict. It feeds adjustedDepth so oscillating searches spend less
    // depth per aspiration retry, recovering time that iterative deepening
    // will reinvest once the root choice stabilizes. The counter is
    // monotonic within a single root search and reset to zero at each new
    // startSearch call.
    int searchAgainCounter = 0;
    bool prevIterationFailLowAfterFailHigh = false;

    for (int depth = 1; depth <= maxDepth; depth++) {
        state.rootDepth = depth;
        state.extensionsOnPath[0] = 0;
        // Refresh the root corrected eval against the live correction tables.
        // Interior nodes read this via improving at ply 2; letting it drift
        // against a frozen seed produces mismatched pruning decisions as the
        // correction tables evolve during the iteration.
        state.staticEvals[0] = correctedEval(rootRawEval, board, state, 0);

        // Roll each root move's score into previousScore and reset the live
        // score so an iteration cut short by state.stopped cannot leak stale
        // scores into the next iteration's sort.
        for (auto &rm : rootMoves) {
            rm.previousScore = rm.score;
            rm.score = -INF_SCORE;
        }

        const int numSlots = std::min<int>(multiPVRequested, static_cast<int>(rootMoves.size()));
        std::vector<Move> excludedMoves;
        excludedMoves.reserve(numSlots);

        bool mateFound = false;
        // Aggregated across slots: did any aspiration loop on this iteration
        // fail low after already failing high? That oscillation is the signal
        // we pass through to next iteration's searchAgainCounter increment.
        bool iterFailLowAfterFailHigh = false;

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
            for (const RootMove &rm : rootMoves) {
                bool excluded = false;
                for (const Move &ex : excludedMoves) {
                    if (isSameMove(ex, rm.move)) {
                        excluded = true;
                        break;
                    }
                }
                if (!excluded) {
                    currentBest = rm.move;
                    break;
                }
            }
            int currentBestScore = -INF_SCORE;

            // Aspiration retry bookkeeping. failedHighCnt counts consecutive
            // fail-highs in this aspiration loop and feeds adjustedDepth so
            // each fail-high retry searches a ply shallower than the previous
            // attempt. A fail-low resets the counter so the next retry runs
            // at the full requested depth, because a fail-low signals that
            // the previous bound was too optimistic and we need real depth
            // to pin down the new worst case accurately. sawFailHigh and
            // sawFailLowAfterFailHigh track the within-iteration oscillation
            // pattern that feeds the cross-iteration searchAgainCounter.
            int failedHighCnt = 0;
            bool sawFailHigh = false;
            bool sawFailLowAfterFailHigh = false;

            while (true) {
                // Shorten the retry by one ply for every consecutive fail-high
                // in this aspiration loop, plus a gradual cross-iteration term
                // derived from searchAgainCounter. The `3 * (n + 1) / 4`
                // scaling yields roughly one extra ply of reduction per four
                // bestmove changes, matching the cadence convention strong
                // engines have converged on for the searchAgain idiom.
                // max(1, ...) preserves a floor so repeated re-widens never
                // collapse the root into qsearch.
                int adjustedDepth =
                    std::max(1, depth - failedHighCnt - 3 * (searchAgainCounter + 1) / 4);

                state.seldepth = 0;
                currentBestScore = -INF_SCORE;
                int localAlpha = alpha;
                state.pvLength[0] = 0;
                bool firstSearched = false;

                for (size_t mi = 0; mi < rootMoves.size(); mi++) {
                    RootMove &rm = rootMoves[mi];
                    const Move &m = rm.move;

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

                    // Capture state before the search so the post-search stamp
                    // decision can tell apart "first move" from "later move
                    // that happened to beat alpha".
                    const bool isFirstMove = !firstSearched;
                    const int alphaAtMoveStart = localAlpha;

                    int score;
                    if (!firstSearched) {
                        // Root PV move: full window, PV-style child.
                        score = -negamax(pos, adjustedDepth - 1, 1, -beta, -localAlpha, state, true,
                                         Move{0, 0, None}, false);
                        firstSearched = true;
                    } else {
                        // PVS: null-window search for non-first moves. The
                        // null-window child is expected to fail high (we are
                        // looking for any move that proves >= localAlpha+1),
                        // so it is a cut-node.
                        score = -negamax(pos, adjustedDepth - 1, 1, -localAlpha - 1, -localAlpha,
                                         state, true, Move{0, 0, None}, true);
                        if (score > localAlpha && score < beta) {
                            // Full-window PVS re-search: PV node, not cut.
                            score = -negamax(pos, adjustedDepth - 1, 1, -beta, -localAlpha, state,
                                             true, Move{0, 0, None}, false);
                        }
                    }

                    pos.unmakeMove(m, undo);
                    if (state.stopped) break;

                    // Match Stockfish's root bookkeeping: only record a real
                    // score when the result is meaningful. The first move
                    // always gets a full-window search, and any later move
                    // that beat alpha was re-searched with a full window. A
                    // move that just failed the null window returns an upper
                    // bound from an essentially unconstrained search below
                    // alpha; stamping that bound would let it outrank the
                    // former PV's real full-window score simply because the
                    // bound happened to land higher numerically. Leaving
                    // rm.score at its top-of-iteration -INF_SCORE sentinel
                    // clusters those moves together at the bottom of the
                    // sort in a stable order, keeping the current and former
                    // PVs naturally adjacent at the top.
                    if (isFirstMove || score > alphaAtMoveStart) rm.score = score;

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
                    // Fail-low: score is below the window, widen downward.
                    // Ensure the PV contains at least the best move for UCI output.
                    if (state.pvLength[0] == 0) {
                        state.pv[0][0] = currentBest;
                        state.pvLength[0] = 1;
                    }
                    printSearchInfo(depth, state, currentBestScore, timeMs, BOUND_UPPER, slot + 1);
                    anyInfoPrinted = true;
                    beta = (alpha + beta) / 2;
                    alpha = std::max(currentBestScore - delta, -INF_SCORE);
                    // Reset the fail-high counter: the next retry returns to
                    // the full requested depth so the widened window gets an
                    // accurate verdict rather than a shallower estimate.
                    failedHighCnt = 0;
                    if (sawFailHigh) sawFailLowAfterFailHigh = true;
                    delta *= 4;
                } else if (currentBestScore >= beta) {
                    // Fail-high: score is above the window, widen upward.
                    if (slot == 0) state.bestMove = currentBest;
                    printSearchInfo(depth, state, currentBestScore, timeMs, BOUND_LOWER, slot + 1);
                    anyInfoPrinted = true;
                    beta = std::min(currentBestScore + delta, INF_SCORE);
                    failedHighCnt++;
                    sawFailHigh = true;
                    delta *= 4;
                } else {
                    // Exact: score is within the aspiration window
                    break;
                }

                // Fall back to full window after enough widening
                if (alpha <= -INF_SCORE && beta >= INF_SCORE) break;
            }

            if (state.stopped) break;

            // Stably sort the tail of rootMoves by the scores just written
            // this iteration so the next slot (and the next iteration's
            // slot 0) sees the strongest candidates first. Moves with a
            // real score (the PV, any fail-high, and the first searched
            // move) float to the top; moves that only failed the null
            // window keep the -INF_SCORE sentinel from the top-of-iteration
            // reset and stable-sort into a deterministic tail preserving
            // their prior relative order. The slice [slot, end) is the
            // right range because earlier slots already locked in their
            // MultiPV picks and must not be reordered.
            std::stable_sort(
                rootMoves.begin() + slot, rootMoves.end(),
                [](const RootMove &a, const RootMove &b) { return a.score > b.score; });

            if (sawFailLowAfterFailHigh) iterFailLowAfterFailHigh = true;

            prevSlotScores[slot] = currentBestScore;

            auto now = std::chrono::steady_clock::now();
            int64_t timeMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - state.startTime)
                    .count();

            if (slot == 0) {
                state.bestMove = currentBest;
                if (state.pvLength[0] >= 2) {
                    state.ponderMove = state.pv[0][1];
                } else {
                    state.ponderMove = {0, 0, None};
                }
                if (std::abs(currentBestScore) >= MATE_SCORE - 100) mateFound = true;
            }

            printSearchInfo(depth, state, currentBestScore, timeMs, BOUND_EXACT, slot + 1);
            anyInfoPrinted = true;

            excludedMoves.push_back(currentBest);
        }

        if (state.stopped) break;

        // Growing the cross-iteration stability signal. The next iteration
        // tightens its aspiration retry budget when the previous iteration
        // already oscillated across its own window (a fail-low that landed
        // after a fail-high), because that oscillation is the clearest
        // signal the engine has that the true root score still sits beyond
        // where the current window is looking. Tying the counter to a
        // specific oscillation pattern rather than to any bestmove change
        // keeps stable searches at full depth and only spends the
        // searchAgain reduction when it is actually likely to help. The
        // counter only grows past an opening warmup threshold to avoid
        // biting at shallow depths where iterative deepening naturally
        // oscillates.
        if (depth > 6 && prevIterationFailLowAfterFailHigh) {
            searchAgainCounter++;
        }
        prevIterationFailLowAfterFailHigh = iterFailLowAfterFailHigh;

        if (mateFound) break;
    }

    if (state.bestMove.from == 0 && state.bestMove.to == 0 && !rootMoves.empty()) {
        state.bestMove = rootMoves[0].move;
    }

    // Fallback for searches that were stopped before any aspiration iteration
    // emitted a scored info line. Without this, fastchess (and any other UCI
    // consumer that scans stdout for the engine's last reported score) would
    // see a bestmove with no score context and warn.
    if (!anyInfoPrinted) {
        auto now = std::chrono::steady_clock::now();
        int64_t timeMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - state.startTime).count();
        printSearchInfo(1, state, state.staticEvals[0], timeMs, BOUND_EXACT, 1);
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

// PV-terminal leaf walk for the Texel tuner. Runs one fixed-depth
// alpha-beta search from the root, follows the principal variation
// recorded in `state.pv[0]` to the search horizon, then qsearches
// from there to land on a quiet leaf. Used by the tuner's
// `--leaf-depth N` option to resolve more tactical noise than plain
// qsearch (Andrew-Grant style PV-terminal corpus).
//
// depth <= 0 falls back to qsearchLeafBoard, which is the existing
// default behaviour. The search runs against the thread_local TT and
// a fresh SearchState, so concurrent worker threads can each call
// pvLeafBoard without contention. Time-limit infrastructure is
// disabled by setting `allocatedTimeMs` to its max so the search
// runs until the depth budget is exhausted.
Board pvLeafBoard(const Board &root, int depth) {
    if (depth <= 0) return qsearchLeafBoard(root);

    tt.clear();
    g_tunerLeafMode = false;

    SearchState state;
    state.startTime = std::chrono::steady_clock::now();
    state.allocatedTimeMs = std::numeric_limits<int64_t>::max();
    state.rootDepth = depth;

    Board working = root;
    (void)negamax(working, depth, 0, -INF_SCORE, INF_SCORE, state);

    Board cur = root;
    int pvLen = state.pvLength[0];
    for (int i = 0; i < pvLen; i++) {
        cur.makeMove(state.pv[0][i]);
    }

    // Re-enter qsearch from the PV terminal so the static eval sits
    // on a quiet position. qsearchLeafBoard handles its own TT clear
    // and resets g_tunerLeafMode, so we leave both untouched here.
    return qsearchLeafBoard(cur);
}

void initSearch() {
    rebuildLmrTable();
}
