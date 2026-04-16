#include "eval.h"

#include "bitboard.h"
#include "pawn_hash.h"
#include "zobrist.h"

#include <algorithm>

// Material values used by SEE and move ordering (MG values, king kept large for SEE)
const int PieceValue[] = {0, 82, 337, 365, 477, 1025, 20000};

// Packed middlegame and endgame material values (king = 0, always present on
// both sides). Encoded with S(mg, eg) so eval accumulates both halves with a
// single add per piece.
static const Score PieceScore[7] = {
    S(0, 0),      // None
    S(82, 94),    // Pawn
    S(337, 281),  // Knight
    S(365, 297),  // Bishop
    S(477, 512),  // Rook
    S(1025, 936), // Queen
    S(0, 0),      // King (material is implicit; all games have exactly one)
};

// Game phase increments per piece type (max total = 24)
static const int GamePhaseInc[] = {0, 0, 1, 1, 2, 4, 0};

// clang-format off

// PeSTO piece-square tables stored in a1=0 order (rank 1 first, rank 8 last),
// with midgame and endgame values packed into a single Score via S(mg, eg).
// Values are from White's perspective; Black mirrors vertically via sq ^ 56.

static const Score PawnPST[64] = {
    S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0),
    S(-35,  13), S( -1,   8), S(-20,   8), S(-23,  10), S(-15,  13), S( 24,   0), S( 38,   2), S(-22,  -7),
    S(-26,   4), S( -4,   7), S( -4,  -6), S(-10,   1), S(  3,   0), S(  3,  -5), S( 33,  -1), S(-12,  -8),
    S(-27,  13), S( -2,   9), S( -5,  -3), S( 12,  -7), S( 17,  -7), S(  6,  -8), S( 10,   3), S(-25,  -1),
    S(-14,  32), S( 13,  24), S(  6,  13), S( 21,   5), S( 23,  -2), S( 12,   4), S( 17,  17), S(-23,  17),
    S( -6,  94), S(  7, 100), S( 26,  85), S( 31,  67), S( 65,  56), S( 56,  53), S( 25,  82), S(-20,  84),
    S( 98, 178), S(134, 173), S( 61, 158), S( 95, 134), S( 68, 147), S(126, 132), S( 34, 165), S(-11, 187),
    S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0),
};

static const Score KnightPST[64] = {
    S(-105, -29), S(-21, -51), S(-58, -23), S(-33, -15), S(-17, -22), S(-28, -18), S(-19, -50), S(-23, -64),
    S( -29, -42), S(-53, -20), S(-12, -10), S( -3,  -5), S( -1,  -2), S( 18, -20), S(-14, -23), S(-19, -44),
    S( -23, -23), S( -9,  -3), S( 12,  -1), S( 10,  15), S( 19,  10), S( 17,  -3), S( 25, -20), S(-16, -22),
    S( -13, -18), S(  4,  -6), S( 16,  16), S( 13,  25), S( 28,  16), S( 19,  17), S( 21,   4), S( -8, -18),
    S(  -9, -17), S( 17,   3), S( 19,  22), S( 53,  22), S( 37,  22), S( 69,  11), S( 18,   8), S( 22, -18),
    S( -47, -24), S( 60, -20), S( 37,  10), S( 65,   9), S( 84,  -1), S(129,  -9), S( 73, -19), S( 44, -41),
    S( -73, -25), S(-41,  -8), S( 72, -25), S( 36,  -2), S( 23,  -9), S( 62, -25), S(  7, -24), S(-17, -52),
    S(-167, -58), S(-89, -38), S(-34, -13), S(-49, -28), S( 61, -31), S(-97, -27), S(-15, -63), S(-107, -99),
};

