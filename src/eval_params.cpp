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
//     unopposed, doubled-isolated, blocked, pawn-island) <= 0 each
//     half. Bishop penalties (BadBishop, BishopPawns) <= 0 each half.
//   - BishopPair / MinorOnKingRing / RookOnKingRing >= 0 against
//     universal chess priors.
//   - `RookOpenFile >= RookSemiOpenFile >= 0` per phase. The classical
//     shelter / storm grids replace the previous semi-open / open file
//     near-king pair: Shelter[d][0] is the no-pawn (semi-open) file
//     penalty (<= 0); other Shelter slots are non-negative; storm slots
//     stay non-negative because they are subtracted at the call site.
//   - King-attack and king-safe-check piece-weight chains:
//     `Queen >= Rook >= max(Bishop, Knight)` per half.
//   - `KingMobilityFactor` >= 0 each half (subtracted from the
//     accumulator at the call site).
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
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(247, 13), S(192, 1), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(287, 1), S(0, 0)},
    S(177, 38), // ThreatByKing
    S(12, 0), // Hanging
    S(72, 28), // WeakQueen
    S(55, 15), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 39), S(0, 55), S(0, 56), S(0, 56), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 24), S(0, 60), S(0, 86), S(0, 114), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-18, -48), S(-18, -60), S(-18, -96), S(-231, -195), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(24, 43), S(24, 73), S(188, 152), S(294, 500), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(38, 18), S(152, 18), S(152, 28), S(245, 28), S(0, 0)},
    S(0, 41), // RookOn7thBonus
    S(-2, -7), // BadBishop
    S(-2, -7), // BishopPawns
    S(30, 0), // Tempo
    {S(0, 0), S(179, 312), S(956, 746), S(1011, 712), S(1496, 1169), S(2663, 2543), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-70, -64), S(-20, -84), S(-46, -81), S(-32, -106), S(22, -80), S(-2, -68), S(28, -73), S(-111, -78),
        S(-86, -78), S(-70, -96), S(-63, -104), S(-51, -109), S(-12, -101), S(-57, -88), S(-19, -107), S(-84, -99),
        S(-84, -52), S(-94, -75), S(-84, -119), S(-5, -141), S(-38, -119), S(-27, -99), S(-64, -98), S(-46, -90),
        S(-33, -26), S(-28, -63), S(47, -100), S(25, -100), S(51, -113), S(56, -97), S(-52, -62), S(-83, -36),
        S(43, 50), S(-27, 52), S(100, 6), S(65, 4), S(61, -13), S(47, 44), S(-104, 80), S(-22, 71),
        S(126, 281), S(115, 329), S(215, 299), S(252, 281), S(296, 266), S(110, 362), S(-1, 335), S(-113, 324),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-340, 18), S(-97, 11), S(-88, -22), S(-99, -29), S(-48, 7), S(-8, -46), S(-92, -15), S(-149, 97),
        S(-185, -1), S(5, -54), S(-70, -9), S(9, 6), S(-28, 2), S(-52, 8), S(-7, 52), S(-37, 8),
        S(-12, -6), S(24, -24), S(-1, 1), S(-23, 29), S(63, 13), S(28, 13), S(44, -5), S(-61, 56),
        S(67, -22), S(106, -2), S(73, 46), S(85, 34), S(83, 37), S(108, 51), S(64, 63), S(43, 31),
        S(204, -27), S(60, 26), S(101, 26), S(156, 14), S(106, 54), S(232, 15), S(108, 17), S(167, -31),
        S(-66, -24), S(39, 4), S(112, 23), S(139, 41), S(291, -13), S(168, -41), S(138, -8), S(94, -12),
        S(-115, -50), S(-120, 27), S(-4, 2), S(57, 35), S(10, 4), S(191, -14), S(133, -38), S(-30, -115),
        S(-566, -87), S(-152, 14), S(-68, 0), S(-117, 35), S(154, -73), S(-399, 10), S(34, -34), S(-423, -129)
    },
    // BishopPST
    {
        S(29, -62), S(99, -75), S(-41, 20), S(-20, -36), S(-123, 37), S(-102, 8), S(73, -15), S(5, -39),
        S(88, -94), S(42, -28), S(87, -46), S(-30, 19), S(1, 3), S(-33, 19), S(37, -46), S(83, -57),
        S(52, 7), S(66, -11), S(-1, 28), S(4, 29), S(-8, 59), S(-9, -13), S(32, 25), S(26, -17),
        S(83, -25), S(56, 15), S(55, 7), S(70, 2), S(43, -11), S(9, 30), S(-6, 11), S(44, -16),
        S(7, 43), S(100, 17), S(77, 22), S(17, -3), S(20, -14), S(101, -1), S(55, 21), S(-12, 4),
        S(33, -2), S(58, 31), S(-80, 30), S(76, 10), S(64, -4), S(177, -23), S(134, -8), S(130, -37),
        S(-82, -8), S(-186, -8), S(-5, 35), S(-62, 27), S(-63, 37), S(37, 8), S(-241, 33), S(-153, -7),
        S(-123, -9), S(-92, 42), S(-167, 42), S(-188, 91), S(-199, 9), S(-192, 39), S(107, -29), S(-5, -109)
    },
    // RookPST
    {
        S(-90, -1), S(-84, 2), S(-38, -3), S(-38, -12), S(-22, -26), S(-15, -15), S(-7, -24), S(-93, -12),
        S(-82, -1), S(-95, -14), S(-40, 1), S(-60, -17), S(-85, -7), S(-35, -31), S(69, -69), S(-199, 10),
        S(-156, 4), S(-52, -14), S(-29, -6), S(-149, -3), S(-86, -17), S(-18, -29), S(20, -37), S(-138, 10),
        S(-103, 21), S(-150, 45), S(-14, -1), S(-82, 35), S(-94, 4), S(-112, 35), S(16, -1), S(14, -43),
        S(-8, 35), S(-26, 35), S(13, 32), S(-67, 13), S(-13, 9), S(18, 2), S(84, 17), S(109, -1),
        S(-14, 28), S(13, 18), S(18, 24), S(122, 0), S(151, -8), S(216, -13), S(130, 14), S(36, 22),
        S(-24, 0), S(-95, 13), S(56, -5), S(223, -27), S(228, -37), S(200, -21), S(-24, 10), S(147, -22),
        S(130, -28), S(28, 11), S(70, 9), S(78, 8), S(210, -13), S(79, 27), S(10, 20), S(-38, 37)
    },
    // QueenPST
    {
        S(15, -80), S(-94, 36), S(-65, -14), S(-43, 6), S(30, -46), S(-76, -57), S(-22, -144), S(-188, -176),
        S(-81, -18), S(43, -38), S(-27, 8), S(-7, 6), S(-5, -19), S(47, -80), S(93, -209), S(-2, -148),
        S(-84, -9), S(-13, -7), S(-30, 48), S(-13, -34), S(9, -13), S(20, 27), S(37, -28), S(-25, -23),
        S(3, -7), S(-35, 4), S(-20, 32), S(15, 59), S(15, 31), S(17, 85), S(9, 69), S(0, -14),
        S(23, -5), S(49, -59), S(-34, 81), S(-13, 60), S(7, 100), S(64, 65), S(41, 78), S(45, 11),
        S(-18, -77), S(49, -59), S(-43, 56), S(6, 32), S(7, 115), S(95, 111), S(131, 75), S(165, -73),
        S(-80, -19), S(-157, 54), S(-125, 132), S(-32, 88), S(-68, 151), S(17, 87), S(-29, 48), S(123, -95),
        S(-180, 42), S(-187, 25), S(140, -47), S(14, 50), S(86, -25), S(58, -22), S(198, -137), S(167, -140)
    },
    // KingPST
    {
        S(-20, -89), S(100, -75), S(58, -52), S(-60, -48), S(-98, -72), S(-119, -58), S(116, -105), S(168, -147),
        S(72, -103), S(45, -54), S(17, -16), S(19, -17), S(26, -24), S(-25, -36), S(126, -61), S(186, -73),
        S(13, -56), S(170, -22), S(15, -18), S(-60, 9), S(-96, 1), S(17, -4), S(160, -38), S(-37, -27),
        S(-110, 2), S(205, -34), S(-120, 33), S(-211, 29), S(-311, 40), S(-104, 20), S(-73, 24), S(20, -38),
        S(90, 3), S(93, 63), S(-167, 65), S(-249, 70), S(-272, 51), S(-222, 64), S(-70, 61), S(-99, 24),
        S(75, 16), S(67, 79), S(-31, 107), S(-135, 100), S(-139, 74), S(-107, 40), S(-61, 43), S(31, 38),
        S(90, -101), S(111, 20), S(-14, 64), S(63, 85), S(91, 87), S(17, 60), S(100, 56), S(44, 37),
        S(-131, -240), S(104, -54), S(130, -2), S(53, 114), S(7, 193), S(150, 85), S(229, 57), S(25, -203)
    },
    {
        {},
        {},
        {S(-111, -171), S(-44, -45), S(-13, -4), S(5, 14), S(33, 25), S(50, 43), S(56, 43), S(56, 43), S(56, 43)},
        {S(-6, 78), S(53, 81), S(83, 100), S(94, 126), S(107, 136), S(108, 156), S(113, 161), S(113, 165), S(113, 165), S(113, 165), S(114, 165), S(114, 165), S(164, 165), S(164, 165)},
        {S(-20, 266), S(0, 294), S(8, 298), S(20, 301), S(20, 303), S(22, 316), S(22, 323), S(26, 326), S(26, 334), S(26, 352), S(39, 353), S(39, 358), S(39, 358), S(39, 358), S(45, 358)},
        {S(114, -21), S(118, 203), S(118, 250), S(130, 260), S(130, 317), S(135, 342), S(141, 353), S(141, 366), S(150, 370), S(153, 370), S(153, 372), S(165, 372), S(165, 394), S(165, 394), S(174, 403), S(174, 403), S(174, 403), S(174, 403), S(195, 403), S(216, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 404), S(230, 404)},
        {},
    },
    {S(0, 0), S(-42, 59), S(-9, 76), S(-9, 76), S(0, 252), S(32, 428), S(239, 537), S(0, 0)},
    {S(0, 0), S(2, -17), S(34, 11), S(59, 13), S(70, 45), S(195, 104), S(432, 127), S(0, 0)},
    S(137, 18), // RookOpenFileBonus
    S(64, 18), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(93, 32), // KnightOutpostBonus
    S(118, 30), // BishopOutpostBonus
    S(-37, 0), // TrappedRookByKingPenalty
    S(92, 0), // RookBehindOurPasserBonus
    S(-40, 132), // RookBehindTheirPasserBonus
    S(23, 16), // MinorBehindPawnBonus
    S(7, 2), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-24, -1), // KingProtector
    S(8, 39), // BishopPair
    // Shelter[edge_distance][pawn_rank], rank 0 = no own pawn (semi-open
    // file penalty); ranks 1-6 are relative own-pawn ranks. Mg only.
    {
        {S(-30, 0), S(40, 0), S(20, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(-50, 0), S(75, 0), S(40, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(-100, 0), S(110, 0), S(60, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(-146, 0), S(136, 0), S(75, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
    },
    // UnblockedStorm[edge_distance][storm_rank], rank 0 = no enemy pawn
    // (structurally zero); ranks 1-6 are relative enemy-pawn ranks. Mg
    // only; subtracted at the call site so values stay non-negative.
    {
        {S(0, 0), S(0, 0), S(15, 0), S(50, 0), S(70, 0), S(40, 0), S(15, 0)},
        {S(0, 0), S(0, 0), S(20, 0), S(70, 0), S(100, 0), S(50, 0), S(15, 0)},
        {S(0, 0), S(0, 0), S(30, 0), S(90, 0), S(132, 0), S(60, 0), S(20, 0)},
        {S(0, 0), S(0, 0), S(40, 0), S(80, 0), S(120, 0), S(60, 0), S(20, 0)},
    },
    // BlockedStorm[storm_rank]: file distance dimension collapses out
    // because the rammer is frontally blocked. Mg only; subtracted.
    {S(0, 0), S(0, 0), S(0, 0), S(40, 0), S(88, 0), S(40, 0), S(15, 0)},
    S(-52, 0), // UndefendedKingZoneSq
    S(8, 0), // KingMobilityFactor
    S(24, 13), // KingAttackByKnight
    S(8, 13), // KingAttackByBishop
    S(24, 13), // KingAttackByRook
    S(24, 18), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 4), S(29, 0), S(29, 8), S(29, 8), S(0, 0)}, // KingSafeCheck
    S(13, 0), // KingRingWeakWeight
    S(31, 35), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -38), // DoubledPawnPenalty
    S(0, -10), // BackwardPawnPenalty
    S(-30, -20), // WeakUnopposedPenalty
    S(0, -35), // DoubledIsolatedPenalty
    {S(-66, -32), S(-53, -99)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-11, -34), // PawnIslandPenalty
    {S(24, 0), S(20, 0)}, // CentralPawnBonus
    S(66, 25), // BishopLongDiagonalBonus
    S(0, -4), // BishopXrayPawns
    S(0, 36), // InitiativePasser
    S(0, 53), // InitiativePawnCount
    S(0, 1), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 68), // InitiativeInfiltrate
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
