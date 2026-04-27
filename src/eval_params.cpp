#include "eval_params.h"

// Compiled-in defaults for the tunable parameters. The in-memory
// `evalParams` instance is initialized from this struct, and
// `resetEvalParams()` snaps it back to these values after a tune run.
//
// Values below come from a Texel tune covering every scalar in
// `EvalParams` (1067 mg/eg halves) over 6.4M qsearch-leaf positions
// extracted from the PR #36 self-play PGN (64,000 games at nodes=25000)
// with mate-scored plies filtered out at extraction. Initial loss was
// 0.0875643; this snapshot was taken after the running tune's pass 3
// of the upgraded leg, then loaded through the canonicalization
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
//   - Periodic K refit (every 5 passes) and optional periodic leaf
//     refresh against the evolving params.
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
    S(245, 60), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(248, 8), S(188, 5), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(263, 39), S(0, 0)},
    S(177, 38), // ThreatByKing
    S(15, 0), // Hanging
    S(72, 28), // WeakQueen
    S(53, 14), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 38), S(0, 52), S(0, 57), S(0, 57), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 24), S(0, 59), S(0, 86), S(0, 114), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-18, -48), S(-18, -61), S(-18, -98), S(-207, -209), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(16, 44), S(24, 75), S(164, 158), S(294, 484), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(42, 16), S(120, 18), S(128, 28), S(245, 28), S(0, 0)},
    S(-22, 47), // RookOn7thBonus
    S(-3, -14), // BadBishopPenalty
    S(36, 0), // Tempo
    {S(0, 0), S(172, 318), S(933, 768), S(969, 737), S(1429, 1211), S(2605, 2598), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-66, -65), S(-18, -85), S(-42, -83), S(-31, -108), S(28, -80), S(-2, -66), S(35, -75), S(-109, -78),
        S(-83, -79), S(-72, -96), S(-61, -106), S(-46, -112), S(-9, -102), S(-58, -88), S(-18, -109), S(-82, -101),
        S(-80, -53), S(-93, -76), S(-81, -121), S(-3, -144), S(-32, -121), S(-26, -101), S(-65, -100), S(-41, -91),
        S(-32, -28), S(-26, -65), S(47, -103), S(28, -103), S(52, -115), S(56, -97), S(-53, -62), S(-80, -38),
        S(28, 48), S(-38, 46), S(77, 4), S(46, 6), S(38, -13), S(32, 44), S(-103, 72), S(-34, 70),
        S(143, 283), S(148, 327), S(208, 313), S(237, 287), S(241, 288), S(167, 372), S(0, 349), S(-96, 324),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-307, 10), S(-102, 23), S(-95, -14), S(-98, -29), S(-47, 11), S(-11, -46), S(-106, 9), S(-132, 65),
        S(-192, -1), S(6, -54), S(-77, -9), S(8, 10), S(-35, 6), S(-59, 8), S(-6, 52), S(-36, 10),
        S(-13, -8), S(25, -28), S(0, 1), S(-24, 30), S(55, 17), S(19, 20), S(40, -3), S(-64, 58),
        S(66, -26), S(99, -2), S(72, 46), S(74, 38), S(80, 41), S(101, 51), S(57, 63), S(40, 31),
        S(197, -23), S(56, 27), S(98, 29), S(157, 18), S(97, 59), S(221, 17), S(101, 22), S(164, -31),
        S(-65, -24), S(32, 4), S(105, 31), S(140, 43), S(260, -5), S(170, -41), S(139, -8), S(95, -20),
        S(-130, -48), S(-119, 27), S(5, -6), S(50, 43), S(19, 4), S(176, -14), S(110, -30), S(-29, -115),
        S(-517, -111), S(-151, 14), S(-67, 0), S(-116, 35), S(155, -73), S(-342, -6), S(35, -34), S(-366, -153)
    },
    // BishopPST
    {
        S(-16, -63), S(94, -76), S(-46, 25), S(-21, -33), S(-120, 36), S(-107, 9), S(52, -8), S(8, -40),
        S(83, -95), S(42, -29), S(86, -47), S(-33, 20), S(1, 4), S(-30, 18), S(41, -46), S(86, -58),
        S(59, -2), S(65, -12), S(-6, 29), S(3, 28), S(-6, 60), S(-20, -10), S(35, 24), S(31, -22),
        S(78, -26), S(55, 14), S(50, 6), S(65, 3), S(46, -12), S(2, 31), S(-11, 10), S(39, -17),
        S(-2, 42), S(103, 16), S(72, 21), S(22, -4), S(13, -14), S(104, -6), S(50, 22), S(-17, 11),
        S(30, -1), S(53, 32), S(-73, 25), S(79, 10), S(59, -5), S(164, -20), S(129, -1), S(129, -38),
        S(-87, -9), S(-171, -13), S(-2, 34), S(-51, 26), S(-52, 40), S(44, 11), S(-182, 18), S(-134, -16),
        S(-120, -10), S(-73, 33), S(-164, 41), S(-169, 82), S(-180, 8), S(-189, 30), S(62, -30), S(-18, -110)
    },
    // RookPST
    {
        S(-83, -3), S(-77, 0), S(-31, -3), S(-27, -14), S(-11, -28), S(-6, -14), S(-6, -24), S(-94, -14),
        S(-67, -7), S(-92, -16), S(-37, -1), S(-63, -15), S(-80, -9), S(-29, -33), S(72, -71), S(-196, 10),
        S(-145, 2), S(-57, -8), S(-18, -8), S(-142, -5), S(-75, -19), S(-7, -35), S(15, -39), S(-147, 8),
        S(-92, 19), S(-135, 47), S(-11, -1), S(-79, 33), S(-83, 1), S(-97, 31), S(19, -3), S(1, -37),
        S(-13, 39), S(-23, 37), S(12, 34), S(-56, 11), S(-10, 11), S(29, 0), S(71, 20), S(72, 9),
        S(-3, 25), S(8, 18), S(13, 22), S(101, 2), S(130, -2), S(171, -5), S(133, 12), S(47, 20),
        S(-9, -5), S(-74, 7), S(65, -7), S(194, -21), S(223, -37), S(195, -23), S(-13, 6), S(158, -24),
        S(93, -26), S(33, 9), S(57, 9), S(73, 6), S(189, -11), S(66, 29), S(5, 18), S(-3, 31)
    },
    // QueenPST
    {
        S(17, -87), S(-68, 1), S(-63, -24), S(-41, 3), S(32, -49), S(-74, -60), S(-36, -147), S(-186, -155),
        S(-79, -21), S(41, -41), S(-23, 1), S(-3, 3), S(-1, -29), S(49, -81), S(71, -180), S(0, -135),
        S(-82, -16), S(-8, -11), S(-30, 51), S(-15, -38), S(11, -16), S(18, 26), S(39, -29), S(-23, -26),
        S(9, -10), S(-41, 17), S(-26, 29), S(17, 56), S(21, 20), S(23, 82), S(7, 75), S(-6, -9),
        S(9, 4), S(39, -34), S(-24, 78), S(-7, 57), S(3, 101), S(50, 70), S(27, 83), S(42, 8),
        S(-10, -80), S(35, -46), S(-41, 53), S(8, 37), S(9, 112), S(97, 108), S(125, 72), S(151, -52),
        S(-86, -22), S(-155, 51), S(-107, 129), S(-22, 85), S(-66, 148), S(43, 68), S(-27, 45), S(109, -62),
        S(-122, 7), S(-129, 2), S(110, -34), S(16, 47), S(88, -20), S(60, -25), S(160, -108), S(121, -87)
    },
    // KingPST
    {
        S(-17, -97), S(91, -73), S(53, -52), S(-57, -55), S(-103, -72), S(-128, -60), S(103, -103), S(155, -147),
        S(59, -103), S(36, -56), S(12, -16), S(14, -19), S(29, -25), S(-30, -36), S(121, -61), S(173, -73),
        S(8, -56), S(141, -18), S(8, -18), S(-61, 9), S(-101, 2), S(12, -4), S(146, -37), S(-46, -27),
        S(-107, -2), S(152, -26), S(-101, 33), S(-160, 27), S(-260, 36), S(-117, 24), S(-78, 24), S(15, -42),
        S(77, 11), S(56, 71), S(-116, 66), S(-198, 66), S(-221, 47), S(-171, 60), S(-59, 65), S(-80, 24),
        S(62, 16), S(70, 83), S(20, 107), S(-84, 100), S(-88, 74), S(-56, 34), S(-18, 43), S(26, 38),
        S(85, -101), S(90, 28), S(-11, 72), S(58, 85), S(78, 95), S(12, 68), S(39, 72), S(23, 41),
        S(-136, -240), S(99, -54), S(125, -2), S(48, 58), S(2, 145), S(105, 101), S(168, 73), S(20, -195)
    },
    {
        {},
        {},
        {S(-119, -171), S(-51, -47), S(-15, -4), S(1, 11), S(31, 24), S(50, 43), S(56, 43), S(56, 43), S(56, 43)},
        {S(-6, 72), S(53, 79), S(85, 99), S(96, 124), S(107, 136), S(108, 154), S(113, 161), S(113, 165), S(113, 165), S(113, 165), S(114, 165), S(114, 165), S(164, 165), S(164, 165)},
        {S(2, 210), S(8, 282), S(8, 292), S(20, 298), S(20, 299), S(22, 316), S(22, 321), S(26, 326), S(26, 335), S(26, 352), S(38, 354), S(39, 358), S(39, 358), S(39, 358), S(101, 358)},
        {S(106, -21), S(118, 147), S(118, 210), S(134, 244), S(134, 304), S(135, 340), S(141, 354), S(141, 362), S(149, 370), S(153, 370), S(153, 372), S(165, 372), S(165, 393), S(165, 393), S(174, 403), S(174, 403), S(174, 403), S(175, 403), S(195, 403), S(208, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 404), S(230, 404)},
        {},
    },
    {S(0, 0), S(-44, 60), S(-9, 76), S(-9, 76), S(0, 262), S(50, 428), S(203, 547), S(0, 0)},
    {S(0, 0), S(1, -17), S(34, 11), S(59, 15), S(70, 47), S(200, 108), S(376, 135), S(0, 0)},
    S(141, 18), // RookOpenFileBonus
    S(67, 18), // RookSemiOpenFileBonus
    S(95, 32), // KnightOutpostBonus
    S(119, 33), // BishopOutpostBonus
    S(-33, 0), // TrappedRookByKingPenalty
    S(97, 0), // RookBehindOurPasserBonus
    S(0, 124), // RookBehindTheirPasserBonus
    S(22, 18), // MinorBehindPawnBonus
    S(11, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-24, 0), // KingProtector
    S(22, 39), // BishopPair
    {S(135, 0), S(75, 0)}, // PawnShieldBonus
    {S(0, 0), S(0, 0), S(84, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(40, 0), S(132, 0), S(42, 0), S(16, 0)}, // UnblockedPawnStorm
    S(-25, 0), // SemiOpenFileNearKing
    S(-146, 0), // OpenFileNearKing
    S(-50, 0), // UndefendedKingZoneSq
    {S(-37, -70), S(0, -23), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)}, // KingSafeSqPenalty
    S(24, 13), // KingAttackByKnight
    S(9, 13), // KingAttackByBishop
    S(24, 13), // KingAttackByRook
    S(24, 18), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 4), S(29, 0), S(29, 8), S(29, 8), S(0, 0)}, // KingSafeCheck
    S(13, 0), // KingRingWeakWeight
    S(29, 35), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -40), // DoubledPawnPenalty
    S(0, -11), // BackwardPawnPenalty
    S(-28, -21), // WeakUnopposedPenalty
    S(0, -35), // DoubledIsolatedPenalty
    {S(-66, -32), S(-41, -96)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-11, -35), // PawnIslandPenalty
    {S(25, 0), S(19, 0)}, // CentralPawnBonus
    S(68, 24), // BishopLongDiagonalBonus
    S(0, 42), // InitiativePasser
    S(0, 54), // InitiativePawnCount
    S(0, 1), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 66), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(79, 0), // SliderOnQueenBishop
    S(67, 0), // SliderOnQueenRook
    S(17, 2), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
