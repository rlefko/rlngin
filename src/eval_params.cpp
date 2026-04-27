#include "eval_params.h"

// Compiled-in defaults for the tunable parameters. The in-memory
// `evalParams` instance is initialized from this struct, and
// `resetEvalParams()` snaps it back to these values after a tune run.
//
// Values below come from a constrained strict-pseudocode Texel tune on
// 3.55M qsearch-leaf positions (64,000 self-play games, nodes=25000).
// Every "penalty"-named scalar is held to its expected sign by the
// tuner (sign predicate per field), and `KingSafeSqPenalty` obeys a
// non-decreasing chain on both halves so a king with more escape
// squares is never scored worse than one with fewer. Clamping the
// starting values into the feasible region snapped 19 scalars; strict
// `while improved` coordinate descent then ran to natural convergence
// in 37 passes. Loss 0.0858592 -> 0.0856473, a better minimum than the
// unconstrained 64k-game tune while also agreeing with chess priors.

// clang-format off
static const EvalParams kDefaultEvalParams = {
    S(177, 126), // ThreatByPawn  (Texel-tuned; SPSA's proposal was reverted)
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(131, 57), S(131, 88), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(168, 138), S(0, 0)},
    S(124, 62), // ThreatByKing
    S(29, -5),  // Hanging       (Texel-tuned; SPSA's proposal was reverted)
    S(53, 29),  // WeakQueen
    S(53, 29),  // SafePawnPush  (Texel-tuned; SPSA's proposal was reverted)
    {S(0, 0), S(0, 0), S(0, 0), S(0, 45), S(0, 67), S(0, 68), S(0, 63), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 32), S(0, 62), S(0, 93), S(0, 123), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-33, -40), S(-4, -57), S(-40, -77), S(-114, -160), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(24, 46), S(20, 77), S(73, 141), S(127, 210), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-8, -4), S(28, 0), S(78, 87), S(84, 88), S(0, 0)},
    S(-7, -16), // BadBishopPenalty
    S(24, 0),   // Tempo         (Texel-tuned; SPSA's proposal was reverted)

    // PieceScore
    {S(0, 0), S(133, 298), S(854, 887), S(884, 869), S(1428, 1428), S(2656, 2695), S(0, 0)},

    // PawnPST
    {
        S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0),
        S( -39,   61), S(   6,   52), S(   5,   21), S( -20,   16), S(  36,  117), S(  51,   61), S(  95,   45), S( -33,   26),
        S( -47,   20), S( -22,   16), S( -25,    3), S(   1,  -10), S(  31,   23), S(  14,   17), S(  44,    1), S( -31,    1),
        S( -40,   61), S( -42,   43), S(  -6,  -27), S(  47,  -47), S(  35,  -37), S(  26,    5), S( -17,   -5), S( -18,    4),
        S( -50,   68), S( -10,   42), S(  17,    6), S(  24,  -15), S(  29,  -37), S(  60,  -33), S( -11,   14), S( -84,   50),
        S( -51,  182), S(   4,  216), S(  85,  195), S(  53,  180), S(  96,  101), S(  88,  136), S(   9,  199), S( -63,  207),
        S( 223,  452), S( 296,  475), S( 170,  465), S( 240,  395), S( 189,  433), S( 296,  389), S(  83,  485), S( -51,  484),
        S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0), S(   0,    0),
    },

    // KnightPST
    {
        S(-237,  -68), S( -59,  -97), S(-112,  -42), S( -59,  -27), S( -15,  -35), S( -46,  -34), S( -21,  -94), S( -34, -155),
        S(-107, -105), S( -97,  -30), S( -45,  -31), S(  46,  -18), S(  18,   -3), S(  54,  -38), S(   4,  -21), S(  -4,  -75),
        S( -71,  -53), S( -19,  -37), S(  -9,  -19), S(   8,   20), S(  67,   13), S(  53,   -5), S(  75,  -37), S( -25,  -20),
        S(   9,  -29), S(  52,    7), S(  29,   42), S(  46,   31), S(  49,   58), S(  65,   48), S(  57,   28), S(  16,  -19),
        S(   6,  -16), S(  13,   21), S(  33,   40), S( 111,   28), S(  51,   53), S( 139,    2), S(  40,   40), S(  70,  -40),
        S(-109,  -58), S( 130,  -65), S(  89,   28), S( 137,   27), S( 192,  -11), S( 290,  -48), S( 164,  -66), S(  96, -106),
        S(-177,  -65), S(-100,  -18), S( 160,  -70), S(  83,   -1), S(  54,  -29), S( 149,  -65), S(  17,  -66), S( -43, -143),
        S(-404, -162), S(-214, -102), S( -82,  -36), S(-118,  -78), S( 146,  -86), S(-235,  -75), S( -35, -173), S(-257, -272),
    },

    // BishopPST
    {
        S( -77,  -61), S(  -6,  -23), S(   2,   -2), S( -41,  -52), S(   0,   -4), S( -22,   10), S( -56,   -1), S( -15,  -30),
        S(  29,  -60), S(  66,  -17), S(  49,  -43), S(  25,    6), S(  59,    6), S(  70,  -17), S(  96,  -68), S(  48,  -73),
        S(  22,  -19), S(  30,  -22), S(  33,   59), S(  20,   19), S(  41,   51), S(  41,   -8), S(  39,  -20), S(  40,  -25),
        S(   4,   -3), S(  32,    6), S(   9,   16), S(  80,   25), S(  81,  -16), S(   8,   31), S(   2,   -1), S(  27,  -20),
        S(  -6,    3), S(  41,   47), S(  37,   21), S(  92,   -3), S(  57,    7), S(  65,   -2), S(  41,   12), S( -32,  -12),
        S( -30,    0), S(  90,   -3), S(  92,   -3), S(  83,  -12), S(  69,  -18), S( 122,   21), S(  79,   -5), S(  -8,   -1),
        S( -63,  -18), S(  25,  -18), S( -41,   22), S( -23,  -26), S(  66,   -7), S( 133,  -36), S(  36,  -15), S(-121,  -38),
        S( -65,  -32), S(  13,  -58), S(-196,  -33), S( -88,  -19), S( -60,  -20), S(-100,  -24), S(  19,  -48), S( -18,  -67),
    },

    // RookPST
    {
        S( -13,    2), S( -14,  -11), S(  25,   -9), S(  24,  -10), S(  38,  -34), S(  29,  -30), S( -55,   24), S( -37,   -6),
        S( -63,   19), S(  -8,  -46), S( -34,   -3), S( -39,    3), S(  -8,  -30), S(  -1,  -38), S(  -7,  -39), S(-145,   13),
        S(-100,   13), S( -42,    2), S( -21,    3), S( -42,   -8), S(   0,  -19), S(  -8,  -40), S( -10,  -28), S( -81,  -29),
        S( -72,   28), S( -48,   30), S( -19,   26), S(  13,   25), S(  11,  -20), S( -24,  -10), S(  14,  -12), S( -45,  -21),
        S( -46,   40), S(   1,   36), S(  36,   51), S(  48,    6), S(  42,    9), S(  66,  -13), S(  -1,   11), S( -38,   26),
        S(   5,   40), S(  43,   27), S(  58,   32), S(  79,   16), S(  47,   12), S( 112,    2), S( 137,  -15), S(  45,    3),
        S(  55,   18), S(  58,   14), S( 119,    3), S( 134,   12), S( 191,  -21), S( 150,  -12), S(  52,    1), S( 116,   -2),
        S(  62,    9), S( 100,   13), S(  65,   32), S( 111,   20), S( 143,   14), S(  32,   45), S(  78,   15), S(  95,    7),
    },

    // QueenPST
    {
        S(   5,  -82), S( -36,  -70), S(  -9,  -50), S(  43,  -94), S( -12,    4), S( -42,  -70), S( -61,  -50), S(-125, -112),
        S( -71,  -50), S(  12,  -46), S(  25,  -80), S(  22,  -28), S(  39,  -30), S(  36,  -72), S(   0,  -96), S(  -3,  -85),
        S( -39,  -37), S(  -1,  -68), S( -11,   54), S( -14,   13), S(  -8,   18), S(  -2,   50), S(  40,   22), S(  -2,    7),
        S(  -1,  -33), S( -60,   84), S( -19,   61), S( -31,  132), S( -15,   88), S(  -3,   98), S(  -5,  115), S(   0,   66),
        S( -51,   20), S( -46,   65), S( -36,   72), S( -44,  123), S(  -7,  148), S(  36,  114), S(  -9,  160), S(   4,  101),
        S( -30,  -50), S( -41,   19), S(  15,   29), S(  16,  133), S(  70,  131), S( 137,   97), S( 113,   52), S( 135,   26),
        S( -58,  -43), S( -97,   58), S( -10,   88), S(  -1,  114), S( -37,  160), S( 138,   69), S(  66,   83), S( 129,    1),
        S( -68,  -25), S(   0,   58), S(  71,   62), S(  28,   75), S( 142,   75), S( 106,   52), S( 104,   28), S( 109,   55),
    },

    // KingPST
    {
        S( -36, -143), S(  84,  -93), S(  22,  -48), S(-114,  -14), S(  -8,  -87), S(-100,  -69), S(  46, -111), S(  20, -143),
        S(   7,  -63), S(  10,  -50), S( -21,    8), S(-118,   40), S( -69,   16), S( -46,   -9), S(  45,  -32), S(  37,  -35),
        S( -32,  -40), S( -12,   25), S( -54,   42), S(-106,   41), S(-112,   44), S( -78,   32), S( -19,    6), S( -58,  -20),
        S(-110,  -43), S(   4,    1), S( -58,   59), S( -96,   55), S(-123,   60), S(-108,   52), S( -78,   36), S(-114,  -24),
        S( -36,  -12), S( -44,   72), S( -27,   70), S( -70,   71), S( -74,   58), S( -61,   82), S( -31,   75), S( -85,   19),
        S( -22,   31), S(  62,   52), S(  10,   72), S( -33,   48), S( -44,   62), S(  11,  112), S(  50,  103), S( -45,   47),
        S(  68,  -40), S(   1,   49), S( -49,   42), S( -14,   55), S( -18,   57), S( -12,  110), S( -87,   74), S( -67,   32),
        S(-159, -205), S(  53,  -92), S(  40,  -52), S( -35,  -46), S(-134,  -26), S( -78,   46), S(   7,   14), S(  30,  -51),
    },

    // MobilityBonus
    {
        {},
        {},
        {S(-142, -217), S( -65, -115), S( -25,  -37), S(   1,    0), S(  13,   27), S(  27,   68), S(  43,   51), S(  48,   52), S(  36,   17)},
        {S( -21,  -79), S(  45,   17), S(  59,   37), S(  75,  108), S(  88,  119), S(  88,  156), S(  93,  151), S( 109,  154), S( 105,  156), S( 103,  179), S( 151,  152), S( 143,  159), S( 215,  186), S( 188,  187)},
        {S( -21,  -89), S(  29,   81), S(  13,  200), S(  18,  235), S(  15,  258), S(  26,  288), S(   7,  318), S(  32,  315), S(  36,  339), S(   6,  370), S(  18,  376), S(  34,  391), S(  61,  388), S(  49,  393), S(  84,  336)},
        {S( -18,  -60), S(  55,   13), S( 101,   76), S( 111,  100), S( 121,  158), S( 124,  201), S( 139,  219), S( 138,  275), S( 150,  284), S( 142,  317), S( 135,  321), S( 152,  341), S( 166,  368), S( 146,  393), S( 181,  402), S( 175,  401), S( 171,  411), S( 167,  412), S( 205,  408), S( 205,  407), S( 224,  425), S( 249,  434), S( 239,  457), S( 241,  465), S( 253,  498), S( 261,  511), S( 270,  558), S( 280,  579)},
        {},
    },

    // PassedPawnBonus / ConnectedPawnBonus
    {S(0, 0), S(-22, 40), S(-9, 64), S(-1, 80), S(89, 228), S(139, 387), S(211, 556), S(0, 0)},
    {S(0, 0), S(7, -28), S(40, 7), S(47, 24), S(66, 65), S(117, 80), S(200, 130), S(0, 0)},

    S(114, -49), // RookOpenFileBonus
    S( 15,  37), // RookSemiOpenFileBonus
    S( 81,  30), // KnightOutpostBonus
    S( 78,  45), // BishopOutpostBonus
    S( -7,   0), // TrappedRookByKingPenalty
    S( 10,  20), // RookBehindOurPasserBonus
    S(  5,  15), // RookBehindTheirPasserBonus
    S( 10,   5), // MinorBehindPawnBonus
    S(  3,   1), // MinorOnKingRing
    S(  5,   0), // RookOnKingRing
    S( -3,  -1), // KingProtector
    S(  8,  74), // BishopPair

    {S(68, -12), S(52, -13)},                        // PawnShieldBonus
    {S(0, 0), S( 3, 0), S( 5, 0), S(1, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(12, 0), S(19, 0), S(5, 0), S(0, 0)}, // UnblockedPawnStorm

    S(-20, 0), // SemiOpenFileNearKing
    S(-95, 0), // OpenFileNearKing
    S(-57, 0), // UndefendedKingZoneSq

    // KingSafeSqPenalty: non-decreasing chain, all <= 0
    {S(-25, -11), S(0, -1), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},

    // King-danger per-attacker weights. Starting magnitudes are
    // deliberately conservative: the eval-level peak mg penalty under
    // the quadratic cap sits at roughly KingDangerMgCap^2 / 32 = 1800
    // internal units, a hair above the old attack-unit curve's max of
    // 1159 so the feature can still bite on sharp positions without
    // overpowering material in typical middlegames. SPSA will move
    // them from here.
    S(14, 2),  // KingAttackByKnight
    S(10, 1),  // KingAttackByBishop
    S(22, 3),  // KingAttackByRook
    S(32, 4),  // KingAttackByQueen

    // KingSafeCheck[pt]: 0 for None/Pawn/King slots so the inner loop can
    // index by piece type directly.
    {S(0, 0), S(0, 0), S(36, 3), S(24, 2), S(28, 3), S(44, 5), S(0, 0)},

    S(6, 0),  // KingRingWeakWeight
    S(36, 0), // KingNoQueenDiscount

    S(-15, -24), // IsolatedPawnPenalty
    S(  0, -83), // DoubledPawnPenalty
    S(-13, -36), // BackwardPawnPenalty
    S( -5, -10), // WeakUnopposedPenalty
    {S(-5, -2), S(-2, -1)}, // BlockedPawnPenalty (rel rank 5, 6)
    // S(  3,   2), // PhalanxBonus (disabled, see eval_params.h)

    // BishopLongDiagonalBonus: MG-heavy because the long diagonal matters
    // most when heavy pieces are still on the board and can be traded onto
    // it; a small endgame trickle keeps fianchetto bishops relevant.
    S(30, 5),

    // Initiative weights: EG-only, small single-digit magnitudes. The
    // constant is negative so a perfectly symmetric quiet position
    // produces a near-zero magnitude before sign selection.
    S(0,  9),  // InitiativePasser
    S(0,  3),  // InitiativePawnCount
    S(0,  8),  // InitiativeOutflank
    S(0,  4),  // InitiativeTension
    S(0, 12),  // InitiativeInfiltrate
    S(0, 18),  // InitiativePureBase
    S(0, -30), // InitiativeConstant

    S(15,  8), // SliderOnQueenBishop
    S(20, 10), // SliderOnQueenRook

    S( 2,  1), // RestrictedPiece
};
// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