static const Score BishopPST[64] = {
    S(-33, -23), S( -3,  -9), S(-14, -23), S(-21,  -5), S(-13,  -9), S(-12, -16), S(-39,  -5), S(-21, -17),
    S(  4, -14), S( 15, -18), S( 16,  -7), S(  0,  -1), S(  7,   4), S( 21,  -9), S( 33, -15), S(  1, -27),
    S(  0, -12), S( 15,  -3), S( 15,   8), S( 15,  10), S( 14,  13), S( 27,   3), S( 18,  -7), S( 10, -15),
    S( -6,  -6), S( 13,   3), S( 13,  13), S( 26,  19), S( 34,   7), S( 12,  10), S( 10,  -3), S(  4,  -9),
    S( -4,  -3), S(  5,   9), S( 19,  12), S( 50,   9), S( 37,  14), S( 37,  10), S(  7,   3), S( -2,   2),
    S(-16,   2), S( 37,  -8), S( 43,   0), S( 40,  -1), S( 35,  -2), S( 50,   6), S( 37,   0), S( -2,   4),
    S(-26,  -8), S( 16,  -4), S(-18,   7), S(-13, -12), S( 30,  -3), S( 59, -13), S( 18,  -4), S(-47, -14),
    S(-29, -14), S(  4, -21), S(-82, -11), S(-37,  -8), S(-25,  -7), S(-42,  -9), S(  7, -17), S( -8, -24),
};

static const Score RookPST[64] = {
    S(-19,  -9), S(-13,   2), S(  1,   3), S( 17,  -1), S( 16,  -5), S(  7, -13), S(-37,   4), S(-26, -20),
    S(-44,  -6), S(-16,  -6), S(-20,   0), S( -9,   2), S( -1,  -9), S( 11,  -9), S( -6, -11), S(-71,  -3),
    S(-45,  -4), S(-25,   0), S(-16,  -5), S(-17,  -1), S(  3,  -7), S(  0, -12), S( -5,  -8), S(-33, -16),
    S(-36,   3), S(-26,   5), S(-12,   8), S( -1,   4), S(  9,  -5), S( -7,  -6), S(  6,  -8), S(-23, -11),
    S(-24,   4), S(-11,   3), S(  7,  13), S( 26,   1), S( 24,   2), S( 35,   1), S( -8,  -1), S(-20,   2),
    S( -5,   7), S( 19,   7), S( 26,   7), S( 36,   5), S( 17,   4), S( 45,  -3), S( 61,  -5), S( 16,  -3),
    S( 27,  11), S( 32,  13), S( 58,  13), S( 62,  11), S( 80,  -3), S( 67,   3), S( 26,   8), S( 44,   3),
    S( 32,  13), S( 42,  10), S( 32,  18), S( 51,  15), S( 63,  12), S(  9,  12), S( 31,   8), S( 43,   5),
};

static const Score QueenPST[64] = {
    S( -1, -33), S(-18, -28), S( -9, -22), S( 10, -43), S(-15,  -5), S(-25, -32), S(-31, -20), S(-50, -41),
    S(-35, -22), S( -8, -23), S( 11, -30), S(  2, -16), S(  8, -16), S( 15, -23), S( -3, -36), S(  1, -32),
    S(-14, -16), S(  2, -27), S(-11,  15), S( -2,   6), S( -5,   9), S(  2,  17), S( 14,  10), S(  5,   5),
    S( -9, -18), S(-26,  28), S( -9,  19), S(-10,  47), S( -2,  31), S( -4,  34), S(  3,  39), S( -3,  23),
    S(-27,   3), S(-27,  22), S(-16,  24), S(-16,  45), S( -1,  57), S( 17,  40), S( -2,  57), S(  1,  36),
    S(-13, -20), S(-17,   6), S(  7,   9), S(  8,  49), S( 29,  47), S( 56,  35), S( 47,  19), S( 57,   9),
    S(-24, -17), S(-39,  20), S( -5,  32), S(  1,  41), S(-16,  58), S( 57,  25), S( 28,  30), S( 54,   0),
    S(-28,  -9), S(  0,  22), S( 29,  22), S( 12,  27), S( 59,  27), S( 44,  19), S( 43,  10), S( 45,  20),
};

static const Score KingPST[64] = {
    S(-15, -53), S( 36, -34), S( 12, -21), S(-54, -11), S(  8, -28), S(-28, -14), S( 24, -24), S( 14, -43),
    S(  1, -27), S(  7, -11), S( -8,   4), S(-64,  13), S(-43,  14), S(-16,   4), S(  9,  -5), S(  8, -17),
    S(-14, -19), S(-14,  -3), S(-22,  11), S(-46,  21), S(-44,  23), S(-30,  16), S(-15,   7), S(-27,  -9),
    S(-49, -18), S( -1,  -4), S(-27,  21), S(-39,  24), S(-46,  27), S(-44,  23), S(-33,   9), S(-51, -11),
    S(-17,  -8), S(-20,  22), S(-12,  24), S(-27,  27), S(-30,  26), S(-25,  33), S(-14,  26), S(-36,   3),
    S( -9,  10), S( 24,  17), S(  2,  23), S(-16,  15), S(-20,  20), S(  6,  45), S( 22,  44), S(-22,  13),
    S( 29, -12), S( -1,  17), S(-20,  14), S( -7,  17), S( -8,  17), S( -4,  38), S(-38,  23), S(-29,  11),
    S(-65, -74), S( 23, -35), S( 16, -18), S(-15, -18), S(-56, -11), S(-34,  15), S(  2,   4), S( 13, -17),
};

