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
    S(245, 65), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(243, 12), S(176, 21), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(255, 63), S(0, 0)},
    S(186, 40), // ThreatByKing
    S(7, 0), // Hanging
    S(72, 28), // WeakQueen
    S(47, 18), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 40), S(0, 50), S(0, 61), S(0, 61), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 58), S(0, 86), S(0, 114), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-18, -48), S(-18, -61), S(-18, -100), S(-183, -222), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(16, 45), S(16, 79), S(140, 158), S(278, 460), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(32, 12), S(112, 16), S(112, 28), S(245, 28), S(0, 0)},
    S(-22, 48), // RookOn7thBonus
    S(0, -14), // BadBishopPenalty
    S(36, 0), // Tempo
    {S(0, 0), S(173, 322), S(927, 774), S(942, 747), S(1399, 1236), S(2579, 2612), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-61, -66), S(-17, -84), S(-41, -85), S(-30, -109), S(22, -75), S(3, -67), S(38, -74), S(-100, -79),
        S(-81, -80), S(-71, -95), S(-63, -106), S(-53, -111), S(-16, -100), S(-53, -88), S(-13, -109), S(-75, -101),
        S(-79, -54), S(-96, -75), S(-76, -124), S(-2, -145), S(-30, -122), S(-26, -102), S(-65, -101), S(-43, -92),
        S(-31, -29), S(-29, -64), S(48, -102), S(27, -103), S(53, -116), S(56, -98), S(-56, -62), S(-79, -39),
        S(17, 47), S(-43, 43), S(70, 3), S(39, 1), S(31, -18), S(25, 43), S(-102, 70), S(-27, 65),
        S(152, 290), S(165, 328), S(193, 328), S(214, 302), S(218, 295), S(192, 371), S(1, 360), S(-95, 327),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-295, 2), S(-104, 23), S(-91, -14), S(-94, -29), S(-39, 11), S(1, -46), S(-96, 9), S(-104, 41),
        S(-196, -9), S(2, -54), S(-81, -9), S(14, 11), S(-30, 8), S(-63, 18), S(-2, 52), S(-20, 2),
        S(-19, -6), S(13, -22), S(-6, 3), S(-32, 35), S(51, 17), S(23, 21), S(42, -3), S(-60, 58),
        S(66, -22), S(99, -2), S(67, 50), S(75, 40), S(79, 43), S(97, 57), S(53, 71), S(42, 39),
        S(177, -21), S(48, 33), S(94, 30), S(153, 22), S(85, 65), S(209, 25), S(101, 26), S(152, -27),
        S(-61, -24), S(36, 6), S(93, 33), S(137, 43), S(240, -1), S(174, -41), S(135, -8), S(99, -20),
        S(-142, -44), S(-115, 27), S(17, -10), S(54, 43), S(23, 4), S(172, -14), S(90, -30), S(-33, -115),
        S(-489, -119), S(-155, 14), S(-63, 0), S(-112, 35), S(159, -73), S(-314, -14), S(23, -50), S(-338, -177)
    },
    // BishopPST
    {
        S(-37, -55), S(81, -77), S(-43, 31), S(-10, -33), S(-109, 36), S(-106, 15), S(31, 0), S(3, -40),
        S(74, -91), S(39, -29), S(85, -47), S(-28, 20), S(4, 6), S(-27, 20), S(44, -46), S(89, -54),
        S(54, -2), S(66, -13), S(-3, 29), S(6, 27), S(-3, 60), S(-17, -10), S(40, 24), S(34, -18),
        S(73, -26), S(58, 14), S(53, 6), S(61, 7), S(41, -11), S(3, 31), S(-8, 10), S(42, -17),
        S(-7, 43), S(98, 20), S(67, 25), S(17, 0), S(8, -10), S(91, 2), S(53, 22), S(-22, 13),
        S(25, 3), S(56, 32), S(-70, 27), S(82, 10), S(62, -5), S(159, -12), S(124, -1), S(108, -34),
        S(-92, -9), S(-144, -21), S(-7, 36), S(-48, 26), S(-41, 32), S(47, 11), S(-155, 10), S(-131, -20),
        S(-117, -10), S(-70, 33), S(-161, 41), S(-158, 82), S(-169, 8), S(-186, 30), S(41, -30), S(-31, -110)
    },
    // RookPST
    {
        S(-73, -3), S(-71, -1), S(-25, -2), S(-19, -15), S(-4, -28), S(9, -15), S(-4, -25), S(-90, -15),
        S(-71, 0), S(-82, -17), S(-39, -2), S(-65, -16), S(-74, -10), S(-23, -34), S(62, -64), S(-190, 9),
        S(-139, 1), S(-51, -9), S(-18, -9), S(-146, -6), S(-77, -20), S(-9, -36), S(5, -36), S(-141, 7),
        S(-86, 20), S(-129, 46), S(-17, 0), S(-73, 32), S(-85, 0), S(-91, 30), S(17, -2), S(7, -38),
        S(-7, 40), S(-25, 37), S(18, 33), S(-50, 10), S(-20, 14), S(27, -1), S(53, 27), S(54, 16),
        S(-13, 26), S(6, 17), S(3, 25), S(83, 3), S(112, 1), S(153, -4), S(139, 14), S(53, 19),
        S(-3, -5), S(-68, 6), S(71, -8), S(176, -18), S(205, -32), S(201, -22), S(1, 5), S(148, -21),
        S(75, -25), S(47, 4), S(39, 10), S(63, 5), S(171, -8), S(56, 30), S(11, 17), S(19, 26)
    },
    // QueenPST
    {
        S(19, -89), S(-50, -25), S(-57, -34), S(-35, 1), S(42, -51), S(-64, -78), S(-50, -133), S(-184, -141),
        S(-77, -23), S(49, -43), S(-19, -5), S(-2, 1), S(1, -31), S(51, -83), S(49, -166), S(2, -137),
        S(-80, -18), S(0, -33), S(-28, 49), S(-15, -40), S(5, -18), S(19, 24), S(35, -27), S(-21, -36),
        S(11, -12), S(-39, 15), S(-24, 27), S(6, 62), S(15, 18), S(17, 80), S(-3, 81), S(-16, 5),
        S(3, 6), S(17, -20), S(-22, 76), S(-13, 63), S(5, 99), S(36, 76), S(21, 89), S(34, 14),
        S(-16, -82), S(13, -32), S(-32, 51), S(-6, 43), S(11, 110), S(107, 106), S(127, 70), S(137, -38),
        S(-84, -24), S(-153, 49), S(-89, 119), S(-20, 91), S(-64, 146), S(69, 66), S(-17, 43), S(111, -56),
        S(-96, -11), S(-103, -16), S(96, -20), S(18, 45), S(90, -14), S(62, -11), S(138, -86), S(99, -65)
    },
    // KingPST
    {
        S(-10, -97), S(90, -73), S(60, -52), S(-41, -58), S(-92, -73), S(-129, -58), S(101, -103), S(138, -147),
        S(58, -103), S(43, -56), S(11, -15), S(19, -19), S(36, -25), S(-26, -38), S(120, -61), S(172, -74),
        S(-9, -56), S(116, -15), S(7, -18), S(-62, 9), S(-102, 2), S(19, -4), S(121, -33), S(-49, -27),
        S(-92, -2), S(127, -24), S(-94, 33), S(-137, 27), S(-237, 34), S(-118, 26), S(-79, 24), S(6, -42),
        S(68, 11), S(39, 75), S(-93, 66), S(-175, 66), S(-198, 45), S(-148, 60), S(-52, 65), S(-65, 24),
        S(45, 16), S(85, 83), S(43, 107), S(-61, 100), S(-65, 74), S(-33, 34), S(5, 43), S(25, 46),
        S(84, -101), S(73, 28), S(-12, 72), S(57, 93), S(77, 103), S(11, 68), S(14, 72), S(-2, 41),
        S(-137, -240), S(98, -54), S(124, -18), S(47, 34), S(-15, 121), S(88, 109), S(143, 81), S(19, -171)
    },
    {
        {},
        {},
        {S(-131, -179), S(-59, -51), S(-23, -4), S(-1, 10), S(29, 24), S(50, 43), S(56, 43), S(56, 43), S(56, 43)},
        {S(3, 55), S(55, 75), S(87, 96), S(96, 122), S(107, 136), S(108, 152), S(111, 161), S(113, 165), S(113, 165), S(113, 165), S(114, 165), S(114, 165), S(164, 165), S(164, 165)},
        {S(4, 186), S(12, 262), S(12, 282), S(20, 294), S(20, 298), S(22, 314), S(22, 321), S(22, 326), S(26, 334), S(26, 352), S(35, 354), S(35, 358), S(35, 358), S(39, 358), S(125, 358)},
        {S(98, -21), S(118, 123), S(118, 186), S(136, 236), S(136, 296), S(136, 339), S(142, 350), S(143, 358), S(149, 367), S(153, 370), S(153, 372), S(165, 373), S(165, 393), S(165, 393), S(174, 403), S(174, 403), S(174, 403), S(175, 403), S(199, 403), S(208, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 412), S(230, 418)},
        {},
    },
    {S(0, 0), S(-50, 60), S(-7, 76), S(-7, 76), S(6, 267), S(70, 426), S(187, 553), S(0, 0)},
    {S(0, 0), S(1, -17), S(36, 11), S(64, 15), S(74, 49), S(208, 108), S(352, 135), S(0, 0)},
    S(149, 18), // RookOpenFileBonus
    S(69, 18), // RookSemiOpenFileBonus
    S(93, 32), // KnightOutpostBonus
    S(119, 33), // BishopOutpostBonus
    S(-28, 0), // TrappedRookByKingPenalty
    S(99, 0), // RookBehindOurPasserBonus
    S(24, 122), // RookBehindTheirPasserBonus
    S(26, 19), // MinorBehindPawnBonus
    S(12, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-22, 0), // KingProtector
    S(24, 39), // BishopPair
    {S(134, 0), S(71, 0)}, // PawnShieldBonus
    {S(0, 0), S(0, 0), S(88, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(42, 0), S(132, 0), S(32, 0), S(8, 0)}, // UnblockedPawnStorm
    S(-25, 0), // SemiOpenFileNearKing
    S(-151, 0), // OpenFileNearKing
    S(-54, 0), // UndefendedKingZoneSq
    {S(-38, -70), S(0, -23), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)}, // KingSafeSqPenalty
    S(24, 13), // KingAttackByKnight
    S(10, 13), // KingAttackByBishop
    S(24, 13), // KingAttackByRook
    S(24, 18), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 4), S(29, 8), S(29, 8), S(29, 8), S(0, 0)}, // KingSafeCheck
    S(13, 0), // KingRingWeakWeight
    S(29, 35), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -42), // DoubledPawnPenalty
    S(0, -13), // BackwardPawnPenalty
    S(-30, -20), // WeakUnopposedPenalty
    S(0, -35), // DoubledIsolatedPenalty
    {S(-65, -32), S(-25, -94)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-11, -35), // PawnIslandPenalty
    {S(23, 0), S(18, 0)}, // CentralPawnBonus
    S(72, 24), // BishopLongDiagonalBonus
    S(0, 60), // InitiativePasser
    S(0, 53), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 67), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(79, 0), // SliderOnQueenBishop
    S(71, 0), // SliderOnQueenRook
    S(16, 1), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
