#include "bitboard.h"
#include "board.h"
#include "catch_amalgamated.hpp"
#include "movegen.h"
#include "zobrist.h"

TEST_CASE("Board default constructor sets start position", "[board]") {
    Board board;

    // White pieces on ranks 1-2
    CHECK(board.squares[0].type == Rook);
    CHECK(board.squares[0].color == White);
    CHECK(board.squares[1].type == Knight);
    CHECK(board.squares[2].type == Bishop);
    CHECK(board.squares[3].type == Queen);
    CHECK(board.squares[4].type == King);
    CHECK(board.squares[5].type == Bishop);
    CHECK(board.squares[6].type == Knight);
    CHECK(board.squares[7].type == Rook);
    for (int sq = 8; sq < 16; sq++) {
        CHECK(board.squares[sq].type == Pawn);
        CHECK(board.squares[sq].color == White);
    }

    // Empty squares in the middle
    for (int sq = 16; sq < 48; sq++) {
        CHECK(board.squares[sq].type == None);
    }

    // Black pieces on ranks 7-8
    for (int sq = 48; sq < 56; sq++) {
        CHECK(board.squares[sq].type == Pawn);
        CHECK(board.squares[sq].color == Black);
    }
    CHECK(board.squares[56].type == Rook);
    CHECK(board.squares[56].color == Black);
    CHECK(board.squares[57].type == Knight);
    CHECK(board.squares[58].type == Bishop);
    CHECK(board.squares[59].type == Queen);
    CHECK(board.squares[60].type == King);
    CHECK(board.squares[61].type == Bishop);
    CHECK(board.squares[62].type == Knight);
    CHECK(board.squares[63].type == Rook);

    CHECK(board.sideToMove == White);
    CHECK(board.castleWK == true);
    CHECK(board.castleWQ == true);
    CHECK(board.castleBK == true);
    CHECK(board.castleBQ == true);
    CHECK(board.enPassantSquare == -1);
    CHECK(board.halfmoveClock == 0);
    CHECK(board.fullmoveNumber == 1);
}

TEST_CASE("setFen parses position correctly", "[board]") {
    Board board;
    board.setFen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

    CHECK(board.sideToMove == White);
    CHECK(board.castleWK == true);
    CHECK(board.castleWQ == true);
    CHECK(board.castleBK == true);
    CHECK(board.castleBQ == true);
    CHECK(board.enPassantSquare == -1);

    // Verify a few key pieces
    CHECK(board.squares[0].type == Rook); // a1
    CHECK(board.squares[0].color == White);
    CHECK(board.squares[4].type == King); // e1
    CHECK(board.squares[4].color == White);
    CHECK(board.squares[60].type == King); // e8
    CHECK(board.squares[60].color == Black);
    CHECK(board.squares[28].type == Pawn); // e4
    CHECK(board.squares[28].color == White);
}

TEST_CASE("setFen parses en passant square", "[board]") {
    Board board;
    board.setFen("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");

    CHECK(board.sideToMove == Black);
    CHECK(board.enPassantSquare == stringToSquare("e3"));
}

TEST_CASE("setFen parses partial castling rights", "[board]") {
    Board board;
    board.setFen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w Kq - 0 1");

    CHECK(board.castleWK == true);
    CHECK(board.castleWQ == false);
    CHECK(board.castleBK == false);
    CHECK(board.castleBQ == true);
}

TEST_CASE("makeMove updates board state for pawn push", "[board]") {
    Board board;
    Move m = stringToMove("e2e4");
    board.makeMove(m);

    CHECK(board.squares[stringToSquare("e2")].type == None);
    CHECK(board.squares[stringToSquare("e4")].type == Pawn);
    CHECK(board.squares[stringToSquare("e4")].color == White);
    CHECK(board.sideToMove == Black);
    CHECK(board.enPassantSquare == stringToSquare("e3"));
    CHECK(board.halfmoveClock == 0);
}

TEST_CASE("makeMove handles kingside castling", "[board]") {
    Board board;
    board.setFen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");

    Move m = stringToMove("e1g1");
    board.makeMove(m);

    CHECK(board.squares[stringToSquare("g1")].type == King);
    CHECK(board.squares[stringToSquare("g1")].color == White);
    CHECK(board.squares[stringToSquare("f1")].type == Rook);
    CHECK(board.squares[stringToSquare("f1")].color == White);
    CHECK(board.squares[stringToSquare("e1")].type == None);
    CHECK(board.squares[stringToSquare("h1")].type == None);
    CHECK(board.castleWK == false);
    CHECK(board.castleWQ == false);
}

TEST_CASE("makeMove handles queenside castling", "[board]") {
    Board board;
    board.setFen("r3k2r/pppppppp/8/8/8/8/PPPPPPPP/R3K2R w KQkq - 0 1");

    Move m = stringToMove("e1c1");
    board.makeMove(m);

    CHECK(board.squares[stringToSquare("c1")].type == King);
    CHECK(board.squares[stringToSquare("d1")].type == Rook);
    CHECK(board.squares[stringToSquare("e1")].type == None);
    CHECK(board.squares[stringToSquare("a1")].type == None);
}

TEST_CASE("makeMove handles promotion", "[board]") {
    Board board;
    board.setFen("8/P7/8/8/8/8/8/4K2k w - - 0 1");

    Move m = {stringToSquare("a7"), stringToSquare("a8"), Queen};
    board.makeMove(m);

    CHECK(board.squares[stringToSquare("a8")].type == Queen);
    CHECK(board.squares[stringToSquare("a8")].color == White);
    CHECK(board.squares[stringToSquare("a7")].type == None);
}

