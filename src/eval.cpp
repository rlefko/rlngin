#include "eval.h"

#include "bitboard.h"
#include "material_hash.h"
#include "pawn_hash.h"
#include "zobrist.h"

#include <algorithm>
#include <iomanip>
#include <ostream>

// Material values used by SEE and move ordering (MG values, king kept large for SEE)
const int PieceValue[] = {0, 198, 817, 836, 1270, 2521, 20000};

// Packed middlegame and endgame material values (king = 0, always present on
// both sides). Encoded with S(mg, eg) so eval accumulates both halves with a
// single add per piece.
static const Score PieceScore[7] = {
    S(0, 0),       // None
    S(198, 258),   // Pawn
    S(817, 846),   // Knight
    S(836, 857),   // Bishop
    S(1270, 1278), // Rook
    S(2521, 2558), // Queen
    S(0, 0),       // King (material is implicit; all games have exactly one)
};

// Game phase increments per piece type (max total = 24)
static const int GamePhaseInc[] = {0, 0, 1, 1, 2, 4, 0};

// clang-format off

// PeSTO piece-square tables stored in a1=0 order (rank 1 first, rank 8 last),
// with midgame and endgame values packed into a single Score via S(mg, eg).
// Values are from White's perspective; Black mirrors vertically via sq ^ 56.

static const Score PawnPST[64] = {
    S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0),
    S( -85,   36), S(  -2,   22), S( -48,   22), S( -56,   27), S( -36,   36), S(  58,    0), S(  92,    5), S( -53,  -19),
    S( -63,   11), S( -10,   19), S( -10,  -16), S( -24,    3), S(   7,    0), S(   7,  -14), S(  80,   -3), S( -29,  -22),
    S( -65,   36), S(  -5,   25), S( -12,   -8), S(  29,  -19), S(  41,  -19), S(  14,  -22), S(  24,    8), S( -60,   -3),
    S( -34,   88), S(  31,   66), S(  14,   36), S(  51,   14), S(  56,   -5), S(  29,   11), S(  41,   47), S( -56,   47),
    S( -14,  258), S(  17,  274), S(  63,  233), S(  75,  184), S( 157,  154), S( 135,  145), S(  60,  225), S( -48,  231),
    S( 237,  489), S( 324,  475), S( 147,  434), S( 229,  368), S( 164,  403), S( 304,  362), S(  82,  453), S( -27,  513),
    S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0),
};

static const Score KnightPST[64] = {
    S(-254,  -80), S( -51, -140), S(-140,  -63), S( -80,  -41), S( -41,  -60), S( -68,  -49), S( -46, -137), S( -56, -176),
    S( -70, -115), S(-128,  -55), S( -29,  -27), S(  -7,  -14), S(  -2,   -5), S(  43,  -55), S( -34,  -63), S( -46, -121),
    S( -56,  -63), S( -22,   -8), S(  29,   -3), S(  24,   41), S(  46,   27), S(  41,   -8), S(  60,  -55), S( -39,  -60),
    S( -31,  -49), S(  10,  -16), S(  39,   44), S(  31,   69), S(  68,   44), S(  46,   47), S(  51,   11), S( -19,  -49),
    S( -22,  -47), S(  41,    8), S(  46,   60), S( 128,   60), S(  89,   60), S( 167,   30), S(  43,   22), S(  53,  -49),
    S(-113,  -66), S( 145,  -55), S(  89,   27), S( 157,   25), S( 203,   -3), S( 311,  -25), S( 176,  -52), S( 106, -113),
    S(-176,  -69), S( -99,  -22), S( 174,  -69), S(  87,   -5), S(  56,  -25), S( 150,  -69), S(  17,  -66), S( -41, -143),
    S(-403, -159), S(-215, -104), S( -82,  -36), S(-118,  -77), S( 147,  -85), S(-234,  -74), S( -36, -173), S(-258, -272),
};

