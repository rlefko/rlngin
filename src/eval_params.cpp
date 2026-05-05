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
    S(183, 50), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(148, 31), S(162, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(212, 5), S(0, 0)},
    S(74, 1), // ThreatByKing
    S(92, 33), // Hanging
    S(137, 4), // WeakQueen
    S(19, 24), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 34), S(0, 34), S(0, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 37), S(0, 67), S(0, 81), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-29, -51), S(-29, -54), S(-29, -84), S(-273, -170), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(17, 25), S(17, 66), S(156, 112), S(157, 356), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-71, 35), S(46, 35), S(51, 35), S(51, 50), S(0, 0)},
    S(0, 54), // RookOn7thBonus
    S(-4, 0), // BadBishop
    S(-5, -11), // BishopPawns
    S(40, 0), // Tempo
    {S(0, 0), S(155, 218), S(833, 568), S(871, 496), S(1270, 808), S(2424, 1757), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-17, -20), S(-11, -6), S(-1, -14), S(32, -7), S(23, -8), S(1, -7), S(-15, -15), S(-23, -29),
        S(-57, -26), S(-35, -23), S(-17, -23), S(1, -29), S(25, -21), S(-13, -16), S(-29, -28), S(-60, -31),
        S(-14, -9), S(-20, -1), S(-12, -25), S(8, -32), S(12, -33), S(-6, -22), S(-12, -10), S(-15, -13),
        S(1, 33), S(-3, 26), S(-7, 2), S(35, -23), S(34, -22), S(2, 3), S(-5, 23), S(3, 28),
        S(-6, 37), S(2, 33), S(25, 16), S(38, 2), S(40, 5), S(25, 15), S(3, 32), S(-10, 34),
        S(-5, 25), S(2, 25), S(22, 27), S(36, 23), S(37, 25), S(22, 27), S(1, 25), S(-6, 25),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-118, -16), S(-95, -26), S(-80, -22), S(-81, -12),
        S(-95, -7), S(-62, -16), S(-47, -18), S(-29, -16),
        S(-80, -7), S(-12, -10), S(-14, -8), S(12, 11),
        S(1, 6), S(21, -1), S(47, 31), S(30, 38),
        S(39, 5), S(60, 17), S(71, 45), S(55, 41),
        S(28, -15), S(69, 22), S(70, 29), S(77, 43),
        S(-7, -37), S(19, -16), S(40, 6), S(63, 34),
        S(-43, -58), S(-2, -27), S(25, -2), S(44, 15)
    },
    // BishopPST (half-board)
    {
        S(-8, -22), S(6, -4), S(-70, 2), S(-31, 3),
        S(9, -37), S(7, -33), S(13, -13), S(-44, 16),
        S(1, -24), S(38, -22), S(-18, -11), S(-7, 19),
        S(33, -27), S(37, -1), S(31, 25), S(20, 8),
        S(1, -6), S(53, 9), S(47, 12), S(32, 16),
        S(20, 9), S(7, 22), S(8, 21), S(20, 19),
        S(-27, -14), S(-36, 0), S(-12, 22), S(-8, 19),
        S(-39, -8), S(-40, -1), S(-31, 6), S(-29, 13)
    },
    // RookPST (half-board)
    {
        S(-53, -14), S(-32, -22), S(-13, -12), S(12, -40),
        S(-60, -35), S(-38, -37), S(-31, -32), S(-12, -42),
        S(-68, -22), S(-28, -21), S(-23, -22), S(-29, -25),
        S(-38, 11), S(-1, 18), S(-15, 21), S(10, -8),
        S(-20, 16), S(26, 32), S(42, 26), S(53, 13),
        S(-3, 25), S(30, 28), S(47, 37), S(57, 16),
        S(-1, -3), S(-5, 7), S(29, 13), S(56, 7),
        S(17, 14), S(17, 17), S(37, 20), S(53, 16)
    },
    // QueenPST (half-board)
    {
        S(-11, -90), S(5, -78), S(35, -72), S(33, -53),
        S(13, -85), S(11, -65), S(27, -39), S(41, -28),
        S(12, -56), S(41, -21), S(39, 1), S(4, 2),
        S(45, -24), S(59, 10), S(30, 34), S(31, 52),
        S(20, -15), S(24, 18), S(29, 35), S(-1, 63),
        S(4, 12), S(2, 36), S(0, 44), S(15, 64),
        S(-65, 9), S(-97, 25), S(-51, 38), S(-29, 47),
        S(-93, 8), S(-77, 22), S(-47, 35), S(-29, 47)
    },
    // KingPST (half-board)
    {
        S(147, -149), S(117, -104), S(37, -65), S(-55, -69),
        S(134, -101), S(92, -55), S(12, -30), S(-12, -20),
        S(34, -67), S(16, -38), S(-8, -18), S(-38, -6),
        S(-7, -32), S(-10, 1), S(-23, 16), S(-40, 19),
        S(-15, 5), S(-18, 37), S(-26, 42), S(-45, 37),
        S(-18, 30), S(-17, 54), S(-24, 57), S(-39, 44),
        S(-22, 36), S(-19, 53), S(-22, 57), S(-25, 48),
        S(-25, 37), S(-22, 48), S(-22, 57), S(-22, 57)
    },
    {
        {},
        {},
        {S(-94, -115), S(-54, -75), S(-21, -36), S(-6, -14), S(9, 9), S(21, 26), S(39, 26), S(51, 26), S(51, 26)},
        {S(15, 41), S(42, 76), S(66, 106), S(77, 128), S(94, 139), S(97, 154), S(109, 154), S(110, 155), S(124, 155), S(124, 155), S(124, 155), S(124, 155), S(137, 155), S(137, 155)},
        {S(-40, 266), S(-10, 274), S(6, 287), S(20, 289), S(20, 299), S(35, 312), S(36, 318), S(45, 325), S(50, 343), S(54, 347), S(54, 353), S(54, 353), S(54, 353), S(54, 353), S(54, 353)},
        {S(73, 291), S(73, 316), S(73, 316), S(85, 333), S(95, 347), S(111, 359), S(111, 373), S(123, 378), S(129, 378), S(143, 390), S(154, 390), S(163, 390), S(164, 390), S(164, 399), S(165, 399), S(168, 403), S(168, 403), S(184, 403), S(185, 403), S(196, 403), S(196, 403), S(221, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(229, 403)},
        {},
    },
    {S(0, 0), S(-30, 11), S(-25, 28), S(-12, 50), S(34, 143), S(159, 360), S(525, 592), S(0, 0)},
    {S(0, 0), S(-6, -5), S(25, 16), S(34, 16), S(69, 29), S(139, 78), S(251, 137), S(0, 0)},
    S(106, 11), // RookOpenFileBonus
    S(58, 11), // RookSemiOpenFileBonus
    S(4, 0), // RookOnQueenFile
    S(65, 13), // KnightOutpostBonus
    S(71, 24), // BishopOutpostBonus
    S(-45, 0), // TrappedRookByKingPenalty
    S(36, 40), // RookBehindOurPasserBonus
    S(-85, 102), // RookBehindTheirPasserBonus
    S(22, 8), // MinorBehindPawnBonus
    S(21, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-15, 0), // KingProtector
    S(21, 16), // BishopPair
    {
        {S(-13, 0), S(126, 0), S(119, 0), S(66, 0), S(75, 0), S(105, 0), S(31, 0)},
        {S(-81, 0), S(189, 0), S(97, 0), S(0, 0), S(0, 0), S(68, 0), S(47, 0)},
        {S(0, 0), S(147, 0), S(64, 0), S(57, 0), S(20, 0), S(122, 0), S(214, 0)},
        {S(0, 0), S(145, 0), S(43, 0), S(58, 0), S(68, 0), S(10, 0), S(135, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(63, 0), S(30, 0), S(36, 0), S(17, 0)},
        {S(0, 0), S(0, 0), S(83, 0), S(38, 0), S(4, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(32, 0), S(163, 0), S(92, 0), S(24, 0), S(7, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(104, 0), S(21, 0), S(2, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(141, 0), S(42, 0), S(0, 0), S(0, 0), S(0, 0)}, // BlockedStorm
    S(-48, 0), // UndefendedKingZoneSq
    S(11, 69), // KingMobilityFactor
    S(30, 34), // KingAttackByKnight
    S(10, 46), // KingAttackByBishop
    S(31, 34), // KingAttackByRook
    S(31, 34), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(35, 531), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(12, 10), // KingRingWeakWeight
    S(16, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -22), // DoubledPawnPenalty
    S(-16, -2), // BackwardPawnPenalty
    S(-29, -31), // WeakUnopposedPenalty
    S(-17, -37), // DoubledIsolatedPenalty
    {S(-22, -38), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-31, 0), // PawnIslandPenalty
    {S(7, 0), S(0, 0)}, // CentralPawnBonus
    S(42, 20), // BishopLongDiagonalBonus
    S(-14, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(67, 19), // SliderOnQueenBishop
    S(36, 0), // SliderOnQueenRook
    S(7, 1), // RestrictedPiece
    S(27, 0), // ThreatByPawnPush
    S(-18, -27), // WeakQueenDefender
    S(67, 0), // KnightOnQueen
    S(-267, -105), // PawnlessFlank
    S(0, 41), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 425), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
