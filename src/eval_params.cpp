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
    S(191, 36), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(140, 0), S(143, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(191, 0), S(0, 0)},
    S(111, 0), // ThreatByKing
    S(86, 19), // Hanging
    S(57, 3), // WeakQueen
    S(20, 18), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 33), S(0, 34), S(0, 37), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 14), S(0, 30), S(0, 60), S(0, 71), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-30, -29), S(-30, -39), S(-30, -68), S(-280, -115), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(7, 22), S(10, 57), S(144, 102), S(170, 371), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-18, 28), S(31, 28), S(51, 28), S(51, 28), S(0, 0)},
    S(0, 48), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-5, -11), // BishopPawns
    S(48, 0), // Tempo
    {S(0, 0), S(192, 209), S(830, 525), S(895, 481), S(1295, 775), S(2378, 1657), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-27, -14), S(-14, -9), S(-10, -10), S(17, -27), S(17, -21), S(7, -1), S(-13, -12), S(-33, -20),
        S(-68, -31), S(-51, -23), S(-24, -23), S(-28, -45), S(-3, -29), S(-25, -15), S(-45, -26), S(-74, -32),
        S(-29, -10), S(-30, -11), S(-20, -27), S(7, -33), S(9, -33), S(-12, -24), S(-27, -18), S(-31, -18),
        S(1, 28), S(-16, 15), S(-9, -2), S(57, -7), S(67, -4), S(2, -1), S(-21, 13), S(1, 23),
        S(5, 36), S(7, 35), S(29, 18), S(70, 18), S(73, 20), S(31, 19), S(7, 32), S(3, 37),
        S(4, 27), S(11, 25), S(33, 27), S(58, 29), S(59, 31), S(33, 27), S(11, 25), S(5, 28),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-128, -29), S(-129, -36), S(-121, -29), S(-86, -16),
        S(-113, -22), S(-73, -16), S(-78, -20), S(-50, -15),
        S(-94, -18), S(-38, -7), S(-14, -11), S(20, 7),
        S(9, 4), S(21, 3), S(48, 22), S(32, 30),
        S(40, 12), S(65, 14), S(75, 32), S(77, 31),
        S(37, -2), S(62, 11), S(80, 29), S(88, 42),
        S(20, -17), S(31, -9), S(50, 5), S(68, 33),
        S(3, -25), S(20, -9), S(38, 10), S(50, 19)
    },
    // BishopPST (half-board)
    {
        S(-7, -16), S(16, 1), S(-85, -5), S(-73, -14),
        S(12, -14), S(-2, -36), S(6, -6), S(-54, 1),
        S(-10, -14), S(22, -9), S(-13, -9), S(11, 15),
        S(23, -12), S(29, 1), S(35, 17), S(23, 13),
        S(-17, -21), S(47, 4), S(50, 14), S(42, 17),
        S(19, 5), S(22, 26), S(21, 13), S(36, 25),
        S(-16, -6), S(-34, -4), S(-9, 19), S(2, 16),
        S(-25, -8), S(-28, 8), S(-24, 5), S(-21, 4)
    },
    // RookPST (half-board)
    {
        S(-81, -24), S(-40, -29), S(-32, -23), S(8, -37),
        S(-80, -33), S(-29, -38), S(-31, -30), S(-12, -35),
        S(-50, -23), S(-16, -22), S(-21, -18), S(-10, -15),
        S(-19, 2), S(-1, 10), S(-11, 17), S(5, 4),
        S(-18, 14), S(19, 36), S(30, 29), S(43, 22),
        S(1, 21), S(28, 34), S(46, 41), S(63, 35),
        S(7, -14), S(11, 2), S(35, 8), S(51, 13),
        S(5, 8), S(18, 23), S(34, 24), S(38, 18)
    },
    // QueenPST (half-board)
    {
        S(0, -79), S(20, -71), S(10, -73), S(34, -55),
        S(21, -75), S(19, -63), S(42, -52), S(44, -38),
        S(26, -52), S(51, -25), S(50, -8), S(31, 0),
        S(45, -22), S(59, 8), S(36, 34), S(23, 43),
        S(20, -25), S(32, 9), S(12, 39), S(5, 70),
        S(5, 8), S(-13, 29), S(-14, 50), S(-3, 86),
        S(-51, -1), S(-90, 19), S(-57, 51), S(-42, 66),
        S(-90, -6), S(-81, 21), S(-68, 41), S(-53, 60)
    },
    // KingPST (half-board)
    {
        S(150, -149), S(121, -105), S(43, -71), S(-70, -67),
        S(153, -105), S(118, -72), S(19, -51), S(-28, -39),
        S(40, -72), S(29, -48), S(-9, -29), S(-31, -18),
        S(-7, -28), S(-5, -4), S(-20, 6), S(-35, 10),
        S(-18, 14), S(-18, 34), S(-26, 38), S(-37, 32),
        S(-26, 38), S(-20, 58), S(-25, 61), S(-31, 55),
        S(-32, 46), S(-28, 63), S(-29, 67), S(-32, 66),
        S(-34, 52), S(-31, 62), S(-31, 68), S(-32, 69)
    },
    {
        {},
        {},
        {S(-97, -102), S(-57, -62), S(-23, -29), S(-3, -5), S(16, 10), S(31, 27), S(47, 27), S(51, 27), S(51, 27)},
        {S(14, 57), S(33, 70), S(65, 97), S(79, 117), S(97, 136), S(100, 151), S(116, 151), S(117, 155), S(124, 155), S(124, 155), S(124, 155), S(124, 155), S(140, 155), S(140, 155)},
        {S(-46, 275), S(-24, 283), S(0, 292), S(20, 293), S(20, 304), S(32, 316), S(39, 322), S(49, 328), S(51, 341), S(54, 343), S(54, 350), S(54, 353), S(54, 353), S(54, 353), S(54, 353)},
        {S(36, 291), S(36, 316), S(61, 322), S(85, 326), S(91, 335), S(112, 353), S(112, 355), S(129, 365), S(133, 378), S(147, 378), S(150, 382), S(163, 389), S(168, 394), S(168, 398), S(174, 403), S(174, 403), S(176, 403), S(188, 403), S(188, 403), S(193, 403), S(214, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(252, 428)},
        {},
    },
    {S(0, 0), S(-34, 11), S(-34, 21), S(-34, 34), S(18, 102), S(155, 318), S(516, 515), S(0, 0)},
    {S(0, 0), S(-14, 1), S(24, 13), S(40, 13), S(87, 27), S(87, 74), S(322, 101), S(0, 0)},
    S(110, 11), // RookOpenFileBonus
    S(53, 11), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(62, 11), // KnightOutpostBonus
    S(68, 16), // BishopOutpostBonus
    S(-54, 0), // TrappedRookByKingPenalty
    S(49, 23), // RookBehindOurPasserBonus
    S(-47, 85), // RookBehindTheirPasserBonus
    S(31, 11), // MinorBehindPawnBonus
    S(34, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-14, 0), // KingProtector
    S(41, 22), // BishopPair
    {
        {S(0, 0), S(117, 0), S(123, 0), S(92, 0), S(73, 0), S(67, 0), S(41, 0)},
        {S(-79, 0), S(184, 0), S(97, 0), S(15, 0), S(0, 0), S(31, 0), S(33, 0)},
        {S(0, 0), S(149, 0), S(47, 0), S(54, 0), S(20, 0), S(88, 0), S(172, 0)},
        {S(0, 0), S(91, 0), S(48, 0), S(49, 0), S(62, 0), S(34, 0), S(62, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(70, 0), S(25, 0), S(22, 0), S(25, 0)},
        {S(0, 0), S(0, 0), S(88, 0), S(35, 0), S(14, 0), S(0, 0), S(8, 0)},
        {S(0, 0), S(0, 0), S(242, 0), S(94, 0), S(28, 0), S(0, 0), S(2, 0)},
        {S(0, 0), S(0, 0), S(151, 0), S(34, 0), S(22, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(183, 0), S(11, 0), S(24, 0), S(0, 0), S(0, 0)}, // BlockedStorm
    S(-49, 0), // UndefendedKingZoneSq
    S(11, 77), // KingMobilityFactor
    S(30, 34), // KingAttackByKnight
    S(6, 0), // KingAttackByBishop
    S(31, 34), // KingAttackByRook
    S(31, 34), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(36, 592), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(6, 0), // KingRingWeakWeight
    S(9, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-3, -18), // DoubledPawnPenalty
    S(-20, -5), // BackwardPawnPenalty
    S(-31, -19), // WeakUnopposedPenalty
    S(-26, -49), // DoubledIsolatedPenalty
    {S(-2, -21), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-40, -12), // PawnIslandPenalty
    {S(19, 0), S(6, 0)}, // CentralPawnBonus
    S(41, 13), // BishopLongDiagonalBonus
    S(-16, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 18), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 29), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(64, 0), // SliderOnQueenBishop
    S(27, 0), // SliderOnQueenRook
    S(11, 0), // RestrictedPiece
    S(30, 0), // ThreatByPawnPush
    S(-21, -18), // WeakQueenDefender
    S(66, 2), // KnightOnQueen
    S(-303, -76), // PawnlessFlank
    S(2, 29), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 97), // KBNKCornerEg
    S(0, 880), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
