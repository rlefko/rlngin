#include "bitboard.h"

const Bitboard FileBB[8] = {FileABB, FileBBB, FileCBB, FileDBB, FileEBB, FileFBB, FileGBB, FileHBB};
const Bitboard RankBB[8] = {Rank1BB, Rank2BB, Rank3BB, Rank4BB, Rank5BB, Rank6BB, Rank7BB, Rank8BB};

Bitboard AdjacentFilesBB[8];
Bitboard ForwardRanksBB[2][8];
Bitboard ForwardFileBB[2][64];
Bitboard PassedPawnMask[2][64];
Bitboard PawnSpanMask[2][64];

Bitboard KnightAttacks[64];
Bitboard KingAttacks[64];
Bitboard PawnAttacks[2][64];

Magic RookMagics[64];
Magic BishopMagics[64];
Bitboard RookTable[102400];
Bitboard BishopTable[5248];

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

// Rook occupancy mask: relevant squares that can block a rook on sq (excludes edges)
static Bitboard rookMask(int sq) {
    Bitboard mask = 0;
    int rank = squareRank(sq);
    int file = squareFile(sq);

    for (int r = rank + 1; r < 7; r++)
        mask |= squareBB(makeSquare(r, file));
    for (int r = rank - 1; r > 0; r--)
        mask |= squareBB(makeSquare(r, file));
    for (int f = file + 1; f < 7; f++)
        mask |= squareBB(makeSquare(rank, f));
    for (int f = file - 1; f > 0; f--)
        mask |= squareBB(makeSquare(rank, f));

    return mask;
}

// Bishop occupancy mask: relevant squares that can block a bishop on sq (excludes edges)
static Bitboard bishopMask(int sq) {
    Bitboard mask = 0;
    int rank = squareRank(sq);
    int file = squareFile(sq);

    for (int r = rank + 1, f = file + 1; r < 7 && f < 7; r++, f++)
        mask |= squareBB(makeSquare(r, f));
    for (int r = rank + 1, f = file - 1; r < 7 && f > 0; r++, f--)
        mask |= squareBB(makeSquare(r, f));
    for (int r = rank - 1, f = file + 1; r > 0 && f < 7; r--, f++)
        mask |= squareBB(makeSquare(r, f));
    for (int r = rank - 1, f = file - 1; r > 0 && f > 0; r--, f--)
        mask |= squareBB(makeSquare(r, f));

    return mask;
}

// Reference rook attacks (used during table init only)
static Bitboard rookAttacksSlow(int sq, Bitboard occ) {
    Bitboard attacks = 0;
    int rank = squareRank(sq);
    int file = squareFile(sq);

    for (int r = rank + 1; r < 8; r++) {
        Bitboard b = squareBB(makeSquare(r, file));
        attacks |= b;
        if (occ & b) break;
    }
    for (int r = rank - 1; r >= 0; r--) {
        Bitboard b = squareBB(makeSquare(r, file));
        attacks |= b;
        if (occ & b) break;
    }
    for (int f = file + 1; f < 8; f++) {
        Bitboard b = squareBB(makeSquare(rank, f));
        attacks |= b;
        if (occ & b) break;
    }
    for (int f = file - 1; f >= 0; f--) {
        Bitboard b = squareBB(makeSquare(rank, f));
        attacks |= b;
        if (occ & b) break;
    }

    return attacks;
}

// Reference bishop attacks (used during table init only)
static Bitboard bishopAttacksSlow(int sq, Bitboard occ) {
    Bitboard attacks = 0;
    int rank = squareRank(sq);
    int file = squareFile(sq);

    for (int r = rank + 1, f = file + 1; r < 8 && f < 8; r++, f++) {
        Bitboard b = squareBB(makeSquare(r, f));
        attacks |= b;
        if (occ & b) break;
    }
    for (int r = rank + 1, f = file - 1; r < 8 && f >= 0; r++, f--) {
        Bitboard b = squareBB(makeSquare(r, f));
        attacks |= b;
        if (occ & b) break;
    }
    for (int r = rank - 1, f = file + 1; r >= 0 && f < 8; r--, f++) {
        Bitboard b = squareBB(makeSquare(r, f));
        attacks |= b;
        if (occ & b) break;
    }
    for (int r = rank - 1, f = file - 1; r >= 0 && f >= 0; r--, f--) {
        Bitboard b = squareBB(makeSquare(r, f));
        attacks |= b;
        if (occ & b) break;
    }

    return attacks;
}

