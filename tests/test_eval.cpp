#include "board.h"
#include "catch_amalgamated.hpp"
#include "eval.h"

TEST_CASE("Eval: starting position is 0", "[eval]") {
    Board board;
    CHECK(evaluate(board) == 0);
}

TEST_CASE("Eval: kings only is 0", "[eval]") {
    Board board;
    board.setFen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 0);
}

TEST_CASE("Eval: extra white queen scores positive for white", "[eval]") {
    Board board;
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 900);
}

TEST_CASE("Eval: score flips with side to move", "[eval]") {
    Board board;
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    int whiteToMove = evaluate(board);

    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 b - - 0 1");
    int blackToMove = evaluate(board);

    CHECK(whiteToMove == -blackToMove);
}

TEST_CASE("Eval: material values are correct", "[eval]") {
    Board board;

    board.setFen("4k3/8/8/8/8/8/P7/4K3 w - - 0 1");
    CHECK(evaluate(board) == 100);

    board.setFen("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    CHECK(evaluate(board) == 300);

    board.setFen("4k3/8/8/8/8/8/8/B3K3 w - - 0 1");
    CHECK(evaluate(board) == 300);

    board.setFen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    CHECK(evaluate(board) == 500);

    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 900);
}
