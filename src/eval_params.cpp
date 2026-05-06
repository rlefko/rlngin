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
    S(148, 50), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(112, 5), S(98, 3), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(135, 2), S(0, 0)},
    S(68, 4), // ThreatByKing
    S(67, 21), // Hanging
    S(80, 0), // WeakQueen
    S(19, 18), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 29), S(0, 32), S(0, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 18), S(0, 39), S(0, 70), S(0, 84), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-29, -19), S(-29, -35), S(-29, -59), S(-231, -114), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 58), S(45, 112), S(238, 398), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-15, 44), S(-15, 44), S(23, 44), S(23, 44), S(0, 0)},
    S(0, 46), // RookOn7thBonus
    S(-16, -12), // BadBishop
    S(-5, -11), // BishopPawns
    S(42, 0), // Tempo
    {S(0, 0), S(152, 220), S(732, 637), S(784, 594), S(1135, 922), S(2270, 1839), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-26, -18), S(-13, -14), S(-8, -11), S(13, -31), S(16, -25), S(13, 0), S(-9, -14), S(-31, -21),
        S(-68, -32), S(-50, -24), S(-27, -23), S(-31, -40), S(-7, -23), S(-29, -14), S(-44, -24), S(-76, -31),
        S(-24, -14), S(-28, -10), S(-23, -27), S(5, -29), S(5, -30), S(-14, -23), S(-25, -18), S(-26, -18),
        S(2, 23), S(-16, 14), S(-13, -1), S(49, -9), S(62, -5), S(1, 2), S(-19, 15), S(5, 21),
        S(8, 30), S(9, 31), S(30, 18), S(66, 23), S(71, 25), S(32, 20), S(9, 28), S(6, 32),
        S(4, 21), S(11, 22), S(33, 27), S(56, 33), S(57, 35), S(33, 27), S(10, 22), S(5, 23),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-123, -23), S(-120, -30), S(-116, -18), S(-83, -5),
        S(-109, -19), S(-68, -10), S(-81, -18), S(-57, -9),
        S(-94, -21), S(-44, -6), S(-24, -8), S(15, 6),
        S(9, 6), S(15, 3), S(37, 25), S(16, 33),
        S(35, 12), S(56, 11), S(62, 31), S(67, 30),
        S(40, -3), S(64, 7), S(83, 23), S(91, 34),
        S(25, -24), S(36, -16), S(59, -1), S(77, 28),
        S(8, -29), S(28, -11), S(50, 6), S(63, 15)
    },
    // BishopPST (half-board)
    {
        S(-8, -13), S(15, 6), S(-81, 1), S(-68, -12),
        S(9, -19), S(-5, -39), S(0, -2), S(-57, 6),
        S(-13, -19), S(15, -13), S(-20, -8), S(6, 17),
        S(23, -15), S(30, -1), S(32, 19), S(20, 13),
        S(-17, -25), S(47, -1), S(50, 13), S(43, 15),
        S(19, 5), S(23, 23), S(18, 14), S(35, 22),
        S(-12, -8), S(-27, -6), S(-4, 20), S(13, 16),
        S(-19, -10), S(-19, 7), S(-17, 4), S(-11, 5)
    },
    // RookPST (half-board)
    {
        S(-86, -28), S(-46, -29), S(-39, -25), S(2, -43),
        S(-80, -32), S(-26, -37), S(-34, -30), S(-7, -38),
        S(-48, -24), S(-8, -23), S(-6, -24), S(2, -20),
        S(-17, 2), S(13, 4), S(3, 9), S(17, -3),
        S(-26, 12), S(25, 34), S(41, 22), S(54, 16),
        S(-17, 24), S(32, 34), S(55, 41), S(69, 29),
        S(-14, -11), S(0, 6), S(26, 10), S(47, 11),
        S(-2, 15), S(13, 32), S(30, 32), S(36, 24)
    },
    // QueenPST (half-board)
    {
        S(4, -78), S(25, -81), S(14, -77), S(25, -53),
        S(21, -84), S(21, -76), S(37, -54), S(33, -38),
        S(21, -52), S(45, -20), S(49, -21), S(34, -10),
        S(37, -16), S(58, 3), S(37, 30), S(25, 44),
        S(10, -13), S(35, 9), S(7, 35), S(-6, 72),
        S(-2, 14), S(-6, 32), S(-16, 56), S(-10, 89),
        S(-44, 13), S(-79, 21), S(-52, 45), S(-43, 60),
        S(-79, 7), S(-76, 25), S(-66, 39), S(-52, 60)
    },
    // KingPST (half-board)
    {
        S(126, -169), S(83, -113), S(32, -80), S(-66, -72),
        S(109, -110), S(72, -76), S(1, -58), S(-11, -47),
        S(10, -70), S(0, -50), S(-20, -36), S(-27, -24),
        S(-21, -25), S(-15, -4), S(-23, 2), S(-33, 6),
        S(-15, 20), S(-15, 36), S(-17, 37), S(-25, 31),
        S(-11, 48), S(-8, 65), S(-11, 63), S(-17, 58),
        S(-15, 56), S(-11, 73), S(-12, 74), S(-15, 71),
        S(-14, 62), S(-11, 71), S(-9, 76), S(-10, 75)
    },
    {
        {},
        {},
        {S(-93, -123), S(-53, -83), S(-31, -43), S(-17, -13), S(-2, 5), S(9, 23), S(21, 26), S(25, 26), S(25, 26)},
        {S(27, 65), S(49, 68), S(72, 97), S(80, 117), S(93, 140), S(97, 151), S(109, 151), S(112, 155), S(113, 155), S(114, 155), S(114, 155), S(114, 155), S(125, 155), S(125, 155)},
        {S(-34, 273), S(-15, 277), S(1, 292), S(16, 292), S(16, 303), S(29, 313), S(34, 321), S(44, 326), S(46, 340), S(52, 341), S(54, 348), S(54, 352), S(54, 353), S(54, 353), S(54, 353)},
        {S(95, 267), S(95, 267), S(95, 292), S(99, 317), S(100, 329), S(113, 354), S(113, 360), S(124, 371), S(126, 380), S(139, 386), S(141, 389), S(155, 393), S(156, 403), S(156, 403), S(162, 403), S(165, 403), S(168, 403), S(170, 403), S(175, 403), S(180, 403), S(186, 403), S(186, 403), S(199, 403), S(199, 403), S(199, 403), S(199, 403), S(220, 403), S(245, 428)},
        {},
    },
    {S(0, 0), S(-22, 11), S(-22, 19), S(-22, 43), S(15, 151), S(138, 367), S(446, 579), S(0, 0)},
    {S(0, 0), S(-14, 1), S(25, 11), S(34, 11), S(79, 23), S(79, 76), S(79, 167), S(0, 0)},
    S(83, 15), // RookOpenFileBonus
    S(44, 15), // RookSemiOpenFileBonus
    S(6, 0), // RookOnQueenFile
    S(50, 14), // KnightOutpostBonus
    S(64, 18), // BishopOutpostBonus
    S(-44, 0), // TrappedRookByKingPenalty
    S(24, 29), // RookBehindOurPasserBonus
    S(-47, 100), // RookBehindTheirPasserBonus
    S(27, 12), // MinorBehindPawnBonus
    S(32, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-13, 0), // KingProtector
    S(34, 29), // BishopPair
    {
        {S(-8, 0), S(103, 0), S(110, 0), S(86, 0), S(67, 0), S(89, 0), S(29, 0)},
        {S(-47, 0), S(171, 0), S(86, 0), S(37, 0), S(0, 0), S(104, 0), S(0, 0)},
        {S(0, 0), S(120, 0), S(44, 0), S(58, 0), S(14, 0), S(92, 0), S(241, 0)},
        {S(0, 0), S(120, 0), S(20, 0), S(40, 0), S(70, 0), S(28, 0), S(110, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(69, 0), S(11, 0), S(10, 0), S(15, 0)},
        {S(0, 0), S(0, 0), S(75, 0), S(39, 0), S(6, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(191, 0), S(92, 0), S(17, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(54, 0), S(56, 0), S(18, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(165, 0), S(20, 0), S(26, 0), S(11, 0), S(23, 0)}, // BlockedStorm
    S(-32, 0), // UndefendedKingZoneSq
    S(13, 50), // KingMobilityFactor
    S(27, 35), // KingAttackByKnight
    S(4, 12), // KingAttackByBishop
    S(27, 35), // KingAttackByRook
    S(27, 50), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(35, 531), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(7, 0), // KingRingWeakWeight
    S(9, 0), // KingNoQueenDiscount
    S(0, -1), // IsolatedPawnPenalty
    S(0, -21), // DoubledPawnPenalty
    S(-11, -4), // BackwardPawnPenalty
    S(-28, -21), // WeakUnopposedPenalty
    S(-26, -47), // DoubledIsolatedPenalty
    {S(0, -25), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-42, -11), // PawnIslandPenalty
    {S(12, 0), S(9, 0)}, // CentralPawnBonus
    S(33, 14), // BishopLongDiagonalBonus
    S(-16, 0), // BishopXrayPawns
    S(0, 15), // InitiativePasser
    S(0, 18), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 24), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(45, 9), // SliderOnQueenBishop
    S(19, 0), // SliderOnQueenRook
    S(10, 3), // RestrictedPiece
    S(25, 0), // ThreatByPawnPush
    S(-12, -20), // WeakQueenDefender
    S(47, 6), // KnightOnQueen
    S(-232, -79), // PawnlessFlank
    S(0, 46), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 723), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
