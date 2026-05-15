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
    S(370, 342), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(72, 2), // ThreatByKing
    S(35, 29), // Hanging
    S(50, 50), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 27), S(0, 32), S(0, 32), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 33), S(0, 61), S(0, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-8, -24), S(-8, -41), S(-8, -67), S(-124, -107), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 49), S(41, 95), S(286, 318), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(32, 28), S(32, 28), S(73, 28), S(73, 28), S(0, 0)},
    S(0, 39), // RookOn7thBonus
    S(-8, -6), // BadBishop
    S(-4, -7), // BishopPawns
    S(40, 0), // Tempo
    {S(0, 0), S(160, 208), S(501, 692), S(601, 769), S(869, 1292), S(1655, 2426), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-39, -7), S(-24, -2), S(-29, 2), S(-26, -24), S(-12, -17), S(-6, 6), S(-25, 2), S(-43, -14),
        S(-69, -24), S(-63, -14), S(-39, -22), S(-61, -40), S(-35, -30), S(-38, -16), S(-56, -21), S(-82, -24),
        S(-29, -7), S(-37, -12), S(-22, -31), S(-2, -47), S(-3, -46), S(-19, -24), S(-38, -16), S(-31, -14),
        S(-2, 20), S(-8, 14), S(2, -2), S(74, -10), S(96, -3), S(12, 0), S(-11, 14), S(0, 19),
        S(13, 26), S(19, 29), S(41, 17), S(93, 20), S(96, 22), S(43, 19), S(19, 26), S(13, 30),
        S(14, 21), S(22, 24), S(47, 22), S(79, 30), S(80, 31), S(47, 22), S(22, 24), S(15, 23),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-121, -39), S(-137, -33), S(-111, -20), S(-80, -13),
        S(-90, -28), S(-71, -13), S(-76, -16), S(-59, -9),
        S(-95, -16), S(-35, -8), S(-20, -13), S(6, 14),
        S(-16, 0), S(11, 7), S(21, 27), S(13, 34),
        S(9, 9), S(32, 15), S(41, 34), S(42, 43),
        S(27, -14), S(56, 3), S(80, 22), S(90, 34),
        S(31, -20), S(45, -9), S(72, 1), S(98, 29),
        S(23, -30), S(44, -11), S(69, 8), S(87, 18)
    },
    // BishopPST (half-board)
    {
        S(1, -24), S(19, -13), S(-80, -8), S(-68, -14),
        S(19, -20), S(1, -53), S(6, -7), S(-43, -4),
        S(-13, -20), S(11, 3), S(-12, -13), S(7, 16),
        S(-5, -17), S(12, 6), S(31, 15), S(14, 8),
        S(-32, -15), S(15, 14), S(30, 18), S(35, 14),
        S(11, 3), S(17, 19), S(23, 6), S(49, 25),
        S(-20, -7), S(-31, -3), S(6, 20), S(26, 26),
        S(-28, -6), S(-19, 14), S(-5, 11), S(4, 13)
    },
    // RookPST (half-board)
    {
        S(-80, -29), S(-39, -32), S(-34, -28), S(-1, -37),
        S(-66, -34), S(-15, -36), S(-19, -33), S(-8, -31),
        S(-41, -25), S(0, -27), S(-16, -18), S(5, -26),
        S(-31, -3), S(9, 2), S(5, 7), S(2, -1),
        S(-41, 27), S(10, 32), S(27, 26), S(27, 19),
        S(-14, 25), S(25, 29), S(48, 32), S(70, 25),
        S(-19, 0), S(-8, 1), S(37, 6), S(61, 12),
        S(5, 20), S(17, 24), S(39, 23), S(46, 20)
    },
    // QueenPST (half-board)
    {
        S(-11, -74), S(24, -72), S(7, -66), S(26, -62),
        S(7, -65), S(16, -66), S(37, -65), S(30, -42),
        S(8, -35), S(35, -22), S(24, -5), S(11, 3),
        S(41, -21), S(47, 4), S(33, 31), S(-5, 62),
        S(8, -25), S(18, 12), S(6, 45), S(-6, 77),
        S(17, -9), S(-11, 19), S(-4, 51), S(5, 77),
        S(-38, -11), S(-78, 20), S(-36, 49), S(-17, 65),
        S(-73, 8), S(-62, 34), S(-46, 45), S(-23, 50)
    },
    // KingPST (half-board)
    {
        S(62, -137), S(37, -95), S(-3, -73), S(-100, -57),
        S(69, -94), S(37, -66), S(-15, -50), S(-23, -44),
        S(-12, -58), S(-4, -41), S(-17, -32), S(-17, -23),
        S(-47, -21), S(-13, -4), S(-7, 0), S(-4, -1),
        S(-18, 21), S(-1, 34), S(5, 36), S(4, 31),
        S(-2, 44), S(12, 60), S(13, 59), S(9, 52),
        S(4, 47), S(10, 58), S(9, 59), S(6, 59),
        S(4, 47), S(8, 53), S(9, 57), S(9, 59)
    },
    {
        {},
        {},
        {S(-78, -74), S(-58, -54), S(-38, -34), S(-22, -14), S(-4, 2), S(5, 20), S(18, 20), S(27, 20), S(29, 20)},
        {S(-40, -42), S(-22, -29), S(-4, -11), S(14, 4), S(30, 22), S(33, 33), S(35, 39), S(37, 42), S(37, 42), S(37, 42), S(37, 43), S(37, 43), S(37, 43), S(37, 43)},
        {S(-28, -28), S(-13, -13), S(-2, 0), S(12, 1), S(12, 6), S(19, 18), S(22, 28), S(26, 33), S(34, 40), S(41, 44), S(48, 50), S(50, 54), S(50, 55), S(50, 55), S(50, 55)},
        {S(2, -62), S(2, -62), S(2, -50), S(2, -38), S(2, -26), S(10, -14), S(10, -2), S(15, 10), S(16, 22), S(24, 22), S(29, 23), S(29, 35), S(33, 35), S(34, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(45, 35), S(45, 35), S(51, 40), S(63, 52)},
        {},
    },
    {S(0, 0), S(-43, 14), S(-43, 25), S(-43, 25), S(0, 126), S(65, 344), S(224, 575), S(0, 0)},
    {S(0, 0), S(-9, 1), S(26, 6), S(28, 9), S(43, 36), S(46, 89), S(46, 222), S(0, 0)},
    S(72, 17), // RookOpenFileBonus
    S(34, 17), // RookSemiOpenFileBonus
    S(5, 0), // RookOnQueenFile
    S(45, 23), // KnightOutpostBonus
    S(55, 13), // BishopOutpostBonus
    S(-41, 0), // TrappedRookByKingPenalty
    S(20, 26), // RookBehindOurPasserBonus
    S(41, 75), // RookBehindTheirPasserBonus
    S(19, 17), // MinorBehindPawnBonus
    S(28, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-7, -3), // KingProtector
    S(23, 12), // BishopPair
    {
        {S(0, 0), S(73, 0), S(82, 0), S(49, 0), S(36, 0), S(54, 0), S(198, 0)},
        {S(-33, 0), S(120, 0), S(75, 0), S(36, 0), S(0, 0), S(46, 0), S(0, 0)},
        {S(-3, 0), S(96, 0), S(19, 0), S(22, 0), S(8, 0), S(38, 0), S(123, 0)},
        {S(0, 0), S(23, 0), S(10, 0), S(7, 0), S(44, 0), S(8, 0), S(60, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(60, 0), S(3, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(12, 0), S(11, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(93, 0), S(37, 0), S(11, 0), S(4, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(30, 0), S(18, 0), S(17, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(109, 0), S(2, 0), S(6, 0), S(17, 0), S(10, 0)}, // BlockedStorm
    S(-26, 0), // UndefendedKingZoneSq
    S(11, 0), // KingMobilityFactor
    S(21, 36), // KingAttackByKnight
    S(5, 40), // KingAttackByBishop
    S(21, 36), // KingAttackByRook
    S(21, 36), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 0), S(28, 619), S(29, 0), S(29, 0), S(0, 0)}, // KingSafeCheck
    S(6, 39), // KingRingWeakWeight
    S(5, 145), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-4, -27), // DoubledPawnPenalty
    S(-17, -3), // BackwardPawnPenalty
    S(-26, -12), // WeakUnopposedPenalty
    S(-32, -44), // DoubledIsolatedPenalty
    {S(0, -23), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-40, -14), // PawnIslandPenalty
    {S(36, 0), S(15, 0)}, // CentralPawnBonus
    S(28, 21), // BishopLongDiagonalBonus
    S(-15, 0), // BishopXrayPawns
    S(0, 6), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 0), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(56, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(7, 4), // RestrictedPiece
    S(85, 8), // ThreatByPawnPush
    S(-18, -3), // WeakQueenDefender
    S(33, 1), // KnightOnQueen
    S(-90, -93), // PawnlessFlank
    S(4, 31), // QueenInfiltration
    S(0, -1), // KingPawnDistEg
    S(0, 21), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 24), // KBNKPushClose
    S(0, 30), // KQKRPushToEdge
    S(0, 15), // KQKRPushClose
    S(0, 1), // KPsKFortressScale
    S(0, 0), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
    S(0, 22), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
