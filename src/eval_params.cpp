#include "eval_params.h"

// Compiled-in defaults for the tunable parameters. The in-memory
// `evalParams` instance is initialized from this struct, and
// `resetEvalParams()` snaps it back to these values after a tune run.
//
// Values below come from a Texel tune covering every scalar in
// `EvalParams` (1067 mg/eg halves) over 6.4M qsearch-leaf positions
// extracted from the PR #36 self-play PGN (64,000 games at nodes=25000)
// with mate-scored plies filtered out at extraction. Initial loss was
// 0.0875643; the snapshot here was taken at end of pass 18 with loss
// 0.0858325, already past PR #36's 0.0858592 final and closing on PR
// #37's 0.0856473. Tuner improvements that produced this run, on top
// of the original strict CPW pseudocode: a non-decreasing step ladder
// `[8, 4, 2, 1]` per scalar (still strictly improvement-only, but
// crosses larger plateaus that strict +/-1 settles into), and a
// tuner-leaf qsearch mode that disables both delta and SEE pruning so
// every plausible capture exchange resolves before the static eval is
// fitted. Sign and monotonicity priors are enforced via `collectParams`
// predicates: bonuses stay >= 0, penalties <= 0, BishopPair and
// MinorOnKingRing / RookOnKingRing are pinned non-negative against
// universal chess priors, `PassedEnemyKingProxPenalty.eg` keeps its
// stored magnitude positive, `Mobility[piece][count]` is non-decreasing
// in the count axis, and `KingSafeSqPenalty` keeps its non-decreasing
// chain on both halves.

