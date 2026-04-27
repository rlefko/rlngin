#include "eval_params.h"

// Compiled-in defaults for the tunable parameters. The in-memory
// `evalParams` instance is initialized from this struct, and
// `resetEvalParams()` snaps it back to these values after a tune run.
//
// Values below come from a Texel tune covering every scalar in
// `EvalParams` (1067 mg/eg halves) over 6.4M qsearch-leaf positions
// extracted from the PR #36 self-play PGN (64,000 games at nodes=25000)
// with mate-scored plies filtered out at extraction. Initial loss was
// 0.0875643. This snapshot reflects pass 22 of the running tune
// (loss ~0.08581) replayed through the post-plateau-fixes tuner: 60
// scalars projected onto their priors on load (mobility / passer /
// king-attack chains, Initiative caps, slider-on-queen eg, pawn
// shield eg, file ordering near the king). Tuner improvements that
// fed into this run: a non-decreasing step ladder `[8, 4, 2, 1]` per
// scalar (still strictly improvement-only, but crosses plateaus that
// strict `+/-1` settles into); a tuner-leaf qsearch mode that
// disables both delta and SEE pruning so every plausible capture
// exchange resolves; and bound-based projection that fixes chain
// violations the prior sign-derived clamp could not climb. Sign and
// monotonicity priors are enforced via `collectParams` factories:
// every threat term stays >= 0, BishopPair / MinorOnKingRing /
// RookOnKingRing pinned >= 0, `OpenFile >= SemiOpen >= 0` for the
// rook files and the same chain inverted for the king-zone files,
// `KingAttackByQueen >= Rook >= max(B, N)` (same chain for
// `KingSafeCheck`), every passer rank chain is non-decreasing (and
// `PassedBlockedPenalty` is non-increasing), Mobility is
// non-decreasing in count, `KingSafeSqPenalty` keeps its chain on
// both halves, `InitiativePureBase.eg` is capped at 48 to stop it
// acting as a residual sink, and `InitiativeConstant.eg` is held
// strictly negative so the Initiative baseline cannot collapse.

