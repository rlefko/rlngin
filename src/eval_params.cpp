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
    S(290, 400), // ThreatByPawn (clamped to SPSA 0..400 upper bound)
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(70, 2), // ThreatByKing
    S(54, 21), // Hanging
    S(50, 50), // WeakQueen
    S(27, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 28), S(0, 33), S(0, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 33), S(0, 59), S(0, 74), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-20, -23), S(-23, -38), S(-23, -66), S(-220, -93), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(2, 51), S(89, 86), S(118, 377), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(31, 28), S(31, 28), S(101, 28), S(101, 28), S(0, 0)},
    S(0, 42), // RookOn7thBonus
    S(-18, 0), // BadBishop
    S(-5, -7), // BishopPawns
    S(38, 0), // Tempo
    {S(0, 0), S(194, 211), S(746, 603), S(818, 568), S(1218, 897), S(2214, 1860), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-22, -4), S(-14, -2), S(-11, -2), S(-19, -15), S(-11, -11), S(-1, 1), S(-13, -2), S(-23, -9),
        S(-38, -15), S(-34, -8), S(-24, -11), S(-35, -22), S(-24, -15), S(-22, -9), S(-30, -12), S(-44, -14),
        S(-18, -5), S(-23, -6), S(-11, -14), S(-1, -21), S(0, -20), S(-10, -12), S(-22, -9), S(-19, -9),
        S(2, 10), S(-2, 8), S(4, 0), S(41, -2), S(51, 1), S(8, 0), S(-4, 8), S(3, 9),
        S(10, 14), S(13, 16), S(24, 10), S(48, 12), S(50, 13), S(26, 11), S(13, 14), S(9, 15),
        S(10, 12), S(15, 13), S(27, 13), S(42, 17), S(42, 17), S(27, 13), S(15, 13), S(11, 12),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-70, -17), S(-80, -11), S(-62, -11), S(-40, -7),
        S(-53, -13), S(-43, -5), S(-43, -7), S(-35, -1),
        S(-50, -9), S(-23, -3), S(-14, -3), S(4, 8),
        S(-6, -1), S(5, 4), S(13, 13), S(12, 16),
        S(9, 2), S(21, 7), S(29, 16), S(33, 19),
        S(16, -6), S(30, 3), S(44, 11), S(53, 17),
        S(15, -10), S(24, -5), S(37, 0), S(50, 15),
        S(12, -15), S(23, -6), S(34, 3), S(43, 7)
    },
    // BishopPST (half-board)
    {
        S(-2, -9), S(7, -4), S(-43, -5), S(-39, -6),
        S(2, -7), S(-7, -19), S(-1, -2), S(-26, -3),
        S(-11, -7), S(2, 4), S(-6, -5), S(3, 8),
        S(-4, -6), S(6, 3), S(15, 9), S(13, 5),
        S(-15, -9), S(11, 5), S(17, 10), S(24, 9),
        S(4, -1), S(11, 9), S(17, 2), S(28, 13),
        S(-8, -6), S(-12, -3), S(7, 9), S(19, 11),
        S(-12, -7), S(-7, 4), S(2, 4), S(7, 4)
    },
    // RookPST (half-board)
    {
        S(-49, -12), S(-23, -15), S(-20, -13), S(-5, -16),
        S(-39, -16), S(-8, -18), S(-12, -15), S(-6, -14),
        S(-25, -13), S(-1, -11), S(-7, -8), S(2, -11),
        S(-15, -3), S(4, 1), S(1, 4), S(4, 0),
        S(-14, 11), S(9, 16), S(19, 14), S(17, 10),
        S(-5, 13), S(17, 16), S(29, 17), S(40, 12),
        S(-9, 0), S(-2, 0), S(18, 3), S(32, 4),
        S(1, 10), S(10, 12), S(23, 12), S(25, 11)
    },
    // QueenPST (half-board)
    {
        S(2, -45), S(22, -43), S(11, -38), S(16, -30),
        S(8, -39), S(14, -39), S(23, -31), S(20, -20),
        S(9, -23), S(20, -11), S(21, -1), S(14, 1),
        S(20, -7), S(24, 5), S(19, 20), S(10, 25),
        S(0, -11), S(7, 6), S(2, 20), S(2, 36),
        S(3, -1), S(-7, 11), S(-10, 28), S(-3, 41),
        S(-23, 2), S(-44, 9), S(-26, 25), S(-15, 36),
        S(-36, 7), S(-35, 17), S(-31, 24), S(-25, 28)
    },
    // KingPST (half-board)
    {
        S(48, -75), S(29, -51), S(1, -38), S(-59, -28),
        S(52, -52), S(33, -35), S(-8, -26), S(-17, -21),
        S(7, -33), S(4, -22), S(-11, -15), S(-10, -11),
        S(-10, -13), S(-5, -2), S(-6, 1), S(-9, 2),
        S(-6, 10), S(-4, 17), S(-4, 19), S(-5, 17),
        S(-3, 23), S(0, 31), S(0, 31), S(-2, 28),
        S(-3, 25), S(-1, 30), S(-1, 31), S(-2, 32),
        S(-3, 25), S(-2, 28), S(-1, 30), S(-1, 32)
    },
    {
        {},
        {},
        {S(-56, -49), S(-36, -29), S(-18, -12), S(-8, -1), S(0, 7), S(9, 13), S(16, 13), S(22, 13), S(22, 13)},
        {S(-5, 38), S(9, 41), S(26, 52), S(34, 60), S(44, 72), S(50, 75), S(52, 77), S(54, 77), S(54, 77), S(56, 77), S(58, 77), S(58, 77), S(58, 77), S(59, 77)},
        {S(-23, 138), S(-10, 143), S(-3, 150), S(5, 153), S(5, 155), S(9, 160), S(11, 165), S(16, 167), S(21, 170), S(24, 173), S(27, 175), S(27, 177), S(27, 177), S(27, 177), S(27, 177)},
        {S(58, 119), S(58, 131), S(58, 144), S(60, 153), S(61, 165), S(65, 175), S(66, 183), S(72, 183), S(73, 192), S(77, 193), S(80, 193), S(81, 198), S(83, 201), S(83, 201), S(87, 201), S(87, 201), S(89, 201), S(90, 201), S(90, 201), S(94, 201), S(95, 201), S(95, 201), S(101, 201), S(101, 201), S(114, 201), S(114, 201), S(114, 201), S(115, 214)},
        {},
    },
    {S(0, 0), S(-39, 9), S(-39, 19), S(-39, 19), S(0, 115), S(115, 313), S(366, 513), S(0, 0)},
    {S(0, 0), S(-12, 1), S(28, 7), S(37, 7), S(56, 31), S(74, 74), S(74, 201), S(0, 0)},
    S(85, 14), // RookOpenFileBonus
    S(35, 14), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(50, 24), // KnightOutpostBonus
    S(62, 13), // BishopOutpostBonus
    S(-47, 0), // TrappedRookByKingPenalty
    S(24, 26), // RookBehindOurPasserBonus
    S(8, 83), // RookBehindTheirPasserBonus
    S(31, 12), // MinorBehindPawnBonus
    S(35, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-12, 0), // KingProtector
    S(43, 11), // BishopPair
    {
        {S(0, 0), S(86, 0), S(95, 0), S(59, 0), S(40, 0), S(55, 0), S(159, 0)},
        {S(-37, 0), S(161, 0), S(88, 0), S(48, 0), S(0, 0), S(54, 0), S(0, 0)},
        {S(0, 0), S(121, 0), S(39, 0), S(41, 0), S(19, 0), S(37, 0), S(76, 0)},
        {S(0, 0), S(91, 0), S(22, 0), S(20, 0), S(50, 0), S(20, 0), S(64, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(80, 0), S(11, 0), S(0, 0), S(11, 0)},
        {S(0, 0), S(0, 0), S(12, 0), S(16, 0), S(0, 0), S(0, 0), S(4, 0)},
        {S(0, 0), S(0, 0), S(102, 0), S(52, 0), S(13, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(44, 0), S(36, 0), S(3, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(145, 0), S(11, 0), S(11, 0), S(17, 0), S(28, 0)}, // BlockedStorm
    S(-25, -3), // UndefendedKingZoneSq
    S(14, 3), // KingMobilityFactor
    S(26, 7), // KingAttackByKnight
    S(4, 40), // KingAttackByBishop (clamped to 0..40)
    S(26, 7), // KingAttackByRook
    S(26, 7), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(33, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(5, 0), // KingRingWeakWeight
    S(9, 64), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-1, -29), // DoubledPawnPenalty
    S(-13, -6), // BackwardPawnPenalty
    S(-30, -13), // WeakUnopposedPenalty
    S(-38, -44), // DoubledIsolatedPenalty
    {S(-4, -27), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-35, -18), // PawnIslandPenalty
    {S(26, 0), S(3, 0)}, // CentralPawnBonus
    S(27, 19), // BishopLongDiagonalBonus
    S(-19, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 18), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(60, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(9, 2), // RestrictedPiece
    S(45, 34), // ThreatByPawnPush
    S(-21, -2), // WeakQueenDefender
    S(42, 2), // KnightOnQueen
    S(-206, -74), // PawnlessFlank
    S(0, 48), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 40), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 25), // KBNKPushClose
    S(0, 30), // KQKRPushToEdge
    S(0, 15), // KQKRPushClose
    S(0, 0), // KPsKFortressScale
    S(0, 1), // KBPKNDrawishScale
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
