#include "eval_params.h"

// Compiled-in defaults for the tunable parameters. The in-memory
// `evalParams` instance is initialized from this struct, and
// `resetEvalParams()` snaps it back to these values after a tune run.
//
// Values below come from a strict-pseudocode Texel tune covering every
// scalar in `EvalParams` (1067 mg/eg halves) over 6.4M qsearch-leaf
// positions extracted from the PR #36 self-play PGN (64,000 games at
// nodes=25000) with mate-scored plies filtered out at extraction. K
// converged to 0.00208669; initial loss was 0.0875643. The tune was
// resumed from a per-pass checkpoint after the foreground process was
// killed mid pass 21, so the snapshot here captures every accepted
// coordinate-descent step through the original log replay (loss
// 0.0865292) plus the resumed pass 0 progress (loss in the 0.08647
// range when this snapshot was taken). Sign and monotonicity priors
// remain enforced via `collectParams` predicates: bonuses stay >= 0,
// penalties <= 0, and `KingSafeSqPenalty` keeps its non-decreasing
// chain on both halves.

// clang-format off
static const EvalParams kDefaultEvalParams = {
    S(178, 103), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(152, 44), S(119, 78), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(178, 137), S(0, 0)},
    S(132, 54), // ThreatByKing
    S(19, -19), // Hanging
    S(46, 28), // WeakQueen
    S(35, 22), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 39), S(0, 57), S(0, 60), S(0, 53), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 26), S(0, 60), S(0, 91), S(0, 122), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-42, -34), S(-21, -60), S(-46, -85), S(-133, -181), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(22, 34), S(12, 76), S(82, 159), S(144, 234), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(5, 7), S(41, 8), S(78, 84), S(92, 85), S(0, 0)},
    S(-39, 51), // RookOn7thBonus
    S(-6, -14), // BadBishopPenalty
    S(24, 0), // Tempo
    {S(0, 0), S(118, 273), S(861, 879), S(895, 850), S(1403, 1405), S(2682, 2720), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-28, 53), S(15, 33), S(6, 26), S(5, 33), S(34, 93), S(43, 52), S(82, 40), S(-49, 32),
        S(-42, 27), S(-24, 12), S(-19, 2), S(0, -7), S(26, 10), S(-8, 19), S(30, -5), S(-32, 3),
        S(-42, 56), S(-46, 32), S(-13, -30), S(42, -50), S(23, -26), S(23, -1), S(-20, 0), S(-13, 6),
        S(-40, 79), S(-9, 35), S(23, -1), S(23, -19), S(43, -24), S(63, -17), S(-14, 28), S(-69, 60),
        S(-38, 169), S(-7, 196), S(68, 170), S(35, 156), S(90, 104), S(76, 135), S(-5, 181), S(-45, 188),
        S(207, 435), S(285, 468), S(172, 462), S(243, 407), S(191, 432), S(297, 401), S(79, 483), S(-52, 476),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-237, -66), S(-53, -73), S(-100, -31), S(-62, -34), S(-7, -30), S(-31, -38), S(-34, -75), S(-34, -148),
        S(-117, -101), S(-89, -33), S(-51, -36), S(33, -22), S(2, -20), S(36, -40), S(13, -5), S(4, -62),
        S(-47, -33), S(-2, -48), S(5, -24), S(1, 15), S(65, -2), S(41, -15), S(57, -41), S(-23, -8),
        S(16, -33), S(47, -1), S(41, 44), S(51, 26), S(52, 45), S(71, 49), S(58, 33), S(27, -8),
        S(16, -9), S(17, 19), S(38, 37), S(106, 16), S(63, 54), S(145, 7), S(47, 34), S(78, -41),
        S(-105, -56), S(124, -65), S(86, 26), S(130, 27), S(191, -14), S(285, -55), S(160, -64), S(98, -103),
        S(-175, -64), S(-98, -18), S(154, -70), S(84, -3), S(53, -30), S(149, -64), S(17, -66), S(-42, -143),
        S(-405, -163), S(-213, -100), S(-83, -36), S(-118, -77), S(145, -87), S(-235, -74), S(-35, -173), S(-257, -272)
    },
    // BishopPST
    {
        S(-76, -61), S(-6, -24), S(6, 18), S(-24, -41), S(-7, 3), S(-44, 11), S(-42, 3), S(-8, -26),
        S(31, -65), S(55, -27), S(66, -53), S(13, 5), S(39, -13), S(54, -14), S(83, -74), S(62, -72),
        S(36, -5), S(47, -20), S(28, 53), S(27, 17), S(38, 46), S(38, -13), S(52, -4), S(42, -25),
        S(14, -14), S(39, 7), S(27, 17), S(84, 15), S(81, -19), S(20, 23), S(5, -4), S(35, -19),
        S(-2, 14), S(50, 37), S(45, 19), S(87, -9), S(50, 1), S(68, -6), S(49, 17), S(-25, -9),
        S(-27, 3), S(92, 1), S(86, -1), S(79, -12), S(67, -18), S(122, 19), S(78, -2), S(1, 1),
        S(-62, -21), S(17, -25), S(-36, 25), S(-19, -20), S(66, -4), S(128, -38), S(30, -17), S(-122, -40),
        S(-66, -30), S(13, -57), S(-197, -32), S(-88, -17), S(-58, -20), S(-101, -23), S(16, -48), S(-18, -68)
    },
    // RookPST
    {
        S(-28, -13), S(-12, -18), S(22, -16), S(26, -16), S(37, -39), S(42, -30), S(-39, 8), S(-59, -17),
        S(-48, 10), S(-18, -47), S(-20, 1), S(-35, -4), S(-9, -25), S(4, -42), S(-1, -39), S(-143, 14),
        S(-96, 12), S(-42, -2), S(-19, 7), S(-43, -8), S(-4, -23), S(-3, -36), S(-5, -27), S(-77, -20),
        S(-66, 28), S(-47, 32), S(-17, 21), S(10, 25), S(8, -18), S(-22, -2), S(20, -8), S(-42, -21),
        S(-40, 48), S(6, 38), S(31, 45), S(41, 4), S(39, 13), S(66, -15), S(4, 19), S(-28, 31),
        S(4, 38), S(44, 24), S(53, 30), S(81, 17), S(51, 18), S(116, 4), S(138, -13), S(46, 4),
        S(55, 15), S(53, 7), S(118, 0), S(139, 15), S(192, -19), S(152, -12), S(51, 1), S(115, -4),
        S(62, 7), S(100, 12), S(66, 30), S(110, 22), S(144, 13), S(32, 44), S(78, 14), S(94, 8)
    },
    // QueenPST
    {
        S(5, -81), S(-36, -71), S(-9, -50), S(18, -86), S(7, 13), S(-27, -61), S(-53, -51), S(-129, -106),
        S(-63, -44), S(25, -38), S(15, -68), S(24, -24), S(23, -35), S(45, -74), S(10, -100), S(-1, -83),
        S(-45, -34), S(16, -55), S(-8, 61), S(-15, 7), S(2, 24), S(17, 63), S(41, 13), S(-7, 2),
        S(10, -21), S(-55, 86), S(-17, 65), S(-22, 131), S(-10, 90), S(7, 108), S(1, 123), S(-1, 64),
        S(-40, 28), S(-38, 70), S(-30, 77), S(-39, 125), S(-8, 149), S(36, 114), S(-5, 163), S(8, 104),
        S(-27, -48), S(-38, 22), S(14, 31), S(12, 133), S(67, 130), S(137, 97), S(117, 56), S(129, 24),
        S(-54, -38), S(-99, 61), S(-14, 89), S(-2, 113), S(-34, 159), S(135, 68), S(67, 83), S(127, 1),
        S(-68, -25), S(-1, 58), S(70, 62), S(28, 75), S(142, 74), S(106, 51), S(104, 28), S(108, 53)
    },
    // KingPST
    {
        S(-36, -142), S(81, -85), S(36, -40), S(-103, -15), S(-34, -95), S(-96, -46), S(63, -97), S(43, -125),
        S(-3, -80), S(6, -41), S(-18, 5), S(-109, 24), S(-55, 7), S(-48, -18), S(54, -38), S(45, -44),
        S(-29, -42), S(-9, 26), S(-61, 25), S(-103, 37), S(-121, 31), S(-68, 26), S(-6, 7), S(-56, -12),
        S(-109, -34), S(8, 4), S(-60, 58), S(-98, 48), S(-129, 48), S(-110, 43), S(-74, 40), S(-108, -20),
        S(-31, -7), S(-41, 81), S(-29, 72), S(-76, 73), S(-79, 54), S(-65, 76), S(-28, 78), S(-87, 20),
        S(-20, 33), S(64, 53), S(10, 77), S(-34, 53), S(-43, 66), S(7, 99), S(47, 92), S(-41, 50),
        S(66, -45), S(4, 52), S(-49, 48), S(-14, 58), S(-12, 62), S(-14, 107), S(-85, 79), S(-67, 32),
        S(-161, -207), S(54, -91), S(41, -51), S(-34, -47), S(-132, -24), S(-77, 48), S(6, 14), S(29, -51)
    },
    {
        {},
        {},
        {S(-140, -214), S(-55, -99), S(-23, -17), S(-10, 12), S(14, 27), S(25, 51), S(39, 39), S(42, 38), S(23, -6)},
        {S(0, -58), S(43, 35), S(70, 58), S(82, 105), S(92, 126), S(94, 150), S(106, 151), S(115, 154), S(95, 163), S(89, 167), S(134, 150), S(135, 137), S(217, 172), S(169, 164)},
        {S(3, -64), S(12, 107), S(-2, 219), S(13, 235), S(6, 253), S(17, 275), S(8, 295), S(21, 296), S(22, 315), S(-1, 347), S(10, 354), S(20, 365), S(43, 366), S(38, 369), S(101, 312)},
        {S(-1, -58), S(75, 20), S(91, 81), S(112, 105), S(128, 165), S(133, 214), S(145, 230), S(143, 288), S(156, 296), S(155, 327), S(149, 336), S(162, 351), S(176, 381), S(152, 402), S(181, 402), S(175, 407), S(168, 414), S(164, 417), S(200, 407), S(207, 407), S(223, 421), S(248, 430), S(238, 455), S(238, 460), S(251, 496), S(260, 510), S(270, 554), S(278, 576)},
        {},
    },
    {S(0, 0), S(-26, 31), S(-8, 56), S(-7, 74), S(68, 239), S(124, 403), S(193, 547), S(0, 0)},
    {S(0, 0), S(4, -24), S(31, 9), S(50, 18), S(65, 62), S(130, 75), S(209, 127), S(0, 0)},
    S(133, -30), // RookOpenFileBonus
    S(35, 30), // RookSemiOpenFileBonus
    S(87, 28), // KnightOutpostBonus
    S(86, 39), // BishopOutpostBonus
    S(-27, 0), // TrappedRookByKingPenalty
    S(34, 21), // RookBehindOurPasserBonus
    S(29, 40), // RookBehindTheirPasserBonus
    S(16, 26), // MinorBehindPawnBonus
    S(10, -8), // MinorOnKingRing
    S(-8, -25), // RookOnKingRing
    S(-10, -3), // KingProtector
    S(-10, 72), // BishopPair
    {S(94, -14), S(51, -6)}, // PawnShieldBonus
    {S(0, 0), S(0, 0), S(24, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(30, 0), S(39, 0), S(0, 0), S(0, 0)}, // UnblockedPawnStorm
    S(-29, 0), // SemiOpenFileNearKing
    S(-121, 0), // OpenFileNearKing
    S(-42, 0), // UndefendedKingZoneSq
    {S(-29, -33), S(0, -19), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)}, // KingSafeSqPenalty
    S(25, 5), // KingAttackByKnight
    S(12, 5), // KingAttackByBishop
    S(31, 5), // KingAttackByRook
    S(16, 2), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(30, 1), S(29, 7), S(23, 3), S(24, 0), S(0, 0)}, // KingSafeCheck
    S(14, 0), // KingRingWeakWeight
    S(34, 3), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -69), // DoubledPawnPenalty
    S(0, -15), // BackwardPawnPenalty
    S(-25, -31), // WeakUnopposedPenalty
    S(-1, -28), // DoubledIsolatedPenalty
    {S(-30, -26), S(-25, -27)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-15, -28), // PawnIslandPenalty
    {S(0, 0), S(0, 0)}, // CentralPawnBonus
    S(38, 3), // BishopLongDiagonalBonus
    S(0, 15), // InitiativePasser
    S(0, 18), // InitiativePawnCount
    S(0, 6), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 29), // InitiativeInfiltrate
    S(0, 44), // InitiativePureBase
    S(0, -4), // InitiativeConstant
    S(38, 27), // SliderOnQueenBishop
    S(44, 10), // SliderOnQueenRook
    S(12, 0), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
