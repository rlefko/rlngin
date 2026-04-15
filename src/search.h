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

struct SearchConfig {
    bool useQseePruning = true;
    bool useRfp = true;
    bool useNullMove = true;
    bool useFutilityPruning = true;
    bool useMoveCountPruning = true;
    bool useLmr = true;
    bool useAspirationWindows = true;
    bool debugSearchStats = false;
};

struct SearchStats {
    uint64_t qseePrunes = 0;
    uint64_t rfpCutoffs = 0;
    uint64_t nmpAttempts = 0;
    uint64_t nmpCutoffs = 0;
    uint64_t fpPrunes = 0;
    uint64_t lmpPrunes = 0;
    uint64_t lmrApplied = 0;
    uint64_t lmrResearched = 0;
    uint64_t aspirationFailLows = 0;
    uint64_t aspirationFailHighs = 0;
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

    // Continuation history: [prev_piece][prev_to][curr_piece][curr_to]
    // Heap-allocated to avoid stack overflow (~400KB)
    struct ContHistoryTable {
        int16_t data[7][64][7][64] = {};
    };
    std::unique_ptr<ContHistoryTable> contHistory = std::make_unique<ContHistoryTable>();

    Move moveStack[MAX_PLY] = {};
    PieceType movedPiece[MAX_PLY] = {};
    uint64_t searchKeys[MAX_PLY] = {};
    std::vector<uint64_t> positionHistory;
    std::chrono::steady_clock::time_point startTime;
    int64_t allocatedTimeMs = 0;
    SearchStats stats;
};

void startSearch(const Board &board, const SearchLimits &limits, SearchState &state,
                 const std::vector<uint64_t> &positionHistory = {},
                 const SearchConfig &config = {});

Move findBestMove(const Board &board, int depth = 1);

void initSearch();
void setHashSize(size_t mb);
void clearTT();
int getHashfull();
void clearHistory(SearchState &state);

#endif
