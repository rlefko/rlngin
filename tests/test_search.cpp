#include "board.h"
#include "catch_amalgamated.hpp"
#include "search.h"

TEST_CASE("Search: captures hanging queen", "[search]") {
    Board board;
    // White knight on e4 can capture undefended black queen on d6
    board.setFen("4k3/8/3q4/8/4N3/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board);
    CHECK(best.from == stringToSquare("e4"));
    CHECK(best.to == stringToSquare("d6"));
}

TEST_CASE("Search: prefers capturing queen over pawn", "[search]") {
    Board board;
    // White bishop on d4 can capture black queen on f6 or black pawn on b2
    board.setFen("4k3/8/5q2/8/3B4/8/1p6/4K3 w - - 0 1");

    Move best = findBestMove(board);
    CHECK(best.from == stringToSquare("d4"));
    CHECK(best.to == stringToSquare("f6"));
}

TEST_CASE("Search: finds mate in 1", "[search]") {
    Board board;
    // Back-rank mate: Re1 can deliver Re8# since f7/g7/h7 pawns trap the king
    board.setFen("6k1/5ppp/8/8/8/8/8/4R2K w - - 0 1");

    Move best = findBestMove(board, 2);
    CHECK(best.from == stringToSquare("e1"));
    CHECK(best.to == stringToSquare("e8"));
}

TEST_CASE("Search: returns valid move at deeper depths", "[search]") {
    Board board;
    board.setStartPos();

    Move best = findBestMove(board, 3);
    CHECK(best.from != best.to);
}

TEST_CASE("Search: alpha-beta prunes nodes at depth 4", "[search]") {
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

TEST_CASE("Search: respects time limit", "[search]") {
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