// clang-format on

static const Score *PST[] = {
    nullptr,   // None
    PawnPST,   // Pawn
    KnightPST, // Knight
    BishopPST, // Bishop
    RookPST,   // Rook
    QueenPST,  // Queen
    KingPST,   // King
};

static void ensureEvalInit() {
    static const bool initialized = []() {
        zobrist::init();
        initBitboards();
        return true;
    }();

    (void)initialized;
}

// clang-format off

// Passed pawn bonus by rank index (0 = rank 1, 7 = rank 8). Indices 0 and 7
// are impossible for pawns; included for indexing simplicity.
static const Score PassedPawnBonus[8] = {
    S( 0,   0),  // rank 1
    S( 5,  10),  // rank 2
    S(10,  17),  // rank 3
    S(15,  32),  // rank 4
    S(30,  62),  // rank 5
    S(55, 107),  // rank 6
    S(90, 170),  // rank 7
    S( 0,   0),  // rank 8
};

// Piece mobility bonus indexed by PieceType and number of attacked
// mobility-area squares. Squares occupied by our own pieces and squares
// attacked by enemy pawns are excluded before counting. Values are derived
// from Stockfish-style tuning and are intentionally nonlinear so the bonus
// leans negative for trapped pieces and flattens once a piece is already
// well developed. Pawn and King rows are unused; the extra dimensions let
// us index by PieceType directly.
static const Score MobilityBonus[7][28] = {
    {},  // None
    {},  // Pawn
    {    // Knight (0..8)
        S(-62, -81), S(-53, -56), S(-12, -30), S( -4, -14), S(  3,   8),
        S( 13,  15), S( 22,  23), S( 28,  27), S( 33,  33),
    },
    {    // Bishop (0..13)
        S(-48, -59), S(-20, -23), S( 16,  -3), S( 26,  13), S( 38,  24),
        S( 51,  42), S( 55,  54), S( 63,  57), S( 63,  65), S( 68,  73),
        S( 81,  78), S( 81,  86), S( 91,  88), S( 98,  97),
    },
    {    // Rook (0..14)
        S(-58, -76), S(-27, -18), S(-15,  28), S(-10,  55), S( -5,  69),
        S( -2,  82), S(  9, 112), S( 16, 118), S( 30, 132), S( 29, 142),
        S( 32, 155), S( 38, 165), S( 46, 166), S( 48, 169), S( 58, 171),
    },
    {    // Queen (0..27)
        S(-39, -36), S(-21, -15), S(  3,   8), S(  3,  18), S( 14,  34),
        S( 22,  54), S( 28,  61), S( 41,  73), S( 43,  79), S( 48,  92),
        S( 56,  94), S( 60, 104), S( 60, 113), S( 66, 120), S( 67, 123),
        S( 70, 126), S( 71, 133), S( 73, 136), S( 79, 140), S( 88, 143),
        S( 88, 148), S( 99, 166), S(102, 170), S(102, 175), S(106, 184),
        S(109, 191), S(113, 206), S(116, 212),
    },
    {},  // King
};

// Rook bonuses for open and semi-open files. An "open file" has no pawns of
// either color; a "semi-open file" has no friendly pawns but at least one
// enemy pawn. Both bonuses are larger in the middlegame where file control
// translates into direct king pressure.
static const Score RookOpenFileBonus     = S(45, 20);
static const Score RookSemiOpenFileBonus = S(20,  7);

// Minor-piece outpost bonuses. A knight or bishop is on an outpost when it
// sits on a relative rank 4-6 square that is defended by a friendly pawn
// and can no longer be challenged by an enemy pawn push. Knights benefit
// more than bishops because bishops see through the square anyway.
static const Score KnightOutpostBonus = S(30, 20);
static const Score BishopOutpostBonus = S(18,  8);

