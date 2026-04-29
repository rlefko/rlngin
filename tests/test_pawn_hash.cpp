#include "board.h"
#include "catch_amalgamated.hpp"
#include "pawn_hash.h"
#include "zobrist.h"

// PawnHashTable unit tests

TEST_CASE("PawnHash: store and probe round-trip", "[pawn_hash]") {
    PawnHashTable table(1);

    table.store(0xABCDEF, 42, -15, 0xF00D, 0xBA11);

    int mg = 0, eg = 0;
    uint64_t whitePassers = 0, blackPassers = 0;
    bool hit = table.probe(0xABCDEF, mg, eg, whitePassers, blackPassers);

    CHECK(hit);
    CHECK(mg == 42);
    CHECK(eg == -15);
    CHECK(whitePassers == 0xF00D);
    CHECK(blackPassers == 0xBA11);
}

TEST_CASE("PawnHash: probe miss returns false", "[pawn_hash]") {
    PawnHashTable table(1);

    int mg = 0, eg = 0;
    uint64_t whitePassers = 0, blackPassers = 0;
    bool hit = table.probe(0xDEADBEEF, mg, eg, whitePassers, blackPassers);

    CHECK_FALSE(hit);
}

TEST_CASE("PawnHash: overwrite replaces values", "[pawn_hash]") {
    PawnHashTable table(1);

    table.store(0xABC, 10, 20, 0, 0);
    table.store(0xABC, 30, 40, 0x1, 0x2);

    int mg = 0, eg = 0;
    uint64_t whitePassers = 0, blackPassers = 0;
    bool hit = table.probe(0xABC, mg, eg, whitePassers, blackPassers);

    CHECK(hit);
    CHECK(mg == 30);
    CHECK(eg == 40);
    CHECK(whitePassers == 0x1);
    CHECK(blackPassers == 0x2);
}

TEST_CASE("PawnHash: clear removes entries", "[pawn_hash]") {
    PawnHashTable table(1);

    table.store(0x123, 50, 60, 0, 0);
    table.clear();

    int mg = 0, eg = 0;
    uint64_t whitePassers = 0, blackPassers = 0;
    CHECK_FALSE(table.probe(0x123, mg, eg, whitePassers, blackPassers));
}

// Pawn key correctness tests

TEST_CASE("PawnKey: starting position matches manual computation", "[pawn_key]") {
    zobrist::init();
    Board board;

    uint64_t expected = 0;
    for (int sq = 8; sq < 16; sq++) {
        expected ^= zobrist::piece_keys[White][Pawn][sq];
    }
    for (int sq = 48; sq < 56; sq++) {
        expected ^= zobrist::piece_keys[Black][Pawn][sq];
    }

    CHECK(board.pawnKey == expected);
}

TEST_CASE("PawnKey: changes on pawn move", "[pawn_key]") {
    zobrist::init();
    Board board;
    uint64_t before = board.pawnKey;

    Move e2e4 = {12, 28, None}; // e2 -> e4
    board.makeMove(e2e4);

    CHECK(board.pawnKey != before);
}

TEST_CASE("PawnKey: unchanged on non-pawn move", "[pawn_key]") {
    zobrist::init();
    Board board;
    uint64_t before = board.pawnKey;

    Move g1f3 = {6, 21, None}; // Ng1-f3
    board.makeMove(g1f3);

    CHECK(board.pawnKey == before);
}

TEST_CASE("PawnKey: restored after unmakeMove", "[pawn_key]") {
    zobrist::init();
    Board board;
    uint64_t before = board.pawnKey;

    Move e2e4 = {12, 28, None};
    UndoInfo undo = board.makeMove(e2e4);
    board.unmakeMove(e2e4, undo);

    CHECK(board.pawnKey == before);
}

TEST_CASE("PawnKey: unchanged after null move cycle", "[pawn_key]") {
    zobrist::init();
    Board board;
    uint64_t before = board.pawnKey;

    UndoInfo undo = board.makeNullMove();
    board.unmakeNullMove(undo);

    CHECK(board.pawnKey == before);
}

TEST_CASE("PawnKey: promotion removes pawn from key", "[pawn_key]") {
    zobrist::init();
    Board board;
    board.setFen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");

    uint64_t before = board.pawnKey;
    CHECK(before != 0); // has one pawn

    Move a7a8Q = {48, 56, Queen}; // a7 -> a8=Q
    board.makeMove(a7a8Q);

    CHECK(board.pawnKey == 0); // no pawns remain
}

