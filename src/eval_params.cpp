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
    S(176, 28), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(126, 0), S(102, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(165, 0), S(0, 0)},
    S(100, 2), // ThreatByKing
    S(70, 15), // Hanging
    S(13, 0), // WeakQueen
    S(24, 14), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 15), S(0, 25), S(0, 32), S(0, 33), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 29), S(0, 57), S(0, 68), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-30, -15), S(-30, -30), S(-30, -47), S(-284, -71), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(1, 17), S(25, 40), S(104, 75), S(104, 371), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(29, 28), S(34, 28), S(92, 28), S(92, 28), S(0, 0)},
    S(0, 34), // RookOn7thBonus
    S(-7, 0), // BadBishop
    S(-4, -7), // BishopPawns
    S(49, 0), // Tempo
    {S(0, 0), S(201, 172), S(808, 531), S(884, 485), S(1307, 754), S(2332, 1642), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-41, -11), S(-21, -9), S(-13, -8), S(-29, -29), S(-16, -19), S(2, -1), S(-21, -8), S(-43, -18),
        S(-73, -29), S(-60, -21), S(-38, -23), S(-62, -40), S(-40, -26), S(-35, -18), S(-51, -26), S(-84, -28),
        S(-36, -11), S(-45, -14), S(-19, -27), S(6, -35), S(9, -34), S(-16, -22), S(-43, -17), S(-37, -17),
        S(1, 17), S(-10, 13), S(8, -2), S(89, -4), S(107, 6), S(16, 1), S(-15, 13), S(4, 16),
        S(11, 24), S(17, 28), S(42, 16), S(93, 24), S(96, 27), S(44, 18), S(17, 24), S(10, 29),
        S(13, 21), S(22, 22), S(47, 22), S(77, 35), S(78, 37), S(47, 23), S(22, 22), S(14, 23),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-153, -31), S(-176, -20), S(-137, -18), S(-89, -8),
        S(-115, -23), S(-94, -7), S(-91, -14), S(-72, -4),
        S(-102, -18), S(-45, -6), S(-20, -8), S(14, 13),
        S(-6, 0), S(20, 9), S(41, 24), S(39, 26),
        S(30, 4), S(59, 13), S(74, 29), S(82, 32),
        S(38, -13), S(64, 7), S(93, 23), S(108, 33),
        S(35, -18), S(50, -8), S(74, 0), S(94, 30),
        S(25, -29), S(45, -11), S(65, 7), S(77, 14)
    },
    // BishopPST (half-board)
    {
        S(2, -18), S(18, -2), S(-92, -8), S(-84, -9),
        S(10, -12), S(-10, -33), S(-2, -1), S(-56, -5),
        S(-18, -14), S(10, 6), S(-6, -12), S(12, 12),
        S(-2, -11), S(20, 5), S(44, 16), S(35, 6),
        S(-24, -18), S(31, 8), S(43, 19), S(54, 13),
        S(8, 0), S(20, 17), S(30, 4), S(53, 25),
        S(-20, -14), S(-30, -10), S(5, 19), S(23, 25),
        S(-34, -16), S(-25, 8), S(-10, 8), S(-4, 11)
    },
    // RookPST (half-board)
    {
        S(-106, -24), S(-47, -27), S(-40, -22), S(-4, -28),
        S(-82, -29), S(-17, -30), S(-24, -25), S(-11, -24),
        S(-50, -26), S(0, -18), S(-13, -14), S(7, -18),
        S(-27, -9), S(11, 2), S(4, 6), S(15, -2),
        S(-25, 15), S(18, 29), S(38, 22), S(38, 14),
        S(-6, 21), S(30, 29), S(56, 31), S(76, 23),
        S(-17, -2), S(-1, -1), S(37, 6), S(61, 10),
        S(-5, 19), S(11, 24), S(35, 23), S(40, 21)
    },
    // QueenPST (half-board)
    {
        S(7, -78), S(45, -70), S(24, -68), S(38, -57),
        S(18, -67), S(33, -71), S(53, -56), S(46, -41),
        S(17, -41), S(44, -23), S(49, -11), S(33, -11),
        S(43, -14), S(56, 4), S(43, 25), S(26, 28),
        S(11, -18), S(22, 10), S(12, 34), S(4, 62),
        S(5, 2), S(-19, 23), S(-23, 53), S(-13, 76),
        S(-56, 10), S(-96, 17), S(-59, 47), S(-43, 68),
        S(-87, 20), S(-87, 40), S(-76, 50), S(-62, 55)
    },
    // KingPST (half-board)
    {
        S(142, -136), S(95, -94), S(26, -71), S(-118, -58),
        S(149, -97), S(98, -66), S(1, -50), S(-43, -40),
        S(32, -63), S(22, -44), S(-19, -31), S(-34, -23),
        S(-11, -27), S(-5, -5), S(-20, 0), S(-30, -1),
        S(-13, 17), S(-12, 31), S(-18, 34), S(-28, 28),
        S(-11, 44), S(-8, 61), S(-15, 61), S(-23, 53),
        S(-14, 48), S(-11, 60), S(-16, 61), S(-21, 62),
        S(-18, 47), S(-16, 54), S(-17, 59), S(-19, 62)
    },
    {
        {},
        {},
        {S(-109, -92), S(-69, -52), S(-32, -15), S(-9, 2), S(12, 15), S(29, 26), S(47, 26), S(54, 26), S(54, 26)},
        {S(-18, 93), S(17, 98), S(52, 117), S(73, 128), S(95, 146), S(110, 150), S(115, 154), S(117, 155), S(117, 155), S(122, 155), S(124, 155), S(124, 155), S(124, 155), S(124, 155)},
        {S(-56, 290), S(-26, 301), S(-7, 309), S(13, 313), S(13, 316), S(22, 324), S(27, 332), S(38, 335), S(48, 340), S(54, 345), S(54, 351), S(54, 355), S(54, 355), S(54, 355), S(54, 355)},
        {S(61, 313), S(61, 313), S(86, 338), S(96, 338), S(104, 341), S(113, 365), S(118, 369), S(132, 372), S(134, 384), S(146, 384), S(151, 388), S(158, 394), S(162, 398), S(162, 400), S(166, 403), S(168, 403), S(176, 403), S(177, 403), S(177, 403), S(183, 403), S(200, 403), S(203, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(229, 428)},
        {},
    },
    {S(0, 0), S(-39, 16), S(-39, 22), S(-39, 35), S(0, 109), S(141, 293), S(478, 456), S(0, 0)},
    {S(0, 0), S(-14, -1), S(25, 7), S(40, 7), S(71, 23), S(86, 64), S(105, 171), S(0, 0)},
    S(100, 12), // RookOpenFileBonus
    S(43, 12), // RookSemiOpenFileBonus
    S(10, 0), // RookOnQueenFile
    S(51, 18), // KnightOutpostBonus
    S(68, 12), // BishopOutpostBonus
    S(-56, 0), // TrappedRookByKingPenalty
    S(29, 15), // RookBehindOurPasserBonus
    S(-26, 76), // RookBehindTheirPasserBonus
    S(34, 10), // MinorBehindPawnBonus
    S(40, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-15, 0), // KingProtector
    S(50, 0), // BishopPair
    {
        {S(0, 0), S(110, 0), S(124, 0), S(80, 0), S(58, 0), S(68, 0), S(158, 0)},
        {S(-65, 0), S(187, 0), S(99, 0), S(45, 0), S(0, 0), S(43, 0), S(11, 0)},
        {S(0, 0), S(151, 0), S(52, 0), S(51, 0), S(23, 0), S(54, 0), S(115, 0)},
        {S(0, 0), S(110, 0), S(41, 0), S(28, 0), S(53, 0), S(26, 0), S(63, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(92, 0), S(16, 0), S(0, 0), S(15, 0)},
        {S(0, 0), S(0, 0), S(58, 0), S(19, 0), S(0, 0), S(0, 0), S(4, 0)},
        {S(0, 0), S(0, 0), S(196, 0), S(66, 0), S(20, 0), S(2, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(122, 0), S(42, 0), S(7, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(165, 0), S(7, 0), S(12, 0), S(8, 0), S(15, 0)}, // BlockedStorm
    S(-38, -1), // UndefendedKingZoneSq
    S(12, 55), // KingMobilityFactor
    S(28, 0), // KingAttackByKnight
    S(5, 0), // KingAttackByBishop
    S(28, 8), // KingAttackByRook
    S(28, 8), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(34, 619), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(5, 0), // KingRingWeakWeight
    S(8, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-3, -18), // DoubledPawnPenalty
    S(-19, -6), // BackwardPawnPenalty
    S(-31, -9), // WeakUnopposedPenalty
    S(-38, -39), // DoubledIsolatedPenalty
    {S(0, -24), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-35, -19), // PawnIslandPenalty
    {S(31, 0), S(6, 0)}, // CentralPawnBonus
    S(31, 19), // BishopLongDiagonalBonus
    S(-20, 0), // BishopXrayPawns
    S(0, 31), // InitiativePasser
    S(0, 31), // InitiativePawnCount
    S(0, 10), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 49), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(55, 30), // SliderOnQueenBishop
    S(25, 0), // SliderOnQueenRook
    S(10, 2), // RestrictedPiece
    S(32, 0), // ThreatByPawnPush
    S(-24, -3), // WeakQueenDefender
    S(55, 0), // KnightOnQueen
    S(-263, -71), // PawnlessFlank
    S(0, 18), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 50), // KBNKCornerEg
    S(0, 300), // LucenaEg
    S(0, 50), // KXKPushToEdge
    S(0, 50), // KXKPushClose
    S(0, 50), // KBNKPushClose
    S(0, 50), // KQKRPushToEdge
    S(0, 50), // KQKRPushClose
    S(0, 0), // KPsKFortressScale
    S(0, 3), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
