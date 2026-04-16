#ifndef BITBOARD_H
#define BITBOARD_H

#include "board.h"
#include <cstdint>

using Bitboard = uint64_t;

// File masks
constexpr Bitboard FileABB = 0x0101010101010101ULL;
constexpr Bitboard FileBBB = 0x0202020202020202ULL;
constexpr Bitboard FileCBB = 0x0404040404040404ULL;
constexpr Bitboard FileDBB = 0x0808080808080808ULL;
constexpr Bitboard FileEBB = 0x1010101010101010ULL;
constexpr Bitboard FileFBB = 0x2020202020202020ULL;
constexpr Bitboard FileGBB = 0x4040404040404040ULL;
constexpr Bitboard FileHBB = 0x8080808080808080ULL;

// Rank masks
constexpr Bitboard Rank1BB = 0x00000000000000FFULL;
constexpr Bitboard Rank2BB = 0x000000000000FF00ULL;
constexpr Bitboard Rank3BB = 0x0000000000FF0000ULL;
constexpr Bitboard Rank4BB = 0x00000000FF000000ULL;
constexpr Bitboard Rank5BB = 0x000000FF00000000ULL;
constexpr Bitboard Rank6BB = 0x0000FF0000000000ULL;
constexpr Bitboard Rank7BB = 0x00FF000000000000ULL;
constexpr Bitboard Rank8BB = 0xFF00000000000000ULL;

inline int popcount(Bitboard b) {
    return __builtin_popcountll(b);
}

inline int lsb(Bitboard b) {
    return __builtin_ctzll(b);
}

inline int popLsb(Bitboard &b) {
    int sq = lsb(b);
    b &= b - 1;
    return sq;
}

inline Bitboard squareBB(int sq) {
    return 1ULL << sq;
}

inline int msb(Bitboard b) {
    return 63 - __builtin_clzll(b);
}

// File and rank convenience arrays (indexed versions of the individual constants)
extern const Bitboard FileBB[8];
extern const Bitboard RankBB[8];

// Pawn structure masks
extern Bitboard AdjacentFilesBB[8];
extern Bitboard ForwardRanksBB[2][8];
extern Bitboard ForwardFileBB[2][64];
extern Bitboard PassedPawnMask[2][64];
extern Bitboard PawnSpanMask[2][64];

// Non-sliding attack tables
extern Bitboard KnightAttacks[64];
extern Bitboard KingAttacks[64];
extern Bitboard PawnAttacks[2][64];

// King zone: king attacks + king square, extended one rank toward the enemy.
// This roughly covers the 3x4 rectangle in front of and around the king where
// incoming piece attacks are most dangerous.
inline Bitboard kingZoneBB(int kingSq, Color side) {
    Bitboard zone = KingAttacks[kingSq] | squareBB(kingSq);
    if (side == White)
        zone |= (zone << 8);
    else
        zone |= (zone >> 8);
    return zone;
}

// Magic bitboard structures
struct Magic {
    Bitboard mask;
    Bitboard magic;
    int shift;
    Bitboard *attacks;
};

extern Magic RookMagics[64];
extern Magic BishopMagics[64];
extern Bitboard RookTable[102400];
extern Bitboard BishopTable[5248];

inline Bitboard rookAttacks(int sq, Bitboard occ) {
    const Magic &m = RookMagics[sq];
    return m.attacks[((occ & m.mask) * m.magic) >> m.shift];
}

inline Bitboard bishopAttacks(int sq, Bitboard occ) {
    const Magic &m = BishopMagics[sq];
    return m.attacks[((occ & m.mask) * m.magic) >> m.shift];
}

inline Bitboard queenAttacks(int sq, Bitboard occ) {
    return rookAttacks(sq, occ) | bishopAttacks(sq, occ);
}

void initBitboards();

inline Bitboard boardOccupancy(const Board &board) {
    return board.occupied;
}

inline Bitboard boardColorOccupancy(const Board &board, Color c) {
    return board.byColor[c];
}

#endif
