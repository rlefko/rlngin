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
    S(294, 538), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(55, 0), // ThreatByKing
    S(39, 29), // Hanging
    S(50, 49), // WeakQueen
    S(2, 1), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 17), S(0, 29), S(0, 38), S(0, 39), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 17), S(0, 32), S(0, 56), S(0, 72), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-21, -31), S(-21, -31), S(-21, -42), S(-183, -78), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 27), S(0, 54), S(51, 92), S(108, 384), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(20, 28), S(20, 28), S(89, 28), S(89, 28), S(0, 0)},
    S(0, 51), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-4, -3), // BishopPawns
    S(24, 0), // Tempo
    {S(0, 0), S(199, 221), S(708, 586), S(810, 603), S(1199, 967), S(2168, 1902), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-21, -21), S(-16, -21), S(-16, -21), S(-6, -21), S(-6, -21), S(-16, -21), S(-16, -21), S(-21, -21),
        S(-26, -21), S(-21, -21), S(-16, -21), S(-6, -21), S(-6, -21), S(-16, -21), S(-21, -21), S(-26, -21),
        S(-21, -16), S(-16, -16), S(-11, -16), S(4, -11), S(4, -11), S(-11, -16), S(-16, -16), S(-21, -16),
        S(-11, -6), S(-6, -6), S(-1, -1), S(14, 4), S(14, 4), S(-1, -1), S(-6, -6), S(-11, -6),
        S(-1, 9), S(4, 14), S(14, 19), S(29, 24), S(29, 24), S(14, 19), S(4, 14), S(-1, 9),
        S(19, 39), S(29, 44), S(39, 49), S(59, 59), S(59, 59), S(39, 49), S(29, 44), S(19, 39),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-54, -33), S(-34, -18), S(-19, -13), S(-4, -8),
        S(-24, -13), S(-9, -8), S(1, -3), S(11, 2),
        S(-19, -8), S(1, 2), S(16, 7), S(26, 12),
        S(-14, -3), S(11, 7), S(26, 12), S(36, 17),
        S(-9, 2), S(16, 12), S(31, 17), S(41, 22),
        S(-14, -3), S(11, 7), S(26, 12), S(36, 17),
        S(-24, -8), S(1, 2), S(16, 7), S(26, 7),
        S(-54, -23), S(-24, -13), S(-9, -3), S(1, 2)
    },
    // BishopPST (half-board)
    {
        S(-20, -10), S(-15, -5), S(-10, -5), S(-5, 0),
        S(-10, -5), S(5, 0), S(0, 0), S(5, 5),
        S(-10, -5), S(10, 0), S(15, 5), S(15, 5),
        S(-5, 0), S(5, 5), S(15, 10), S(20, 10),
        S(-5, 0), S(5, 5), S(15, 10), S(20, 10),
        S(-10, -5), S(10, 0), S(15, 5), S(15, 5),
        S(-15, -10), S(0, -5), S(5, 0), S(5, 0),
        S(-20, -10), S(-10, -5), S(-10, -5), S(-5, -5)
    },
    // RookPST (half-board)
    {
        S(-14, -9), S(-9, -7), S(-4, -7), S(1, -4),
        S(-14, -9), S(-9, -4), S(-4, -4), S(1, -1),
        S(-14, -9), S(-4, -4), S(1, -1), S(1, 1),
        S(-9, -4), S(-4, -1), S(1, 1), S(6, 1),
        S(-9, 1), S(1, 1), S(1, 6), S(6, 6),
        S(-4, 6), S(1, 6), S(6, 6), S(6, 6),
        S(11, 6), S(16, 11), S(16, 11), S(16, 11),
        S(-4, 1), S(1, 1), S(6, 1), S(6, 1)
    },
    // QueenPST (half-board)
    {
        S(-8, -23), S(-3, -13), S(-3, -8), S(2, -3),
        S(-3, -13), S(2, -3), S(2, 2), S(2, 7),
        S(-3, -8), S(2, 2), S(2, 7), S(2, 12),
        S(2, -3), S(2, 7), S(7, 12), S(7, 17),
        S(2, -3), S(2, 7), S(7, 12), S(7, 17),
        S(-3, -8), S(2, 2), S(2, 7), S(2, 12),
        S(-3, -13), S(2, -3), S(2, 2), S(2, 7),
        S(-8, -23), S(-3, -13), S(-3, -8), S(2, -3)
    },
    // KingPST (half-board)
    {
        S(52, -57), S(62, -37), S(42, -17), S(22, -7),
        S(42, -37), S(42, -17), S(22, 3), S(12, 13),
        S(22, -17), S(22, 3), S(2, 13), S(-8, 18),
        S(-8, 3), S(-8, 13), S(-18, 18), S(-18, 23),
        S(-18, 13), S(-18, 18), S(-18, 23), S(-18, 23),
        S(-18, 3), S(-18, 13), S(-18, 18), S(-18, 18),
        S(-18, -7), S(-18, 3), S(-18, 8), S(-18, 8),
        S(-18, -17), S(-18, -7), S(-18, -2), S(-18, -2)
    },
    {
        {},
        {},
        {S(-50, -45), S(-30, -25), S(-10, -10), S(0, 0), S(10, 10), S(20, 15), S(25, 20), S(30, 20), S(35, 20)},
        {S(-33, -28), S(-15, -10), S(0, 0), S(10, 10), S(20, 20), S(25, 25), S(30, 30), S(35, 35), S(40, 40), S(45, 40), S(50, 45), S(55, 45), S(55, 50), S(60, 50)},
        {S(-30, -25), S(-15, -10), S(0, 0), S(5, 10), S(10, 20), S(15, 25), S(20, 30), S(25, 35), S(30, 40), S(35, 45), S(40, 50), S(45, 50), S(50, 55), S(50, 55), S(50, 55)},
        {S(-27, -27), S(-15, -15), S(-5, -5), S(0, 0), S(5, 5), S(10, 10), S(15, 15), S(20, 20), S(25, 25), S(30, 30), S(30, 30), S(30, 30), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(40, 40)},
        {},
    },
    {S(0, 0), S(-40, -2), S(-39, -2), S(-39, 11), S(8, 105), S(134, 280), S(325, 465), S(0, 0)},
    {S(0, 0), S(-17, 1), S(-17, 1), S(11, 1), S(82, 13), S(82, 60), S(85, 144), S(0, 0)},
    S(91, 40), // RookOpenFileBonus
    S(52, 33), // RookSemiOpenFileBonus
    S(28, 0), // RookOnQueenFile
    S(59, 8), // KnightOutpostBonus
    S(56, 24), // BishopOutpostBonus
    S(-94, 0), // TrappedRookByKingPenalty
    S(16, 41), // RookBehindOurPasserBonus
    S(7, 93), // RookBehindTheirPasserBonus
    S(14, 4), // MinorBehindPawnBonus
    S(47, 0), // MinorOnKingRing
    S(2, 0), // RookOnKingRing
    S(0, -8), // KingProtector
    S(42, 12), // BishopPair
    {
        {S(0, 0), S(71, 0), S(87, 0), S(60, 0), S(44, 0), S(47, 0), S(144, 0)},
        {S(-27, 0), S(156, 0), S(102, 0), S(45, 0), S(0, 0), S(42, 0), S(0, 0)},
        {S(-2, 0), S(117, 0), S(39, 0), S(34, 0), S(0, 0), S(26, 0), S(77, 0)},
        {S(0, 0), S(31, 0), S(0, 0), S(0, 0), S(61, 0), S(13, 0), S(58, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(78, 0), S(7, 0), S(0, 0), S(4, 0)},
        {S(0, 0), S(0, 0), S(0, 0), S(4, 0), S(0, 0), S(0, 0), S(9, 0)},
        {S(0, 0), S(0, 0), S(92, 0), S(54, 0), S(17, 0), S(9, 0), S(8, 0)},
        {S(0, 0), S(0, 0), S(16, 0), S(47, 0), S(13, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(147, 0), S(11, 0), S(17, 0), S(3, 0), S(0, 0)}, // BlockedStorm
    S(-31, -1), // UndefendedKingZoneSq
    S(15, 0), // KingMobilityFactor
    S(25, 7), // KingAttackByKnight
    S(6, 47), // KingAttackByBishop
    S(26, 7), // KingAttackByRook
    S(26, 7), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(37, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(4, 42), // KingRingWeakWeight
    S(6, 37), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-8, -36), // DoubledPawnPenalty
    S(-37, -4), // BackwardPawnPenalty
    S(-33, -23), // WeakUnopposedPenalty
    S(-52, -39), // DoubledIsolatedPenalty
    {S(0, 0), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-36, -19), // PawnIslandPenalty
    {S(37, 0), S(5, 0)}, // CentralPawnBonus
    S(23, 19), // BishopLongDiagonalBonus
    S(0, 0), // BishopXrayPawns
    S(0, 16), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 28), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(60, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(0, 0), // RestrictedPiece
    S(57, 34), // ThreatByPawnPush
    S(-7, -7), // WeakQueenDefender
    S(42, 0), // KnightOnQueen
    S(-187, -60), // PawnlessFlank
    S(0, 65), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 40), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 25), // KBNKPushClose
    S(0, 30), // KQKRPushToEdge
    S(0, 15), // KQKRPushClose
    S(0, 1), // KPsKFortressScale
    S(0, 0), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
    S(0, 15), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
