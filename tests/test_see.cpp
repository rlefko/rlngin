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

TEST_CASE("seeGE: winning capture at threshold 0", "[see]") {
    ensureInit();
    Board board;
    // White pawn on e4, black pawn on d5 undefended
    board.setFen("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1");

    Move m;
    m.from = stringToSquare("e4");
    m.to = stringToSquare("d5");
    m.promotion = None;

    CHECK(seeGE(board, m, 0) == true);
}

TEST_CASE("seeGE: losing capture at threshold 0", "[see]") {
    ensureInit();
    Board board;
    // White queen on d1, black pawn on d5 defended by pawn on e6
    board.setFen("4k3/8/4p3/3p4/8/8/8/3QK3 w - - 0 1");

    Move m;
    m.from = stringToSquare("d1");
    m.to = stringToSquare("d5");
    m.promotion = None;

    CHECK(seeGE(board, m, 0) == false);
}

TEST_CASE("seeGE: positive threshold rejects small gain", "[see]") {
    ensureInit();
    Board board;
    // White knight on e4, black pawn on d5 undefended. SEE = PieceValue[Pawn] = 198
    board.setFen("4k3/8/8/3p4/4N3/8/8/4K3 w - - 0 1");

    Move m;
    m.from = stringToSquare("e4");
    m.to = stringToSquare("d5");
    m.promotion = None;

    CHECK(seeGE(board, m, 500) == false);
    CHECK(seeGE(board, m, 198) == true);
    CHECK(seeGE(board, m, 199) == false);
}

TEST_CASE("seeGE: negative threshold accepts losing capture", "[see]") {
    ensureInit();
    Board board;
    // White queen captures pawn defended by pawn. SEE ~ 198 - 2521 = -2323
    board.setFen("4k3/8/4p3/3p4/8/8/8/3QK3 w - - 0 1");

    Move m;
    m.from = stringToSquare("d1");
    m.to = stringToSquare("d5");
    m.promotion = None;

    int seeVal = see(board, m);
    CHECK(seeGE(board, m, -1200) == false);
    CHECK(seeGE(board, m, seeVal) == true);
    CHECK(seeGE(board, m, seeVal + 1) == false);
}

TEST_CASE("seeGE: consistency with see() across positions", "[see]") {
    ensureInit();

    struct TestPos {
        const char *fen;
        const char *from;
        const char *to;
        PieceType promo;
    };

    TestPos positions[] = {
        // Pawn takes pawn
        {"4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", "e4", "d5", None},
        // Knight takes defended pawn
        {"4k3/8/4p3/3p4/4N3/8/8/4K3 w - - 0 1", "e4", "d5", None},
        // Knight takes undefended queen
        {"4k3/8/3q4/8/4N3/8/8/4K3 w - - 0 1", "e4", "d6", None},
        // Rook x-ray
        {"r3k3/r7/8/8/8/8/R7/R3K3 w - - 0 1", "a2", "a7", None},
        // En passant
        {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", "e5", "d6", None},
        // Promotion capture
        {"1r2k3/P7/8/8/8/8/8/4K3 w - - 0 1", "a7", "b8", Queen},
    };

    for (const auto &pos : positions) {
        Board board;
        board.setFen(pos.fen);

        Move m;
        m.from = stringToSquare(pos.from);
        m.to = stringToSquare(pos.to);
        m.promotion = pos.promo;

        int seeVal = see(board, m);
        CHECK(seeGE(board, m, seeVal) == true);
        CHECK(seeGE(board, m, seeVal + 1) == false);
    }
}

TEST_CASE("seeGE: quiet move to attacked square", "[see]") {
    ensureInit();
    Board board;
    // White knight on e4, black pawn on d5 attacks e4's potential squares.
    // Knight moves to d6 which is attacked by pawn on e7.
    board.setFen("4k3/4p3/8/8/4N3/8/8/4K3 w - - 0 1");

    Move m;
    m.from = stringToSquare("e4");
    m.to = stringToSquare("d6");
    m.promotion = None;

    // Moving to a square attacked by a pawn: SEE = -PieceValue[Knight] = -817
    CHECK(seeGE(board, m, 0) == false);
    CHECK(seeGE(board, m, -817) == true);
    CHECK(seeGE(board, m, -816) == false);
}