// Map an index to a specific subset of set bits in the mask
static Bitboard indexToOccupancy(int index, Bitboard mask) {
    Bitboard occ = 0;
    int bits = popcount(mask);
    for (int i = 0; i < bits; i++) {
        int sq = popLsb(mask);
        if (index & (1 << i)) occ |= squareBB(sq);
    }
    return occ;
}

// clang-format off
static const Bitboard RookMagicNumbers[64] = {
    0x0080001020400080ULL, 0x0040001000200040ULL, 0x0080081000200080ULL,
    0x0080040800100080ULL, 0x0080020400080080ULL, 0x0080010200040080ULL,
    0x0080008001000200ULL, 0x0080002040800100ULL, 0x0000800020400080ULL,
    0x0000400020005000ULL, 0x0000801000200080ULL, 0x0000800800100080ULL,
    0x0000800400080080ULL, 0x0000800200040080ULL, 0x0000800100020080ULL,
    0x0000800040800100ULL, 0x0000208000400080ULL, 0x0000404000201000ULL,
    0x0000808010002000ULL, 0x0000808008001000ULL, 0x0000808004000800ULL,
    0x0000808002000400ULL, 0x0000010100020004ULL, 0x0000020000408104ULL,
    0x0000208080004000ULL, 0x0000200040005000ULL, 0x0000100080200080ULL,
    0x0000080080100080ULL, 0x0000040080080080ULL, 0x0000020080040080ULL,
    0x0000010080800200ULL, 0x0000800080004100ULL, 0x0000204000800080ULL,
    0x0000200040401000ULL, 0x0000100080802000ULL, 0x0000080080801000ULL,
    0x0000040080800800ULL, 0x0000020080800400ULL, 0x0000020001010004ULL,
    0x0000800040800100ULL, 0x0000204000808000ULL, 0x0000200040008080ULL,
    0x0000100020008080ULL, 0x0000080010008080ULL, 0x0000040008008080ULL,
    0x0000020004008080ULL, 0x0000010002008080ULL, 0x0000004081020004ULL,
    0x0000204000800080ULL, 0x0000200040008080ULL, 0x0000100020008080ULL,
    0x0000080010008080ULL, 0x0000040008008080ULL, 0x0000020004008080ULL,
    0x0000800100020080ULL, 0x0000800041000080ULL, 0x00FFFCDDFCED714AULL,
    0x007FFCDDFCED714AULL, 0x003FFFCDFFD88096ULL, 0x0000040810002101ULL,
    0x0001000204080011ULL, 0x0001000204000801ULL, 0x0001000082000401ULL,
    0x0001FFFAABFAD1A2ULL,
};

static const Bitboard BishopMagicNumbers[64] = {
    0x0002020202020200ULL, 0x0002020202020000ULL, 0x0004010202000000ULL,
    0x0004040080000000ULL, 0x0001104000000000ULL, 0x0000821040000000ULL,
    0x0000410410400000ULL, 0x0000104104104000ULL, 0x0000040404040400ULL,
    0x0000020202020200ULL, 0x0000040102020000ULL, 0x0000040400800000ULL,
    0x0000011040000000ULL, 0x0000008210400000ULL, 0x0000004104104000ULL,
    0x0000002082082000ULL, 0x0004000808080800ULL, 0x0002000404040400ULL,
    0x0001000202020200ULL, 0x0000800802004000ULL, 0x0000800400A00000ULL,
    0x0000200100884000ULL, 0x0000400082082000ULL, 0x0000200041041000ULL,
    0x0002080010101000ULL, 0x0001040008080800ULL, 0x0000208004010400ULL,
    0x0000404004010200ULL, 0x0000840000802000ULL, 0x0000404002011000ULL,
    0x0000808001041000ULL, 0x0000404000820800ULL, 0x0001041000202000ULL,
    0x0000820800101000ULL, 0x0000104400080800ULL, 0x0000020080080080ULL,
    0x0000404040040100ULL, 0x0000808100020100ULL, 0x0001010100020800ULL,
    0x0000808080010400ULL, 0x0000820820004000ULL, 0x0000410410002000ULL,
    0x0000082088001000ULL, 0x0000002011000800ULL, 0x0000080100400400ULL,
    0x0001010101000200ULL, 0x0002020202000400ULL, 0x0001010101000200ULL,
    0x0000410410400000ULL, 0x0000208208200000ULL, 0x0000002084100000ULL,
    0x0000000020880000ULL, 0x0000001002020000ULL, 0x0000040408020000ULL,
    0x0004040404040000ULL, 0x0002020202020000ULL, 0x0000104104104000ULL,
    0x0000002082082000ULL, 0x0000000020841000ULL, 0x0000000000208800ULL,
    0x0000000010020200ULL, 0x0000000404080200ULL, 0x0000040404040400ULL,
    0x0002020202020200ULL,
};
// clang-format on

