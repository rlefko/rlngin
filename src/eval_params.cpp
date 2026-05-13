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
    S(286, 439), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 88), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(60, 1), // ThreatByKing
    S(29, 24), // Hanging
    S(50, 50), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 29), S(0, 39), S(0, 39), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 11), S(0, 30), S(0, 57), S(0, 74), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-14, -30), S(-14, -45), S(-14, -75), S(-161, -118), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 26), S(0, 60), S(18, 114), S(121, 378), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(23, 28), S(23, 28), S(73, 28), S(73, 28), S(0, 0)},
    S(0, 41), // RookOn7thBonus
    S(-15, 0), // BadBishop
    S(-3, -9), // BishopPawns
    S(42, 0), // Tempo
    {S(0, 0), S(133, 232), S(577, 655), S(680, 722), S(914, 1259), S(1911, 2298), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-28, -15), S(-14, -2), S(-16, -3), S(-21, 1), S(-6, 12), S(-6, 1), S(-16, 0), S(-32, -23),
        S(-54, -26), S(-48, -18), S(-33, -23), S(-51, -41), S(-28, -33), S(-31, -17), S(-40, -20), S(-59, -26),
        S(-27, -10), S(-28, -10), S(-12, -28), S(3, -46), S(8, -43), S(-10, -23), S(-25, -13), S(-28, -15),
        S(-1, 19), S(-6, 12), S(10, -8), S(60, -8), S(79, -8), S(15, -9), S(-5, 11), S(0, 17),
        S(11, 28), S(14, 26), S(32, 16), S(61, 15), S(63, 15), S(34, 16), S(14, 24), S(11, 30),
        S(13, 23), S(17, 23), S(35, 22), S(53, 23), S(53, 24), S(35, 22), S(17, 23), S(13, 23),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-106, -35), S(-109, -26), S(-90, -22), S(-66, -16),
        S(-89, -27), S(-68, -14), S(-65, -15), S(-50, -6),
        S(-73, -20), S(-35, -8), S(-9, -9), S(4, 14),
        S(-5, -2), S(8, 8), S(21, 22), S(11, 29),
        S(21, 6), S(30, 14), S(44, 27), S(54, 34),
        S(27, -6), S(45, 6), S(65, 24), S(77, 33),
        S(25, -15), S(36, -4), S(55, 7), S(71, 28),
        S(16, -23), S(32, -7), S(47, 6), S(58, 16)
    },
    // BishopPST (half-board)
    {
        S(-8, -20), S(6, -13), S(-77, -12), S(-67, -15),
        S(7, -18), S(-11, -38), S(-2, -8), S(-47, -5),
        S(-13, -16), S(8, 2), S(-10, -5), S(11, 7),
        S(-3, -15), S(6, 4), S(27, 17), S(17, 13),
        S(-20, -15), S(1, 13), S(27, 19), S(34, 20),
        S(11, -4), S(14, 16), S(25, 13), S(38, 26),
        S(-8, -12), S(-11, 0), S(10, 19), S(21, 23),
        S(-11, -7), S(-6, 6), S(4, 10), S(10, 14)
    },
    // RookPST (half-board)
    {
        S(-72, -31), S(-36, -32), S(-27, -29), S(-2, -35),
        S(-65, -37), S(-28, -37), S(-22, -30), S(-11, -30),
        S(-42, -28), S(-6, -18), S(-11, -17), S(1, -21),
        S(-30, -5), S(2, 7), S(2, 11), S(3, 2),
        S(-13, 19), S(14, 31), S(23, 30), S(24, 22),
        S(3, 23), S(24, 32), S(37, 35), S(51, 32),
        S(-3, -2), S(1, 3), S(28, 11), S(46, 14),
        S(14, 15), S(22, 23), S(35, 26), S(39, 23)
    },
    // QueenPST (half-board)
    {
        S(3, -72), S(16, -69), S(8, -65), S(24, -64),
        S(15, -62), S(18, -63), S(33, -51), S(29, -35),
        S(25, -38), S(35, -18), S(30, 3), S(20, 9),
        S(42, -14), S(44, 12), S(30, 41), S(-10, 72),
        S(10, -19), S(16, 10), S(7, 37), S(1, 62),
        S(5, -4), S(-14, 18), S(-8, 44), S(3, 67),
        S(-33, 0), S(-100, 20), S(-43, 38), S(-22, 54),
        S(-50, 0), S(-61, 19), S(-46, 32), S(-31, 43)
    },
    // KingPST (half-board)
    {
        S(62, -125), S(41, -96), S(5, -77), S(-81, -66),
        S(64, -85), S(35, -63), S(-13, -55), S(-27, -44),
        S(3, -57), S(-1, -40), S(-15, -32), S(-20, -28),
        S(-16, -19), S(-8, -3), S(-7, 1), S(-6, -9),
        S(-8, 20), S(-4, 32), S(-2, 35), S(-2, 31),
        S(-1, 44), S(4, 56), S(5, 58), S(3, 54),
        S(-1, 48), S(4, 58), S(4, 60), S(3, 60),
        S(0, 50), S(3, 56), S(4, 60), S(3, 61)
    },
    {
        {},
        {},
        {S(-44, -65), S(-24, -45), S(-24, -25), S(-6, -10), S(9, 5), S(14, 20), S(23, 20), S(35, 20), S(35, 20)},
        {S(-40, -27), S(-22, -24), S(-4, -6), S(14, 7), S(27, 23), S(27, 37), S(28, 42), S(32, 43), S(32, 43), S(34, 43), S(37, 43), S(38, 43), S(38, 43), S(38, 43)},
        {S(-21, -9), S(-17, -6), S(-5, 3), S(5, 12), S(5, 16), S(14, 26), S(19, 31), S(26, 37), S(31, 44), S(39, 49), S(43, 54), S(50, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-27, -57), S(-15, -45), S(-15, -33), S(-10, -31), S(2, -19), S(2, -13), S(2, -2), S(2, 10), S(7, 22), S(16, 24), S(18, 26), S(18, 35), S(26, 35), S(29, 35), S(33, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(47, 35), S(59, 47)},
        {},
    },
    {S(0, 0), S(-39, 17), S(-39, 17), S(-39, 18), S(0, 122), S(92, 326), S(276, 570), S(0, 0)},
    {S(0, 0), S(-6, -2), S(30, 2), S(34, 3), S(49, 32), S(56, 87), S(115, 177), S(0, 0)},
    S(64, 17), // RookOpenFileBonus
    S(30, 17), // RookSemiOpenFileBonus
    S(10, 0), // RookOnQueenFile
    S(34, 28), // KnightOutpostBonus
    S(50, 24), // BishopOutpostBonus
    S(-37, 0), // TrappedRookByKingPenalty
    S(32, 19), // RookBehindOurPasserBonus
    S(6, 83), // RookBehindTheirPasserBonus
    S(22, 9), // MinorBehindPawnBonus
    S(24, 3), // MinorOnKingRing
    S(1, 0), // RookOnKingRing
    S(-7, -3), // KingProtector
    S(18, 17), // BishopPair
    {
        {S(0, 0), S(78, 0), S(78, 0), S(52, 0), S(40, 0), S(45, 0), S(121, 0)},
        {S(-35, 0), S(129, 0), S(77, 0), S(39, 0), S(0, 0), S(35, 0), S(0, 0)},
        {S(-2, 0), S(111, 0), S(36, 0), S(41, 0), S(21, 0), S(39, 0), S(81, 0)},
        {S(0, 0), S(4, 0), S(10, 0), S(12, 0), S(47, 0), S(24, 0), S(67, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(59, 0), S(6, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(6, 0), S(1, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(59, 0), S(35, 0), S(0, 0), S(0, 0), S(1, 0)},
        {S(0, 0), S(0, 0), S(38, 0), S(29, 0), S(5, 0), S(0, 0), S(31, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(111, 0), S(3, 0), S(12, 0), S(8, 0), S(6, 0)}, // BlockedStorm
    S(-25, -1), // UndefendedKingZoneSq
    S(14, 3), // KingMobilityFactor
    S(21, 1), // KingAttackByKnight
    S(2, 15), // KingAttackByBishop
    S(21, 13), // KingAttackByRook
    S(21, 13), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(27, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(2, 85), // KingRingWeakWeight
    S(0, 37), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-32, -28), // DoubledPawnPenalty
    S(-14, -2), // BackwardPawnPenalty
    S(-22, -15), // WeakUnopposedPenalty
    S(-19, -49), // DoubledIsolatedPenalty
    {S(0, -21), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-31, -18), // PawnIslandPenalty
    {S(23, 0), S(1, 0)}, // CentralPawnBonus
    S(25, 12), // BishopLongDiagonalBonus
    S(-13, 0), // BishopXrayPawns
    S(0, 20), // InitiativePasser
    S(0, 9), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -3), // InitiativeConstant
    S(46, 60), // SliderOnQueenBishop
    S(60, 41), // SliderOnQueenRook
    S(8, 1), // RestrictedPiece
    S(88, 27), // ThreatByPawnPush
    S(-17, -1), // WeakQueenDefender
    S(29, 0), // KnightOnQueen
    S(-140, -95), // PawnlessFlank
    S(0, 61), // QueenInfiltration
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
    S(0, 18), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