// Penalty for a rook shut in on the same side of the board as its own king
// when the rook has little room to breathe. Pure middlegame concern; in the
// endgame the king activates and the rook typically walks free. Doubled
// when all castling rights are gone, since O-O / O-O-O cannot relocate the
// rook to an active square.
static const Score TrappedRookByKingPenalty = S(-52, 0);

// Space evaluation: count safe central squares on our own side of the board.
// Stockfish weights the result quadratically by non-pawn piece count so the
// term fades in the endgame where space is no longer a trump. Gated on a
// minimum piece count to keep the bonus from firing in thin positions where
// it does not translate into a real plan.
static const int SpaceWeightDivisor = 16;
static const int SpaceMinPieceCount = 2;

// Connected pawn bonus by rank index
static const Score ConnectedPawnBonus[8] = {
    S( 0,  0),  // rank 1
    S( 7,  0),  // rank 2
    S( 8,  3),  // rank 3
    S(12,  7),  // rank 4
    S(25, 17),  // rank 5
    S(45, 30),  // rank 6
    S(70, 42),  // rank 7
    S( 0,  0),  // rank 8
};

// --- King safety constants ---

// Pawn shield bonus per shield-file pawn by relative rank.
// Index 0 = pawn on 2nd rank (unmoved, strongest), index 1 = pawn on 3rd rank.
// Values are conservative so the shield signal cannot dominate material,
// PST, or pawn structure in normal middlegame positions.
static const Score PawnShieldBonus[2] = {
    S(20, 3),  // 2nd rank
    S(12, 2),  // 3rd rank
};

// Pawn storm penalty indexed by rank distance from our king.
// Index 0 = 4+ ranks away, index 4 = on the same rank (blocked)
static const Score PawnStormPenalty[5] = {
    S( 0, 0),  // 4+ ranks away
    S(10, 0),  // 3 ranks away
    S(25, 0),  // 2 ranks away
    S(40, 0),  // 1 rank away
    S(10, 0),  // same rank (blocked, less dangerous)
};

// Per-file penalty when our pawns or all pawns are missing near the king.
// A shield pawn's absence is the same signal as the file being semi-open
// or open for us, so we express it only once here rather than stacking a
// separate "missing shield" penalty on top.
static const Score SemiOpenFileNearKing = S(-10, 0);
static const Score OpenFileNearKing     = S(-15, 0);

// Attack units per piece type when it attacks the king zone
static const int KingAttackUnits[] = {
    0,   // None
    0,   // Pawn (handled via pawn storm)
    2,   // Knight
    2,   // Bishop
    3,   // Rook
    4,   // Queen
    0    // King
};

// Modest king-zone penalties indexed by attack units, capped at 12.
// These stay intentionally small because this engine does not yet score
// richer mating features like safe checks, attack-square multiplicity,
// or defender saturation.
static const int KingAttackPenalty[13] = {
    0,   0,   0,   12,  32,  60,  96,
    140, 192, 252, 320, 396, 480,
};

// Penalty per king-zone square attacked by enemy but not defended by
// any friendly piece.
static const Score UndefendedKingZoneSq = S(-7, -1);

// Penalty by number of safe squares the king can move to (0 = most
// dangerous). Index is the count of safe squares, capped at 8.
static const Score KingSafeSqPenalty[9] = {
    S(-50, -5),  // 0 safe squares
    S(-35, -3),  // 1
    S(-20, -1),  // 2
    S(-10,  0),  // 3
    S( -4,  0),  // 4
    S(  0,  0),  // 5
    S(  0,  0),  // 6
    S(  0,  0),  // 7
    S(  0,  0),  // 8
};

// clang-format on

static const Score IsolatedPawnPenalty = S(-15, -20);
static const Score DoubledPawnPenalty = S(-10, -20);
static const Score BackwardPawnPenalty = S(-10, -15);

// Per-evaluation derived context: enemy-pawn attack maps and mobility-area
// bitboards reused by every piece-activity term.
struct EvalContext {
    Bitboard pawnAttacks[2];
    Bitboard mobilityArea[2];
};

