#include "bitboard.h"
#include "board.h"
#include "catch_amalgamated.hpp"
#include "eval.h"
#include "see.h"

static void ensureInit() {
    static bool done = false;
    if (!done) {
        initBitboards();
        done = true;
    }
}

TEST_CASE("SEE: pawn captures undefended pawn", "[see]") {
    ensureInit();
    Board board;
    // White pawn on e4, black pawn on d5 undefended
    board.setFen("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1");

    Move m;
    m.from = stringToSquare("e4");
    m.to = stringToSquare("d5");
    m.promotion = None;

    CHECK(see(board, m) == PieceValue[Pawn]);
}

TEST_CASE("SEE: knight captures pawn defended by pawn", "[see]") {
    ensureInit();
    Board board;
    // White knight on e4, black pawn on d5 defended by pawn on e6
    board.setFen("4k3/8/4p3/3p4/4N3/8/8/4K3 w - - 0 1");

    Move m;
    m.from = stringToSquare("e4");
    m.to = stringToSquare("d5");
    m.promotion = None;

    // Knight (320) captures pawn (100), recaptured by pawn: 100 - 320 = -220
    CHECK(see(board, m) < 0);
}

TEST_CASE("SEE: queen captures defended pawn", "[see]") {
    ensureInit();
    Board board;
    // White queen on d1, black pawn on d5 defended by pawn on e6
    board.setFen("4k3/8/4p3/3p4/8/8/8/3QK3 w - - 0 1");

    Move m;
    m.from = stringToSquare("d1");
    m.to = stringToSquare("d5");
    m.promotion = None;

    // Queen takes pawn, pawn takes queen: 100 - 900 = -800
    CHECK(see(board, m) < 0);
}

TEST_CASE("SEE: capture of undefended piece", "[see]") {
    ensureInit();
    Board board;
    // White knight on e4, black queen on d6 undefended
    board.setFen("4k3/8/3q4/8/4N3/8/8/4K3 w - - 0 1");

    Move m;
    m.from = stringToSquare("e4");
    m.to = stringToSquare("d6");
    m.promotion = None;

    CHECK(see(board, m) == PieceValue[Queen]);
}

TEST_CASE("SEE: rook x-ray through rook", "[see]") {
    ensureInit();
    Board board;
    // White rooks on a1 and a2, black rook on a7 defended by rook on a8
    // Ra2xa7 RxR Ra1xa7: rook takes rook (500), recaptured (500-500=0), x-ray recapture (500)
    board.setFen("r3k3/r7/8/8/8/8/R7/R3K3 w - - 0 1");

    Move m;
    m.from = stringToSquare("a2");
    m.to = stringToSquare("a7");
    m.promotion = None;

    // White wins a rook: capture rook (500), lose rook (-500), recapture rook (+500) = 500
    CHECK(see(board, m) == PieceValue[Rook]);
}

TEST_CASE("SEE: en passant capture", "[see]") {
    ensureInit();
    Board board;
    // White pawn on e5, black pawn on d5 (just double-pushed), en passant on d6
    board.setFen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");

    Move m;
    m.from = stringToSquare("e5");
    m.to = stringToSquare("d6");
    m.promotion = None;

    CHECK(see(board, m) == PieceValue[Pawn]);
}

TEST_CASE("SEE: promotion capture", "[see]") {
    ensureInit();
    Board board;
    // White pawn on a7, black rook on b8
    board.setFen("1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1");

    Move m;
    m.from = stringToSquare("a7");
    m.to = stringToSquare("b8");
    m.promotion = Queen;

    // Capture rook (500) + promotion bonus (900-100=800) = 1300, minus recapture of queen
    // No defenders of b8, so SEE = 500 + 800 = 1300
    CHECK(see(board, m) == PieceValue[Rook] + PieceValue[Queen] - PieceValue[Pawn]);
}

TEST_CASE("SEE: bishop captures knight defended by pawn", "[see]") {
    ensureInit();
    Board board;
    // White bishop on c1, black knight on f4 defended by pawn on g5
    board.setFen("4k3/8/8/6p1/5n2/8/8/2B1K3 w - - 0 1");

    Move m;
    m.from = stringToSquare("c1");
    m.to = stringToSquare("f4");
    m.promotion = None;

    // Bishop (330) takes knight (320), pawn takes bishop: 320 - 330 = -10
    CHECK(see(board, m) < 0);
}