TEST_CASE("makeMove handles en passant capture", "[board]") {
    Board board;
    board.setFen("rnbqkbnr/ppp1pppp/8/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3");

    Move m = stringToMove("e5d6");
    board.makeMove(m);

    CHECK(board.squares[stringToSquare("d6")].type == Pawn);
    CHECK(board.squares[stringToSquare("d6")].color == White);
    CHECK(board.squares[stringToSquare("d5")].type == None); // captured pawn removed
    CHECK(board.squares[stringToSquare("e5")].type == None);
}

TEST_CASE("makeMove increments fullmove number after black moves", "[board]") {
    Board board;
    CHECK(board.fullmoveNumber == 1);

    board.makeMove(stringToMove("e2e4"));
    CHECK(board.fullmoveNumber == 1);

    board.makeMove(stringToMove("e7e5"));
    CHECK(board.fullmoveNumber == 2);
}

TEST_CASE("makeMove updates halfmove clock", "[board]") {
    Board board;
    board.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 5 1");

    // Pawn move resets clock
    board.makeMove(stringToMove("e2e4"));
    CHECK(board.halfmoveClock == 0);
}

TEST_CASE("pieceAt returns correct piece", "[board]") {
    Board board;
    Piece p = board.pieceAt(0);
    CHECK(p.type == Rook);
    CHECK(p.color == White);

    Piece empty = board.pieceAt(32);
    CHECK(empty.type == None);
}

TEST_CASE("pieceCount matches board contents in start position", "[board][material]") {
    Board board;
    CHECK(board.pieceCount[White][Pawn] == 8);
    CHECK(board.pieceCount[White][Knight] == 2);
    CHECK(board.pieceCount[White][Bishop] == 2);
    CHECK(board.pieceCount[White][Rook] == 2);
    CHECK(board.pieceCount[White][Queen] == 1);
    CHECK(board.pieceCount[White][King] == 1);
    CHECK(board.pieceCount[Black][Pawn] == 8);
    CHECK(board.pieceCount[Black][Knight] == 2);
    CHECK(board.pieceCount[Black][Bishop] == 2);
    CHECK(board.pieceCount[Black][Rook] == 2);
    CHECK(board.pieceCount[Black][Queen] == 1);
    CHECK(board.pieceCount[Black][King] == 1);
}

TEST_CASE("materialKey equals computed key from piece counts", "[board][material]") {
    Board board;
    uint64_t expected = 0;
    for (int c = 0; c < 2; c++)
        for (int pt = 1; pt < 7; pt++)
            expected ^= zobrist::material_keys[c][pt][board.pieceCount[c][pt]];
    CHECK(board.materialKey == expected);
}

static void perftCheck(Board &board, int depth) {
    if (depth == 0) return;
    uint64_t savedKey = board.key;
    uint64_t savedMatKey = board.materialKey;
    int savedCounts[2][7];
    for (int c = 0; c < 2; c++)
        for (int pt = 0; pt < 7; pt++)
            savedCounts[c][pt] = board.pieceCount[c][pt];

    std::vector<Move> moves = generateLegalMoves(board);
    for (const Move &m : moves) {
        UndoInfo undo = board.makeMove(m);
        perftCheck(board, depth - 1);
        board.unmakeMove(m, undo);
        REQUIRE(board.key == savedKey);
        REQUIRE(board.materialKey == savedMatKey);
        for (int c = 0; c < 2; c++)
            for (int pt = 0; pt < 7; pt++) {
                REQUIRE(board.pieceCount[c][pt] == savedCounts[c][pt]);
            }
    }
}

TEST_CASE("pieceCount and materialKey round-trip through make/unmake", "[board][material]") {
    zobrist::init();
    initBitboards();
    Board board;
    board.setFen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

    perftCheck(board, 3);
}

TEST_CASE("materialKey changes on captures and matches recomputed", "[board][material]") {
    Board board;
    board.setFen("4k3/8/8/3p4/4N3/8/8/4K3 w - - 0 1");

    Move capture = stringToMove("e4d6");
    (void)capture;
    // Knight takes pawn on d5 (e4xd5)
    Move m = stringToMove("e4d5");
    board.makeMove(m);

    CHECK(board.pieceCount[Black][Pawn] == 0);
    CHECK(board.pieceCount[White][Knight] == 1);

    uint64_t expected = 0;
    for (int c = 0; c < 2; c++)
        for (int pt = 1; pt < 7; pt++)
            expected ^= zobrist::material_keys[c][pt][board.pieceCount[c][pt]];
    CHECK(board.materialKey == expected);
}

TEST_CASE("materialKey changes on promotion and matches recomputed", "[board][material]") {
    Board board;
    board.setFen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");

    Move m = stringToMove("a7a8q");
    board.makeMove(m);

    CHECK(board.pieceCount[White][Pawn] == 0);
    CHECK(board.pieceCount[White][Queen] == 1);

    uint64_t expected = 0;
    for (int c = 0; c < 2; c++)
        for (int pt = 1; pt < 7; pt++)
            expected ^= zobrist::material_keys[c][pt][board.pieceCount[c][pt]];
    CHECK(board.materialKey == expected);
}