static inline Bitboard pawnAttacksBB(Bitboard pawns, Color c) {
    if (c == White) {
        return ((pawns & ~FileABB) << 7) | ((pawns & ~FileHBB) << 9);
    }
    return ((pawns & ~FileABB) >> 9) | ((pawns & ~FileHBB) >> 7);
}

static PawnHashTable pawnHashTable(2);

static void evaluatePawns(const Board &board, Score &out) {
    int mgCached = 0, egCached = 0;
    if (pawnHashTable.probe(board.pawnKey, mgCached, egCached)) {
        out = S(mgCached, egCached);
        return;
    }

    Score score = 0;

    Bitboard whitePawns = board.byPiece[Pawn] & board.byColor[White];
    Bitboard blackPawns = board.byPiece[Pawn] & board.byColor[Black];

    for (int c = 0; c < 2; c++) {
        Bitboard ourPawns = (c == White) ? whitePawns : blackPawns;
        Bitboard theirPawns = (c == White) ? blackPawns : whitePawns;
        int sign = (c == White) ? 1 : -1;

        Bitboard pawns = ourPawns;
        while (pawns) {
            int sq = popLsb(pawns);
            int r = squareRank(sq);
            int f = squareFile(sq);
            int relativeRank = (c == White) ? r : (7 - r);

            // Doubled pawn: another friendly pawn ahead on the same file
            bool isDoubled = (ForwardFileBB[c][sq] & ourPawns) != 0;
            if (isDoubled) {
                score += sign * DoubledPawnPenalty;
            }

            // Passed pawn: no enemy pawns ahead on same or adjacent files,
            // and no friendly pawn ahead on the same file (rear doubled pawns
            // are not passed)
            if (!isDoubled && !(PassedPawnMask[c][sq] & theirPawns)) {
                score += sign * PassedPawnBonus[relativeRank];
            }

            // Isolated pawn: no friendly pawns on adjacent files
            bool isolated = !(AdjacentFilesBB[f] & ourPawns);
            if (isolated) {
                score += sign * IsolatedPawnPenalty;
            }

            // Connected pawn: phalanx (same rank, adjacent file) or defended by friendly pawn
            bool phalanx = (ourPawns & AdjacentFilesBB[f] & RankBB[r]) != 0;
            bool defended = (PawnAttacks[c ^ 1][sq] & ourPawns) != 0;
            if (phalanx || defended) {
                score += sign * ConnectedPawnBonus[relativeRank];
            }

            // Backward pawn: not connected, not isolated, all adjacent friendly pawns
            // are ahead, and the stop square is controlled by an enemy pawn
            if (!phalanx && !defended && !isolated) {
                bool noneBelow = !(PawnSpanMask[c ^ 1][sq] & ourPawns);
                if (noneBelow) {
                    int stopSq = (c == White) ? sq + 8 : sq - 8;
                    if (PawnAttacks[c][stopSq] & theirPawns) {
                        score += sign * BackwardPawnPenalty;
                    }
                }
            }
        }
    }

    pawnHashTable.store(board.pawnKey, mg_value(score), eg_value(score));
    out = score;
}

