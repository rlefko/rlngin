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
    S(280, 503), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 91), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(61, 0), // ThreatByKing
    S(27, 26), // Hanging
    S(50, 50), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 28), S(0, 39), S(0, 39), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 11), S(0, 30), S(0, 58), S(0, 75), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-14, -31), S(-14, -47), S(-14, -75), S(-157, -118), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 27), S(0, 61), S(20, 115), S(127, 382), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(23, 28), S(23, 28), S(73, 28), S(73, 28), S(0, 0)},
    S(0, 42), // RookOn7thBonus
    S(-16, 0), // BadBishop
    S(-2, -9), // BishopPawns
    S(42, 0), // Tempo
    {S(0, 0), S(132, 234), S(571, 662), S(666, 733), S(897, 1275), S(1877, 2327), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-28, -15), S(-15, -1), S(-17, -2), S(-21, 6), S(-6, 18), S(-8, 2), S(-16, 1), S(-33, -23),
        S(-53, -27), S(-49, -17), S(-34, -22), S(-53, -39), S(-30, -31), S(-33, -16), S(-42, -19), S(-59, -26),
        S(-26, -10), S(-28, -11), S(-14, -27), S(2, -45), S(7, -42), S(-12, -23), S(-26, -13), S(-27, -15),
        S(0, 19), S(-5, 12), S(10, -6), S(59, -5), S(79, -6), S(16, -7), S(-4, 12), S(1, 18),
        S(12, 28), S(15, 26), S(32, 17), S(62, 15), S(64, 16), S(33, 17), S(15, 24), S(12, 30),
        S(13, 22), S(18, 23), S(35, 20), S(54, 22), S(54, 23), S(35, 20), S(18, 23), S(13, 23),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-103, -37), S(-106, -30), S(-89, -23), S(-67, -16),
        S(-88, -28), S(-67, -15), S(-64, -16), S(-49, -6),
        S(-72, -21), S(-35, -8), S(-8, -10), S(4, 14),
        S(-4, -2), S(9, 7), S(22, 21), S(11, 29),
        S(20, 6), S(29, 14), S(44, 27), S(56, 33),
        S(25, -5), S(43, 7), S(64, 24), S(76, 33),
        S(24, -13), S(36, -3), S(54, 7), S(70, 29),
        S(17, -21), S(33, -6), S(48, 7), S(59, 16)
    },
    // BishopPST (half-board)
    {
        S(-8, -19), S(7, -13), S(-75, -11), S(-66, -15),
        S(7, -19), S(-10, -38), S(-1, -9), S(-45, -6),
        S(-12, -17), S(9, 1), S(-8, -6), S(13, 5),
        S(-1, -15), S(6, 3), S(29, 15), S(18, 13),
        S(-19, -15), S(1, 13), S(27, 17), S(34, 20),
        S(12, -3), S(13, 17), S(25, 12), S(38, 24),
        S(-7, -11), S(-11, 0), S(10, 18), S(21, 22),
        S(-10, -6), S(-6, 7), S(3, 10), S(8, 13)
    },
    // RookPST (half-board)
    {
        S(-72, -29), S(-37, -32), S(-27, -29), S(-2, -35),
        S(-65, -37), S(-27, -37), S(-22, -31), S(-12, -29),
        S(-42, -28), S(-6, -19), S(-10, -18), S(1, -20),
        S(-31, -4), S(2, 6), S(3, 10), S(3, 3),
        S(-13, 19), S(15, 31), S(25, 30), S(23, 23),
        S(4, 23), S(24, 32), S(38, 35), S(50, 33),
        S(-2, -2), S(2, 3), S(29, 10), S(45, 14),
        S(15, 15), S(24, 22), S(37, 26), S(42, 24)
    },
    // QueenPST (half-board)
    {
        S(5, -74), S(19, -72), S(11, -70), S(29, -72),
        S(17, -65), S(22, -68), S(36, -56), S(33, -41),
        S(29, -44), S(39, -24), S(34, -2), S(22, 6),
        S(45, -18), S(47, 8), S(32, 39), S(-13, 73),
        S(12, -20), S(17, 10), S(8, 38), S(0, 64),
        S(4, -1), S(-15, 20), S(-10, 47), S(3, 68),
        S(-39, 7), S(-105, 26), S(-45, 42), S(-22, 57),
        S(-56, 8), S(-65, 27), S(-48, 36), S(-32, 46)
    },
    // KingPST (half-board)
    {
        S(55, -125), S(35, -96), S(-1, -77), S(-87, -65),
        S(59, -85), S(29, -63), S(-18, -54), S(-35, -42),
        S(-1, -57), S(-5, -40), S(-19, -31), S(-22, -28),
        S(-17, -20), S(-8, -2), S(-7, 2), S(-6, -6),
        S(-8, 20), S(-2, 32), S(1, 35), S(1, 31),
        S(1, 44), S(8, 57), S(9, 58), S(7, 53),
        S(3, 48), S(8, 58), S(8, 60), S(8, 60),
        S(3, 50), S(7, 56), S(8, 60), S(8, 61)
    },
    {
        {},
        {},
        {S(-44, -65), S(-24, -45), S(-24, -25), S(-6, -10), S(11, 3), S(14, 20), S(22, 20), S(34, 20), S(34, 20)},
        {S(-39, -36), S(-22, -26), S(-4, -8), S(14, 5), S(27, 20), S(27, 36), S(27, 42), S(32, 43), S(32, 43), S(34, 43), S(35, 43), S(38, 43), S(38, 43), S(38, 43)},
        {S(-20, -19), S(-17, -11), S(-4, -1), S(5, 12), S(5, 15), S(14, 25), S(19, 30), S(25, 37), S(31, 43), S(38, 49), S(43, 53), S(50, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-27, -57), S(-15, -45), S(-10, -45), S(-10, -38), S(2, -26), S(2, -14), S(2, -2), S(2, 10), S(5, 22), S(14, 25), S(18, 26), S(18, 35), S(27, 35), S(31, 35), S(34, 35), S(34, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(47, 35), S(59, 47)},
        {},
    },
    {S(0, 0), S(-39, 19), S(-39, 19), S(-39, 19), S(0, 125), S(90, 331), S(271, 577), S(0, 0)},
    {S(0, 0), S(-6, -2), S(30, 2), S(34, 3), S(46, 33), S(50, 90), S(103, 185), S(0, 0)},
    S(63, 17), // RookOpenFileBonus
    S(30, 17), // RookSemiOpenFileBonus
    S(9, 0), // RookOnQueenFile
    S(32, 29), // KnightOutpostBonus
    S(50, 24), // BishopOutpostBonus
    S(-36, 0), // TrappedRookByKingPenalty
    S(32, 19), // RookBehindOurPasserBonus
    S(8, 84), // RookBehindTheirPasserBonus
    S(22, 9), // MinorBehindPawnBonus
    S(24, 3), // MinorOnKingRing
    S(1, 0), // RookOnKingRing
    S(-7, -3), // KingProtector
    S(18, 19), // BishopPair
    {
        {S(0, 0), S(77, 0), S(76, 0), S(50, 0), S(40, 0), S(44, 0), S(120, 0)},
        {S(-35, 0), S(125, 0), S(75, 0), S(38, 0), S(0, 0), S(29, 0), S(0, 0)},
        {S(0, 0), S(113, 0), S(38, 0), S(39, 0), S(23, 0), S(44, 0), S(92, 0)},
        {S(0, 0), S(0, 0), S(7, 0), S(12, 0), S(48, 0), S(22, 0), S(72, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(56, 0), S(4, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(10, 0), S(1, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(58, 0), S(35, 0), S(0, 0), S(0, 0), S(1, 0)},
        {S(0, 0), S(0, 0), S(44, 0), S(28, 0), S(3, 0), S(0, 0), S(21, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(110, 0), S(3, 0), S(11, 0), S(10, 0), S(7, 0)}, // BlockedStorm
    S(-26, 0), // UndefendedKingZoneSq
    S(15, 0), // KingMobilityFactor
    S(21, 9), // KingAttackByKnight
    S(2, 9), // KingAttackByBishop
    S(21, 13), // KingAttackByRook
    S(21, 13), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(28, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(3, 85), // KingRingWeakWeight
    S(0, 41), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-36, -25), // DoubledPawnPenalty
    S(-14, -2), // BackwardPawnPenalty
    S(-21, -15), // WeakUnopposedPenalty
    S(-17, -52), // DoubledIsolatedPenalty
    {S(0, -22), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-32, -18), // PawnIslandPenalty
    {S(21, 0), S(4, 0)}, // CentralPawnBonus
    S(25, 11), // BishopLongDiagonalBonus
    S(-13, 0), // BishopXrayPawns
    S(0, 19), // InitiativePasser
    S(0, 8), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -3), // InitiativeConstant
    S(47, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(7, 1), // RestrictedPiece
    S(95, 25), // ThreatByPawnPush
    S(-19, 0), // WeakQueenDefender
    S(28, 0), // KnightOnQueen
    S(-135, -97), // PawnlessFlank
    S(0, 58), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 0), // KBNKPushClose
    S(0, 0), // KQKRPushToEdge
    S(0, 0), // KQKRPushClose
    S(0, 1), // KPsKFortressScale
    S(0, 0), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
    S(0, 17), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
