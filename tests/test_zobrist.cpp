#include "board.h"
#include "catch_amalgamated.hpp"
#include "zobrist.h"

TEST_CASE("Zobrist: init is called before tests", "[zobrist]") {
    zobrist::init();
}

TEST_CASE("Zobrist: key changes after a move", "[zobrist]") {
    Board board;
    uint64_t before = board.key;
    board.makeMove(stringToMove("e2e4"));
    CHECK(board.key != before);
}

TEST_CASE("Zobrist: key differs by side to move", "[zobrist]") {
    Board a;
    a.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    Board b;
    b.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");
    CHECK(a.key != b.key);
}

TEST_CASE("Zobrist: key differs by castling rights", "[zobrist]") {
    Board a;
    a.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    Board b;
    b.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w Kq - 0 1");
    CHECK(a.key != b.key);
}

TEST_CASE("Zobrist: key differs by en passant", "[zobrist]") {
    Board a;
    a.setFen("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    Board b;
    b.setFen("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1");
    CHECK(a.key != b.key);
}

TEST_CASE("Zobrist: incremental key matches from-scratch computation", "[zobrist]") {
    // Play a short game and verify incremental key matches FEN recomputation
    Board board;

    // 1. e4 e5 2. Nf3 Nc6 3. Bb5
    board.makeMove(stringToMove("e2e4"));
    board.makeMove(stringToMove("e7e5"));
    board.makeMove(stringToMove("g1f3"));
    board.makeMove(stringToMove("b8c6"));
    board.makeMove(stringToMove("f1b5"));

    uint64_t incremental = board.key;

    // Same position set via FEN
    Board fromFen;
    fromFen.setFen("r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3");

    CHECK(incremental == fromFen.key);
}

TEST_CASE("Zobrist: key updates correctly through castling", "[zobrist]") {
    // Position where white can castle kingside
    Board board;
    board.setFen("r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4");

    board.makeMove(stringToMove("e1g1")); // White castles kingside
    uint64_t incremental = board.key;

    Board fromFen;
    fromFen.setFen("r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQ1RK1 b kq - 5 4");

    CHECK(incremental == fromFen.key);
}

TEST_CASE("Zobrist: key updates correctly through en passant capture", "[zobrist]") {
    // White pawn on e5, black plays d7d5, white captures en passant
    Board board;
    board.setFen("rnbqkbnr/ppp1pppp/8/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3");

    board.makeMove(stringToMove("e5d6")); // En passant capture
    uint64_t incremental = board.key;

    Board fromFen;
    fromFen.setFen("rnbqkbnr/ppp1pppp/3P4/8/8/8/PPPP1PPP/RNBQKBNR b KQkq - 0 3");

    CHECK(incremental == fromFen.key);
}

TEST_CASE("Zobrist: key updates correctly through promotion", "[zobrist]") {
    Board board;
    board.setFen("8/P7/8/8/8/8/8/4K2k w - - 0 1");

    board.makeMove(stringToMove("a7a8q")); // Promote to queen
    uint64_t incremental = board.key;

    Board fromFen;
    fromFen.setFen("Q7/8/8/8/8/8/8/4K2k b - - 0 1");

    CHECK(incremental == fromFen.key);
}