static const Score BishopPST[64] = {
    S( -80,  -63), S(  -7,  -25), S( -34,  -63), S( -51,  -14), S( -31,  -25), S( -29,  -44), S( -94,  -14), S( -51,  -47),
    S(  10,  -38), S(  36,  -49), S(  39,  -19), S(   0,   -3), S(  17,   11), S(  51,  -25), S(  80,  -41), S(   2,  -74),
    S(   0,  -33), S(  36,   -8), S(  36,   22), S(  36,   27), S(  34,   36), S(  65,    8), S(  43,  -19), S(  24,  -41),
    S( -14,  -16), S(  31,    8), S(  31,   36), S(  63,   52), S(  82,   19), S(  29,   27), S(  24,   -8), S(  10,  -25),
    S( -10,   -8), S(  12,   25), S(  46,   33), S( 121,   25), S(  89,   38), S(  89,   27), S(  17,    8), S(  -5,    5),
    S( -39,    5), S(  89,  -22), S( 104,    0), S(  97,   -3), S(  85,   -5), S( 121,   16), S(  89,    0), S(  -5,   11),
    S( -63,  -22), S(  39,  -11), S( -43,   19), S( -31,  -33), S(  72,   -8), S( 142,  -36), S(  43,  -11), S(-113,  -38),
    S( -70,  -38), S(  10,  -58), S(-198,  -30), S( -89,  -22), S( -60,  -19), S(-101,  -25), S(  17,  -47), S( -19,  -66),
};

static const Score RookPST[64] = {
    S( -46,  -25), S( -31,    5), S(   2,    8), S(  41,   -3), S(  39,  -14), S(  17,  -36), S( -89,   11), S( -63,  -55),
    S(-106,  -16), S( -39,  -16), S( -48,    0), S( -22,    5), S(  -2,  -25), S(  27,  -25), S( -14,  -30), S(-171,   -8),
    S(-109,  -11), S( -60,    0), S( -39,  -14), S( -41,   -3), S(   7,  -19), S(   0,  -33), S( -12,  -22), S( -80,  -44),
    S( -87,    8), S( -63,   14), S( -29,   22), S(  -2,   11), S(  22,  -14), S( -17,  -16), S(  14,  -22), S( -56,  -30),
    S( -58,   11), S( -27,    8), S(  17,   36), S(  63,    3), S(  58,    5), S(  85,    3), S( -19,   -3), S( -48,    5),
    S( -12,   19), S(  46,   19), S(  63,   19), S(  87,   14), S(  41,   11), S( 109,   -8), S( 147,  -14), S(  39,   -8),
    S(  65,   30), S(  77,   36), S( 140,   36), S( 150,   30), S( 193,   -8), S( 162,    8), S(  63,   22), S( 106,    8),
    S(  77,   36), S( 101,   27), S(  77,   49), S( 123,   41), S( 152,   33), S(  22,   33), S(  75,   22), S( 104,   14),
};

static const Score QueenPST[64] = {
    S(  -2,  -91), S( -43,  -77), S( -22,  -60), S(  24, -118), S( -36,  -14), S( -60,  -88), S( -75,  -55), S(-121, -113),
    S( -85,  -60), S( -19,  -63), S(  27,  -82), S(   5,  -44), S(  19,  -44), S(  36,  -63), S(  -7,  -99), S(   2,  -88),
    S( -34,  -44), S(   5,  -74), S( -27,   41), S(  -5,   16), S( -12,   25), S(   5,   47), S(  34,   27), S(  12,   14),
    S( -22,  -49), S( -63,   77), S( -22,   52), S( -24,  129), S(  -5,   85), S( -10,   93), S(   7,  107), S(  -7,   63),
    S( -65,    8), S( -65,   60), S( -39,   66), S( -39,  124), S(  -2,  156), S(  41,  110), S(  -5,  156), S(   2,   99),
    S( -31,  -55), S( -41,   16), S(  17,   25), S(  19,  134), S(  70,  129), S( 135,   96), S( 113,   52), S( 138,   25),
    S( -58,  -47), S( -94,   55), S( -12,   88), S(   2,  113), S( -39,  159), S( 138,   69), S(  68,   82), S( 130,    0),
    S( -68,  -25), S(   0,   60), S(  70,   60), S(  29,   74), S( 142,   74), S( 106,   52), S( 104,   27), S( 109,   55),
};

