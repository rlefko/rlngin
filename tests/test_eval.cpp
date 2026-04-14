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
    CHECK(evaluate(board) == 905);
}

TEST_CASE("Eval: score flips with side to move", "[eval]") {
    Board board;
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    int whiteToMove = evaluate(board);

    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 b - - 0 1");
    int blackToMove = evaluate(board);

    CHECK(whiteToMove == -blackToMove);
}

TEST_CASE("Eval: material values include PST bonuses", "[eval]") {
    Board board;

    // Pawn on a2 (sq 8): PawnPST[8] = 5, so 100 + 5 = 105
    board.setFen("4k3/8/8/8/8/8/P7/4K3 w - - 0 1");
    CHECK(evaluate(board) == 105);

    // Knight on a1 (sq 0): KnightPST[0] = -50, so 320 + (-50) = 270
    board.setFen("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    CHECK(evaluate(board) == 270);

    // Bishop on a1 (sq 0): BishopPST[0] = -20, so 330 + (-20) = 310
    board.setFen("4k3/8/8/8/8/8/8/B3K3 w - - 0 1");
    CHECK(evaluate(board) == 310);

    // Rook on a1 (sq 0): RookPST[0] = 0, so 500 + 0 = 500
    board.setFen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    CHECK(evaluate(board) == 500);

    // Queen on d5 (sq 35): QueenPST[35] = 5, so 900 + 5 = 905
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 905);
}

TEST_CASE("Eval: central knight scores higher than corner knight", "[eval]") {
    Board board;

    // Knight on e4 (sq 28): KnightPST[28] = 20
    board.setFen("4k3/8/8/8/4N3/8/8/4K3 w - - 0 1");
    int centralKnight = evaluate(board);

    // Knight on a1 (sq 0): KnightPST[0] = -50
    board.setFen("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    int cornerKnight = evaluate(board);

    CHECK(centralKnight > cornerKnight);
}

TEST_CASE("Eval: endgame king prefers center", "[eval]") {
    Board board;

    // No queens -> endgame. King on d4 (sq 27): KingEGPST[27] = 40
    board.setFen("4k3/8/8/8/3K4/8/8/8 w - - 0 1");
    int centralKing = evaluate(board);

    // King on a1 (sq 0): KingEGPST[0] = -50
    board.setFen("4k3/8/8/8/8/8/8/K7 w - - 0 1");
    int cornerKing = evaluate(board);

    CHECK(centralKing > cornerKing);
}

TEST_CASE("Eval: middlegame king prefers castled position", "[eval]") {
    Board board;

    // Queen + rook -> not endgame. King on g1 (sq 6): KingMGPST[6] = 30
    board.setFen("r2qk3/8/8/8/8/8/8/R2Q2K1 w - - 0 1");
    int castledKing = evaluate(board);

    // King on e4 (sq 28): KingMGPST[28] = -40
    board.setFen("r2qk3/8/8/8/4K3/8/8/R2Q4 w - - 0 1");
    int exposedKing = evaluate(board);

    CHECK(castledKing > exposedKing);
}

TEST_CASE("Eval: endgame detection", "[eval]") {
    Board board;

    // No queens -> endgame (king prefers center)
    board.setFen("4k3/8/8/8/3K4/8/8/8 w - - 0 1");
    int noQueensCenter = evaluate(board);
    board.setFen("4k3/8/8/8/8/8/8/3K4 w - - 0 1");
    int noQueensEdge = evaluate(board);
    CHECK(noQueensCenter > noQueensEdge);

    // Queen + rook -> not endgame (king prefers safety)
    board.setFen("3qk3/8/8/8/8/8/8/R2QK3 w - - 0 1");
    int qrCastled = evaluate(board);
    board.setFen("3qk3/8/8/8/4K3/8/8/R2Q4 w - - 0 1");
    int qrExposed = evaluate(board);
    CHECK(qrCastled > qrExposed);

    // Queen + one minor -> endgame (king prefers center)
    board.setFen("3qk3/8/8/8/3K4/8/8/3QN3 w - - 0 1");
    int qnCenter = evaluate(board);
    board.setFen("3qk3/8/8/8/8/8/8/3QNK2 w - - 0 1");
    int qnEdge = evaluate(board);
    CHECK(qnCenter > qnEdge);
}

TEST_CASE("Eval: symmetric positions score 0", "[eval]") {
    Board board;

    // Mirror position: identical pieces on mirrored squares
    board.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1");
    CHECK(evaluate(board) == 0);
}
