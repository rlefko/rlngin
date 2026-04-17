#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

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
    int seldepth = 0;
    Move bestMove = {0, 0, None};
    Move ponderMove = {0, 0, None};
    Move pv[MAX_PLY][MAX_PLY];
    int pvLength[MAX_PLY] = {};
    Move killers[MAX_PLY][2] = {};
    int captureHistory[7][64][7] = {};

    // Heap-allocated history tables. Grouping the large tables keeps
    // SearchState's stack footprint small and centralizes the clear path.
    struct HistoryTables {
        int mainHistory[2][64][64] = {};
        int16_t contHistory[7][64][7][64] = {};
    };
    std::unique_ptr<HistoryTables> historyTables = std::make_unique<HistoryTables>();

    Move moveStack[MAX_PLY] = {};
    PieceType movedPiece[MAX_PLY] = {};
    int staticEvals[MAX_PLY] = {};
    uint64_t searchKeys[MAX_PLY] = {};
    std::vector<uint64_t> positionHistory;
    std::chrono::steady_clock::time_point startTime;
    int64_t allocatedTimeMs = 0;
};

void startSearch(const Board &board, const SearchLimits &limits, SearchState &state,
                 const std::vector<uint64_t> &positionHistory = {});

Move findBestMove(const Board &board, int depth = 1);

void initSearch();
void setHashSize(size_t mb);
void clearTT();
int getHashfull();
void clearHistory(SearchState &state);

#endif