static const Score KingPST[64] = {
    S( -36, -145), S(  87,  -93), S(  29,  -58), S(-130,  -30), S(  19,  -77), S( -68,  -38), S(  58,  -66), S(  34, -118),
    S(   2,  -74), S(  17,  -30), S( -19,   11), S(-155,   36), S(-104,   38), S( -39,   11), S(  22,  -14), S(  19,  -47),
    S( -34,  -52), S( -34,   -8), S( -53,   30), S(-111,   58), S(-106,   63), S( -72,   44), S( -36,   19), S( -65,  -25),
    S(-118,  -49), S(  -2,  -11), S( -65,   58), S( -94,   66), S(-111,   74), S(-106,   63), S( -80,   25), S(-123,  -30),
    S( -41,  -22), S( -48,   60), S( -29,   66), S( -65,   74), S( -72,   71), S( -60,   91), S( -34,   71), S( -87,    8),
    S( -22,   27), S(  58,   47), S(   5,   63), S( -39,   41), S( -48,   55), S(  14,  124), S(  53,  121), S( -53,   36),
    S(  70,  -33), S(  -2,   47), S( -48,   38), S( -17,   47), S( -19,   47), S( -10,  104), S( -92,   63), S( -70,   30),
    S(-157, -203), S(  56,  -96), S(  39,  -49), S( -36,  -49), S(-135,  -30), S( -82,   41), S(   5,   11), S(  31,  -47),
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
    S(  0,   0),  // rank 1
    S( 12,  27),  // rank 2
    S( 24,  47),  // rank 3
    S( 36,  88),  // rank 4
    S( 72, 170),  // rank 5
    S(133, 294),  // rank 6
    S(217, 467),  // rank 7
    S(  0,   0),  // rank 8
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
        S(-150, -222), S(-128, -154), S( -29,  -82), S( -10,  -38), S(   7,   22),
        S(  31,   41), S(  53,   63), S(  68,   74), S(  80,   91),
    },
    {    // Bishop (0..13)
        S(-116, -162), S( -48,  -63), S(  39,   -8), S(  63,   36), S(  92,   66),
        S( 123,  115), S( 133,  148), S( 152,  156), S( 152,  178), S( 164,  200),
        S( 196,  214), S( 196,  236), S( 220,  242), S( 237,  266),
    },
    {    // Rook (0..14)
        S(-140, -209), S( -65,  -49), S( -36,   77), S( -24,  151), S( -12,  189),
        S(  -5,  225), S(  22,  307), S(  39,  324), S(  72,  362), S(  70,  390),
        S(  77,  425), S(  92,  453), S( 111,  456), S( 116,  464), S( 140,  469),
    },
    {    // Queen (0..27)
        S( -94,  -99), S( -51,  -41), S(   7,   22), S(   7,   49), S(  34,   93),
        S(  53,  148), S(  68,  167), S(  99,  200), S( 104,  217), S( 116,  253),
        S( 135,  258), S( 145,  285), S( 145,  310), S( 159,  329), S( 162,  338),
        S( 169,  346), S( 171,  365), S( 176,  373), S( 191,  384), S( 212,  392),
        S( 212,  406), S( 239,  456), S( 246,  467), S( 246,  480), S( 256,  505),
        S( 263,  524), S( 273,  565), S( 280,  582),
    },
    {},  // King
};

// Rook bonuses for open and semi-open files. An "open file" has no pawns of
// either color; a "semi-open file" has no friendly pawns but at least one
// enemy pawn. Both bonuses are larger in the middlegame where file control
// translates into direct king pressure.
static const Score RookOpenFileBonus     = S(109, 55);
static const Score RookSemiOpenFileBonus = S( 48, 19);

// Minor-piece outpost bonuses. A knight or bishop is on an outpost when it
// sits on a relative rank 4-6 square that is defended by a friendly pawn
// and can no longer be challenged by an enemy pawn push. Knights benefit
// more than bishops because bishops see through the square anyway.
static const Score KnightOutpostBonus = S(72, 55);
static const Score BishopOutpostBonus = S(43, 22);

// Penalty for a rook shut in on the same side of the board as its own king
// when the rook has little room to breathe. Pure middlegame concern; in the
// endgame the king activates and the rook typically walks free. Doubled
// when all castling rights are gone, since O-O / O-O-O cannot relocate the
// rook to an active square.
static const Score TrappedRookByKingPenalty = S(-126, 0);

// Space evaluation: count safe central squares on our own side of the board.
// Stockfish weights the result quadratically by non-pawn piece count so the
// term fades in the endgame where space is no longer a trump. Gated on a
// minimum piece count to keep the bonus from firing in thin positions where
// it does not translate into a real plan.
static const int SpaceWeightDivisor = 16;
static const int SpaceMinPieceCount = 2;

