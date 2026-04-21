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

TEST_CASE("Zobrist: non-pawn key differs from starting position after knight move", "[zobrist]") {
    Board board;
    uint64_t whiteBefore = board.nonPawnKey[White];
    uint64_t blackBefore = board.nonPawnKey[Black];
    uint64_t minorBefore = board.minorKey;

    board.makeMove(stringToMove("g1f3"));

    CHECK(board.nonPawnKey[White] != whiteBefore);
    CHECK(board.nonPawnKey[Black] == blackBefore);
    CHECK(board.minorKey != minorBefore);
}

TEST_CASE("Zobrist: non-pawn key is unchanged by a pawn move", "[zobrist]") {
    Board board;
    uint64_t whiteBefore = board.nonPawnKey[White];
    uint64_t blackBefore = board.nonPawnKey[Black];
    uint64_t minorBefore = board.minorKey;

    board.makeMove(stringToMove("e2e4"));

    CHECK(board.nonPawnKey[White] == whiteBefore);
    CHECK(board.nonPawnKey[Black] == blackBefore);
    CHECK(board.minorKey == minorBefore);
}

TEST_CASE("Zobrist: minor key is unchanged by a rook move", "[zobrist]") {
    Board board;
    board.setFen("r3k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    uint64_t minorBefore = board.minorKey;
    uint64_t whiteNonPawnBefore = board.nonPawnKey[White];

    board.makeMove(stringToMove("a1a4"));

    CHECK(board.minorKey == minorBefore);
    CHECK(board.nonPawnKey[White] != whiteNonPawnBefore);
}

TEST_CASE("Zobrist: incremental non-pawn and minor keys match from-scratch computation",
          "[zobrist]") {
    Board board;
    board.makeMove(stringToMove("e2e4"));
    board.makeMove(stringToMove("e7e5"));
    board.makeMove(stringToMove("g1f3"));
    board.makeMove(stringToMove("b8c6"));
    board.makeMove(stringToMove("f1b5"));

    Board fromFen;
    fromFen.setFen("r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 3 3");

    CHECK(board.nonPawnKey[White] == fromFen.nonPawnKey[White]);
    CHECK(board.nonPawnKey[Black] == fromFen.nonPawnKey[Black]);
    CHECK(board.minorKey == fromFen.minorKey);
}

TEST_CASE("Zobrist: non-pawn and minor keys survive make and unmake", "[zobrist]") {
    const char *fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
        "rnbqkbnr/ppp1pppp/8/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3",
        "8/P7/8/8/8/8/8/4K2k w - - 0 1",
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
    };
    const char *moves[] = {
        "g1f3",  // knight move
        "e1g1",  // white castles kingside
        "e5d6",  // en passant capture
        "a7a8q", // pawn promotion to queen
        "e1c1",  // queenside castle
    };

    for (int i = 0; i < 5; i++) {
        Board board;
        board.setFen(fens[i]);
        uint64_t keyBefore = board.key;
        uint64_t pawnBefore = board.pawnKey;
        uint64_t materialBefore = board.materialKey;
        uint64_t whiteBefore = board.nonPawnKey[White];
        uint64_t blackBefore = board.nonPawnKey[Black];
        uint64_t minorBefore = board.minorKey;

        UndoInfo undo = board.makeMove(stringToMove(moves[i]));
        board.unmakeMove(stringToMove(moves[i]), undo);

        CHECK(board.key == keyBefore);
        CHECK(board.pawnKey == pawnBefore);
        CHECK(board.materialKey == materialBefore);
        CHECK(board.nonPawnKey[White] == whiteBefore);
        CHECK(board.nonPawnKey[Black] == blackBefore);
        CHECK(board.minorKey == minorBefore);
    }
}

TEST_CASE("Zobrist: minor key tracks a bishop capturing a knight", "[zobrist]") {
    Board board;
    board.setFen("4k3/8/8/3n4/8/3B4/8/4K3 w - - 0 1");

    Board after;
    after.setFen("4k3/8/8/3B4/8/8/8/4K3 b - - 0 1");

    board.makeMove(stringToMove("d3d5"));

    CHECK(board.minorKey == after.minorKey);
    CHECK(board.nonPawnKey[White] == after.nonPawnKey[White]);
    CHECK(board.nonPawnKey[Black] == after.nonPawnKey[Black]);
}

TEST_CASE("Zobrist: promoting to a knight updates the minor key", "[zobrist]") {
    Board board;
    board.setFen("8/P7/8/8/8/8/8/4K2k w - - 0 1");
    uint64_t minorBefore = board.minorKey;

    board.makeMove(stringToMove("a7a8n"));

    CHECK(board.minorKey != minorBefore);

    Board fromFen;
    fromFen.setFen("N7/8/8/8/8/8/8/4K2k b - - 0 1");
    CHECK(board.minorKey == fromFen.minorKey);
    CHECK(board.nonPawnKey[White] == fromFen.nonPawnKey[White]);
}
