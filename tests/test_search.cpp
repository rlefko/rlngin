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