// Connected pawn bonus by rank index
static const Score ConnectedPawnBonus[8] = {
    S(  0,   0),  // rank 1
    S( 17,   0),  // rank 2
    S( 19,   8),  // rank 3
    S( 29,  19),  // rank 4
    S( 60,  47),  // rank 5
    S(109,  82),  // rank 6
    S(169, 115),  // rank 7
    S(  0,   0),  // rank 8
};

// --- King safety constants ---

// Pawn shield bonus per shield-file pawn by relative rank.
// Index 0 = pawn on 2nd rank (unmoved, strongest), index 1 = pawn on 3rd rank.
// Values are conservative so the shield signal cannot dominate material,
// PST, or pawn structure in normal middlegame positions.
static const Score PawnShieldBonus[2] = {
    S(48, 8),  // 2nd rank
    S(29, 5),  // 3rd rank
};

// Pawn storm penalty indexed by rank distance from our king.
// Index 0 = 4+ ranks away, index 4 = on the same rank (blocked)
static const Score PawnStormPenalty[5] = {
    S( 0, 0),  // 4+ ranks away
    S(24, 0),  // 3 ranks away
    S(60, 0),  // 2 ranks away
    S(97, 0),  // 1 rank away
    S(24, 0),  // same rank (blocked, less dangerous)
};

// Per-file penalty when our pawns or all pawns are missing near the king.
// A shield pawn's absence is the same signal as the file being semi-open
// or open for us, so we express it only once here rather than stacking a
// separate "missing shield" penalty on top.
static const Score SemiOpenFileNearKing = S(-24, 0);
static const Score OpenFileNearKing     = S(-36, 0);

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
    0,   0,    0,   29,  77,  145, 232,
    338, 464,  608, 773, 956, 1159,
};

// Penalty per king-zone square attacked by enemy but not defended by
// any friendly piece.
static const Score UndefendedKingZoneSq = S(-17, -3);

// Penalty by number of safe squares the king can move to (0 = most
// dangerous). Index is the count of safe squares, capped at 8.
static const Score KingSafeSqPenalty[9] = {
    S(-121, -14),  // 0 safe squares
    S( -85,  -8),  // 1
    S( -48,  -3),  // 2
    S( -24,   0),  // 3
    S( -10,   0),  // 4
    S(   0,   0),  // 5
    S(   0,   0),  // 6
    S(   0,   0),  // 7
    S(   0,   0),  // 8
};

// clang-format on

static const Score IsolatedPawnPenalty = S(-36, -55);
static const Score DoubledPawnPenalty = S(-24, -55);
static const Score BackwardPawnPenalty = S(-24, -41);

// Two bishops together control complementary diagonals that no other piece
// combination can cover, so the pair is worth noticeably more than the sum
// of its parts. Slightly larger in the endgame where open diagonals matter
// most.
static const Score BishopPair = S(75, 120);

// clang-format off

// Stockfish-lineage quadratic imbalance tables. Indexed by [pt1][pt2] with
// pt in { BishopPair=0, Pawn=1, Knight=2, Bishop=3, Rook=4, Queen=5 }. The
// "ours" table captures synergy between own-side pieces (e.g. knight value
// rising with own pawn count); the "theirs" table captures pressure from
// enemy pieces. Only the lower triangle is used; the upper triangle stays
// at zero so the inner loop can cap at pt2 <= pt1.
static const int QuadraticOurs[6][6] = {
    // BPair,  P,    N,    B,    R,    Q
    { 1438,    0,    0,    0,    0,    0 }, // BishopPair
    {   40,   38,    0,    0,    0,    0 }, // Pawn
    {   32,  255,  -62,    0,    0,    0 }, // Knight
    {    0,  104,    4,    0,    0,    0 }, // Bishop
    {  -26,   -2,   47,  105, -208,    0 }, // Rook
    { -189,   24,  117,  133, -134,   -6 }, // Queen
};

