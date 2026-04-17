#include "bitboard.h"
#include "catch_amalgamated.hpp"

#include <random>

namespace {

// Straight-line reference attacks used to validate whichever indexing path
// the runtime was compiled with (PEXT or magic multiply). A mismatch here
// means the initialization populated the table at the wrong slot.
Bitboard referenceRookAttacks(int sq, Bitboard occ) {
    Bitboard attacks = 0;
    int rank = squareRank(sq);
    int file = squareFile(sq);
    auto trace = [&](int dr, int df) {
        for (int r = rank + dr, f = file + df; r >= 0 && r < 8 && f >= 0 && f < 8;
             r += dr, f += df) {
            Bitboard bb = squareBB(r * 8 + f);
            attacks |= bb;
            if (occ & bb) break;
        }
    };
    trace(1, 0);
    trace(-1, 0);
    trace(0, 1);
    trace(0, -1);
    return attacks;
}

Bitboard referenceBishopAttacks(int sq, Bitboard occ) {
    Bitboard attacks = 0;
    int rank = squareRank(sq);
    int file = squareFile(sq);
    auto trace = [&](int dr, int df) {
        for (int r = rank + dr, f = file + df; r >= 0 && r < 8 && f >= 0 && f < 8;
             r += dr, f += df) {
            Bitboard bb = squareBB(r * 8 + f);
            attacks |= bb;
            if (occ & bb) break;
        }
    };
    trace(1, 1);
    trace(1, -1);
    trace(-1, 1);
    trace(-1, -1);
    return attacks;
}

} // namespace

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

TEST_CASE("Rook attacks on empty board", "[bitboard][rook]") {
    initBitboards();

    // Rook on a1 with no blockers attacks all of rank 1 and file a (minus a1)
    Bitboard occ = 0;
    Bitboard attacks = rookAttacks(stringToSquare("a1"), occ);
    Bitboard expected = (Rank1BB | FileABB) & ~squareBB(stringToSquare("a1"));
    CHECK(attacks == expected);
    CHECK(popcount(attacks) == 14);
}

TEST_CASE("Rook attacks with blockers", "[bitboard][rook]") {
    initBitboards();

    // Rook on e4, blockers on e7 and b4
    int e4 = stringToSquare("e4");
    Bitboard occ = squareBB(stringToSquare("e7")) | squareBB(stringToSquare("b4"));
    Bitboard attacks = rookAttacks(e4, occ);

    // Should reach e5, e6, e7 (blocked), e3, e2, e1, f4, g4, h4, d4, c4, b4 (blocked)
    CHECK((attacks & squareBB(stringToSquare("e5"))) != 0);
    CHECK((attacks & squareBB(stringToSquare("e7"))) != 0);
    CHECK((attacks & squareBB(stringToSquare("e8"))) == 0);
    CHECK((attacks & squareBB(stringToSquare("b4"))) != 0);
    CHECK((attacks & squareBB(stringToSquare("a4"))) == 0);
    CHECK((attacks & squareBB(stringToSquare("h4"))) != 0);
}

TEST_CASE("Bishop attacks on empty board", "[bitboard][bishop]") {
    initBitboards();

    // Bishop on d4 with no blockers
    Bitboard occ = 0;
    Bitboard attacks = bishopAttacks(stringToSquare("d4"), occ);
    CHECK(popcount(attacks) == 13);
}

TEST_CASE("Bishop attacks with blockers", "[bitboard][bishop]") {
    initBitboards();

    // Bishop on d4, blocker on f6
    int d4 = stringToSquare("d4");
    Bitboard occ = squareBB(stringToSquare("f6"));
    Bitboard attacks = bishopAttacks(d4, occ);

    CHECK((attacks & squareBB(stringToSquare("e5"))) != 0);
    CHECK((attacks & squareBB(stringToSquare("f6"))) != 0);
    CHECK((attacks & squareBB(stringToSquare("g7"))) == 0);
}

TEST_CASE("Queen attacks combine rook and bishop", "[bitboard][queen]") {
    initBitboards();

    int d4 = stringToSquare("d4");
    Bitboard occ = 0;
    Bitboard qa = queenAttacks(d4, occ);
    Bitboard ra = rookAttacks(d4, occ);
    Bitboard ba = bishopAttacks(d4, occ);
    CHECK(qa == (ra | ba));
    CHECK(popcount(qa) == 27);
}

TEST_CASE("Adjacent files for edge file", "[bitboard][pawn]") {
    initBitboards();

    // File A (index 0) only has file B as neighbor
    CHECK(AdjacentFilesBB[0] == FileBBB);
    // File H (index 7) only has file G as neighbor
    CHECK(AdjacentFilesBB[7] == FileGBB);
}

TEST_CASE("Adjacent files for center file", "[bitboard][pawn]") {
    initBitboards();

    // File D (index 3) has files C and E as neighbors
    CHECK(AdjacentFilesBB[3] == (FileCBB | FileEBB));
}

