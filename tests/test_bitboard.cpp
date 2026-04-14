#include "bitboard.h"
#include "catch_amalgamated.hpp"

TEST_CASE("Knight attacks from center", "[bitboard][knight]") {
    initBitboards();

    // Knight on e4 (square 28) attacks d2, f2, c3, g3, c5, g5, d6, f6
    Bitboard expected = squareBB(stringToSquare("d2")) | squareBB(stringToSquare("f2")) |
                        squareBB(stringToSquare("c3")) | squareBB(stringToSquare("g3")) |
                        squareBB(stringToSquare("c5")) | squareBB(stringToSquare("g5")) |
                        squareBB(stringToSquare("d6")) | squareBB(stringToSquare("f6"));
    CHECK(KnightAttacks[stringToSquare("e4")] == expected);
    CHECK(popcount(KnightAttacks[stringToSquare("e4")]) == 8);
}

TEST_CASE("Knight attacks from corner", "[bitboard][knight]") {
    initBitboards();

    // Knight on a1 (square 0) attacks b3, c2
    CHECK(popcount(KnightAttacks[0]) == 2);
    Bitboard expected = squareBB(stringToSquare("b3")) | squareBB(stringToSquare("c2"));
    CHECK(KnightAttacks[0] == expected);
}

TEST_CASE("King attacks from center", "[bitboard][king]") {
    initBitboards();

    // King on e4 attacks all 8 surrounding squares
    CHECK(popcount(KingAttacks[stringToSquare("e4")]) == 8);
}

TEST_CASE("King attacks from corner", "[bitboard][king]") {
    initBitboards();

    // King on a1 attacks a2, b1, b2
    CHECK(popcount(KingAttacks[0]) == 3);
    Bitboard expected = squareBB(stringToSquare("a2")) | squareBB(stringToSquare("b1")) |
                        squareBB(stringToSquare("b2"));
    CHECK(KingAttacks[0] == expected);
}

TEST_CASE("Pawn attacks", "[bitboard][pawn]") {
    initBitboards();

    // White pawn on e4 attacks d5 and f5
    int e4 = stringToSquare("e4");
    Bitboard expected = squareBB(stringToSquare("d5")) | squareBB(stringToSquare("f5"));
    CHECK(PawnAttacks[White][e4] == expected);

    // Black pawn on e5 attacks d4 and f4
    int e5 = stringToSquare("e5");
    expected = squareBB(stringToSquare("d4")) | squareBB(stringToSquare("f4"));
    CHECK(PawnAttacks[Black][e5] == expected);

    // White pawn on a2 attacks only b3 (no wrapping)
    int a2 = stringToSquare("a2");
    CHECK(PawnAttacks[White][a2] == squareBB(stringToSquare("b3")));

    // Black pawn on h7 attacks only g6 (no wrapping)
    int h7 = stringToSquare("h7");
    CHECK(PawnAttacks[Black][h7] == squareBB(stringToSquare("g6")));
}

TEST_CASE("Board occupancy", "[bitboard]") {
    initBitboards();

    Board board;
    Bitboard occ = boardOccupancy(board);
    CHECK(popcount(occ) == 32);

    Bitboard white = boardColorOccupancy(board, White);
    Bitboard black = boardColorOccupancy(board, Black);
    CHECK(popcount(white) == 16);
    CHECK(popcount(black) == 16);
    CHECK((white & black) == 0);
    CHECK((white | black) == occ);
}