// Accumulate piece-activity terms: mobility for every non-pawn non-king
// piece, rook bonuses for open and semi-open files, and outpost bonuses
// for knights and bishops. Mobility is intentionally pseudo-legal --
// pinned pieces still get credit for the squares they attack because the
// search resolves pin tactics on its own, which matches Stockfish's
// choice here.
static void evaluatePieces(const Board &board, const EvalContext &ctx, Score scores[2]) {
    Bitboard occ = board.occupied;

    for (int c = 0; c < 2; c++) {
        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[c];
        Bitboard theirPawns = board.byPiece[Pawn] & board.byColor[c ^ 1];

        Bitboard knights = board.byPiece[Knight] & board.byColor[c];
        while (knights) {
            int sq = popLsb(knights);
            int count = popcount(KnightAttacks[sq] & ctx.mobilityArea[c]);
            scores[c] += MobilityBonus[Knight][count];

            if ((squareBB(sq) & OutpostRanks[c]) && (PawnAttacks[c ^ 1][sq] & ourPawns) &&
                !(PawnSpanMask[c][sq] & theirPawns)) {
                scores[c] += KnightOutpostBonus;
            }
        }

        Bitboard bishops = board.byPiece[Bishop] & board.byColor[c];
        while (bishops) {
            int sq = popLsb(bishops);
            int count = popcount(bishopAttacks(sq, occ) & ctx.mobilityArea[c]);
            scores[c] += MobilityBonus[Bishop][count];

            if ((squareBB(sq) & OutpostRanks[c]) && (PawnAttacks[c ^ 1][sq] & ourPawns) &&
                !(PawnSpanMask[c][sq] & theirPawns)) {
                scores[c] += BishopOutpostBonus;
            }
        }

        Bitboard kingBB = board.byPiece[King] & board.byColor[c];
        int kingSq = kingBB ? lsb(kingBB) : -1;
        int kingFile = (kingSq >= 0) ? squareFile(kingSq) : -1;
        bool lostShortCastle = (c == White) ? !board.castleWK : !board.castleBK;
        bool lostLongCastle = (c == White) ? !board.castleWQ : !board.castleBQ;

        Bitboard rooks = board.byPiece[Rook] & board.byColor[c];
        while (rooks) {
            int sq = popLsb(rooks);
            int count = popcount(rookAttacks(sq, occ) & ctx.mobilityArea[c]);
            scores[c] += MobilityBonus[Rook][count];

            Bitboard fileMask = FileBB[squareFile(sq)];
            bool noOurPawns = !(fileMask & ourPawns);
            bool noTheirPawns = !(fileMask & theirPawns);
            if (noOurPawns && noTheirPawns) {
                scores[c] += RookOpenFileBonus;
            } else if (noOurPawns) {
                scores[c] += RookSemiOpenFileBonus;
            }

            // Trapped rook: little room to move and our king is on the same
            // side of the board, so the rook cannot swing across. Gating on
            // mobility rather than piece-square heuristics avoids the old
            // loophole where stepping the king off the back rank silenced
            // the penalty without actually freeing the rook.
            if (count <= 3 && kingSq >= 0) {
                int rookFile = squareFile(sq);
                bool sameSide = (kingFile < 4) == (rookFile < kingFile);
                if (sameSide) {
                    Score penalty = TrappedRookByKingPenalty;
                    if (lostShortCastle && lostLongCastle) penalty *= 2;
                    scores[c] += penalty;
                }
            }
        }

        Bitboard queens = board.byPiece[Queen] & board.byColor[c];
        while (queens) {
            int sq = popLsb(queens);
            int count = popcount(queenAttacks(sq, occ) & ctx.mobilityArea[c]);
            scores[c] += MobilityBonus[Queen][count];
        }
    }
}

// Count central squares on our side of the board that are safe from enemy
// pawn attacks and not occupied by our own pawns. Squares behind our own
// pawn chain count double: they are already committed territory and
// amplify the bonus Stockfish-style. Scaled quadratically by the number
// of our non-pawn non-king pieces so the term only bites in middlegames
// with enough material to exploit the extra space.
static void evaluateSpace(const Board &board, const EvalContext &ctx, Score scores[2]) {
    for (int c = 0; c < 2; c++) {
        Bitboard ourPieces = board.byColor[c] & ~board.byPiece[Pawn] & ~board.byPiece[King];
        int weight = popcount(ourPieces);
        if (weight < SpaceMinPieceCount) continue;

        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[c];
        Bitboard safe = SpaceMask[c] & ~ctx.pawnAttacks[c ^ 1] & ~ourPawns;

        Bitboard behind = ourPawns;
        if (c == White) {
            behind |= behind >> 8;
            behind |= behind >> 16;
        } else {
            behind |= behind << 8;
            behind |= behind << 16;
        }

        int bonus = popcount(safe) + popcount(safe & behind);
        scores[c] += S(bonus * weight * weight / SpaceWeightDivisor, 0);
    }
}

