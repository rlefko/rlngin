#include "bitboard.h"

Bitboard KnightAttacks[64];
Bitboard KingAttacks[64];
Bitboard PawnAttacks[2][64];

static void initKnightAttacks() {
    const int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    const int df[] = {-1, 1, -2, 2, -2, 2, -1, 1};

    for (int sq = 0; sq < 64; sq++) {
        Bitboard attacks = 0;
        int rank = squareRank(sq);
        int file = squareFile(sq);
        for (int i = 0; i < 8; i++) {
            int r = rank + dr[i];
            int f = file + df[i];
            if (r >= 0 && r < 8 && f >= 0 && f < 8) {
                attacks |= squareBB(makeSquare(r, f));
            }
        }
        KnightAttacks[sq] = attacks;
    }
}

static void initKingAttacks() {
    for (int sq = 0; sq < 64; sq++) {
        Bitboard attacks = 0;
        int rank = squareRank(sq);
        int file = squareFile(sq);
        for (int dr = -1; dr <= 1; dr++) {
            for (int df = -1; df <= 1; df++) {
                if (dr == 0 && df == 0) continue;
                int r = rank + dr;
                int f = file + df;
                if (r >= 0 && r < 8 && f >= 0 && f < 8) {
                    attacks |= squareBB(makeSquare(r, f));
                }
            }
        }
        KingAttacks[sq] = attacks;
    }
}

static void initPawnAttacks() {
    for (int sq = 0; sq < 64; sq++) {
        int rank = squareRank(sq);
        int file = squareFile(sq);

        // White pawn attacks (upward)
        Bitboard white = 0;
        if (rank < 7) {
            if (file > 0) white |= squareBB(makeSquare(rank + 1, file - 1));
            if (file < 7) white |= squareBB(makeSquare(rank + 1, file + 1));
        }
        PawnAttacks[White][sq] = white;

        // Black pawn attacks (downward)
        Bitboard black = 0;
        if (rank > 0) {
            if (file > 0) black |= squareBB(makeSquare(rank - 1, file - 1));
            if (file < 7) black |= squareBB(makeSquare(rank - 1, file + 1));
        }
        PawnAttacks[Black][sq] = black;
    }
}

void initBitboards() {
    initKnightAttacks();
    initKingAttacks();
    initPawnAttacks();
}