// clang-format off
static const EvalParams kDefaultEvalParams = {
    S(209, 67), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(203, 20), S(152, 29), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(207, 87), S(0, 0)},
    S(194, 32), // ThreatByKing
    S(15, 0), // Hanging
    S(56, 20), // WeakQueen
    S(44, 19), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 40), S(0, 53), S(0, 53), S(0, 53), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 26), S(0, 59), S(0, 92), S(0, 112), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-10, -28), S(-10, -49), S(-10, -90), S(-199, -199), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-12, 29), S(-12, 79), S(92, 162), S(238, 420), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(20, 14), S(72, 14), S(72, 20), S(221, 20), S(0, 0)},
    S(-34, 56), // RookOn7thBonus
    S(-8, -14), // BadBishopPenalty
    S(24, 0), // Tempo
    {S(0, 0), S(122, 227), S(878, 814), S(905, 757), S(1332, 1234), S(2525, 2546), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-28, 39), S(15, 23), S(0, 22), S(10, 2), S(53, 40), S(26, 46), S(59, 36), S(-63, 29),
        S(-39, 23), S(-26, 7), S(-16, -1), S(-4, -8), S(29, 3), S(-17, 17), S(20, -5), S(-36, 2),
        S(-36, 48), S(-52, 29), S(-19, -23), S(49, -43), S(22, -22), S(23, -1), S(-20, 0), S(-2, 9),
        S(0, 80), S(8, 40), S(73, 0), S(54, -1), S(74, -13), S(85, 1), S(-7, 37), S(-26, 64),
        S(26, 146), S(-15, 150), S(71, 116), S(60, 110), S(48, 97), S(42, 147), S(-53, 161), S(6, 164),
        S(177, 393), S(230, 439), S(210, 439), S(231, 421), S(235, 414), S(265, 458), S(50, 479), S(-46, 446),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-232, -58), S(-81, -25), S(-68, -66), S(-79, -73), S(-24, -49), S(-16, -86), S(-85, -32), S(-57, -43),
        S(-157, -45), S(-15, -90), S(-54, -61), S(11, -43), S(-27, -40), S(-48, -38), S(-3, 0), S(-25, -50),
        S(-4, -58), S(20, -70), S(-1, -41), S(-21, -9), S(50, -27), S(21, -29), S(33, -51), S(-49, 6),
        S(61, -74), S(66, -38), S(56, 8), S(62, -4), S(62, -2), S(88, 13), S(52, 23), S(32, -6),
        S(136, -61), S(47, -15), S(81, -11), S(124, -26), S(83, 22), S(192, -31), S(84, -18), S(135, -71),
        S(-46, -68), S(59, -54), S(76, -5), S(120, -1), S(223, -49), S(221, -101), S(134, -60), S(90, -64),
        S(-127, -72), S(-92, -19), S(64, -54), S(61, -1), S(46, -32), S(163, -46), S(49, -57), S(-42, -143),
        S(-466, -171), S(-180, -54), S(-64, -36), S(-97, -25), S(166, -117), S(-267, -66), S(-10, -134), S(-291, -245)
    },
    // BishopPST
    {
        S(-40, -41), S(62, -85), S(-18, 13), S(15, -63), S(-64, 10), S(-81, 1), S(12, -14), S(16, -62),
        S(75, -105), S(60, -45), S(90, -65), S(-3, 6), S(17, -14), S(-2, -2), S(53, -74), S(86, -88),
        S(67, -11), S(71, -32), S(16, 28), S(15, 14), S(18, 44), S(4, -21), S(49, 2), S(39, -32),
        S(70, -40), S(63, -1), S(58, -4), S(82, -7), S(50, -23), S(24, 17), S(5, -8), S(47, -31),
        S(14, 37), S(103, 5), S(80, 7), S(46, -16), S(37, -20), S(88, -20), S(54, 10), S(-9, -9),
        S(34, -11), S(69, 20), S(-9, 9), S(83, -4), S(83, -19), S(148, -22), S(121, -15), S(105, -48),
        S(-39, -19), S(-83, -23), S(6, 26), S(-19, 12), S(-4, 22), S(84, -11), S(-102, -4), S(-70, -38),
        S(-96, 0), S(-25, 19), S(-132, 27), S(-105, 60), S(-108, -10), S(-157, 24), S(46, -36), S(-10, -92)
    },
    // RookPST
    {
        S(-38, -9), S(-24, -13), S(6, -10), S(4, -13), S(19, -30), S(28, -26), S(27, -35), S(-58, -15),
        S(-36, -3), S(-31, -31), S(4, -2), S(-30, -12), S(-31, -12), S(16, -43), S(73, -70), S(-143, 11),
        S(-104, 6), S(-24, -15), S(1, -1), S(-87, 0), S(-34, -17), S(18, -34), S(24, -34), S(-94, 13),
        S(-51, 20), S(-74, 40), S(2, 6), S(-22, 32), S(-34, -2), S(-48, 28), S(36, -8), S(2, -36),
        S(-4, 42), S(-6, 37), S(45, 32), S(-7, 16), S(7, 20), S(46, -9), S(48, 25), S(53, 14),
        S(6, 28), S(33, 19), S(30, 29), S(102, 11), S(115, -1), S(172, -6), S(166, -4), S(64, 7),
        S(24, -9), S(-11, -1), S(90, -8), S(195, -20), S(224, -41), S(204, -32), S(40, -1), S(143, -29),
        S(94, -23), S(106, -2), S(58, 18), S(98, 11), S(174, -10), S(67, 24), S(70, 11), S(94, 12)
    },
    // QueenPST
    {
        S(17, -89), S(-36, -39), S(-35, -28), S(-25, -3), S(32, -49), S(-26, -92), S(-44, -99), S(-170, -115),
        S(-55, -13), S(47, -49), S(-9, 5), S(12, 1), S(5, -21), S(57, -81), S(39, -148), S(-8, -111),
        S(-54, -16), S(10, -31), S(-15, 67), S(-11, -22), S(16, 8), S(25, 42), S(31, 3), S(-7, -22),
        S(21, -22), S(-57, 73), S(-22, 57), S(8, 100), S(13, 56), S(23, 110), S(3, 123), S(-10, 55),
        S(-7, 48), S(-9, 42), S(-20, 94), S(-15, 97), S(11, 125), S(38, 118), S(19, 155), S(33, 72),
        S(-18, -56), S(-5, 10), S(-2, 53), S(-8, 93), S(41, 128), S(121, 108), S(165, 80), S(127, 12),
        S(-46, -6), S(-123, 63), S(-51, 109), S(-38, 109), S(-34, 180), S(107, 60), S(21, 69), S(109, -14),
        S(-82, -9), S(-49, 18), S(78, 14), S(16, 63), S(112, 28), S(60, 15), S(112, -28), S(73, 1)
    },
    // KingPST
    {
        S(-7, -98), S(69, -58), S(39, -41), S(-62, -44), S(-101, -61), S(-134, -41), S(63, -83), S(115, -134),
        S(37, -88), S(14, -39), S(-26, 2), S(-30, -6), S(-9, -11), S(-63, -18), S(59, -38), S(127, -63),
        S(-38, -41), S(55, 6), S(-46, 1), S(-91, 26), S(-155, 23), S(-25, 14), S(60, -14), S(-62, -12),
        S(-89, 9), S(50, -5), S(-107, 48), S(-150, 40), S(-242, 51), S(-155, 41), S(-132, 47), S(-55, -19),
        S(15, 26), S(-22, 90), S(-82, 73), S(-188, 77), S(-183, 60), S(-161, 69), S(-81, 72), S(-94, 31),
        S(-16, 39), S(64, 86), S(22, 102), S(-50, 103), S(-78, 85), S(-38, 49), S(0, 50), S(-28, 45),
        S(47, -78), S(12, 51), S(-49, 71), S(4, 92), S(8, 102), S(-34, 83), S(-63, 95), S(-63, 48),
        S(-174, -225), S(53, -79), S(47, -43), S(-30, 9), S(-92, 96), S(11, 100), S(66, 80), S(-18, -116)
    },
    {
        {},
        {},
        {S(-123, -195), S(-67, -51), S(-31, -9), S(-13, 7), S(13, 22), S(30, 39), S(42, 39), S(42, 39), S(42, 39)},
        {S(3, 31), S(48, 67), S(75, 85), S(86, 115), S(98, 130), S(100, 148), S(111, 151), S(111, 153), S(111, 165), S(111, 165), S(116, 165), S(122, 165), S(164, 165), S(164, 165)},
        {S(12, 130), S(12, 238), S(12, 272), S(12, 278), S(12, 280), S(12, 294), S(12, 305), S(22, 307), S(22, 317), S(22, 344), S(22, 344), S(26, 354), S(26, 358), S(26, 358), S(149, 358)},
        {S(98, -21), S(118, 83), S(119, 162), S(135, 188), S(140, 248), S(140, 305), S(145, 326), S(145, 348), S(153, 355), S(153, 358), S(153, 367), S(165, 369), S(165, 400), S(165, 401), S(175, 411), S(175, 411), S(175, 411), S(175, 411), S(183, 411), S(209, 411), S(224, 411), S(224, 411), S(228, 411), S(228, 411), S(228, 411), S(238, 411), S(238, 452), S(238, 458)},
        {},
    },
    {S(0, 0), S(-36, 52), S(-31, 69), S(-31, 80), S(6, 247), S(50, 453), S(147, 546), S(0, 0)},
    {S(0, 0), S(1, -17), S(30, 9), S(51, 14), S(64, 47), S(192, 88), S(312, 135), S(0, 0)},
    S(152, 18), // RookOpenFileBonus
    S(53, 18), // RookSemiOpenFileBonus
    S(83, 33), // KnightOutpostBonus
    S(103, 33), // BishopOutpostBonus
    S(-24, 0), // TrappedRookByKingPenalty
    S(97, 0), // RookBehindOurPasserBonus
    S(48, 100), // RookBehindTheirPasserBonus
    S(18, 19), // MinorBehindPawnBonus
    S(10, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-22, -2), // KingProtector
    S(0, 47), // BishopPair
    {S(127, 0), S(69, 0)}, // PawnShieldBonus
    {S(0, 0), S(0, 0), S(64, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(28, 0), S(88, 0), S(0, 0), S(1, 0)}, // UnblockedPawnStorm
    S(-24, 0), // SemiOpenFileNearKing
    S(-131, 0), // OpenFileNearKing
    S(-42, 0), // UndefendedKingZoneSq
    {S(-42, -64), S(0, -30), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)}, // KingSafeSqPenalty
    S(25, 5), // KingAttackByKnight
    S(11, 5), // KingAttackByBishop
    S(25, 5), // KingAttackByRook
    S(25, 5), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(25, 0), S(25, 0), S(25, 0), S(25, 0), S(0, 0)}, // KingSafeCheck
    S(15, 0), // KingRingWeakWeight
    S(42, 7), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -45), // DoubledPawnPenalty
    S(0, -12), // BackwardPawnPenalty
    S(-30, -21), // WeakUnopposedPenalty
    S(0, -39), // DoubledIsolatedPenalty
    {S(-58, -33), S(-26, -86)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-14, -31), // PawnIslandPenalty
    {S(4, 0), S(4, 0)}, // CentralPawnBonus
    S(52, 18), // BishopLongDiagonalBonus
    S(0, 19), // InitiativePasser
    S(0, 29), // InitiativePawnCount
    S(0, 5), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 23), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(79, 0), // SliderOnQueenBishop
    S(68, 0), // SliderOnQueenRook
    S(15, 2), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