// clang-format off
static const EvalParams kDefaultEvalParams = {
    S(210, 68), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(204, 23), S(156, 29), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(205, 95), S(0, 0)},
    S(198, 32), // ThreatByKing
    S(15, -4), // Hanging
    S(56, 20), // WeakQueen
    S(44, 19), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 40), S(0, 53), S(0, 55), S(0, 45), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 26), S(0, 59), S(0, 92), S(0, 114), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-60, -28), S(-40, -49), S(-16, -90), S(-191, -199), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(37, 31), S(-12, 83), S(92, 162), S(230, 388), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(20, 21), S(115, 18), S(72, 68), S(197, 34), S(0, 0)},
    S(-30, 56), // RookOn7thBonus
    S(-8, -14), // BadBishopPenalty
    S(24, 0), // Tempo
    {S(0, 0), S(121, 233), S(880, 827), S(905, 774), S(1335, 1262), S(2565, 2586), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-29, 42), S(16, 23), S(2, 21), S(14, 0), S(53, 40), S(28, 47), S(59, 36), S(-63, 31),
        S(-41, 25), S(-27, 9), S(-15, -2), S(-4, -9), S(29, 3), S(-15, 18), S(21, -6), S(-36, 2),
        S(-37, 50), S(-52, 30), S(-18, -25), S(48, -45), S(22, -22), S(23, -1), S(-20, 0), S(-2, 7),
        S(0, 82), S(6, 42), S(71, 1), S(56, -5), S(74, -13), S(85, 1), S(-7, 37), S(-26, 64),
        S(26, 146), S(-15, 152), S(71, 116), S(60, 110), S(48, 95), S(44, 147), S(-53, 165), S(6, 164),
        S(177, 395), S(238, 443), S(202, 431), S(231, 421), S(235, 410), S(265, 458), S(50, 479), S(-46, 446),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-232, -58), S(-77, -21), S(-63, -66), S(-71, -72), S(-16, -49), S(-16, -86), S(-85, -24), S(-49, -55),
        S(-155, -53), S(-31, -91), S(-54, -60), S(11, -39), S(-27, -40), S(-48, -38), S(-3, 0), S(-25, -50),
        S(-4, -58), S(20, -70), S(-1, -41), S(-21, -9), S(50, -27), S(21, -29), S(33, -51), S(-49, 6),
        S(61, -74), S(66, -38), S(56, 8), S(62, -4), S(62, -2), S(88, 13), S(52, 23), S(32, -6),
        S(128, -57), S(47, -13), S(81, -11), S(124, -26), S(83, 22), S(192, -31), S(84, -18), S(135, -67),
        S(-46, -68), S(59, -54), S(76, -5), S(120, -1), S(223, -49), S(221, -101), S(134, -60), S(90, -62),
        S(-127, -72), S(-92, -19), S(64, -54), S(61, -1), S(46, -32), S(163, -46), S(49, -57), S(-42, -143),
        S(-466, -173), S(-180, -54), S(-64, -36), S(-97, -25), S(166, -115), S(-267, -66), S(-10, -134), S(-291, -245)
    },
    // BishopPST
    {
        S(-40, -41), S(62, -85), S(-18, 13), S(15, -63), S(-64, 10), S(-81, 1), S(12, -14), S(16, -62),
        S(75, -105), S(60, -45), S(90, -65), S(-3, 6), S(17, -14), S(-2, -2), S(53, -73), S(86, -88),
        S(67, -11), S(71, -32), S(16, 29), S(15, 14), S(18, 44), S(4, -21), S(49, 2), S(39, -32),
        S(70, -40), S(63, -1), S(58, -4), S(82, -7), S(50, -23), S(24, 17), S(9, -8), S(47, -31),
        S(14, 37), S(103, 5), S(80, 7), S(46, -16), S(37, -20), S(88, -20), S(54, 10), S(-9, -9),
        S(34, -11), S(69, 20), S(-1, 9), S(83, -4), S(83, -19), S(148, -22), S(121, -15), S(105, -48),
        S(-39, -19), S(-83, -23), S(6, 26), S(-19, 12), S(-4, 22), S(84, -11), S(-94, -4), S(-70, -38),
        S(-94, 0), S(-17, 19), S(-133, 27), S(-105, 60), S(-108, -10), S(-149, 32), S(46, -36), S(-14, -92)
    },
    // RookPST
    {
        S(-38, -9), S(-24, -13), S(7, -10), S(4, -13), S(20, -30), S(28, -26), S(27, -35), S(-58, -15),
        S(-36, -1), S(-32, -31), S(4, -2), S(-30, -11), S(-31, -12), S(16, -47), S(65, -62), S(-151, 11),
        S(-104, 6), S(-24, -15), S(1, -1), S(-87, 0), S(-34, -17), S(14, -34), S(24, -34), S(-94, 13),
        S(-51, 20), S(-74, 40), S(2, 6), S(-22, 32), S(-34, -2), S(-48, 28), S(36, -8), S(2, -36),
        S(-4, 42), S(-6, 37), S(45, 32), S(-7, 16), S(7, 20), S(54, -9), S(44, 25), S(53, 14),
        S(6, 28), S(33, 19), S(30, 29), S(102, 11), S(115, -1), S(172, -6), S(166, -4), S(64, 7),
        S(24, -9), S(-11, -1), S(90, -8), S(195, -20), S(224, -41), S(204, -32), S(40, -1), S(143, -29),
        S(90, -23), S(106, -2), S(58, 18), S(98, 11), S(174, -10), S(67, 24), S(70, 11), S(94, 12)
    },
    // QueenPST
    {
        S(17, -89), S(-36, -39), S(-35, -28), S(-21, -11), S(31, -45), S(-26, -84), S(-44, -99), S(-170, -115),
        S(-55, -13), S(47, -49), S(-5, -3), S(12, 1), S(5, -22), S(49, -89), S(39, -140), S(-8, -111),
        S(-54, -20), S(10, -31), S(-15, 67), S(-11, -22), S(16, 8), S(25, 42), S(31, 4), S(-7, -22),
        S(21, -26), S(-57, 73), S(-22, 57), S(8, 100), S(13, 56), S(23, 110), S(3, 123), S(-10, 55),
        S(-7, 48), S(-9, 42), S(-20, 94), S(-15, 97), S(11, 125), S(38, 118), S(19, 155), S(33, 80),
        S(-14, -60), S(-13, 18), S(-6, 51), S(-12, 101), S(49, 120), S(129, 107), S(157, 88), S(125, 20),
        S(-38, -14), S(-123, 61), S(-43, 105), S(-30, 117), S(-42, 172), S(107, 60), S(29, 67), S(107, -6),
        S(-78, -17), S(-41, 22), S(62, 22), S(20, 63), S(120, 36), S(68, 23), S(120, -20), S(71, 5)
    },
    // KingPST
    {
        S(1, -102), S(71, -58), S(39, -40), S(-66, -40), S(-101, -61), S(-138, -37), S(63, -83), S(113, -138),
        S(29, -90), S(10, -39), S(-25, 2), S(-30, -6), S(-9, -11), S(-64, -18), S(59, -38), S(127, -63),
        S(-40, -41), S(55, 6), S(-46, 1), S(-95, 26), S(-155, 23), S(-25, 14), S(60, -14), S(-62, -12),
        S(-89, 9), S(50, -5), S(-107, 48), S(-150, 40), S(-234, 47), S(-155, 41), S(-132, 47), S(-55, -19),
        S(11, 26), S(-22, 90), S(-82, 73), S(-180, 79), S(-175, 52), S(-153, 68), S(-81, 72), S(-96, 30),
        S(-16, 39), S(64, 86), S(22, 102), S(-50, 103), S(-78, 83), S(-38, 49), S(8, 50), S(-28, 45),
        S(47, -78), S(12, 51), S(-49, 71), S(4, 92), S(8, 102), S(-34, 83), S(-63, 95), S(-63, 48),
        S(-174, -225), S(53, -79), S(49, -43), S(-30, 9), S(-92, 88), S(3, 92), S(58, 72), S(-10, -108)
    },
    {
        {},
        {},
        {S(-131, -195), S(-67, -51), S(-31, -9), S(-13, 7), S(13, 22), S(30, 39), S(42, 39), S(42, 38), S(23, 38)},
        {S(1, 27), S(48, 67), S(75, 83), S(86, 113), S(98, 128), S(100, 148), S(110, 151), S(115, 153), S(95, 163), S(96, 168), S(116, 150), S(122, 136), S(164, 138), S(164, 138)},
        {S(12, 90), S(12, 233), S(-3, 267), S(5, 273), S(5, 278), S(12, 294), S(12, 301), S(22, 303), S(22, 315), S(-1, 342), S(22, 342), S(26, 350), S(26, 358), S(26, 366), S(141, 309)},
        {S(90, -29), S(119, 79), S(119, 154), S(135, 172), S(140, 232), S(140, 297), S(145, 314), S(145, 336), S(152, 347), S(156, 348), S(149, 367), S(165, 367), S(176, 396), S(152, 401), S(175, 410), S(175, 411), S(168, 411), S(164, 417), S(183, 407), S(201, 407), S(228, 407), S(228, 410), S(228, 411), S(228, 412), S(230, 416), S(234, 418), S(236, 470), S(238, 474)},
        {},
    },
    {S(0, 0), S(-38, 52), S(-9, 68), S(-29, 80), S(10, 246), S(58, 455), S(139, 554), S(0, 0)},
    {S(0, 0), S(1, -17), S(30, 9), S(51, 14), S(64, 47), S(200, 88), S(296, 131), S(0, 0)},
    S(152, -18), // RookOpenFileBonus
    S(53, 18), // RookSemiOpenFileBonus
    S(83, 33), // KnightOutpostBonus
    S(103, 33), // BishopOutpostBonus
    S(-24, 0), // TrappedRookByKingPenalty
    S(97, -6), // RookBehindOurPasserBonus
    S(48, 100), // RookBehindTheirPasserBonus
    S(18, 19), // MinorBehindPawnBonus
    S(10, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-22, -2), // KingProtector
    S(0, 47), // BishopPair
    {S(127, -20), S(68, -7)}, // PawnShieldBonus
    {S(0, 0), S(0, 0), S(64, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(29, 0), S(80, 0), S(0, 0), S(1, 0)}, // UnblockedPawnStorm
    S(-24, 0), // SemiOpenFileNearKing
    S(-130, 0), // OpenFileNearKing
    S(-42, 0), // UndefendedKingZoneSq
    {S(-42, -64), S(0, -31), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)}, // KingSafeSqPenalty
    S(26, 9), // KingAttackByKnight
    S(11, 5), // KingAttackByBishop
    S(27, 5), // KingAttackByRook
    S(14, 2), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(30, 9), S(25, 0), S(24, 0), S(0, 0)}, // KingSafeCheck
    S(15, 0), // KingRingWeakWeight
    S(43, 7), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -47), // DoubledPawnPenalty
    S(0, -12), // BackwardPawnPenalty
    S(-30, -21), // WeakUnopposedPenalty
    S(0, -40), // DoubledIsolatedPenalty
    {S(-58, -33), S(-26, -86)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-15, -31), // PawnIslandPenalty
    {S(4, 0), S(5, 0)}, // CentralPawnBonus
    S(52, 18), // BishopLongDiagonalBonus
    S(0, 18), // InitiativePasser
    S(0, 28), // InitiativePawnCount
    S(0, 5), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 23), // InitiativeInfiltrate
    S(0, 199), // InitiativePureBase
    S(0, 0), // InitiativeConstant
    S(79, -55), // SliderOnQueenBishop
    S(68, -26), // SliderOnQueenRook
    S(15, 2), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
