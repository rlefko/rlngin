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
    S(172, 53), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(142, 31), S(152, 8), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(197, 18), S(0, 0)},
    S(57, 5), // ThreatByKing
    S(87, 36), // Hanging
    S(161, 28), // WeakQueen
    S(18, 24), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 34), S(0, 34), S(0, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 39), S(0, 71), S(0, 83), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-29, -49), S(-29, -53), S(-29, -81), S(-265, -170), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(21, 23), S(21, 65), S(117, 118), S(189, 352), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-68, 35), S(37, 35), S(37, 48), S(91, 58), S(0, 0)},
    S(0, 50), // RookOn7thBonus
    S(-3, 0), // BadBishop
    S(-5, -11), // BishopPawns
    S(38, 0), // Tempo
    {S(0, 0), S(146, 221), S(809, 597), S(844, 522), S(1238, 842), S(2390, 1806), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-16, -22), S(-11, -8), S(0, -14), S(29, -7), S(21, -9), S(2, -7), S(-15, -17), S(-22, -30),
        S(-55, -27), S(-34, -24), S(-17, -24), S(1, -30), S(24, -20), S(-14, -16), S(-28, -29), S(-57, -32),
        S(-12, -12), S(-19, -3), S(-12, -25), S(7, -30), S(11, -31), S(-7, -22), S(-10, -11), S(-11, -14),
        S(0, 34), S(-3, 24), S(-9, 4), S(33, -23), S(33, -22), S(0, 4), S(-5, 22), S(1, 29),
        S(-7, 34), S(2, 31), S(24, 18), S(39, 4), S(42, 8), S(25, 17), S(3, 30), S(-11, 32),
        S(-7, 23), S(2, 23), S(21, 25), S(35, 19), S(36, 21), S(21, 25), S(1, 23), S(-8, 23),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-113, -16), S(-92, -23), S(-80, -17), S(-79, -6),
        S(-92, -10), S(-57, -16), S(-47, -15), S(-31, -13),
        S(-76, -10), S(-14, -12), S(-16, -8), S(11, 7),
        S(3, 9), S(20, -2), S(45, 30), S(26, 38),
        S(36, 3), S(59, 16), S(69, 46), S(53, 38),
        S(27, -14), S(68, 22), S(69, 31), S(73, 42),
        S(-9, -36), S(19, -14), S(41, 2), S(62, 30),
        S(-45, -60), S(-3, -28), S(25, -5), S(41, 11)
    },
    // BishopPST (half-board)
    {
        S(-5, -23), S(8, 0), S(-68, 8), S(-27, 5),
        S(7, -39), S(6, -32), S(15, -16), S(-44, 16),
        S(0, -28), S(36, -24), S(-21, -9), S(-8, 19),
        S(33, -20), S(38, -5), S(29, 25), S(22, 9),
        S(1, -7), S(54, 8), S(47, 13), S(27, 17),
        S(22, 11), S(6, 25), S(5, 24), S(18, 22),
        S(-27, -20), S(-36, -4), S(-14, 23), S(-10, 19),
        S(-40, -10), S(-38, -4), S(-32, 2), S(-26, 12)
    },
    // RookPST (half-board)
    {
        S(-53, -16), S(-33, -22), S(-10, -17), S(13, -41),
        S(-59, -31), S(-39, -34), S(-31, -34), S(-6, -42),
        S(-65, -19), S(-27, -21), S(-15, -26), S(-26, -26),
        S(-36, 12), S(2, 16), S(-9, 19), S(10, -11),
        S(-21, 13), S(26, 32), S(44, 22), S(57, 10),
        S(-8, 26), S(29, 29), S(48, 37), S(54, 15),
        S(1, -2), S(-7, 7), S(24, 14), S(52, 8),
        S(11, 13), S(16, 21), S(34, 22), S(49, 17)
    },
    // QueenPST (half-board)
    {
        S(-6, -89), S(9, -80), S(41, -81), S(34, -54),
        S(18, -87), S(13, -66), S(30, -45), S(41, -28),
        S(15, -51), S(40, -24), S(42, -7), S(3, 2),
        S(46, -24), S(60, 3), S(34, 26), S(29, 54),
        S(16, -5), S(22, 15), S(33, 30), S(-1, 62),
        S(0, 16), S(0, 39), S(-1, 47), S(7, 69),
        S(-66, 21), S(-97, 35), S(-51, 46), S(-33, 52),
        S(-96, 12), S(-79, 26), S(-56, 37), S(-41, 47)
    },
    // KingPST (half-board)
    {
        S(143, -151), S(110, -104), S(39, -67), S(-51, -69),
        S(126, -100), S(83, -56), S(13, -31), S(-4, -22),
        S(26, -64), S(11, -37), S(-10, -18), S(-33, -7),
        S(-6, -32), S(-12, 1), S(-20, 16), S(-40, 18),
        S(-15, 3), S(-17, 34), S(-27, 41), S(-45, 37),
        S(-15, 30), S(-14, 54), S(-27, 56), S(-44, 44),
        S(-17, 38), S(-16, 55), S(-22, 58), S(-28, 50),
        S(-20, 41), S(-20, 51), S(-21, 58), S(-21, 59)
    },
    {
        {},
        {},
        {S(-94, -121), S(-54, -81), S(-22, -41), S(-7, -15), S(6, 8), S(18, 25), S(35, 26), S(47, 26), S(47, 26)},
        {S(17, 45), S(44, 78), S(66, 107), S(78, 128), S(93, 142), S(95, 155), S(108, 155), S(109, 155), S(122, 155), S(124, 155), S(124, 155), S(124, 155), S(137, 155), S(137, 155)},
        {S(-39, 271), S(-9, 273), S(6, 285), S(20, 285), S(20, 299), S(35, 311), S(35, 319), S(43, 324), S(50, 341), S(53, 346), S(54, 353), S(54, 353), S(54, 353), S(54, 353), S(54, 353)},
        {S(73, 315), S(73, 316), S(73, 316), S(86, 329), S(97, 342), S(111, 360), S(111, 371), S(123, 379), S(128, 379), S(144, 385), S(153, 390), S(164, 390), S(164, 390), S(164, 399), S(164, 399), S(164, 403), S(168, 403), S(179, 403), S(192, 403), S(196, 403), S(196, 403), S(207, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(229, 403)},
        {},
    },
    {S(0, 0), S(-28, 9), S(-22, 26), S(-9, 53), S(37, 149), S(158, 375), S(521, 602), S(0, 0)},
    {S(0, 0), S(-6, -5), S(24, 16), S(32, 16), S(64, 29), S(134, 75), S(219, 137), S(0, 0)},
    S(99, 11), // RookOpenFileBonus
    S(56, 11), // RookSemiOpenFileBonus
    S(3, 0), // RookOnQueenFile
    S(62, 10), // KnightOutpostBonus
    S(70, 23), // BishopOutpostBonus
    S(-43, 0), // TrappedRookByKingPenalty
    S(27, 42), // RookBehindOurPasserBonus
    S(-89, 109), // RookBehindTheirPasserBonus
    S(21, 5), // MinorBehindPawnBonus
    S(20, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-15, 0), // KingProtector
    S(14, 21), // BishopPair
    {
        {S(-14, 0), S(124, 0), S(118, 0), S(64, 0), S(73, 0), S(118, 0), S(7, 0)},
        {S(-74, 0), S(187, 0), S(97, 0), S(0, 0), S(0, 0), S(70, 0), S(3, 0)},
        {S(0, 0), S(142, 0), S(65, 0), S(59, 0), S(22, 0), S(122, 0), S(250, 0)},
        {S(0, 0), S(154, 0), S(37, 0), S(58, 0), S(74, 0), S(12, 0), S(143, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(12, 0), S(64, 0), S(23, 0), S(31, 0), S(15, 0)},
        {S(0, 0), S(0, 0), S(83, 0), S(37, 0), S(4, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(12, 0), S(145, 0), S(89, 0), S(22, 0), S(8, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(78, 0), S(22, 0), S(0, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(136, 0), S(49, 0), S(0, 0), S(0, 0), S(0, 0)}, // BlockedStorm
    S(-43, 0), // UndefendedKingZoneSq
    S(11, 15), // KingMobilityFactor
    S(29, 18), // KingAttackByKnight
    S(10, 34), // KingAttackByBishop
    S(30, 34), // KingAttackByRook
    S(30, 46), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(34, 531), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(12, 0), // KingRingWeakWeight
    S(16, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -24), // DoubledPawnPenalty
    S(-15, -1), // BackwardPawnPenalty
    S(-28, -31), // WeakUnopposedPenalty
    S(-19, -36), // DoubledIsolatedPenalty
    {S(-21, -40), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-31, 0), // PawnIslandPenalty
    {S(6, 0), S(0, 0)}, // CentralPawnBonus
    S(40, 19), // BishopLongDiagonalBonus
    S(-13, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(57, 29), // SliderOnQueenBishop
    S(37, 0), // SliderOnQueenRook
    S(7, 1), // RestrictedPiece
    S(26, 0), // ThreatByPawnPush
    S(-17, -28), // WeakQueenDefender
    S(57, 0), // KnightOnQueen
    S(-265, -104), // PawnlessFlank
    S(0, 41), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 353), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
