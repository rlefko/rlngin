#include "board.h"
#include "catch_amalgamated.hpp"
#include "movegen.h"

static uint64_t perft(Board board, int depth) {
    if (depth == 0) return 1;
    std::vector<Move> moves = generateLegalMoves(board);
    uint64_t nodes = 0;
    for (const Move &m : moves) {
        Board copy = board;
        copy.makeMove(m);
        nodes += perft(copy, depth - 1);
    }
    return nodes;
}

TEST_CASE("Perft: starting position", "[movegen][perft]") {
    Board board;

    CHECK(perft(board, 1) == 20);
    CHECK(perft(board, 2) == 400);
    CHECK(perft(board, 3) == 8902);
    CHECK(perft(board, 4) == 197281);
}

TEST_CASE("Perft: kiwipete", "[movegen][perft]") {
    Board board;
    board.setFen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

    CHECK(perft(board, 1) == 48);
    CHECK(perft(board, 2) == 2039);
    CHECK(perft(board, 3) == 97862);
}

TEST_CASE("Perft: position 3", "[movegen][perft]") {
    Board board;
    board.setFen("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");

    CHECK(perft(board, 1) == 14);
    CHECK(perft(board, 2) == 191);
    CHECK(perft(board, 3) == 2812);
    CHECK(perft(board, 4) == 43238);
}

TEST_CASE("Perft: position 4", "[movegen][perft]") {
    Board board;
    board.setFen("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");

    CHECK(perft(board, 1) == 6);
    CHECK(perft(board, 2) == 264);
    CHECK(perft(board, 3) == 9467);
}

TEST_CASE("Perft: position 5", "[movegen][perft]") {
    Board board;
    board.setFen("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8");

    CHECK(perft(board, 1) == 44);
}

TEST_CASE("Move generation: no moves in checkmate", "[movegen]") {
    Board board;
    board.setFen("rnb1kbnr/pppp1ppp/4p3/8/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3");

    std::vector<Move> moves = generateLegalMoves(board);
    CHECK(moves.empty());
}

TEST_CASE("Move generation: stalemate has no moves", "[movegen]") {
    Board board;
    board.setFen("k7/8/1K6/8/8/8/8/8 b - - 0 1");

    // Black king is not in check but has no legal moves (stalemate)
    // Actually let's pick a real stalemate position
    board.setFen("8/8/8/8/8/5k2/5p2/5K2 w - - 0 1");
    std::vector<Move> moves = generateLegalMoves(board);
    CHECK(moves.empty());
}

TEST_CASE("isSquareAttacked detects knight attacks", "[movegen]") {
    Board board;
    board.setFen("8/8/8/4N3/8/8/8/4K2k w - - 0 1");

    // Knight on e5 attacks d3, f3, c4, g4, c6, g6, d7, f7
    CHECK(isSquareAttacked(board, stringToSquare("d3"), White));
    CHECK(isSquareAttacked(board, stringToSquare("f3"), White));
    CHECK(isSquareAttacked(board, stringToSquare("c4"), White));
    CHECK(isSquareAttacked(board, stringToSquare("g4"), White));
    CHECK(isSquareAttacked(board, stringToSquare("c6"), White));
    CHECK(isSquareAttacked(board, stringToSquare("g6"), White));
    CHECK(isSquareAttacked(board, stringToSquare("d7"), White));
    CHECK(isSquareAttacked(board, stringToSquare("f7"), White));

    // Not attacked
    CHECK_FALSE(isSquareAttacked(board, stringToSquare("e5"), White));
    CHECK_FALSE(isSquareAttacked(board, stringToSquare("a1"), White));
}

TEST_CASE("Castling blocked when king passes through check", "[movegen]") {
    Board board;
    // Rook on f8 controls f1, blocking kingside castling
    board.setFen("5r2/8/8/8/8/8/8/R3K2R w KQ - 0 1");

    std::vector<Move> moves = generateLegalMoves(board);
    bool hasKingsideCastle = false;
    for (const Move &m : moves) {
        if (m.from == stringToSquare("e1") && m.to == stringToSquare("g1")) {
            hasKingsideCastle = true;
        }
    }
    CHECK_FALSE(hasKingsideCastle);
}
