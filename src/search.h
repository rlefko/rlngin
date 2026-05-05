#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// Shared cap between the MovePicker scratch buffers and the LMR reduction
// table. Chess positions max out at around 218 legal moves; 256 is the
// theoretical safe upper bound with comfortable headroom.
constexpr int MOVE_PICKER_BUFFER_SIZE = 256;

// Pawn history table modulus. A power of two so the index reduction can use
// a bitmask instead of integer modulo. 512 mirrors what Stockfish-lineage
// engines pick: large enough to keep collisions sparse for the same pawn
// structure across distinct game phases, small enough that the full table
// fits inside roughly half a megabyte.
constexpr int PAWN_HIST_SIZE = 512;

// Scored entry shared between MovePicker and SearchState. Declared here so
// SearchState can preallocate per-ply scratch buffers on the heap; the
// MovePicker borrows those slots instead of carrying large arrays on the
// stack, which would blow the std::thread default stack limit at deep
// recursion.
struct ScoredMove {
    int score;
    int histScore;
    Move move;
};

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
        // Pawn-keyed quiet history: `[stm][pawnKey & (PAWN_HIST_SIZE - 1)][pieceType][toSquare]`.
        // Captures the signal that, given the current pawn structure, a
        // particular piece type moving to a particular square has refuted
        // recent search nodes. Distinct from `pawnCorrHist` (which corrects
        // the static eval); this table reorders quiet candidates the same
        // way the butterfly and continuation tables do.
        int16_t pawnHistory[2][PAWN_HIST_SIZE][7][64] = {};
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
        // Continuation correction keyed by a two-ply move chain:
        // `[prev2Piece][prev2To][prev1Piece][prev1To]`. Matches the context
        // granularity Stockfish uses so the table can tell apart sequences
        // like Nf3 then Nc6 from Nf3 then e5.
        int16_t contCorrHist[7][64][7][64] = {};
    };
    std::unique_ptr<HistoryTables> historyTables = std::make_unique<HistoryTables>();

    // Per-ply scratch buffers for the staged MovePicker. Holding these on
    // the heap keeps every negamax frame small; inlining them on the stack
    // cost about 11 KB per frame and overflowed the std::thread default
    // 512 KB stack at deep ply counts. The buffers are declared `mutable`
    // so a MovePicker built from a `const SearchState&` can still write
    // through them without triggering const-correctness UB under -O2.
    struct PickerBuffers {
        ScoredMove caps[MOVE_PICKER_BUFFER_SIZE];
        ScoredMove quiets[MOVE_PICKER_BUFFER_SIZE];
        int badCapIdx[MOVE_PICKER_BUFFER_SIZE];
    };
    mutable std::unique_ptr<PickerBuffers[]> pickerBuffers =
        std::unique_ptr<PickerBuffers[]>(new PickerBuffers[MAX_PLY]);

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

// Run a fixed-depth alpha-beta search from `root`, walk the resulting
// principal variation to its terminal position, then re-enter qsearch
// to land on a quiet leaf. Used by the Texel tuner's `--leaf-depth N`
// option to resolve more tactical noise (Andrew-Grant style PV-terminal
// corpus). depth <= 0 falls back to plain qsearchLeafBoard. Same TT
// preconditions as qsearchLeafBoard: thread_local TT means workers can
// run concurrently, but the call clears its own thread_local TT so a
// caller must not interleave it with other search activity on the same
// thread.
Board pvLeafBoard(const Board &root, int depth);

// Aggregate stats accumulated by every `qsearchLeafBoard` call since
// the last `resetQsearchLeafCounters()`. The tuner inspects these
// after `precomputeLeaves` so anomalies (returned-in-check leaves,
// TT-miss exits, hitting the iteration cap) surface explicitly rather
// than silently corrupting Texel labels.
struct QsearchLeafStats {
    uint64_t total = 0;
    uint64_t inCheck = 0;
    uint64_t ttMiss = 0;
    uint64_t cappedIterations = 0;
};
QsearchLeafStats qsearchLeafCounters();
void resetQsearchLeafCounters();

#endif
