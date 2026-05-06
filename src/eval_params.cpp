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
    S(170, 57), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(135, 29), S(160, 3), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(196, 0), S(0, 0)},
    S(62, 3), // ThreatByKing
    S(86, 33), // Hanging
    S(127, 53), // WeakQueen
    S(21, 23), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 26), S(0, 34), S(0, 34), S(0, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 22), S(0, 42), S(0, 75), S(0, 87), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-29, -39), S(-29, -52), S(-29, -77), S(-298, -135), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(17, 19), S(23, 61), S(50, 125), S(192, 349), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-55, 39), S(14, 44), S(29, 44), S(29, 44), S(0, 0)},
    S(0, 43), // RookOn7thBonus
    S(-5, -7), // BadBishop
    S(-4, -11), // BishopPawns
    S(40, 0), // Tempo
    {S(0, 0), S(139, 208), S(795, 614), S(820, 541), S(1198, 834), S(2391, 1786), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-7, -22), S(-5, -16), S(2, -20), S(30, -7), S(21, -10), S(10, -7), S(-9, -19), S(-6, -27),
        S(-45, -26), S(-28, -26), S(-9, -31), S(9, -29), S(34, -14), S(-14, -17), S(-24, -25), S(-49, -27),
        S(-13, -9), S(-22, -4), S(-10, -25), S(11, -27), S(12, -27), S(0, -22), S(-13, -14), S(-17, -12),
        S(3, 33), S(-12, 20), S(-3, -2), S(19, -26), S(18, -23), S(5, -3), S(-15, 16), S(1, 29),
        S(-6, 37), S(-2, 31), S(21, 14), S(28, 5), S(33, 10), S(21, 13), S(-1, 30), S(-13, 35),
        S(-7, 24), S(-2, 24), S(19, 29), S(30, 23), S(31, 25), S(18, 28), S(-3, 25), S(-8, 26),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-105, -16), S(-86, -26), S(-76, -10), S(-55, -1),
        S(-82, -10), S(-39, -6), S(-47, -18), S(-29, -15),
        S(-72, -11), S(-12, -7), S(-9, -11), S(10, 6),
        S(10, 9), S(25, -2), S(43, 21), S(22, 29),
        S(40, 14), S(60, 10), S(71, 33), S(53, 33),
        S(24, -12), S(56, 10), S(76, 25), S(72, 39),
        S(-18, -37), S(9, -14), S(33, -2), S(50, 30),
        S(-62, -62), S(-13, -23), S(19, -1), S(40, 16)
    },
    // BishopPST (half-board)
    {
        S(-6, -32), S(-4, -2), S(-73, 3), S(-33, -9),
        S(-1, -38), S(11, -36), S(8, -10), S(-41, 16),
        S(10, -25), S(38, -26), S(-8, -15), S(-9, 24),
        S(33, -15), S(39, -7), S(22, 27), S(17, 10),
        S(14, -18), S(58, 5), S(53, 7), S(29, 11),
        S(31, 12), S(16, 32), S(3, 25), S(15, 18),
        S(-22, -10), S(-36, 4), S(-28, 20), S(-15, 15),
        S(-26, -1), S(-33, 7), S(-37, 8), S(-35, 13)
    },
    // RookPST (half-board)
    {
        S(-52, -12), S(-27, -17), S(-11, -14), S(14, -36),
        S(-70, -29), S(-40, -25), S(-31, -24), S(-8, -42),
        S(-72, -11), S(-31, -15), S(-22, -20), S(-24, -23),
        S(-41, 20), S(-8, 13), S(-17, 18), S(10, -4),
        S(-15, 5), S(32, 27), S(41, 17), S(58, 2),
        S(-7, 13), S(31, 25), S(44, 32), S(52, 17),
        S(3, -9), S(-5, 9), S(20, 9), S(50, 6),
        S(6, 1), S(14, 15), S(27, 14), S(49, 7)
    },
    // QueenPST (half-board)
    {
        S(5, -91), S(25, -80), S(44, -95), S(45, -58),
        S(30, -89), S(28, -73), S(37, -50), S(45, -37),
        S(27, -65), S(47, -30), S(51, -17), S(26, -13),
        S(41, -27), S(57, 8), S(30, 32), S(36, 52),
        S(27, -9), S(31, 26), S(12, 46), S(-18, 79),
        S(-9, 17), S(-5, 45), S(-22, 60), S(-7, 82),
        S(-77, 17), S(-102, 30), S(-66, 51), S(-49, 59),
        S(-101, 12), S(-83, 29), S(-61, 39), S(-45, 59)
    },
    // KingPST (half-board)
    {
        S(141, -150), S(116, -102), S(33, -63), S(-74, -66),
        S(110, -89), S(79, -52), S(1, -33), S(-19, -23),
        S(31, -59), S(9, -36), S(-8, -17), S(-33, -7),
        S(-3, -24), S(-5, 3), S(-19, 14), S(-36, 17),
        S(-10, 11), S(-9, 38), S(-25, 40), S(-42, 33),
        S(-15, 34), S(-11, 56), S(-23, 52), S(-43, 35),
        S(-19, 36), S(-13, 55), S(-17, 58), S(-23, 48),
        S(-21, 37), S(-16, 48), S(-14, 55), S(-14, 58)
    },
    {
        {},
        {},
        {S(-90, -121), S(-50, -81), S(-27, -41), S(-11, -13), S(4, 3), S(12, 22), S(27, 26), S(34, 26), S(34, 26)},
        {S(12, 53), S(47, 72), S(67, 107), S(75, 126), S(91, 139), S(94, 152), S(105, 155), S(108, 155), S(114, 155), S(114, 155), S(114, 155), S(114, 155), S(140, 155), S(140, 155)},
        {S(-40, 267), S(-11, 280), S(8, 288), S(20, 288), S(20, 298), S(31, 315), S(37, 319), S(45, 325), S(47, 340), S(54, 343), S(54, 351), S(54, 353), S(54, 353), S(54, 353), S(54, 353)},
        {S(84, 265), S(84, 290), S(84, 304), S(95, 321), S(99, 346), S(113, 359), S(113, 365), S(124, 379), S(130, 379), S(140, 390), S(148, 390), S(158, 393), S(161, 397), S(164, 399), S(164, 399), S(164, 402), S(164, 403), S(170, 403), S(174, 403), S(186, 403), S(200, 403), S(200, 403), S(221, 403), S(221, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403)},
        {},
    },
    {S(0, 0), S(-22, 17), S(-22, 30), S(-14, 57), S(34, 164), S(150, 393), S(543, 607), S(0, 0)},
    {S(0, 0), S(-5, -7), S(26, 13), S(32, 13), S(75, 25), S(100, 83), S(158, 98), S(0, 0)},
    S(92, 16), // RookOpenFileBonus
    S(48, 16), // RookSemiOpenFileBonus
    S(3, 0), // RookOnQueenFile
    S(63, 13), // KnightOutpostBonus
    S(76, 23), // BishopOutpostBonus
    S(-50, 0), // TrappedRookByKingPenalty
    S(37, 29), // RookBehindOurPasserBonus
    S(-76, 118), // RookBehindTheirPasserBonus
    S(18, 8), // MinorBehindPawnBonus
    S(26, 0), // MinorOnKingRing
    S(4, 0), // RookOnKingRing
    S(-18, -2), // KingProtector
    S(12, 27), // BishopPair
    {
        {S(-23, 0), S(110, 0), S(108, 0), S(70, 0), S(75, 0), S(105, 0), S(0, 0)},
        {S(-74, 0), S(186, 0), S(94, 0), S(7, 0), S(0, 0), S(192, 0), S(0, 0)},
        {S(0, 0), S(135, 0), S(66, 0), S(54, 0), S(14, 0), S(117, 0), S(259, 0)},
        {S(0, 0), S(145, 0), S(32, 0), S(57, 0), S(69, 0), S(13, 0), S(143, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(29, 0), S(62, 0), S(20, 0), S(23, 0), S(1, 0)},
        {S(0, 0), S(0, 0), S(90, 0), S(29, 0), S(24, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(224, 0), S(76, 0), S(23, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(39, 0), S(68, 0), S(8, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(169, 0), S(31, 0), S(28, 0), S(11, 0), S(0, 0)}, // BlockedStorm
    S(-36, 0), // UndefendedKingZoneSq
    S(11, 44), // KingMobilityFactor
    S(30, 41), // KingAttackByKnight
    S(7, 15), // KingAttackByBishop
    S(30, 41), // KingAttackByRook
    S(30, 56), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(35, 531), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(13, 6), // KingRingWeakWeight
    S(21, 0), // KingNoQueenDiscount
    S(-2, -2), // IsolatedPawnPenalty
    S(0, -18), // DoubledPawnPenalty
    S(-7, -3), // BackwardPawnPenalty
    S(-20, -29), // WeakUnopposedPenalty
    S(-12, -40), // DoubledIsolatedPenalty
    {S(-20, -37), S(-6, -7)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-25, 0), // PawnIslandPenalty
    {S(8, 0), S(0, 0)}, // CentralPawnBonus
    S(34, 18), // BishopLongDiagonalBonus
    S(-14, 0), // BishopXrayPawns
    S(0, 21), // InitiativePasser
    S(0, 18), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 29), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(51, 20), // SliderOnQueenBishop
    S(34, 0), // SliderOnQueenRook
    S(4, 4), // RestrictedPiece
    S(23, 0), // ThreatByPawnPush
    S(-13, -34), // WeakQueenDefender
    S(67, 0), // KnightOnQueen
    S(-255, -102), // PawnlessFlank
    S(0, 34), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 135), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