TEST_CASE("PawnKey: en passant capture updates key", "[pawn_key]") {
    zobrist::init();
    Board board;
    // White pawn on e5, black pawn on d5 with en passant on d6
    board.setFen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");

    uint64_t before = board.pawnKey;

    Move exd6 = {36, 43, None}; // e5 -> d6 (en passant)
    board.makeMove(exd6);

    // Black pawn removed, white pawn moved
    CHECK(board.pawnKey != before);

    // Only white pawn on d6 remains
    uint64_t expected = zobrist::piece_keys[White][Pawn][43];
    CHECK(board.pawnKey == expected);
}

TEST_CASE("PawnKey: piece capturing pawn updates key", "[pawn_key]") {
    zobrist::init();
    Board board;
    // White knight on c3, black pawn on d5
    board.setFen("4k3/8/8/3p4/8/2N5/8/4K3 w - - 0 1");

    uint64_t before = board.pawnKey;
    CHECK(before == zobrist::piece_keys[Black][Pawn][35]); // only d5 pawn

    Move Nc3xd5 = {18, 35, None};
    board.makeMove(Nc3xd5);

    CHECK(board.pawnKey == 0); // pawn captured, knight not in pawn key
}

TEST_CASE("PawnKey: castling does not change key", "[pawn_key]") {
    zobrist::init();
    Board board;
    board.setFen("4k3/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQ - 0 1");

    uint64_t before = board.pawnKey;

    Move OO = {4, 6, None}; // e1 -> g1 (kingside castle)
    board.makeMove(OO);

    CHECK(board.pawnKey == before);
}

TEST_CASE("PawnKey: same pawns with different pieces yield same key", "[pawn_key]") {
    zobrist::init();
    Board board1;
    Board board2;

    // Same pawn placement, different pieces
    board1.setFen("4k3/8/8/8/4P3/8/8/4K3 w - - 0 1");
    board2.setFen("3qk3/8/8/8/4P3/8/8/3QK3 w - - 0 1");

    CHECK(board1.pawnKey == board2.pawnKey);
}

// --- Shelter cache ---

TEST_CASE("PawnHash: shelter probe matches stored entry exactly", "[pawn_hash]") {
    PawnHashTable table(1);
    table.store(0xCAFE, 0, 0, 0, 0);
    table.storeShelter(0xCAFE, /*side=*/0, /*kingFile=*/4, /*castlingMask=*/3, 88, -7);

    int mg = 0, eg = 0;
    REQUIRE(table.probeShelter(0xCAFE, 0, 4, 3, mg, eg));
    CHECK(mg == 88);
    CHECK(eg == -7);
}

TEST_CASE("PawnHash: shelter probe misses on king file change", "[pawn_hash]") {
    PawnHashTable table(1);
    table.store(0xCAFE, 0, 0, 0, 0);
    table.storeShelter(0xCAFE, 0, 4, 3, 88, -7);

    int mg = 0, eg = 0;
    CHECK_FALSE(table.probeShelter(0xCAFE, 0, 5, 3, mg, eg));
}

TEST_CASE("PawnHash: shelter probe misses on castling change", "[pawn_hash]") {
    PawnHashTable table(1);
    table.store(0xCAFE, 0, 0, 0, 0);
    table.storeShelter(0xCAFE, 0, 4, 3, 88, -7);

    int mg = 0, eg = 0;
    CHECK_FALSE(table.probeShelter(0xCAFE, 0, 4, 0, mg, eg));
}

TEST_CASE("PawnHash: shelter probe misses on side change", "[pawn_hash]") {
    PawnHashTable table(1);
    table.store(0xCAFE, 0, 0, 0, 0);
    table.storeShelter(0xCAFE, 0, 4, 3, 88, -7);

    int mg = 0, eg = 0;
    CHECK_FALSE(table.probeShelter(0xCAFE, 1, 4, 3, mg, eg));
}

TEST_CASE("PawnHash: shelter probe misses after pawn key change wipes the cache", "[pawn_hash]") {
    PawnHashTable table(1);
    table.store(0xCAFE, 0, 0, 0, 0);
    table.storeShelter(0xCAFE, 0, 4, 3, 88, -7);
    // Different pawn key collides at the same index because the table
    // is a single bucket; the new store invalidates the shelter.
    table.store(0xBEEF, 1, 1, 0, 0);

    int mg = 0, eg = 0;
    CHECK_FALSE(table.probeShelter(0xCAFE, 0, 4, 3, mg, eg));
    CHECK_FALSE(table.probeShelter(0xBEEF, 0, 4, 3, mg, eg));
}
