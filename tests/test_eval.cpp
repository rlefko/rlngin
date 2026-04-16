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
    CHECK(evaluate(board) == 985);
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

    // Pawn on a2 (sq 8): phase 0, pure endgame: 94 + EgPawnTable[8] = 94 + 13 = 107
    // Pawn structure: isolated (-20 EG) + passed rank 1 (+10 EG) = -10 -> 97
    board.setFen("4k3/8/8/8/8/8/P7/4K3 w - - 0 1");
    CHECK(evaluate(board) == 97);

    // Knight on a1 (sq 0): phase 1, tapered: (232*1 + 252*23) / 24 = 251
    board.setFen("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    CHECK(evaluate(board) == 251);

    // Bishop on a1 (sq 0): phase 1, tapered: (332*1 + 274*23) / 24 = 276
    board.setFen("4k3/8/8/8/8/8/8/B3K3 w - - 0 1");
    CHECK(evaluate(board) == 276);

    // Rook on a1 (sq 0): phase 2, tapered: (458*2 + 503*22) / 24 = 499
    board.setFen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    CHECK(evaluate(board) == 499);

    // Queen on d5 (sq 35): phase 4, tapered: (1009*4 + 981*20) / 24 = 985
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 985);
}

TEST_CASE("Eval: central knight scores higher than corner knight", "[eval]") {
    Board board;

    board.setFen("4k3/8/8/8/4N3/8/8/4K3 w - - 0 1");
    int centralKnight = evaluate(board);

    board.setFen("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    int cornerKnight = evaluate(board);

    CHECK(centralKnight > cornerKnight);
}

TEST_CASE("Eval: endgame king prefers center", "[eval]") {
    Board board;

    // No queens, no pieces -> phase 0, pure endgame: king prefers center
    board.setFen("4k3/8/8/8/3K4/8/8/8 w - - 0 1");
    int centralKing = evaluate(board);

    board.setFen("4k3/8/8/8/8/8/8/K7 w - - 0 1");
    int cornerKing = evaluate(board);

    CHECK(centralKing > cornerKing);
}

TEST_CASE("Eval: middlegame king prefers castled position", "[eval]") {
    Board board;

    // Heavy material (Q+R+B+N per side, phase 16) -> MG-dominated, king prefers safety
    board.setFen("rnbqk3/8/8/8/8/8/8/RNBQ2K1 w - - 0 1");
    int castledKing = evaluate(board);

    board.setFen("rnbqk3/8/8/8/4K3/8/8/RNBQ4 w - - 0 1");
    int exposedKing = evaluate(board);

    CHECK(castledKing > exposedKing);
}

TEST_CASE("Eval: tapered eval blends middlegame and endgame", "[eval]") {
    Board board;

    // Pure endgame (phase 0): king prefers center over edge
    board.setFen("4k3/8/8/8/3K4/8/8/8 w - - 0 1");
    int egCenter = evaluate(board);
    board.setFen("4k3/8/8/8/8/8/8/3K4 w - - 0 1");
    int egEdge = evaluate(board);
    CHECK(egCenter > egEdge);

    // Light material (Q+minor per side, phase ~10): center king still viable
    board.setFen("3qk3/8/8/8/3K4/8/8/3QN3 w - - 0 1");
    int lightCenter = evaluate(board);
    board.setFen("3qk3/8/8/8/8/8/8/3QNK2 w - - 0 1");
    int lightEdge = evaluate(board);
    CHECK(lightCenter > lightEdge);

    // Heavy material (Q+R+B+N per side, phase 16): castled king preferred
    board.setFen("rnbqk3/8/8/8/8/8/8/RNBQ2K1 w - - 0 1");
    int heavyCastled = evaluate(board);
    board.setFen("rnbqk3/8/8/8/4K3/8/8/RNBQ4 w - - 0 1");
    int heavyExposed = evaluate(board);
    CHECK(heavyCastled > heavyExposed);
}

TEST_CASE("Eval: symmetric positions score 0", "[eval]") {
    Board board;

    // Mirror position: identical pieces on mirrored squares
    board.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1");
    CHECK(evaluate(board) == 0);
}

TEST_CASE("Eval: passed pawn scores higher than blocked pawn", "[eval][pawn]") {
    Board board;

    // White pawn on e5, no black pawns on d/e/f files ahead = passed
    board.setFen("4k3/8/8/4P3/8/8/8/4K3 w - - 0 1");
    int passedScore = evaluate(board);

    // White pawn on e5, black pawn on e6 blocks = not passed
    board.setFen("4k3/8/4p3/4P3/8/8/8/4K3 w - - 0 1");
    int blockedScore = evaluate(board);

    CHECK(passedScore > blockedScore);
}

TEST_CASE("Eval: advanced passed pawn worth more than rear passed pawn", "[eval][pawn]") {
    Board board;

    // White passed pawn on e6 (rank index 5)
    board.setFen("4k3/8/4P3/8/8/8/8/4K3 w - - 0 1");
    int advanced = evaluate(board);

    // White passed pawn on e3 (rank index 2)
    board.setFen("4k3/8/8/8/8/4P3/8/4K3 w - - 0 1");
    int rear = evaluate(board);

    CHECK(advanced > rear);
}

TEST_CASE("Eval: isolated pawn scores lower than supported pawn", "[eval][pawn]") {
    Board board;

    // White pawn on e4, alone (isolated)
    board.setFen("4k3/8/8/8/4P3/8/8/4K3 w - - 0 1");
    int isolated = evaluate(board);

    // White pawn on e4 with friendly pawn on d3 (not isolated, d3 supports)
    board.setFen("4k3/8/8/8/4P3/3P4/8/4K3 w - - 0 1");
    int supported = evaluate(board);

    CHECK(supported > isolated);
}

TEST_CASE("Eval: doubled pawns score lower than separated pawns", "[eval][pawn]") {
    Board board;

    // Two white pawns doubled on e-file
    board.setFen("4k3/8/8/8/4P3/4P3/8/4K3 w - - 0 1");
    int doubled = evaluate(board);

    // Two white pawns on adjacent files (not doubled)
    board.setFen("4k3/8/8/8/4P3/3P4/8/4K3 w - - 0 1");
    int separated = evaluate(board);

    CHECK(separated > doubled);
}

TEST_CASE("Eval: connected pawns score higher than disconnected pawns", "[eval][pawn]") {
    Board board;

    // Two white pawns side by side (phalanx) on d4, e4
    board.setFen("4k3/8/8/8/3PP3/8/8/4K3 w - - 0 1");
    int connected = evaluate(board);

    // Two white pawns far apart (a4, h4), both isolated
    board.setFen("4k3/8/8/8/P6P/8/8/4K3 w - - 0 1");
    int disconnected = evaluate(board);

    CHECK(connected > disconnected);
}

TEST_CASE("Eval: backward pawn scores lower than non-backward pawn", "[eval][pawn]") {
    Board board;

    // White pawn on e3, friendly pawn on d4 (ahead on adjacent file)
    // Black pawns on d5, f5 control e4 (stop square) -> e3 is backward
    board.setFen("4k3/8/8/3p1p2/3P4/4P3/8/4K3 w - - 0 1");
    int backward = evaluate(board);

    // Same but remove f5 so stop square is not fully controlled
    board.setFen("4k3/8/8/3p4/3P4/4P3/8/4K3 w - - 0 1");
    int notBackward = evaluate(board);

    CHECK(notBackward > backward);
}

TEST_CASE("Eval: pawn chain gives connected bonus", "[eval][pawn]") {
    Board board;

    // White pawn chain: e4 defended by d3
    board.setFen("4k3/8/8/8/4P3/3P4/8/4K3 w - - 0 1");
    int chain = evaluate(board);

    // Two isolated white pawns on a4 and h3
    board.setFen("4k3/8/8/8/P7/7P/8/4K3 w - - 0 1");
    int noChain = evaluate(board);

    CHECK(chain > noChain);
}

TEST_CASE("Eval: symmetric pawn structure scores 0", "[eval][pawn]") {
    Board board;

    board.setFen("4k3/pppppppp/8/8/8/8/PPPPPPPP/4K3 w - - 0 1");
    CHECK(evaluate(board) == 0);
}