TEST_CASE("Forward ranks", "[bitboard][pawn]") {
    initBitboards();

    // White forward from rank index 3 (4th rank) covers rank indices 4-7
    CHECK(ForwardRanksBB[White][3] == (Rank5BB | Rank6BB | Rank7BB | Rank8BB));
    // Black forward from rank index 3 covers rank indices 0-2
    CHECK(ForwardRanksBB[Black][3] == (Rank1BB | Rank2BB | Rank3BB));
    // White forward from rank 7 is empty (nothing above rank 8)
    CHECK(ForwardRanksBB[White][7] == 0);
    // Black forward from rank 0 is empty (nothing below rank 1)
    CHECK(ForwardRanksBB[Black][0] == 0);
}

TEST_CASE("Forward file", "[bitboard][pawn]") {
    initBitboards();

    // White pawn on e2 (sq 12): forward file covers e3-e8
    int e2 = stringToSquare("e2");
    Bitboard fwd = ForwardFileBB[White][e2];
    CHECK((fwd & squareBB(stringToSquare("e3"))) != 0);
    CHECK((fwd & squareBB(stringToSquare("e8"))) != 0);
    CHECK((fwd & squareBB(stringToSquare("e2"))) == 0);
    CHECK((fwd & squareBB(stringToSquare("e1"))) == 0);
    CHECK(popcount(fwd) == 6);
}

TEST_CASE("Passed pawn mask", "[bitboard][pawn]") {
    initBitboards();

    // White pawn on e4: mask covers d5-d8, e5-e8, f5-f8
    int e4 = stringToSquare("e4");
    Bitboard mask = PassedPawnMask[White][e4];
    CHECK((mask & squareBB(stringToSquare("e5"))) != 0);
    CHECK((mask & squareBB(stringToSquare("d7"))) != 0);
    CHECK((mask & squareBB(stringToSquare("f8"))) != 0);
    CHECK((mask & squareBB(stringToSquare("e4"))) == 0);
    CHECK((mask & squareBB(stringToSquare("c5"))) == 0);
    CHECK(popcount(mask) == 12);
}

TEST_CASE("Pawn span mask", "[bitboard][pawn]") {
    initBitboards();

    // White pawn on e4: span covers d5-d8, f5-f8 (adjacent files only, not own file)
    int e4 = stringToSquare("e4");
    Bitboard span = PawnSpanMask[White][e4];
    CHECK((span & squareBB(stringToSquare("d5"))) != 0);
    CHECK((span & squareBB(stringToSquare("f7"))) != 0);
    CHECK((span & squareBB(stringToSquare("e5"))) == 0);
    CHECK(popcount(span) == 8);
}

TEST_CASE("Outpost ranks cover relative ranks 4-6", "[bitboard][outpost]") {
    initBitboards();

    CHECK(OutpostRanks[White] == (Rank4BB | Rank5BB | Rank6BB));
    CHECK(OutpostRanks[Black] == (Rank3BB | Rank4BB | Rank5BB));
    CHECK((OutpostRanks[White] & Rank1BB) == 0);
    CHECK((OutpostRanks[White] & Rank7BB) == 0);
    CHECK((OutpostRanks[Black] & Rank8BB) == 0);
    CHECK((OutpostRanks[Black] & Rank2BB) == 0);
}

TEST_CASE("Space mask covers central files on own half", "[bitboard][space]") {
    initBitboards();

    Bitboard centerFiles = FileCBB | FileDBB | FileEBB | FileFBB;
    CHECK(SpaceMask[White] == (centerFiles & (Rank2BB | Rank3BB | Rank4BB)));
    CHECK(SpaceMask[Black] == (centerFiles & (Rank5BB | Rank6BB | Rank7BB)));
    CHECK(popcount(SpaceMask[White]) == 12);
    CHECK(popcount(SpaceMask[Black]) == 12);
    CHECK((SpaceMask[White] & FileABB) == 0);
    CHECK((SpaceMask[White] & FileHBB) == 0);
}

TEST_CASE("Flank file unions", "[bitboard][flank]") {
    initBitboards();

    CHECK(KingSideBB == (FileFBB | FileGBB | FileHBB));
    CHECK(QueenSideBB == (FileABB | FileBBB | FileCBB));
    CHECK((KingSideBB & QueenSideBB) == 0);
    CHECK(popcount(KingSideBB) == 24);
    CHECK(popcount(QueenSideBB) == 24);
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

TEST_CASE("Sliding attack parity vs reference", "[bitboard][sliding]") {
    initBitboards();

    // Seeded so a failure on one worker reproduces on another. 256 random
    // occupancies per square covers the blocker space densely enough to catch
    // any off-by-one in the table init while keeping the run fast.
    std::mt19937_64 rng(0xC0FFEEBABE1234ULL);
    constexpr int kOccupanciesPerSquare = 256;

    for (int sq = 0; sq < 64; sq++) {
        for (int i = 0; i < kOccupanciesPerSquare; i++) {
            Bitboard occ = rng();
            CHECK(rookAttacks(sq, occ) == referenceRookAttacks(sq, occ));
            CHECK(bishopAttacks(sq, occ) == referenceBishopAttacks(sq, occ));
        }

        // Also probe the empty-board case explicitly so regressions in the
        // empty-index slot can't hide behind random noise.
        CHECK(rookAttacks(sq, 0) == referenceRookAttacks(sq, 0));
        CHECK(bishopAttacks(sq, 0) == referenceBishopAttacks(sq, 0));
    }
}
