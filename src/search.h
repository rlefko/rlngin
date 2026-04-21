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
        Move counterMoves[2][7][64] = {};
        // contHistory[tier][prevPt][prevTo][currPt][currTo] where tier 0 is
        // 1-ply back, tier 1 is 2-ply back, tier 2 is 4-ply back.
        int16_t contHistory[3][7][64][7][64] = {};
        // Pawn-keyed static eval correction: `[color][pawnKey % N]`. The signal
        // here is that for a given pawn structure, the search score tends to
        // diverge from the static eval in a particular direction.
        int16_t pawnCorrHist[2][16384] = {};
        // Non-pawn-keyed static eval correction: `[stm][pieceColor][nonPawnKey % N]`.
        // Each color's piece placement contributes an independent term so that
        // e.g. a white kingside-fianchetto bias is separable from black's.
        int16_t nonPawnCorrHist[2][2][16384] = {};
        // Minor-piece-keyed static eval correction: `[stm][minorKey % N]`.
        // Captures positional bias that depends on the bishop / knight layout
        // independent of the heavier pieces and pawn structure.
        int16_t minorCorrHist[2][16384] = {};
    };
    std::unique_ptr<HistoryTables> historyTables = std::make_unique<HistoryTables>();

    Move moveStack[MAX_PLY] = {};
    PieceType movedPiece[MAX_PLY] = {};
    int staticEvals[MAX_PLY] = {};
    uint64_t searchKeys[MAX_PLY] = {};
    // Running count of extensions accumulated from root down to this ply.
    // Used to cap total extensions per search path so forcing lines cannot
    // indefinitely expand the tree.
    int extensionsOnPath[MAX_PLY] = {};
    int rootDepth = 0;
    std::vector<uint64_t> positionHistory;
    std::chrono::steady_clock::time_point startTime;
    int64_t allocatedTimeMs = 0;
};

void startSearch(const Board &board, const SearchLimits &limits, SearchState &state,
                 const std::vector<uint64_t> &positionHistory = {});

Move findBestMove(const Board &board, int depth = 1);

void initSearch();
void rebuildLmrTable();
void setHashSize(size_t mb);
void clearTT();
int getHashfull();
void clearHistory(SearchState &state);
void setMultiPV(int n);
int getMultiPV();

// Quiescence-resolved static evaluation for use outside the search (e.g. the
// Texel tuner). Runs qsearch from the given position with an empty state and
// returns the side-to-move POV score.
int qsearchScore(const Board &board);

// Run qsearch from `root` and walk the resulting TT's best-move chain to
// return the quiet leaf position. Clears the TT internally so the walk
// only follows entries written by this call. Intended for Texel tuner
// dataset preparation: tuning against pre-resolved leaves lets the loss
// loop run cheap static evaluate() calls instead of full qsearch.
Board qsearchLeafBoard(const Board &root);

#endif