static void evaluateKingSafety(const Board &board, Score scores[2]) {
    Bitboard occ = board.occupied;

    for (int c = 0; c < 2; c++) {
        Color us = static_cast<Color>(c);
        Color them = static_cast<Color>(c ^ 1);

        Bitboard kingBB = board.byPiece[King] & board.byColor[us];
        if (!kingBB) continue;
        int kingSq = lsb(kingBB);
        int kingFile = squareFile(kingSq);
        int kingRank = squareRank(kingSq);

        Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[us];
        Bitboard theirPawns = board.byPiece[Pawn] & board.byColor[them];

        int shieldFileMin = std::max(0, kingFile - 1);
        int shieldFileMax = std::min(7, kingFile + 1);

        // Pawn shield, pawn storm, and open file evaluation per shield file
        for (int f = shieldFileMin; f <= shieldFileMax; f++) {
            Bitboard fileMask = FileBB[f];
            Bitboard ourPawnsOnFile = ourPawns & fileMask;
            Bitboard theirPawnsOnFile = theirPawns & fileMask;

            // Pawn shield: find the closest friendly pawn to our back rank.
            // When the file has no friendly pawn, the missing-shield signal
            // is captured by the semi-open / open file penalties below, so
            // no separate penalty is applied here.
            if (ourPawnsOnFile) {
                int pawnSq = (us == White) ? lsb(ourPawnsOnFile) : msb(ourPawnsOnFile);
                int relativeRank = (us == White) ? squareRank(pawnSq) : (7 - squareRank(pawnSq));

                if (relativeRank == 1) {
                    scores[us] += PawnShieldBonus[0];
                } else if (relativeRank == 2) {
                    scores[us] += PawnShieldBonus[1];
                }
            }

            // Pawn storm: find the most-advanced enemy pawn on this file
            if (theirPawnsOnFile) {
                int stormSq = (us == White) ? lsb(theirPawnsOnFile) : msb(theirPawnsOnFile);
                int distance = std::abs(squareRank(stormSq) - kingRank);
                int idx = std::max(0, 4 - std::min(4, distance));
                scores[us] -= PawnStormPenalty[idx];
            }

            // Open and semi-open file penalties
            if (!ourPawnsOnFile && !theirPawnsOnFile) {
                scores[us] += OpenFileNearKing;
            } else if (!ourPawnsOnFile) {
                scores[us] += SemiOpenFileNearKing;
            }
        }

        // King zone attack evaluation
        Bitboard kZone = kingZoneBB(kingSq, us);
        int attackerCount = 0;
        int attackUnits = 0;
        bool attackingQueenPresent = false;

        // Accumulate all enemy attacks for square control evaluation
        Bitboard enemyAttacks = 0;

        // Enemy pawn attacks
        if (them == White) {
            enemyAttacks |= ((theirPawns & ~FileHBB) << 9) | ((theirPawns & ~FileABB) << 7);
        } else {
            enemyAttacks |= ((theirPawns & ~FileABB) >> 9) | ((theirPawns & ~FileHBB) >> 7);
        }

        // Enemy king attacks
        Bitboard theirKingBB = board.byPiece[King] & board.byColor[them];
        if (theirKingBB) enemyAttacks |= KingAttacks[lsb(theirKingBB)];

        Bitboard theirKnights = board.byPiece[Knight] & board.byColor[them];
        while (theirKnights) {
            int sq = popLsb(theirKnights);
            Bitboard atk = KnightAttacks[sq];
            enemyAttacks |= atk;
            if (atk & kZone) {
                attackerCount++;
                attackUnits += KingAttackUnits[Knight];
            }
        }

        Bitboard theirBishops = board.byPiece[Bishop] & board.byColor[them];
        while (theirBishops) {
            int sq = popLsb(theirBishops);
            Bitboard atk = bishopAttacks(sq, occ);
            enemyAttacks |= atk;
            if (atk & kZone) {
                attackerCount++;
                attackUnits += KingAttackUnits[Bishop];
            }
        }

        Bitboard theirRooks = board.byPiece[Rook] & board.byColor[them];
        while (theirRooks) {
            int sq = popLsb(theirRooks);
            Bitboard atk = rookAttacks(sq, occ);
            enemyAttacks |= atk;
            if (atk & kZone) {
                attackerCount++;
                attackUnits += KingAttackUnits[Rook];
            }
        }

        Bitboard theirQueens = board.byPiece[Queen] & board.byColor[them];
        while (theirQueens) {
            int sq = popLsb(theirQueens);
            Bitboard atk = queenAttacks(sq, occ);
            enemyAttacks |= atk;
            if (atk & kZone) {
                attackerCount++;
                attackUnits += KingAttackUnits[Queen];
                attackingQueenPresent = true;
            }
        }

        // Build friendly defense map from all our pieces -- reused by both
        // the undefended-square term and the gated safe-square term
        Bitboard friendlyDefense = KingAttacks[kingSq];
        if (us == White) {
            friendlyDefense |= ((ourPawns & ~FileHBB) << 9) | ((ourPawns & ~FileABB) << 7);
        } else {
            friendlyDefense |= ((ourPawns & ~FileABB) >> 9) | ((ourPawns & ~FileHBB) >> 7);
        }
        Bitboard ourKnights = board.byPiece[Knight] & board.byColor[us];
        while (ourKnights)
            friendlyDefense |= KnightAttacks[popLsb(ourKnights)];
        Bitboard ourBishops = board.byPiece[Bishop] & board.byColor[us];
        while (ourBishops)
            friendlyDefense |= bishopAttacks(popLsb(ourBishops), occ);
        Bitboard ourRooks = board.byPiece[Rook] & board.byColor[us];
        while (ourRooks)
            friendlyDefense |= rookAttacks(popLsb(ourRooks), occ);
        Bitboard ourQueens = board.byPiece[Queen] & board.byColor[us];
        while (ourQueens)
            friendlyDefense |= queenAttacks(popLsb(ourQueens), occ);

        // Undefended zone squares -- self-regulating: with no enemy piece
        // attacking the zone this contributes zero, so no gate is needed
        Bitboard undefAttacked = kZone & enemyAttacks & ~friendlyDefense;
        int undefCount = popcount(undefAttacked);
        scores[us] += undefCount * UndefendedKingZoneSq;

        // Only penalize when at least 2 pieces attack the zone -- a single
        // piece rarely creates a real mating threat on its own
        if (attackerCount >= 2) {
            int penalty = KingAttackPenalty[std::min(attackUnits, 12)];
            if (!attackingQueenPresent) {
                penalty /= 2;
            }
            scores[us] -= S(penalty, penalty / 8);

            // Gate the safe-square penalty on the same multi-attacker
            // condition so that a king boxed in by its own pawns is not
            // scored as if it were under attack
            Bitboard kingMoves = KingAttacks[kingSq] & ~board.byColor[us];
            int safeCount = std::min(popcount(kingMoves & ~enemyAttacks), 8);
            scores[us] += KingSafeSqPenalty[safeCount];
        }
    }
}