// Number of relevant bits for each rook square
static const int RookBits[64] = {
    12, 11, 11, 11, 11, 11, 11, 12, 11, 10, 10, 10, 10, 10, 10, 11, 11, 10, 10, 10, 10, 10,
    10, 11, 11, 10, 10, 10, 10, 10, 10, 11, 11, 10, 10, 10, 10, 10, 10, 11, 11, 10, 10, 10,
    10, 10, 10, 11, 11, 10, 10, 10, 10, 10, 10, 11, 12, 11, 11, 11, 11, 11, 11, 12,
};

// Number of relevant bits for each bishop square
static const int BishopBits[64] = {
    6, 5, 5, 5, 5, 5, 5, 6, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 7, 7, 7, 7, 5, 5, 5, 5, 7, 9, 9, 7, 5, 5,
    5, 5, 7, 9, 9, 7, 5, 5, 5, 5, 7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 5, 5, 5, 5, 5, 5, 6,
};

static void initMagics(Magic magics[], Bitboard table[], const Bitboard magicNumbers[],
                       const int bits[], Bitboard (*maskFn)(int),
                       Bitboard (*attacksFn)(int, Bitboard)) {
    Bitboard *tablePtr = table;

    for (int sq = 0; sq < 64; sq++) {
        Magic &m = magics[sq];
        m.mask = maskFn(sq);
        m.magic = magicNumbers[sq];
        m.shift = 64 - bits[sq];
        m.attacks = tablePtr;

        int numEntries = 1 << bits[sq];
        for (int i = 0; i < numEntries; i++) {
            Bitboard occ = indexToOccupancy(i, m.mask);
            int index = static_cast<int>((occ * m.magic) >> m.shift);
            m.attacks[index] = attacksFn(sq, occ);
        }

        tablePtr += numEntries;
    }
}

static void initPawnMasks() {
    for (int f = 0; f < 8; f++) {
        AdjacentFilesBB[f] = (f > 0 ? FileBB[f - 1] : 0) | (f < 7 ? FileBB[f + 1] : 0);
    }

    for (int r = 0; r < 8; r++) {
        Bitboard above = 0;
        for (int rr = r + 1; rr < 8; rr++)
            above |= RankBB[rr];
        ForwardRanksBB[White][r] = above;

        Bitboard below = 0;
        for (int rr = r - 1; rr >= 0; rr--)
            below |= RankBB[rr];
        ForwardRanksBB[Black][r] = below;
    }

    for (int sq = 0; sq < 64; sq++) {
        int f = squareFile(sq);
        int r = squareRank(sq);

        ForwardFileBB[White][sq] = FileBB[f] & ForwardRanksBB[White][r];
        ForwardFileBB[Black][sq] = FileBB[f] & ForwardRanksBB[Black][r];

        Bitboard fileSpan = FileBB[f] | AdjacentFilesBB[f];
        PassedPawnMask[White][sq] = fileSpan & ForwardRanksBB[White][r];
        PassedPawnMask[Black][sq] = fileSpan & ForwardRanksBB[Black][r];

        PawnSpanMask[White][sq] = AdjacentFilesBB[f] & ForwardRanksBB[White][r];
        PawnSpanMask[Black][sq] = AdjacentFilesBB[f] & ForwardRanksBB[Black][r];
    }
}

void initBitboards() {
    initKnightAttacks();
    initKingAttacks();
    initPawnAttacks();
    initPawnMasks();
    initMagics(RookMagics, RookTable, RookMagicNumbers, RookBits, rookMask, rookAttacksSlow);
    initMagics(BishopMagics, BishopTable, BishopMagicNumbers, BishopBits, bishopMask,
               bishopAttacksSlow);
}
