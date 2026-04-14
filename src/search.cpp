#include "search.h"
#include "eval.h"
#include "movegen.h"
#include <algorithm>
#include <iostream>
#include <limits>

static const int MATE_SCORE = 30000;

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
    for (int sq = 0; sq < 64; sq++) {
        if (board.squares[sq].type == King && board.squares[sq].color == side) {
            return isSquareAttacked(board, sq, opponent);
        }
    }
    return false;
}

static int negamax(const Board &board, int depth, int ply, SearchState &state) {
    state.nodes++;

    if (state.nodes % 1024 == 0) checkTime(state);
    if (state.stopped) return 0;

    std::vector<Move> moves = generateLegalMoves(board);

    if (moves.empty()) {
        if (isInCheck(board)) return -(MATE_SCORE - ply);
        return 0;
    }

    if (depth == 0) return evaluate(board);

    int bestScore = std::numeric_limits<int>::min();

    for (const Move &m : moves) {
        Board copy = board;
        copy.makeMove(m);
        int score = -negamax(copy, depth - 1, ply + 1, state);
        if (state.stopped) return 0;
        if (score > bestScore) bestScore = score;
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
        int currentBestScore = std::numeric_limits<int>::min();

        for (const Move &m : rootMoves) {
            Board copy = board;
            copy.makeMove(m);
            int score = -negamax(copy, depth - 1, 0, state);
            if (state.stopped) break;
            if (score > currentBestScore) {
                currentBestScore = score;
                currentBest = m;
            }
        }

        if (state.stopped) break;

        state.bestMove = currentBest;

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
