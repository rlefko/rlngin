#include "eval_params.h"

// Compiled-in defaults for the tunable parameters. The in-memory
// `evalParams` instance is initialized from this struct, and
// `resetEvalParams()` snaps it back to these values after a tune run.
//
// Values below merge the tuned snapshot from main with the structural
// refactor on this branch. Single-field terms (ThreatBy*, Tempo,
// PieceScore, the rank chains, mobility, passed pawn extras, etc.)
// keep main's tuned values verbatim. The structural rewrites convert
// the corresponding tuned values into the new shape:
//   - BadBishopPenalty -> BadBishop + BishopPawns split. Even split
//     so `BadBishop + BishopPawns` reproduces the legacy total at one
//     same-color pawn with no closed center.
//   - Non-pawn PSTs -> 32-entry half-board layout. Each entry is the
//     mean of the matching file pair from main's 64-entry table.
//   - PawnShieldBonus, BlockedPawnStorm, UnblockedPawnStorm, and the
//     Semi/OpenFileNearKing pair -> Shelter[4][7], UnblockedStorm[4][7],
//     BlockedStorm[7]. The dominant peaks of the legacy tables are
//     reproduced at the appropriate (edge_distance, rank) slots; the
//     remaining cells are conservative starting values that the next
//     Texel pass will refine.
//   - KingSafeSqPenalty -> KingMobilityFactor (linear). The next
//     Texel pass will fit the per-square weight against the new
//     accumulator-folded form.
//
// Tuner improvements that produced the underlying main snapshot:
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
    S(237, 27), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(194, 10), S(221, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(266, 22), S(0, 0)},
    S(118, 0), // ThreatByKing
    S(110, 24), // Hanging
    S(14, 0), // WeakQueen
    S(19, 18), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 35), S(0, 35), S(0, 35), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 18), S(0, 35), S(0, 65), S(0, 77), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-36, -18), S(-36, -38), S(-62, -59), S(-358, -105), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(36, 11), S(37, 47), S(131, 95), S(131, 332), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-43, 20), S(46, 28), S(51, 28), S(51, 28), S(0, 0)},
    S(0, 26), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -7), // BishopPawns
    S(50, 0), // Tempo
    {S(0, 0), S(170, 157), S(889, 493), S(926, 422), S(1368, 652), S(2460, 1525), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-22, -7), S(-13, -12), S(-23, -16), S(-4, -12), S(18, 7), S(-11, -10), S(-10, -15), S(-34, -30),
        S(-57, -11), S(-60, -20), S(-21, -31), S(-21, -26), S(16, -20), S(-36, -18), S(-38, -32), S(-58, -27),
        S(-35, 1), S(-46, -13), S(-23, -34), S(7, -34), S(18, -34), S(-1, -30), S(-26, -17), S(-45, -19),
        S(15, 33), S(7, 9), S(2, -9), S(32, -24), S(56, -27), S(24, -20), S(-17, 5), S(-1, 18),
        S(11, 52), S(18, 53), S(38, 8), S(67, 5), S(84, 11), S(50, 1), S(14, 60), S(-21, 61),
        S(18, 32), S(-15, 38), S(26, 26), S(68, 27), S(57, 18), S(28, 26), S(-13, 39), S(11, 37),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-112, -28), S(-140, 0), S(-80, -17), S(-87, 6),
        S(-99, -6), S(-62, -3), S(-36, -6), S(-28, -3),
        S(-59, 2), S(8, -2), S(11, -12), S(17, 19),
        S(-21, 14), S(51, 20), S(61, 22), S(49, 31),
        S(51, 15), S(76, 16), S(98, 23), S(80, 28),
        S(-9, -2), S(39, 7), S(72, 18), S(92, 31),
        S(-30, -38), S(-2, -8), S(41, -3), S(53, 20),
        S(-81, -111), S(-16, -37), S(25, -1), S(35, 5)
    },
    // BishopPST (half-board)
    {
        S(-7, -25), S(12, -21), S(-91, 5), S(-44, -1),
        S(9, -25), S(22, -22), S(22, -11), S(-25, 2),
        S(-3, -9), S(51, 8), S(9, -7), S(-13, 22),
        S(10, -7), S(29, -2), S(37, 13), S(25, 3),
        S(-23, -5), S(51, 19), S(56, 9), S(34, 5),
        S(31, 0), S(21, 18), S(44, 8), S(36, 9),
        S(-31, -22), S(-59, -9), S(-14, 9), S(-17, -4),
        S(-31, 11), S(-45, 18), S(-34, 12), S(-31, 9)
    },
    // RookPST (half-board)
    {
        S(-45, -17), S(-25, -9), S(-5, -6), S(7, -11),
        S(-71, -29), S(-35, -27), S(-22, -12), S(-28, -14),
        S(-69, -17), S(-29, -13), S(-41, -12), S(-50, -13),
        S(-45, 1), S(-18, 6), S(-36, 10), S(-4, -1),
        S(-18, 12), S(13, 24), S(24, 15), S(25, 5),
        S(20, 12), S(18, 21), S(41, 18), S(62, 11),
        S(30, 1), S(15, 7), S(65, -2), S(65, 8),
        S(29, 9), S(34, 16), S(51, 12), S(61, 9)
    },
    // QueenPST (half-board)
    {
        S(9, -78), S(21, -61), S(13, -54), S(51, -48),
        S(20, -57), S(41, -51), S(58, -47), S(72, -36),
        S(17, -41), S(53, -28), S(62, -3), S(20, 5),
        S(51, -27), S(69, 4), S(24, 29), S(22, 33),
        S(33, -22), S(18, 9), S(30, 53), S(-1, 52),
        S(23, -13), S(-20, 24), S(-32, 58), S(-1, 86),
        S(-87, 4), S(-136, 32), S(-78, 59), S(-59, 73),
        S(-106, -16), S(-88, 12), S(-49, 34), S(-45, 32)
    },
    // KingPST (half-board)
    {
        S(141, -97), S(147, -68), S(23, -41), S(-123, -47),
        S(173, -59), S(128, -37), S(3, -14), S(-64, -5),
        S(37, -40), S(47, -20), S(-1, -2), S(-46, 3),
        S(-1, -18), S(4, 7), S(-21, 23), S(-49, 21),
        S(-10, 8), S(-14, 45), S(-29, 47), S(-47, 44),
        S(-19, 18), S(-20, 49), S(-31, 60), S(-39, 56),
        S(-31, -9), S(-21, 28), S(-20, 38), S(-29, 47),
        S(-31, -48), S(-21, 0), S(-24, -5), S(-28, 27)
    },
    {
        {},
        {},
        {S(-95, -95), S(-55, -55), S(-15, -18), S(4, -6), S(21, 7), S(34, 20), S(54, 22), S(60, 27), S(60, 27)},
        {S(-16, 53), S(19, 88), S(54, 118), S(77, 130), S(98, 135), S(104, 146), S(113, 150), S(114, 153), S(124, 153), S(124, 155), S(124, 155), S(124, 159), S(128, 159), S(128, 159)},
        {S(-37, 260), S(-7, 290), S(20, 298), S(20, 311), S(20, 311), S(22, 323), S(24, 326), S(35, 331), S(44, 332), S(53, 341), S(53, 347), S(53, 348), S(53, 351), S(54, 353), S(54, 353)},
        {S(11, 291), S(36, 316), S(61, 341), S(86, 344), S(107, 345), S(121, 349), S(131, 357), S(147, 357), S(148, 372), S(156, 374), S(157, 381), S(171, 381), S(171, 390), S(173, 394), S(175, 403), S(175, 403), S(179, 403), S(193, 403), S(193, 403), S(204, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403)},
        {},
    },
    {S(0, 0), S(-37, 28), S(-37, 30), S(-31, 69), S(4, 132), S(122, 324), S(561, 509), S(0, 0)},
    {S(0, 0), S(-3, -8), S(43, 6), S(47, 10), S(77, 23), S(81, 84), S(386, 132), S(0, 0)},
    S(127, 8), // RookOpenFileBonus
    S(54, 8), // RookSemiOpenFileBonus
    S(7, 0), // RookOnQueenFile
    S(70, 7), // KnightOutpostBonus
    S(79, 14), // BishopOutpostBonus
    S(-78, 0), // TrappedRookByKingPenalty
    S(79, 6), // RookBehindOurPasserBonus
    S(-59, 88), // RookBehindTheirPasserBonus
    S(26, 5), // MinorBehindPawnBonus
    S(25, 0), // MinorOnKingRing
    S(1, 0), // RookOnKingRing
    S(-17, -3), // KingProtector
    S(6, 0), // BishopPair
    {
        {S(0, 0), S(150, 0), S(143, 0), S(98, 0), S(76, 0), S(63, 0), S(2, 0)},
        {S(-92, 0), S(209, 0), S(125, 0), S(5, 0), S(0, 0), S(23, 0), S(116, 0)},
        {S(-1, 0), S(184, 0), S(63, 0), S(35, 0), S(30, 0), S(115, 0), S(83, 0)},
        {S(0, 0), S(107, 0), S(92, 0), S(77, 0), S(43, 0), S(58, 0), S(30, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(3, 0), S(7, 0), S(0, 0), S(5, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(109, 0), S(18, 0), S(6, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(109, 0), S(304, 0), S(66, 0), S(21, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(20, 0), S(268, 0), S(66, 0), S(10, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(137, 0), S(11, 0), S(0, 0), S(0, 0), S(0, 0)}, // BlockedStorm
    S(-49, 0), // UndefendedKingZoneSq
    S(9, 71), // KingMobilityFactor
    S(31, 32), // KingAttackByKnight
    S(11, 56), // KingAttackByBishop
    S(31, 34), // KingAttackByRook
    S(31, 34), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(33, 418), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(14, 13), // KingRingWeakWeight
    S(23, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -14), // DoubledPawnPenalty
    S(-12, -6), // BackwardPawnPenalty
    S(-22, -15), // WeakUnopposedPenalty
    S(-6, -31), // DoubledIsolatedPenalty
    {S(-66, -25), S(0, -45)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-13, -14), // PawnIslandPenalty
    {S(16, 0), S(0, 0)}, // CentralPawnBonus
    S(47, 20), // BishopLongDiagonalBonus
    S(-17, 0), // BishopXrayPawns
    S(0, 42), // InitiativePasser
    S(0, 51), // InitiativePawnCount
    S(0, 2), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 38), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(70, 6), // SliderOnQueenBishop
    S(47, 0), // SliderOnQueenRook
    S(5, 2), // RestrictedPiece
    S(37, 0), // ThreatByPawnPush
    S(-33, -14), // WeakQueenDefender
    S(83, 0), // KnightOnQueen
    S(-272, -108), // PawnlessFlank
    S(1, 9), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 78), // KBNKCornerEg
    S(0, 515), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
