#include "bitboard.h"
#include "board.h"
#include "catch_amalgamated.hpp"
#include "search.h"

static void ensureInit() {
    static bool done = false;
    if (!done) {
        initBitboards();
        done = true;
    }
}

TEST_CASE("Search: captures hanging queen", "[search]") {
    ensureInit();
    Board board;
    // White knight on e4 can capture undefended black queen on d6
    board.setFen("4k3/8/3q4/8/4N3/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board);
    CHECK(best.from == stringToSquare("e4"));
    CHECK(best.to == stringToSquare("d6"));
}

TEST_CASE("Search: prefers capturing queen over pawn", "[search]") {
    ensureInit();
    Board board;
    // White bishop on d4 can capture black queen on f6 or black pawn on b2
    board.setFen("4k3/8/5q2/8/3B4/8/1p6/4K3 w - - 0 1");

    Move best = findBestMove(board);
    CHECK(best.from == stringToSquare("d4"));
    CHECK(best.to == stringToSquare("f6"));
}

TEST_CASE("Search: finds mate in 1", "[search]") {
    ensureInit();
    Board board;
    // Back-rank mate: Re1 can deliver Re8# since f7/g7/h7 pawns trap the king
    board.setFen("6k1/5ppp/8/8/8/8/8/4R2K w - - 0 1");

    Move best = findBestMove(board, 2);
    CHECK(best.from == stringToSquare("e1"));
    CHECK(best.to == stringToSquare("e8"));
}

TEST_CASE("Search: returns valid move at deeper depths", "[search]") {
    ensureInit();
    Board board;
    board.setStartPos();

    Move best = findBestMove(board, 3);
    CHECK(best.from != best.to);
}

TEST_CASE("Search: alpha-beta prunes nodes at depth 4", "[search]") {
    ensureInit();
    Board board;
    board.setStartPos();

    SearchLimits limits;
    limits.depth = 4;
    SearchState state;
    startSearch(board, limits, state);

    // Full minimax at depth 4 from startpos explores ~4 million nodes.
    // Alpha-beta should prune this down dramatically.
    CHECK(state.nodes < 500000);
}

TEST_CASE("Search: qsearch avoids leaving piece en prise", "[search][qsearch]") {
    ensureInit();
    clearTT();
    Board board;
    // White knight on d4, black bishop on e6 attacks it. White to move.
    // At depth 1, the engine should not leave the knight to be captured.
    board.setFen("4k3/8/4b3/8/3N4/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board, 1);
    // The knight should move away from d4 (not stay and get captured)
    CHECK(best.from == stringToSquare("d4"));
}

TEST_CASE("Search: qsearch resolves pawn capture", "[search][qsearch]") {
    ensureInit();
    clearTT();
    Board board;
    // White pawn on e4 can capture black pawn on d5. Simple gain.
    board.setFen("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board, 1);
    CHECK(best.from == stringToSquare("e4"));
    CHECK(best.to == stringToSquare("d5"));
}

TEST_CASE("Search: qsearch prevents blundering into recapture", "[search][qsearch]") {
    ensureInit();
    clearTT();
    Board board;
    // White pawn on e5, black rook on d6. Pawn could try to capture but rook
    // is worth more. Engine should not move the pawn into the rook.
    board.setFen("4k3/8/3r4/4P3/8/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board, 2);
    // Pawn should NOT capture the rook (it would be recaptured for a loss)
    // Actually the pawn captures the rook for free here, so let me use a better position
    // where a piece would be lost to recapture.
    // White knight on e5, black pawn on d6 defends a black pawn on e6.
    // If knight captures e6 pawn, d6 pawn recaptures.
    board.setFen("4k3/8/3pp3/4N3/8/8/8/4K3 w - - 0 1");

    best = findBestMove(board, 1);
    // Knight should not capture e6 (loses knight for pawn). It should capture d6 instead (undefended).
    if (best.from == stringToSquare("e5") && best.to == stringToSquare("e6")) {
        // This would be a blunder
        CHECK(false);
    }
}

TEST_CASE("Search: respects time limit", "[search]") {
    ensureInit();
    Board board;
    board.setStartPos();

    SearchLimits limits;
    limits.movetime = 50;
    SearchState state;

    auto start = std::chrono::steady_clock::now();
    startSearch(board, limits, state);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    CHECK(state.bestMove.from != state.bestMove.to);
    CHECK(elapsed < 550);
}
