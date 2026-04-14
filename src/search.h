#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include <atomic>
#include <chrono>
#include <cstdint>

struct SearchLimits {
    int depth = 0;
    int movetime = 0;
    int wtime = 0;
    int btime = 0;
    int winc = 0;
    int binc = 0;
    int movestogo = 0;
    bool infinite = false;
};

struct SearchState {
    std::atomic<bool> stopped{false};
    int64_t nodes = 0;
    Move bestMove = {0, 0, None};
    std::chrono::steady_clock::time_point startTime;
    int64_t allocatedTimeMs = 0;
};

void startSearch(const Board &board, const SearchLimits &limits, SearchState &state);

Move findBestMove(const Board &board, int depth = 1);

#endif