static const int QuadraticTheirs[6][6] = {
    // BPair,  P,    N,    B,    R,    Q
    {    0,    0,    0,    0,    0,    0 }, // BishopPair
    {   36,    0,    0,    0,    0,    0 }, // Pawn
    {    9,   63,    0,    0,    0,    0 }, // Knight
    {   59,   65,   42,    0,    0,    0 }, // Bishop
    {   46,   39,   24,  -24,    0,    0 }, // Rook
    {   97,  100,  -42,  137,  268,    0 }, // Queen
};

// clang-format on

// Evaluate the quadratic imbalance for side `us`. `pc[c][0]` carries a
// synthetic bishop-pair count (0 or 1); `pc[c][1..5]` mirror the piece
// counts for pawns through queens in the same order as the tables above.
// The /16 divisor matches Stockfish's convention.
static int imbalance(const int pc[2][6], int us) {
    int them = us ^ 1;
    int bonus = 0;
    for (int pt1 = 0; pt1 < 6; pt1++) {
        if (!pc[us][pt1]) continue;
        int v = 0;
        for (int pt2 = 0; pt2 <= pt1; pt2++) {
            v += QuadraticOurs[pt1][pt2] * pc[us][pt2];
            v += QuadraticTheirs[pt1][pt2] * pc[them][pt2];
        }
        bonus += pc[us][pt1] * v;
    }
    return bonus / 16;
}

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
static MaterialHashTable materialHashTable(1);

// Compute the pure material contribution for this position: piece values
// (MG/EG), bishop pair bonus, quadratic imbalance, and game phase. The
// result depends only on piece counts, so it can be cached by the material
// zobrist key. PSTs are not included here because they depend on squares.
static void evaluateMaterial(const Board &board, Score outScores[2], int &outPhase) {
    int probeMg = 0, probeEg = 0, probePhase = 0;
    if (materialHashTable.probe(board.materialKey, probeMg, probeEg, probePhase)) {
        // Material hash stores a single white-minus-black packed score and
        // the game phase; assign it to White's slot and leave Black at 0.
        outScores[White] = S(probeMg, probeEg);
        outScores[Black] = 0;
        outPhase = probePhase;
        return;
    }

    int pc[2][6];
    int phase = 0;
    Score scores[2] = {0, 0};
    for (int c = 0; c < 2; c++) {
        for (int pt = 1; pt < 7; pt++) {
            int cnt = board.pieceCount[c][pt];
            scores[c] += PieceScore[pt] * cnt;
            phase += GamePhaseInc[pt] * cnt;
        }
        pc[c][0] = (board.pieceCount[c][Bishop] >= 2) ? 1 : 0;
        pc[c][1] = board.pieceCount[c][Pawn];
        pc[c][2] = board.pieceCount[c][Knight];
        pc[c][3] = board.pieceCount[c][Bishop];
        pc[c][4] = board.pieceCount[c][Rook];
        pc[c][5] = board.pieceCount[c][Queen];
        if (pc[c][0]) scores[c] += BishopPair;
    }
    int imbW = imbalance(pc, White);
    int imbB = imbalance(pc, Black);
    scores[White] += S(imbW, imbW);
    scores[Black] += S(imbB, imbB);

    Score delta = scores[White] - scores[Black];
    materialHashTable.store(board.materialKey, mg_value(delta), eg_value(delta), phase);
    outScores[White] = delta;
    outScores[Black] = 0;
    outPhase = phase;
}

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

