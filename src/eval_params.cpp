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
    // KnightPST (half-board: 4 file buckets x 8 ranks)
    {
        S(-244, 58), S(-94, -2), S(-48, -34), S(-74, -11),
        S(-111, 4), S(-1, -1), S(-61, 0), S(-10, 4),
        S(-36, 25), S(34, -14), S(14, 7), S(20, 21),
        S(55, 4), S(85, 30), S(90, 48), S(84, 36),
        S(186, -29), S(84, 22), S(166, 20), S(131, 34),
        S(14, -18), S(88, -2), S(140, -9), S(215, 14),
        S(-72, -82), S(6, -6), S(94, -6), S(34, 20),
        S(-494, -108), S(-59, -10), S(-234, 5), S(18, -19)
    },
    // BishopPST (half-board)
    {
        S(17, -50), S(86, -45), S(-72, 14), S(-72, 0),
        S(86, -76), S(40, -37), S(27, -14), S(-14, 11),
        S(39, -5), S(49, 7), S(-5, 8), S(-2, 44),
        S(64, -20), S(25, 13), S(32, 18), S(56, -4),
        S(-2, 24), S(78, 19), S(89, 10), S(18, -8),
        S(82, -20), S(96, 12), S(48, 4), S(70, 3),
        S(-118, -8), S(-214, 12), S(16, 22), S(-62, 32),
        S(-64, -59), S(8, 6), S(-180, 40), S(-194, 50)
    },
    // RookPST (half-board)
    {
        S(-92, -6), S(-46, -11), S(-26, -9), S(-30, -19),
        S(-140, 4), S(-13, -42), S(-38, -15), S(-72, -12),
        S(-147, 7), S(-16, -26), S(-24, -18), S(-118, -10),
        S(-44, -11), S(-67, 22), S(-63, 17), S(-88, 20),
        S(50, 17), S(29, 26), S(16, 17), S(-40, 11),
        S(11, 25), S(72, 16), S(117, 6), S(136, -4),
        S(62, -11), S(-60, 12), S(128, -13), S(226, -32),
        S(46, 4), S(19, 16), S(74, 18), S(144, -2)
    },
    // QueenPST (half-board)
    {
        S(-86, -128), S(-58, -54), S(-70, -36), S(-6, -20),
        S(-42, -83), S(68, -124), S(10, -36), S(-6, -6),
        S(-54, -16), S(12, -18), S(-5, 38), S(-2, -24),
        S(2, -10), S(-13, 36), S(-2, 58), S(15, 45),
        S(34, 3), S(45, 10), S(15, 73), S(-3, 80),
        S(74, -75), S(90, 8), S(26, 84), S(6, 74),
        S(22, -57), S(-93, 51), S(-54, 110), S(-50, 120),
        S(-6, -49), S(6, -56), S(99, -34), S(50, 12)
    },
    // KingPST (half-board)
    {
        S(74, -118), S(108, -90), S(-30, -55), S(-79, -60),
        S(129, -88), S(86, -58), S(-4, -26), S(22, -20),
        S(-12, -42), S(165, -30), S(16, -11), S(-78, 5),
        S(-45, -18), S(66, -5), S(-112, 26), S(-261, 34),
        S(-4, 14), S(12, 62), S(-194, 64), S(-260, 60),
        S(53, 27), S(3, 61), S(-69, 74), S(-137, 87),
        S(67, -32), S(106, 38), S(2, 62), S(77, 86),
        S(-53, -222), S(166, 2), S(140, 42), S(30, 154)
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