int evaluate(const Board &board) {
    ensureEvalInit();

    Score scores[2] = {0, 0};
    int gamePhase = 0;

    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type == None) continue;

        int idx = (p.color == White) ? sq : (sq ^ 56);
        scores[p.color] += PieceScore[p.type] + PST[p.type][idx];
        gamePhase += GamePhaseInc[p.type];
    }

    Score pawnScore = 0;
    evaluatePawns(board, pawnScore);

    EvalContext ctx;
    Bitboard whitePawnsCtx = board.byPiece[Pawn] & board.byColor[White];
    Bitboard blackPawnsCtx = board.byPiece[Pawn] & board.byColor[Black];
    ctx.pawnAttacks[White] = pawnAttacksBB(whitePawnsCtx, White);
    ctx.pawnAttacks[Black] = pawnAttacksBB(blackPawnsCtx, Black);
    ctx.mobilityArea[White] = ~board.byColor[White] & ~ctx.pawnAttacks[Black];
    ctx.mobilityArea[Black] = ~board.byColor[Black] & ~ctx.pawnAttacks[White];

    evaluatePieces(board, ctx, scores);
    evaluateSpace(board, ctx, scores);
    evaluateKingSafety(board, scores);

    Score total = scores[White] - scores[Black] + pawnScore;

    int mg = mg_value(total);
    int eg = eg_value(total);

    int mgPhase = std::min(gamePhase, 24);
    int egPhase = 24 - mgPhase;
    int result = (mg * mgPhase + eg * egPhase) / 24;

    // Scale evaluation toward 0 as the halfmove clock approaches 100 so the
    // engine prefers moves that make progress (captures, pawn pushes) and
    // avoids blundering into 50-move rule draws beyond the search horizon.
    // Skip when no pawns remain: in pawnless endgames like KQ vs K the winning
    // side has no way to reset the clock other than capturing, so the search
    // tree itself must handle the 50-move horizon without penalizing the eval.
    if (board.byPiece[Pawn]) {
        result = result * (200 - board.halfmoveClock) / 200;
    }

    return (board.sideToMove == White) ? result : -result;
}

void clearPawnHash() {
    pawnHashTable.clear();
}

void setPawnHashSize(size_t mb) {
    pawnHashTable.resize(mb);
}
