#include "search.h"
#include "bitboard.h"
#include "eval.h"
#include "movegen.h"
#include "tt.h"
#include <algorithm>
#include <iostream>
#include <limits>

static TranspositionTable tt(16);

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

static int negamax(const Board &board, int depth, int ply, int alpha, int beta,
                   SearchState &state) {
    state.nodes++;

    if (state.nodes % 1024 == 0) checkTime(state);
    if (state.stopped) return 0;

    int origAlpha = alpha;

    // TT probe
    TTEntry ttEntry;
    Move ttMove = {0, 0, None};
    if (tt.probe(board.key, ttEntry, ply)) {
        ttMove = ttEntry.best_move;
        if (ttEntry.depth >= depth) {
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

    std::vector<Move> moves = generateLegalMoves(board);

    if (moves.empty()) {
        if (isInCheck(board)) return -(MATE_SCORE - ply);
        return 0;
    }

    if (depth == 0) return evaluate(board);

    // TT move ordering: move the TT move to the front
    if (ttMove.from != ttMove.to) {
        for (size_t i = 0; i < moves.size(); i++) {
            if (moves[i].from == ttMove.from && moves[i].to == ttMove.to &&
                moves[i].promotion == ttMove.promotion) {
                std::swap(moves[0], moves[i]);
                break;
            }
        }
    }

    int bestScore = -INF_SCORE;
    Move bestMove = moves[0];

    for (const Move &m : moves) {
        Board copy = board;
        copy.makeMove(m);
        int score = -negamax(copy, depth - 1, ply + 1, -beta, -alpha, state);
        if (state.stopped) return 0;
        if (score > bestScore) {
            bestScore = score;
            bestMove = m;
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }

    // TT store
    TTFlag flag;
    if (bestScore <= origAlpha) {
        flag = TT_UPPER_BOUND;
    } else if (bestScore >= beta) {
        flag = TT_LOWER_BOUND;
    } else {
        flag = TT_EXACT;
    }
    tt.store(board.key, bestScore, depth, flag, bestMove, ply);

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

void startSearch(const Board &board, const SearchLimits &limits, SearchState &state) {
    state.stopped = false;
    state.nodes = 0;
    state.bestMove = {0, 0, None};
    state.startTime = std::chrono::steady_clock::now();
    state.allocatedTimeMs = computeTimeAllocation(board, limits);

    int maxDepth = (limits.depth > 0) ? limits.depth : 100;

    std::vector<Move> rootMoves = generateLegalMoves(board);
    if (rootMoves.empty()) return;

    for (int depth = 1; depth <= maxDepth; depth++) {
        Move currentBest = rootMoves[0];
        int currentBestScore = -INF_SCORE;
        int alpha = -INF_SCORE;
        int beta = INF_SCORE;

        for (const Move &m : rootMoves) {
            Board copy = board;
            copy.makeMove(m);
            int score = -negamax(copy, depth - 1, 0, -beta, -alpha, state);
            if (state.stopped) break;
            if (score > currentBestScore) {
                currentBestScore = score;
                currentBest = m;
            }
            if (score > alpha) alpha = score;
        }

        if (state.stopped) break;

        state.bestMove = currentBest;

        // Move the best move to the front for the next iteration
        for (size_t i = 0; i < rootMoves.size(); i++) {
            if (rootMoves[i].from == currentBest.from && rootMoves[i].to == currentBest.to &&
                rootMoves[i].promotion == currentBest.promotion) {
                std::swap(rootMoves[0], rootMoves[i]);
                break;
            }
        }

        std::cout << "info depth " << depth << " score cp " << currentBestScore << " nodes "
                  << state.nodes << std::endl;

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
