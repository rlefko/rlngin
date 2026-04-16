#include "catch_amalgamated.hpp"
#include "types.h"

TEST_CASE("squareRank and squareFile", "[types]") {
    // a1 = square 0, rank 0, file 0
    CHECK(squareRank(0) == 0);
    CHECK(squareFile(0) == 0);

    // h1 = square 7, rank 0, file 7
    CHECK(squareRank(7) == 0);
    CHECK(squareFile(7) == 7);

    // a8 = square 56, rank 7, file 0
    CHECK(squareRank(56) == 7);
    CHECK(squareFile(56) == 0);

    // h8 = square 63, rank 7, file 7
    CHECK(squareRank(63) == 7);
    CHECK(squareFile(63) == 7);

    // e4 = square 28, rank 3, file 4
    CHECK(squareRank(28) == 3);
    CHECK(squareFile(28) == 4);
}

TEST_CASE("makeSquare", "[types]") {
    CHECK(makeSquare(0, 0) == 0);
    CHECK(makeSquare(0, 7) == 7);
    CHECK(makeSquare(7, 0) == 56);
    CHECK(makeSquare(7, 7) == 63);
    CHECK(makeSquare(3, 4) == 28);
}

TEST_CASE("squareToString and stringToSquare roundtrip", "[types]") {
    CHECK(squareToString(0) == "a1");
    CHECK(squareToString(7) == "h1");
    CHECK(squareToString(56) == "a8");
    CHECK(squareToString(63) == "h8");
    CHECK(squareToString(28) == "e4");

    CHECK(stringToSquare("a1") == 0);
    CHECK(stringToSquare("h1") == 7);
    CHECK(stringToSquare("a8") == 56);
    CHECK(stringToSquare("h8") == 63);
    CHECK(stringToSquare("e4") == 28);

    // Roundtrip
    for (int sq = 0; sq < 64; sq++) {
        CHECK(stringToSquare(squareToString(sq)) == sq);
    }
}

TEST_CASE("stringToSquare invalid input", "[types]") {
    CHECK(stringToSquare("") == -1);
    CHECK(stringToSquare("a") == -1);
    CHECK(stringToSquare("i1") == -1);
    CHECK(stringToSquare("a9") == -1);
}

TEST_CASE("moveToString and stringToMove roundtrip", "[types]") {
    // Normal move
    Move m1 = {stringToSquare("e2"), stringToSquare("e4"), None};
    CHECK(moveToString(m1) == "e2e4");
    Move parsed1 = stringToMove("e2e4");
    CHECK(parsed1.from == m1.from);
    CHECK(parsed1.to == m1.to);
    CHECK(parsed1.promotion == None);

    // Promotion to queen
    Move m2 = {stringToSquare("e7"), stringToSquare("e8"), Queen};
    CHECK(moveToString(m2) == "e7e8q");
    Move parsed2 = stringToMove("e7e8q");
    CHECK(parsed2.from == m2.from);
    CHECK(parsed2.to == m2.to);
    CHECK(parsed2.promotion == Queen);

    // Promotion to knight
    Move m3 = {stringToSquare("a7"), stringToSquare("a8"), Knight};
    CHECK(moveToString(m3) == "a7a8n");
    Move parsed3 = stringToMove("a7a8n");
    CHECK(parsed3.promotion == Knight);

    // Promotion to rook
    Move m4 = {stringToSquare("h7"), stringToSquare("h8"), Rook};
    CHECK(moveToString(m4) == "h7h8r");

    // Promotion to bishop
    Move m5 = {stringToSquare("b7"), stringToSquare("b8"), Bishop};
    CHECK(moveToString(m5) == "b7b8b");
}

TEST_CASE("packed Score round-trips mg and eg halves", "[types][score]") {
    CHECK(mg_value(S(0, 0)) == 0);
    CHECK(eg_value(S(0, 0)) == 0);

    CHECK(mg_value(S(100, 200)) == 100);
    CHECK(eg_value(S(100, 200)) == 200);

    // Negative middlegame must not corrupt the endgame half.
    CHECK(mg_value(S(-50, 75)) == -50);
    CHECK(eg_value(S(-50, 75)) == 75);

    // Negative endgame must not corrupt the middlegame half.
    CHECK(mg_value(S(60, -90)) == 60);
    CHECK(eg_value(S(60, -90)) == -90);

    // Both halves negative.
    CHECK(mg_value(S(-120, -240)) == -120);
    CHECK(eg_value(S(-120, -240)) == -240);

    // Extremes near the 16-bit range.
    CHECK(mg_value(S(32000, -32000)) == 32000);
    CHECK(eg_value(S(32000, -32000)) == -32000);
}

TEST_CASE("packed Score adds componentwise", "[types][score]") {
    Score a = S(100, 200);
    Score b = S(-40, 55);
    Score c = a + b;
    CHECK(mg_value(c) == 60);
    CHECK(eg_value(c) == 255);

    Score d = a - b;
    CHECK(mg_value(d) == 140);
    CHECK(eg_value(d) == 145);

    // Scaling by an integer multiplier also works componentwise.
    Score e = S(30, 40) * 3;
    CHECK(mg_value(e) == 90);
    CHECK(eg_value(e) == 120);
}
