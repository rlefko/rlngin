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
    S(240, 57), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(247, 13), S(196, 1), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(283, 7), S(0, 0)},
    S(177, 40), // ThreatByKing
    S(12, 0), // Hanging
    S(72, 28), // WeakQueen
    S(55, 15), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 39), S(0, 55), S(0, 56), S(0, 56), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 24), S(0, 60), S(0, 86), S(0, 114), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-18, -48), S(-18, -61), S(-18, -96), S(-223, -201), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(24, 43), S(24, 73), S(188, 156), S(294, 500), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(48, 18), S(136, 18), S(144, 28), S(245, 28), S(0, 0)},
    S(0, 40), // RookOn7thBonus
    S(-4, -14), // BadBishopPenalty
    S(30, 0), // Tempo
    {S(0, 0), S(179, 312), S(948, 754), S(997, 720), S(1468, 1186), S(2638, 2568), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-70, -64), S(-20, -84), S(-46, -81), S(-32, -106), S(26, -78), S(-2, -67), S(28, -73), S(-111, -78),
        S(-86, -78), S(-70, -96), S(-62, -104), S(-51, -108), S(-12, -101), S(-57, -88), S(-19, -107), S(-83, -99),
        S(-86, -52), S(-94, -75), S(-84, -119), S(-5, -141), S(-38, -117), S(-25, -99), S(-64, -98), S(-47, -90),
        S(-33, -26), S(-28, -63), S(47, -100), S(25, -100), S(51, -113), S(55, -97), S(-54, -62), S(-79, -36),
        S(39, 50), S(-35, 52), S(92, 6), S(53, 6), S(53, -13), S(39, 46), S(-104, 80), S(-26, 71),
        S(134, 281), S(123, 329), S(207, 299), S(252, 281), S(272, 266), S(134, 362), S(-1, 343), S(-105, 324),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-332, 18), S(-97, 11), S(-88, -14), S(-99, -29), S(-48, 7), S(-12, -46), S(-96, -7), S(-149, 97),
        S(-193, -1), S(5, -54), S(-70, -9), S(7, 8), S(-32, 4), S(-48, 8), S(-7, 52), S(-37, 8),
        S(-13, -6), S(24, -24), S(-1, 1), S(-25, 30), S(62, 13), S(24, 15), S(40, -3), S(-61, 56),
        S(67, -22), S(106, -2), S(71, 46), S(81, 36), S(83, 37), S(109, 51), S(56, 63), S(39, 31),
        S(204, -27), S(60, 26), S(101, 28), S(152, 14), S(104, 53), S(228, 15), S(104, 16), S(163, -31),
        S(-66, -24), S(39, 4), S(104, 25), S(131, 41), S(283, -13), S(169, -41), S(138, -8), S(94, -12),
        S(-123, -50), S(-120, 27), S(4, 2), S(57, 35), S(10, 4), S(183, -14), S(125, -38), S(-30, -115),
        S(-550, -95), S(-152, 14), S(-68, 0), S(-117, 35), S(154, -73), S(-375, 2), S(34, -34), S(-399, -137)
    },
    // BishopPST
    {
        S(13, -62), S(99, -75), S(-41, 20), S(-20, -36), S(-123, 37), S(-104, 10), S(65, -15), S(5, -39),
        S(88, -94), S(42, -28), S(83, -46), S(-30, 19), S(1, 3), S(-33, 19), S(37, -46), S(83, -57),
        S(56, 7), S(66, -11), S(-5, 30), S(4, 29), S(-9, 59), S(-9, -13), S(32, 25), S(26, -17),
        S(75, -25), S(52, 15), S(53, 7), S(66, 2), S(43, -11), S(7, 30), S(-6, 11), S(44, -16),
        S(-1, 43), S(100, 17), S(77, 22), S(15, -3), S(20, -15), S(101, -1), S(51, 21), S(-12, 8),
        S(29, -2), S(58, 31), S(-76, 30), S(76, 10), S(64, -4), S(169, -23), S(134, -8), S(126, -37),
        S(-82, -8), S(-186, -8), S(-5, 35), S(-62, 27), S(-63, 37), S(37, 8), S(-217, 29), S(-145, -7),
        S(-123, -9), S(-84, 42), S(-167, 42), S(-180, 91), S(-199, 9), S(-192, 39), S(91, -29), S(-5, -109)
    },
    // RookPST
    {
        S(-86, -2), S(-80, 1), S(-38, -2), S(-34, -13), S(-18, -27), S(-15, -15), S(-3, -25), S(-89, -13),
        S(-78, -2), S(-91, -15), S(-36, 0), S(-64, -18), S(-85, -8), S(-31, -32), S(73, -70), S(-203, 9),
        S(-152, 3), S(-48, -15), S(-25, -7), S(-149, -4), S(-82, -18), S(-14, -30), S(24, -38), S(-134, 9),
        S(-99, 20), S(-146, 44), S(-10, -2), S(-82, 34), S(-90, 3), S(-112, 34), S(20, -2), S(10, -44),
        S(-4, 34), S(-30, 34), S(17, 31), S(-63, 12), S(-9, 8), S(22, 1), S(80, 16), S(97, 2),
        S(-10, 27), S(17, 17), S(14, 23), S(110, 3), S(139, -5), S(196, -10), S(126, 13), S(40, 21),
        S(-16, -2), S(-89, 12), S(58, -6), S(219, -28), S(232, -38), S(204, -22), S(-20, 9), S(151, -23),
        S(118, -29), S(26, 10), S(66, 10), S(74, 7), S(206, -14), S(75, 26), S(6, 19), S(-26, 36)
    },
    // QueenPST
    {
        S(16, -85), S(-85, 35), S(-64, -16), S(-42, 5), S(31, -47), S(-75, -58), S(-29, -145), S(-187, -169),
        S(-80, -19), S(44, -39), S(-26, 7), S(-6, 5), S(-4, -20), S(48, -81), S(78, -194), S(-1, -149),
        S(-83, -14), S(-12, -8), S(-31, 47), S(-12, -35), S(10, -14), S(19, 28), S(38, -28), S(-24, -24),
        S(4, -8), S(-34, 3), S(-19, 31), S(16, 58), S(20, 30), S(18, 84), S(10, 69), S(1, -15),
        S(24, -6), S(42, -48), S(-33, 80), S(-12, 59), S(0, 103), S(57, 64), S(42, 77), S(46, 10),
        S(-15, -78), S(50, -60), S(-42, 55), S(7, 31), S(8, 114), S(96, 110), S(132, 74), S(166, -66),
        S(-79, -20), S(-156, 53), S(-124, 131), S(-31, 87), S(-67, 150), S(26, 78), S(-28, 47), S(108, -84),
        S(-155, 33), S(-162, 20), S(133, -48), S(15, 49), S(87, -26), S(59, -23), S(175, -130), S(144, -117)
    },
    // KingPST
    {
        S(-23, -89), S(97, -75), S(55, -52), S(-63, -47), S(-101, -72), S(-122, -58), S(113, -105), S(165, -147),
        S(69, -103), S(42, -54), S(14, -16), S(16, -19), S(31, -24), S(-28, -36), S(123, -61), S(179, -73),
        S(10, -56), S(167, -22), S(12, -18), S(-63, 9), S(-99, 1), S(14, -4), S(158, -38), S(-40, -27),
        S(-113, 2), S(186, -30), S(-123, 33), S(-190, 29), S(-290, 38), S(-107, 20), S(-76, 24), S(17, -38),
        S(87, 3), S(82, 63), S(-146, 65), S(-228, 70), S(-251, 51), S(-201, 64), S(-73, 61), S(-86, 24),
        S(72, 16), S(64, 79), S(-10, 107), S(-114, 100), S(-118, 74), S(-86, 38), S(-48, 43), S(28, 38),
        S(87, -101), S(100, 20), S(-17, 64), S(60, 85), S(88, 87), S(14, 60), S(73, 64), S(33, 37),
        S(-134, -240), S(101, -54), S(127, -2), S(50, 90), S(4, 177), S(131, 85), S(202, 65), S(22, -203)
    },
    {
        {},
        {},
        {S(-111, -171), S(-43, -45), S(-13, -4), S(5, 12), S(33, 25), S(50, 43), S(56, 43), S(56, 43), S(56, 43)},
        {S(-6, 78), S(53, 81), S(83, 100), S(96, 126), S(107, 136), S(108, 156), S(113, 161), S(113, 165), S(113, 165), S(113, 165), S(114, 165), S(114, 165), S(164, 165), S(164, 165)},
        {S(-16, 242), S(4, 290), S(8, 294), S(20, 302), S(20, 303), S(22, 316), S(22, 321), S(26, 326), S(26, 334), S(26, 352), S(39, 353), S(39, 358), S(39, 358), S(39, 358), S(69, 358)},
        {S(106, -21), S(118, 179), S(118, 242), S(130, 260), S(132, 312), S(135, 342), S(141, 353), S(141, 366), S(151, 370), S(153, 370), S(153, 372), S(165, 372), S(165, 393), S(165, 393), S(174, 403), S(174, 403), S(174, 403), S(174, 403), S(195, 403), S(216, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 404), S(230, 404)},
        {},
    },
    {S(0, 0), S(-40, 59), S(-9, 76), S(-9, 76), S(0, 254), S(34, 428), S(227, 541), S(0, 0)},
    {S(0, 0), S(2, -17), S(34, 11), S(59, 13), S(70, 45), S(197, 104), S(408, 135), S(0, 0)},
    S(137, 18), // RookOpenFileBonus
    S(64, 18), // RookSemiOpenFileBonus
    S(93, 32), // KnightOutpostBonus
    S(117, 31), // BishopOutpostBonus
    S(-37, 0), // TrappedRookByKingPenalty
    S(92, 0), // RookBehindOurPasserBonus
    S(-16, 128), // RookBehindTheirPasserBonus
    S(22, 16), // MinorBehindPawnBonus
    S(7, 2), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-24, -1), // KingProtector
    S(10, 39), // BishopPair
    {S(136, 0), S(75, 0)}, // PawnShieldBonus
    {S(0, 0), S(0, 0), S(88, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(40, 0), S(132, 0), S(50, 0), S(16, 0)}, // UnblockedPawnStorm
    S(-21, 0), // SemiOpenFileNearKing
    S(-146, 0), // OpenFileNearKing
    S(-54, 0), // UndefendedKingZoneSq
    {S(-41, -70), S(0, -23), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)}, // KingSafeSqPenalty
    S(24, 13), // KingAttackByKnight
    S(9, 13), // KingAttackByBishop
    S(24, 13), // KingAttackByRook
    S(24, 18), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 4), S(29, 0), S(29, 8), S(29, 8), S(0, 0)}, // KingSafeCheck
    S(13, 0), // KingRingWeakWeight
    S(30, 35), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -38), // DoubledPawnPenalty
    S(0, -10), // BackwardPawnPenalty
    S(-30, -20), // WeakUnopposedPenalty
    S(0, -35), // DoubledIsolatedPenalty
    {S(-66, -32), S(-49, -99)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-11, -34), // PawnIslandPenalty
    {S(24, 0), S(20, 0)}, // CentralPawnBonus
    S(66, 25), // BishopLongDiagonalBonus
    S(0, 36), // InitiativePasser
    S(0, 53), // InitiativePawnCount
    S(0, 1), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 64), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(79, 0), // SliderOnQueenBishop
    S(69, 0), // SliderOnQueenRook
    S(17, 2), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