// Shared evaluation body used by both the hot path (``evaluate``) and the
// tracing path (``evaluateTraced``). ``if constexpr (Trace)`` drops every
// capture branch out of the untraced instantiation, so the non-trace build
// compiles to the same sequence it did before this refactor.
template <bool Trace> static int evaluateImpl(const Board &board, EvalTrace *trace) {
    ensureEvalInit();

    Score scores[2] = {0, 0};
    int gamePhase = 0;

    // PST-only accumulation per square; piece material, bishop pair, and
    // imbalance come from the cached material probe below.
    Score pstScores[2] = {0, 0};
    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type == None) continue;

        int idx = (p.color == White) ? sq : (sq ^ 56);
        pstScores[p.color] += PST[p.type][idx];
    }
    scores[White] += pstScores[White];
    scores[Black] += pstScores[Black];

    Score matScores[2];
    evaluateMaterial(board, matScores, gamePhase);
    scores[White] += matScores[White];
    scores[Black] += matScores[Black];

    // The material cache stores white-minus-black in a single slot, which is
    // ambiguous for a per-side trace table. Recompute the true per-side split
    // only under tracing so the hot path keeps its one-probe cost.
    Score matPerSide[2] = {0, 0};
    if constexpr (Trace) {
        int pc[2][6];
        for (int c = 0; c < 2; c++) {
            for (int pt = 1; pt < 7; pt++) {
                matPerSide[c] += PieceScore[pt] * board.pieceCount[c][pt];
            }
            pc[c][0] = (board.pieceCount[c][Bishop] >= 2) ? 1 : 0;
            pc[c][1] = board.pieceCount[c][Pawn];
            pc[c][2] = board.pieceCount[c][Knight];
            pc[c][3] = board.pieceCount[c][Bishop];
            pc[c][4] = board.pieceCount[c][Rook];
            pc[c][5] = board.pieceCount[c][Queen];
            if (pc[c][0]) matPerSide[c] += BishopPair;
        }
        int imbW = imbalance(pc, White);
        int imbB = imbalance(pc, Black);
        matPerSide[White] += S(imbW, imbW);
        matPerSide[Black] += S(imbB, imbB);
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

    Score piecesBefore[2];
    if constexpr (Trace) {
        piecesBefore[White] = scores[White];
        piecesBefore[Black] = scores[Black];
    }
    evaluatePieces(board, ctx, scores);
    Score piecesDelta[2];
    if constexpr (Trace) {
        piecesDelta[White] = scores[White] - piecesBefore[White];
        piecesDelta[Black] = scores[Black] - piecesBefore[Black];
    }

    Score spaceBefore[2];
    if constexpr (Trace) {
        spaceBefore[White] = scores[White];
        spaceBefore[Black] = scores[Black];
    }
    evaluateSpace(board, ctx, scores);
    Score spaceDelta[2];
    if constexpr (Trace) {
        spaceDelta[White] = scores[White] - spaceBefore[White];
        spaceDelta[Black] = scores[Black] - spaceBefore[Black];
    }

    Score ksBefore[2];
    if constexpr (Trace) {
        ksBefore[White] = scores[White];
        ksBefore[Black] = scores[Black];
    }
    evaluateKingSafety(board, scores);
    Score ksDelta[2];
    if constexpr (Trace) {
        ksDelta[White] = scores[White] - ksBefore[White];
        ksDelta[Black] = scores[Black] - ksBefore[Black];
    }

    Score total = scores[White] - scores[Black] + pawnScore;

    int mg = mg_value(total);
    int eg = eg_value(total);

    int mgPhase = std::min(gamePhase, 24);
    int egPhase = 24 - mgPhase;
    int result = (mg * mgPhase + eg * egPhase) / 24;
    int rawTapered = result;

    // Scale evaluation toward 0 as the halfmove clock approaches 100 so the
    // engine prefers moves that make progress (captures, pawn pushes) and
    // avoids blundering into 50-move rule draws beyond the search horizon.
    // Skip when no pawns remain: in pawnless endgames like KQ vs K the winning
    // side has no way to reset the clock other than capturing, so the search
    // tree itself must handle the 50-move horizon without penalizing the eval.
    if (board.byPiece[Pawn]) {
        result = result * (200 - board.halfmoveClock) / 200;
    }

    int final_ = (board.sideToMove == White) ? result : -result;

    if constexpr (Trace) {
        trace->material[White] = matPerSide[White];
        trace->material[Black] = matPerSide[Black];
        trace->pst[White] = pstScores[White];
        trace->pst[Black] = pstScores[Black];
        trace->pawns = pawnScore;
        trace->pieces[White] = piecesDelta[White];
        trace->pieces[Black] = piecesDelta[Black];
        trace->space[White] = spaceDelta[White];
        trace->space[Black] = spaceDelta[Black];
        trace->kingSafety[White] = ksDelta[White];
        trace->kingSafety[Black] = ksDelta[Black];
        trace->gamePhase = gamePhase;
        trace->halfmoveClock = board.halfmoveClock;
        trace->sideToMove = board.sideToMove;
        trace->rawTapered = rawTapered;
        trace->finalFromWhite = result;
        trace->finalFromStm = final_;
    }

    return final_;
}

int evaluate(const Board &board) {
    return evaluateImpl<false>(board, nullptr);
}

