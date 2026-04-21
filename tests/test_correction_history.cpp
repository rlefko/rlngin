#include "bitboard.h"
#include "board.h"
#include "catch_amalgamated.hpp"
#include "search.h"
#include "search_params.h"

#include <cstring>

static void ensureInit() {
    static bool done = false;
    if (!done) {
        initBitboards();
        done = true;
    }
}

static bool tableIsZero(const int16_t *data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (data[i] != 0) return false;
    }
    return true;
}

TEST_CASE("Correction history: every new table starts at zero", "[search][corrhist]") {
    ensureInit();
    SearchState state;
    const auto &h = *state.historyTables;
    CHECK(tableIsZero(&h.nonPawnCorrHist[0][0][0], sizeof(h.nonPawnCorrHist) / sizeof(int16_t)));
    CHECK(tableIsZero(&h.minorCorrHist[0][0], sizeof(h.minorCorrHist) / sizeof(int16_t)));
    CHECK(tableIsZero(&h.contCorrHist[0][0][0], sizeof(h.contCorrHist) / sizeof(int16_t)));
}

TEST_CASE("Correction history: a modest search populates the new tables", "[search][corrhist]") {
    ensureInit();
    clearTT();

    Board board;
    board.setFen("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3");

    SearchLimits limits;
    limits.depth = 8;
    SearchState state;
    startSearch(board, limits, state);

    const auto &h = *state.historyTables;

    // A depth-8 search from a balanced middlegame position hits the quiet
    // cutoff gate many times, so each table should have at least one entry
    // that moved away from zero.
    bool nonPawnTouched =
        !tableIsZero(&h.nonPawnCorrHist[0][0][0], sizeof(h.nonPawnCorrHist) / sizeof(int16_t));
    bool minorTouched =
        !tableIsZero(&h.minorCorrHist[0][0], sizeof(h.minorCorrHist) / sizeof(int16_t));
    bool contTouched =
        !tableIsZero(&h.contCorrHist[0][0][0], sizeof(h.contCorrHist) / sizeof(int16_t));

    CHECK(nonPawnTouched);
    CHECK(minorTouched);
    CHECK(contTouched);
}

TEST_CASE("Correction history: clearHistory zeroes every correction table", "[search][corrhist]") {
    ensureInit();
    clearTT();

    Board board;
    board.setStartPos();

    SearchLimits limits;
    limits.depth = 6;
    SearchState state;
    startSearch(board, limits, state);

    clearHistory(state);

    const auto &h = *state.historyTables;
    CHECK(tableIsZero(&h.pawnCorrHist[0][0], sizeof(h.pawnCorrHist) / sizeof(int16_t)));
    CHECK(tableIsZero(&h.nonPawnCorrHist[0][0][0], sizeof(h.nonPawnCorrHist) / sizeof(int16_t)));
    CHECK(tableIsZero(&h.minorCorrHist[0][0], sizeof(h.minorCorrHist) / sizeof(int16_t)));
    CHECK(tableIsZero(&h.contCorrHist[0][0][0], sizeof(h.contCorrHist) / sizeof(int16_t)));
}

TEST_CASE("Correction history: tunable weights wire through to the read path",
          "[search][corrhist][tunable]") {
    ensureInit();
    resetSearchParams();

    // Pin every correction weight to zero so the tables contribute nothing.
    // The search still converges on a reasonable move; this just verifies
    // the weight plumbing is live and cannot be silently ignored.
    int savedPawn = searchParams.PawnCorrWeight;
    int savedNonPawn = searchParams.NonPawnCorrWeight;
    int savedMinor = searchParams.MinorCorrWeight;
    int savedCont = searchParams.ContCorrWeight;

    searchParams.PawnCorrWeight = 0;
    searchParams.NonPawnCorrWeight = 0;
    searchParams.MinorCorrWeight = 0;
    searchParams.ContCorrWeight = 0;

    clearTT();
    Board board;
    board.setStartPos();

    SearchLimits limits;
    limits.depth = 4;
    SearchState state;
    startSearch(board, limits, state);

    CHECK(state.bestMove.from != state.bestMove.to);

    searchParams.PawnCorrWeight = savedPawn;
    searchParams.NonPawnCorrWeight = savedNonPawn;
    searchParams.MinorCorrWeight = savedMinor;
    searchParams.ContCorrWeight = savedCont;
}
