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
    S(238, 27), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(183, 11), S(224, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(281, 10), S(0, 0)},
    S(109, 0), // ThreatByKing
    S(110, 26), // Hanging
    S(54, 0), // WeakQueen
    S(17, 19), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 34), S(0, 34), S(0, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 17), S(0, 35), S(0, 65), S(0, 77), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-35, -17), S(-35, -39), S(-44, -60), S(-344, -104), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(29, 13), S(36, 43), S(127, 95), S(127, 306), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-46, 20), S(51, 28), S(51, 28), S(51, 28), S(0, 0)},
    S(0, 28), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -6), // BishopPawns
    S(52, 0), // Tempo
    {S(0, 0), S(168, 150), S(892, 487), S(927, 417), S(1360, 638), S(2470, 1487), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-23, -8), S(-12, -9), S(-18, -11), S(2, -4), S(9, 4), S(-8, -6), S(-12, -11), S(-29, -23),
        S(-53, -13), S(-50, -19), S(-20, -25), S(-9, -24), S(11, -16), S(-24, -16), S(-42, -27), S(-52, -21),
        S(-34, 0), S(-38, -11), S(-19, -29), S(12, -30), S(15, -30), S(-10, -27), S(-32, -13), S(-39, -12),
        S(6, 31), S(-3, 11), S(5, -8), S(34, -21), S(43, -21), S(9, -13), S(-13, 9), S(-2, 23),
        S(3, 38), S(11, 34), S(25, 2), S(50, 1), S(52, 3), S(26, 1), S(10, 35), S(-1, 40),
        S(8, 28), S(11, 27), S(29, 16), S(46, 17), S(46, 16), S(29, 16), S(11, 27), S(7, 29),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-115, -19), S(-130, -7), S(-78, -16), S(-77, 1),
        S(-93, -8), S(-68, -4), S(-39, -6), S(-28, -4),
        S(-54, 0), S(4, -2), S(10, -11), S(17, 16),
        S(-15, 13), S(41, 16), S(57, 20), S(50, 26),
        S(42, 13), S(68, 16), S(90, 22), S(76, 27),
        S(11, -6), S(40, 6), S(67, 17), S(84, 28),
        S(-21, -36), S(9, -10), S(40, 1), S(50, 18),
        S(-55, -87), S(-9, -31), S(20, -3), S(32, 6)
    },
    // BishopPST (half-board)
    {
        S(0, -25), S(6, -24), S(-88, -1), S(-47, -1),
        S(8, -22), S(21, -23), S(12, -9), S(-28, 2),
        S(-1, -10), S(43, 5), S(10, -7), S(-15, 17),
        S(8, -8), S(29, 0), S(34, 12), S(22, 1),
        S(-14, -2), S(42, 19), S(46, 11), S(32, 4),
        S(18, 0), S(19, 15), S(33, 11), S(30, 9),
        S(-25, -19), S(-41, -6), S(-15, 9), S(-13, 1),
        S(-33, 5), S(-35, 13), S(-29, 13), S(-25, 10)
    },
    // RookPST (half-board)
    {
        S(-44, -17), S(-26, -9), S(-3, -7), S(6, -11),
        S(-65, -28), S(-35, -26), S(-19, -14), S(-28, -15),
        S(-62, -19), S(-32, -14), S(-40, -12), S(-43, -14),
        S(-40, 0), S(-19, 5), S(-27, 9), S(-7, 0),
        S(-12, 9), S(12, 21), S(20, 14), S(24, 2),
        S(21, 10), S(21, 18), S(40, 16), S(53, 11),
        S(28, -1), S(21, 4), S(54, -2), S(57, 6),
        S(30, 9), S(32, 14), S(48, 10), S(57, 7)
    },
    // QueenPST (half-board)
    {
        S(2, -69), S(17, -57), S(19, -55), S(51, -50),
        S(23, -55), S(38, -51), S(56, -43), S(69, -34),
        S(27, -45), S(55, -29), S(59, -2), S(22, 3),
        S(50, -25), S(61, 5), S(29, 27), S(20, 33),
        S(37, -24), S(20, 11), S(27, 44), S(6, 50),
        S(16, -12), S(-22, 22), S(-26, 52), S(-3, 78),
        S(-81, 5), S(-121, 27), S(-73, 52), S(-47, 67),
        S(-108, -3), S(-97, 22), S(-67, 39), S(-53, 40)
    },
    // KingPST (half-board)
    {
        S(141, -96), S(149, -72), S(25, -46), S(-111, -54),
        S(165, -59), S(121, -41), S(5, -19), S(-61, -11),
        S(49, -42), S(39, -22), S(-11, -5), S(-47, -1),
        S(4, -17), S(-1, 5), S(-22, 18), S(-45, 17),
        S(-12, 9), S(-12, 34), S(-26, 41), S(-42, 38),
        S(-20, 20), S(-20, 41), S(-24, 52), S(-32, 48),
        S(-25, 10), S(-22, 27), S(-24, 36), S(-24, 42),
        S(-28, 1), S(-23, 14), S(-26, 18), S(-25, 33)
    },
    {
        {},
        {},
        {S(-95, -95), S(-55, -55), S(-16, -17), S(2, -5), S(20, 7), S(36, 18), S(53, 22), S(60, 26), S(60, 26)},
        {S(-16, 53), S(19, 88), S(52, 116), S(76, 128), S(95, 132), S(103, 143), S(111, 147), S(113, 149), S(124, 149), S(124, 151), S(124, 151), S(124, 155), S(128, 155), S(128, 155)},
        {S(-37, 264), S(-7, 294), S(20, 300), S(20, 312), S(20, 312), S(22, 324), S(24, 326), S(33, 332), S(44, 333), S(53, 341), S(53, 347), S(54, 348), S(54, 351), S(54, 353), S(54, 353)},
        {S(11, 291), S(36, 316), S(61, 341), S(86, 341), S(103, 343), S(118, 349), S(129, 357), S(144, 358), S(145, 372), S(154, 375), S(154, 380), S(168, 380), S(171, 387), S(171, 391), S(175, 401), S(175, 403), S(176, 403), S(185, 403), S(185, 403), S(201, 403), S(224, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403)},
        {},
    },
    {S(0, 0), S(-37, 26), S(-37, 29), S(-29, 67), S(8, 132), S(121, 337), S(547, 518), S(0, 0)},
    {S(0, 0), S(-2, -8), S(39, 7), S(44, 10), S(78, 22), S(83, 83), S(418, 124), S(0, 0)},
    S(129, 7), // RookOpenFileBonus
    S(56, 7), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(70, 8), // KnightOutpostBonus
    S(75, 15), // BishopOutpostBonus
    S(-80, 0), // TrappedRookByKingPenalty
    S(82, 6), // RookBehindOurPasserBonus
    S(-46, 85), // RookBehindTheirPasserBonus
    S(23, 4), // MinorBehindPawnBonus
    S(25, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-17, -4), // KingProtector
    S(3, 0), // BishopPair
    {
        {S(0, 0), S(146, 0), S(142, 0), S(96, 0), S(80, 0), S(68, 0), S(0, 0)},
        {S(-96, 0), S(208, 0), S(130, 0), S(2, 0), S(0, 0), S(21, 0), S(84, 0)},
        {S(0, 0), S(180, 0), S(61, 0), S(38, 0), S(34, 0), S(139, 0), S(103, 0)},
        {S(0, 0), S(94, 0), S(89, 0), S(74, 0), S(46, 0), S(44, 0), S(50, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(11, 0), S(0, 0), S(6, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(125, 0), S(19, 0), S(11, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(69, 0), S(296, 0), S(70, 0), S(32, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(8, 0), S(252, 0), S(70, 0), S(13, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(127, 0), S(9, 0), S(0, 0), S(1, 0), S(0, 0)}, // BlockedStorm
    S(-50, 0), // UndefendedKingZoneSq
    S(9, 81), // KingMobilityFactor
    S(31, 34), // KingAttackByKnight
    S(11, 49), // KingAttackByBishop
    S(31, 34), // KingAttackByRook
    S(31, 34), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(34, 482), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(14, 16), // KingRingWeakWeight
    S(25, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -15), // DoubledPawnPenalty
    S(-15, -5), // BackwardPawnPenalty
    S(-20, -16), // WeakUnopposedPenalty
    S(-12, -30), // DoubledIsolatedPenalty
    {S(-62, -24), S(0, -28)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-14, -12), // PawnIslandPenalty
    {S(15, 0), S(0, 0)}, // CentralPawnBonus
    S(44, 19), // BishopLongDiagonalBonus
    S(-16, 0), // BishopXrayPawns
    S(0, 39), // InitiativePasser
    S(0, 53), // InitiativePawnCount
    S(0, 1), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 36), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(72, 1), // SliderOnQueenBishop
    S(46, 0), // SliderOnQueenRook
    S(5, 2), // RestrictedPiece
    S(37, 0), // ThreatByPawnPush
    S(-32, -14), // WeakQueenDefender
    S(81, 0), // KnightOnQueen
    S(-282, -106), // PawnlessFlank
    S(0, 8), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 78), // KBNKCornerEg
    S(0, 627), // LucenaEg
    S(0, 20), // KXKPushToEdge
    S(0, 10), // KXKPushClose
    S(0, 10), // KBNKPushClose
    S(0, 25), // KQKRPushToEdge
    S(0, 15), // KQKRPushClose
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