int evaluateTraced(const Board &board, EvalTrace &trace) {
    return evaluateImpl<true>(board, &trace);
}

namespace {

char pieceGlyph(Piece p) {
    if (p.type == None) return '.';
    const char symbols[] = " PNBRQK";
    char c = symbols[p.type];
    if (p.color == Black) c = static_cast<char>(c - 'A' + 'a');
    return c;
}

void printBoardAscii(std::ostream &os, const Board &board) {
    static const char *line = " +---+---+---+---+---+---+---+---+\n";
    for (int r = 7; r >= 0; r--) {
        os << line;
        os << " |";
        for (int f = 0; f < 8; f++) {
            Piece p = board.squares[r * 8 + f];
            os << " " << pieceGlyph(p) << " |";
        }
        os << " " << (r + 1) << "\n";
    }
    os << line;
    os << "   a   b   c   d   e   f   g   h\n";
}

void printScorePair(std::ostream &os, Score s) {
    os << std::setw(5) << mg_value(s) << " " << std::setw(5) << eg_value(s);
}

void printTermRow(std::ostream &os, const char *label, const Score white, const Score black) {
    Score total = white - black;
    os << " " << std::left << std::setw(14) << label << std::right << "| ";
    printScorePair(os, white);
    os << " | ";
    printScorePair(os, black);
    os << " | ";
    printScorePair(os, total);
    os << "\n";
}

void printTotalOnlyRow(std::ostream &os, const char *label, Score total) {
    os << " " << std::left << std::setw(14) << label << std::right << "|      ---     ---"
       << " |      ---     ---"
       << " | ";
    printScorePair(os, total);
    os << "\n";
}

} // namespace

void printEvalTrace(std::ostream &os, const Board &board) {
    EvalTrace trace;
    int stmEval = evaluateTraced(board, trace);

    printBoardAscii(os, board);
    os << "\n";
    os << " Side to move: " << (board.sideToMove == White ? "White" : "Black") << "\n\n";

    os << " Term          |    White     |    Black     |    Total\n";
    os << "               |   MG     EG  |   MG     EG  |   MG     EG\n";
    os << " --------------+--------------+--------------+--------------\n";
    printTermRow(os, "Material", trace.material[White], trace.material[Black]);
    printTermRow(os, "PST", trace.pst[White], trace.pst[Black]);
    printTotalOnlyRow(os, "Pawns", trace.pawns);
    printTermRow(os, "Pieces", trace.pieces[White], trace.pieces[Black]);
    printTermRow(os, "Space", trace.space[White], trace.space[Black]);
    printTermRow(os, "King safety", trace.kingSafety[White], trace.kingSafety[Black]);
    os << " --------------+--------------+--------------+--------------\n";

    // Total row: the packed pre-taper combined score (white minus black plus
    // pawns) in MG/EG form. Reassembling from the terms keeps the table
    // self-consistent; printing the pre-taper pair makes the phase weighting
    // immediately below easy to read.
    Score combined = (trace.material[White] + trace.pst[White] + trace.pieces[White] +
                      trace.space[White] + trace.kingSafety[White]) -
                     (trace.material[Black] + trace.pst[Black] + trace.pieces[Black] +
                      trace.space[Black] + trace.kingSafety[Black]) +
                     trace.pawns;
    os << " " << std::left << std::setw(14) << "Total" << std::right
       << "|              |              | ";
    printScorePair(os, combined);
    os << "\n\n";

    os << " Phase:         " << trace.gamePhase << " / 24\n";
    os << " 50-move clock: " << trace.halfmoveClock << " / 100\n";
    os << " Tapered:       " << trace.rawTapered << " internal ("
       << (trace.rawTapered * 100 / NormalizePawn) << " cp)\n";
    os << " After 50-move: " << trace.finalFromWhite << " internal ("
       << (trace.finalFromWhite * 100 / NormalizePawn) << " cp) from White\n";
    os << " Final:         " << stmEval << " internal (" << (stmEval * 100 / NormalizePawn)
       << " cp) from side to move\n";
}

void clearPawnHash() {
    pawnHashTable.clear();
}

void setPawnHashSize(size_t mb) {
    pawnHashTable.resize(mb);
}

void clearMaterialHash() {
    materialHashTable.clear();
}

void setMaterialHashSize(size_t mb) {
    materialHashTable.resize(mb);
}
