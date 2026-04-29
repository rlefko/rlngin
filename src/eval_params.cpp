#include "eval_params.h"

// Compiled-in defaults for the tunable parameters. The in-memory
// `evalParams` instance is initialized from this struct, and
// `resetEvalParams()` snaps it back to these values after a tune run.
//
// Values below come from a Texel tune covering every scalar in
// `EvalParams` (1065 mg/eg halves) over 5.47M unique qsearch-leaf
// positions extracted from a fresh 64,000-game self-play PGN at
// nodes=100000 with mate-scored plies filtered out and identical
// positions folded into a single weighted-average row at extraction
// time. Loss after twenty full passes was ~0.1074; this snapshot
// was taken mid-run, then loaded through the canonicalization
// pipeline: project violators onto current bounds, then center each
// piece's PST around zero by mean and push the mean into PieceScore
// (eval is bit-identical to the un-centered checkpoint, but the
// per-term values stop drifting in the PST/material gauge null
// direction).
//
// Tuner improvements that produced this state:
//   - Bounds-based ParamRef with iterative-clamp projection that
//     repairs chain violations the original sign-derived clamp could
//     not (knight mobility ending in S(42, 23) is the canonical
//     example).
//   - Step ladder `[8, 4, 2, 1]` per scalar, still strictly
//     improvement-only but crosses plateaus that strict `+/-1` would
//     settle into.
//   - Tuner-leaf qsearch mode that disables both delta and SEE
//     pruning so every plausible capture exchange resolves before the
//     static eval is fitted, plus a walk that keeps stepping past
//     in-check positions instead of stopping with a noisy label.
//   - Thread-local pawn / material hashes (no race on shared writes
//     across worker threads under the threaded loss).
//   - Threaded passes at relThreshold 1e-7, deterministic
//     single-thread finalizer at 1e-8 to recover sub-noise wins.
//   - Periodic K refit (every 4 passes) and periodic leaf refresh
//     (every 8 passes) against the evolving params.
//   - PST mean centering after every accepted pass to keep the
//     PST/material gauge from wandering.
//
// Constraint set (enforced via `collectParams` bounds factories,
// validated on every load / project step):
//   - Threat terms (`ThreatBy*`, `Hanging`, `WeakQueen`,
//     `SafePawnPush`, slider-on-queen, restricted piece) >= 0 each
//     half.
//   - Pawn-structure penalties (isolated, doubled, backward, weak
//     unopposed, doubled-isolated, blocked, pawn-island,
//     bad-bishop) <= 0 each half.
//   - BishopPair / MinorOnKingRing / RookOnKingRing >= 0 against
//     universal chess priors.
//   - `RookOpenFile >= RookSemiOpenFile >= 0` per phase, and
//     `OpenFileNearKing <= SemiOpenFileNearKing <= 0` mirrored for
//     the king-zone files.
//   - King-attack and king-safe-check piece-weight chains:
//     `Queen >= Rook >= max(Bishop, Knight)` per half.
//   - `KingSafeSqPenalty` non-decreasing chain on both halves.
//   - Mobility non-decreasing chain in count, per piece type and
//     half.
//   - Passed pawn rank chains: `PassedPawnBonus`, `ConnectedPawnBonus`,
//     `ConnectedPassersBonus`, `PassedSupportedBonus` non-decreasing
//     in rank; `PassedBlockedPenalty` non-increasing (advanced block
//     hurts more); `PassedKingProxBonus` and the
//     `PassedEnemyKingProxPenalty` magnitude both non-decreasing in
//     rank, both held >= 0.
//   - Floor constraints layered on the rank chains:
//     `PassedSupportedBonus` >= 0 every rank; `PassedPawnBonus[r].mg
//     >= 0` for r >= 4; `ConnectedPawnBonus[r].eg >= 0` for r >= 2.
//   - Initiative system: `InitiativePureBase.eg` capped at [0, 48]
//     to stop it acting as a residual sink in pure pawn endings;
//     `InitiativeConstant.eg <= -1` so the negative baseline cannot
//     collapse; the other Initiative scalars >= 0.

// clang-format off
static const EvalParams kDefaultEvalParams = {
    S(229, 36), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(213, 13), S(200, 10), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(246, 0), S(0, 0)},
    S(209, 25), // ThreatByKing
    S(0, 0), // Hanging
    S(56, 20), // WeakQueen
    S(59, 18), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 32), S(0, 46), S(0, 48), S(0, 48), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 23), S(0, 53), S(0, 84), S(0, 95), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-24, -47), S(-24, -52), S(-82, -66), S(-259, -151), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(33, 23), S(33, 69), S(229, 130), S(230, 428), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(76, 25), S(88, 28), S(128, 28), S(129, 28), S(0, 0)},
    S(0, 35), // RookOn7thBonus
    S(-11, -11), // BadBishopPenalty
    S(36, 0), // Tempo
    {S(0, 0), S(257, 274), S(995, 697), S(1041, 644), S(1596, 999), S(2512, 2376), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-34, -66), S(-55, -68), S(-52, -68), S(-43, -79), S(-48, -46), S(-13, -56), S(23, -68), S(-115, -82),
        S(-75, -78), S(-119, -76), S(-81, -84), S(-89, -82), S(-23, -82), S(-87, -75), S(-39, -90), S(-82, -87),
        S(-66, -57), S(-85, -61), S(-91, -93), S(-32, -106), S(-48, -90), S(-32, -95), S(-87, -81), S(-70, -79),
        S(-3, -32), S(11, -49), S(25, -88), S(18, -92), S(38, -81), S(22, -102), S(-46, -49), S(-72, -34),
        S(85, 58), S(67, 55), S(116, 16), S(130, 5), S(101, 8), S(145, 43), S(10, 82), S(-3, 75),
        S(280, 195), S(-19, 269), S(153, 250), S(174, 285), S(218, 184), S(264, 234), S(-175, 315), S(-71, 260),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-481, 4), S(-67, -16), S(-109, -10), S(-8, -27), S(-31, -8), S(-80, -26), S(-102, -13), S(-66, -13),
        S(-126, -59), S(40, -18), S(-67, -7), S(14, -14), S(-20, -3), S(15, -36), S(-85, 37), S(-65, -18),
        S(3, -19), S(27, -17), S(26, -11), S(20, 8), S(68, 12), S(46, -20), S(41, -15), S(-56, 18),
        S(22, 16), S(49, 18), S(94, 16), S(122, 20), S(76, 33), S(89, 34), S(81, 13), S(-4, 23),
        S(39, 18), S(95, 15), S(124, 25), S(166, 27), S(90, 27), S(239, -11), S(89, 20), S(96, -11),
        S(-175, 10), S(32, 16), S(135, 0), S(118, 11), S(182, 19), S(211, -17), S(197, 4), S(-31, 6),
        S(-72, -4), S(-77, 23), S(63, 16), S(172, -21), S(-99, 30), S(306, -48), S(120, -44), S(13, -17),
        S(-499, -101), S(-205, 0), S(-233, 58), S(-98, 33), S(237, 25), S(-228, -44), S(37, -8), S(-572, 1)
    },
    // BishopPST
    {
        S(-43, -55), S(35, -12), S(-29, -10), S(-36, -7), S(-79, 4), S(-86, -11), S(-19, -14), S(-7, -34),
        S(49, -37), S(46, -23), S(62, -22), S(-24, 19), S(2, -3), S(9, 7), S(45, -38), S(-23, -74),
        S(5, 4), S(70, 31), S(-9, 9), S(32, 13), S(-11, 37), S(6, -3), S(15, 2), S(35, -38),
        S(3, -12), S(53, 2), S(57, 25), S(65, 11), S(30, 17), S(32, 15), S(12, 12), S(54, -4),
        S(20, 22), S(78, 24), S(36, 24), S(53, 15), S(75, -8), S(147, -4), S(74, 19), S(2, -19),
        S(89, 3), S(83, 30), S(-112, 25), S(23, -2), S(124, 17), S(265, -22), S(206, 9), S(98, -13),
        S(-82, -1), S(-106, 5), S(11, 16), S(2, 10), S(-55, 10), S(29, -21), S(-193, 14), S(-177, -30),
        S(-83, 0), S(-68, 47), S(-159, -5), S(-172, 76), S(-215, 42), S(-240, -4), S(-69, -12), S(-13, -92)
    },
    // RookPST
    {
        S(-76, -16), S(-74, -3), S(-50, -6), S(-36, -13), S(-6, -14), S(1, -15), S(-22, -3), S(-29, -31),
        S(-146, -12), S(-128, -6), S(-85, -12), S(-22, -20), S(-32, -20), S(-17, 1), S(-17, -31), S(-213, 6),
        S(-186, 4), S(-100, -24), S(-115, -5), S(-114, -11), S(-61, -25), S(-7, -19), S(-27, -21), S(-56, -32),
        S(-99, 12), S(-24, 13), S(-92, 15), S(-37, 7), S(8, -12), S(-22, 19), S(60, 3), S(-72, -5),
        S(-130, 34), S(8, 24), S(-1, 36), S(47, 7), S(33, 8), S(86, 2), S(150, 1), S(-41, 3),
        S(-81, 51), S(-26, 20), S(3, 32), S(76, 12), S(201, -21), S(282, -21), S(244, 2), S(22, 8),
        S(-19, -10), S(-23, 4), S(17, 6), S(145, -9), S(130, -14), S(322, -41), S(114, 5), S(101, -21),
        S(-36, 12), S(62, 6), S(148, 6), S(18, 18), S(100, 13), S(-23, 27), S(-156, 48), S(76, 9)
    },
    // QueenPST
    {
        S(-102, -13), S(-51, -45), S(-32, -53), S(-4, -27), S(17, -57), S(-149, -26), S(81, -217), S(-125, -89),
        S(-74, -51), S(-32, -39), S(15, -45), S(0, -17), S(16, -46), S(43, -57), S(64, -144), S(-115, -85),
        S(-65, 10), S(4, -27), S(27, -13), S(-15, -11), S(13, 0), S(3, 46), S(34, 9), S(-30, -24),
        S(5, -38), S(-60, 31), S(-9, 31), S(1, 54), S(50, 26), S(66, 40), S(18, 34), S(-14, -17),
        S(4, -14), S(-20, 10), S(37, 64), S(6, 41), S(42, 79), S(72, 34), S(-16, 115), S(66, -61),
        S(1, -49), S(0, -44), S(-2, 29), S(1, 35), S(6, 98), S(30, 150), S(66, 50), S(60, 38),
        S(-123, -46), S(-142, 25), S(-78, 55), S(7, 63), S(-97, 150), S(68, 18), S(-190, 111), S(126, -80),
        S(-109, 17), S(-116, 72), S(163, -16), S(13, 49), S(149, -34), S(89, 25), S(197, -66), S(78, -29)
    },
    // KingPST
    {
        S(128, -124), S(167, -80), S(103, -33), S(-118, -21), S(-58, -39), S(-169, -13), S(142, -71), S(197, -132),
        S(244, -70), S(173, -45), S(115, -14), S(61, -6), S(-6, 2), S(31, -5), S(148, -33), S(222, -65),
        S(81, -55), S(46, -9), S(131, 5), S(-57, 40), S(20, 18), S(69, 10), S(171, -18), S(-25, -23),
        S(-18, -25), S(113, -15), S(-108, 39), S(-111, 58), S(-147, 64), S(12, 55), S(99, 32), S(-88, -1),
        S(-74, 20), S(25, 52), S(-119, 52), S(-245, 79), S(-268, 76), S(-154, 81), S(-10, 91), S(-63, 15),
        S(71, 5), S(-97, 68), S(-195, 70), S(-299, 109), S(-271, 103), S(-47, 125), S(39, 112), S(-45, 31),
        S(166, -72), S(91, 9), S(-114, 33), S(-101, 118), S(63, 64), S(149, 89), S(72, 69), S(-32, -38),
        S(-295, -403), S(244, -65), S(134, 59), S(-87, -25), S(-157, 30), S(-14, -70), S(193, 30), S(-139, -366)
    },
    {
        {},
        {},
        {S(-111, -167), S(-49, -45), S(-2, -16), S(9, 7), S(28, 24), S(44, 43), S(56, 43), S(56, 43), S(56, 43)},
        {S(-17, 82), S(33, 90), S(67, 112), S(85, 128), S(103, 145), S(112, 150), S(112, 159), S(112, 161), S(122, 163), S(122, 163), S(122, 165), S(122, 165), S(148, 165), S(148, 165)},
        {S(-41, 246), S(6, 285), S(12, 293), S(20, 299), S(20, 299), S(26, 314), S(26, 320), S(36, 328), S(36, 334), S(36, 344), S(43, 347), S(43, 356), S(43, 357), S(43, 357), S(43, 357)},
        {S(10, -21), S(102, 219), S(110, 290), S(110, 316), S(130, 346), S(130, 346), S(143, 348), S(146, 370), S(160, 370), S(160, 372), S(160, 373), S(173, 380), S(174, 387), S(174, 388), S(174, 392), S(176, 398), S(176, 403), S(192, 403), S(212, 403), S(212, 403), S(212, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 404), S(230, 404)},
        {},
    },
    {S(0, 0), S(-20, 41), S(-12, 52), S(2, 69), S(8, 215), S(79, 381), S(385, 414), S(0, 0)},
    {S(0, 0), S(3, -14), S(53, 3), S(64, 7), S(65, 33), S(66, 79), S(264, 135), S(0, 0)},
    S(138, 18), // RookOpenFileBonus
    S(57, 18), // RookSemiOpenFileBonus
    S(94, 23), // KnightOutpostBonus
    S(95, 24), // BishopOutpostBonus
    S(-82, 0), // TrappedRookByKingPenalty
    S(66, 0), // RookBehindOurPasserBonus
    S(-9, 96), // RookBehindTheirPasserBonus
    S(29, 12), // MinorBehindPawnBonus
    S(27, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-20, -4), // KingProtector
    S(39, 15), // BishopPair
    {S(129, 0), S(63, 0)}, // PawnShieldBonus
    {S(0, 0), S(9, 0), S(87, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(52, 0), S(144, 0), S(0, 0), S(0, 0)}, // UnblockedPawnStorm
    S(-34, 0), // SemiOpenFileNearKing
    S(-186, 0), // OpenFileNearKing
    S(-45, 0), // UndefendedKingZoneSq
    {S(-13, -70), S(0, -15), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)}, // KingSafeSqPenalty
    S(24, 13), // KingAttackByKnight
    S(6, 13), // KingAttackByBishop
    S(24, 13), // KingAttackByRook
    S(24, 18), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 4), S(29, 8), S(29, 8), S(29, 8), S(0, 0)}, // KingSafeCheck
    S(16, 0), // KingRingWeakWeight
    S(37, 35), // KingNoQueenDiscount
    S(-10, -1), // IsolatedPawnPenalty
    S(-5, -36), // DoubledPawnPenalty
    S(0, -11), // BackwardPawnPenalty
    S(-25, -20), // WeakUnopposedPenalty
    S(0, -24), // DoubledIsolatedPenalty
    {S(-102, -27), S(-52, -100)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-13, -25), // PawnIslandPenalty
    {S(28, 0), S(24, 0)}, // CentralPawnBonus
    S(64, 17), // BishopLongDiagonalBonus
    S(0, 52), // InitiativePasser
    S(0, 46), // InitiativePawnCount
    S(0, 12), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 24), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(77, 0), // SliderOnQueenBishop
    S(67, 0), // SliderOnQueenRook
    S(13, 3), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
