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
    S(312, 329), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 98), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(59, 7), // ThreatByKing
    S(39, 23), // Hanging
    S(50, 50), // WeakQueen
    S(0, 17), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 18), S(0, 27), S(0, 33), S(0, 33), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 13), S(0, 33), S(0, 60), S(0, 81), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-10, -28), S(-10, -47), S(-10, -74), S(-114, -121), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 24), S(0, 54), S(17, 111), S(235, 342), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(19, 28), S(19, 28), S(64, 28), S(94, 28), S(0, 0)},
    S(0, 41), // RookOn7thBonus
    S(-9, -3), // BadBishop
    S(-3, -8), // BishopPawns
    S(38, 0), // Tempo
    {S(0, 0), S(153, 219), S(507, 699), S(609, 779), S(855, 1322), S(1719, 2436), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-34, -11), S(-18, -4), S(-27, 0), S(-24, -22), S(-10, -15), S(-6, 4), S(-20, 1), S(-37, -17),
        S(-64, -27), S(-59, -15), S(-38, -22), S(-57, -44), S(-32, -34), S(-38, -15), S(-51, -21), S(-77, -24),
        S(-29, -9), S(-37, -12), S(-15, -29), S(5, -43), S(5, -41), S(-13, -25), S(-37, -15), S(-31, -14),
        S(-1, 20), S(-8, 15), S(1, -4), S(66, -8), S(86, -4), S(11, -1), S(-11, 15), S(2, 20),
        S(14, 26), S(18, 30), S(37, 17), S(82, 19), S(85, 20), S(39, 19), S(18, 27), S(13, 30),
        S(14, 19), S(20, 22), S(42, 21), S(70, 29), S(71, 30), S(42, 22), S(20, 22), S(15, 21),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-117, -43), S(-132, -33), S(-104, -27), S(-72, -18),
        S(-88, -32), S(-70, -17), S(-71, -17), S(-56, -9),
        S(-88, -20), S(-35, -11), S(-16, -9), S(10, 13),
        S(-9, -7), S(8, 8), S(24, 26), S(16, 32),
        S(16, 5), S(31, 17), S(46, 34), S(53, 40),
        S(27, -9), S(48, 8), S(74, 26), S(88, 36),
        S(30, -15), S(40, -6), S(64, 6), S(87, 33),
        S(19, -23), S(38, -6), S(59, 12), S(73, 20)
    },
    // BishopPST (half-board)
    {
        S(-10, -18), S(13, -13), S(-82, -9), S(-72, -18),
        S(12, -20), S(-5, -45), S(0, -8), S(-45, -10),
        S(-20, -18), S(5, 3), S(-13, -7), S(7, 10),
        S(-7, -14), S(6, 4), S(29, 16), S(17, 12),
        S(-30, -17), S(13, 9), S(30, 19), S(38, 20),
        S(11, -4), S(17, 17), S(27, 9), S(45, 27),
        S(-11, -12), S(-18, -2), S(11, 20), S(24, 24),
        S(-15, -10), S(-8, 10), S(2, 10), S(6, 11)
    },
    // RookPST (half-board)
    {
        S(-81, -31), S(-39, -34), S(-35, -27), S(-4, -35),
        S(-68, -33), S(-20, -37), S(-25, -30), S(-13, -31),
        S(-43, -26), S(-1, -23), S(-11, -17), S(8, -26),
        S(-28, -5), S(5, 5), S(1, 10), S(4, 2),
        S(-25, 20), S(12, 31), S(27, 26), S(26, 21),
        S(-5, 24), S(23, 31), S(39, 33), S(61, 33),
        S(-13, -3), S(-6, 3), S(29, 10), S(49, 16),
        S(9, 19), S(21, 24), S(36, 26), S(40, 24)
    },
    // QueenPST (half-board)
    {
        S(-3, -86), S(27, -86), S(9, -73), S(27, -66),
        S(16, -76), S(21, -71), S(35, -59), S(33, -46),
        S(16, -45), S(34, -21), S(26, -1), S(19, 1),
        S(42, -20), S(46, 5), S(33, 37), S(8, 56),
        S(5, -18), S(13, 13), S(6, 44), S(-4, 76),
        S(11, -1), S(-13, 23), S(-15, 53), S(-4, 84),
        S(-35, -4), S(-75, 20), S(-42, 51), S(-28, 70),
        S(-64, 8), S(-59, 33), S(-51, 47), S(-40, 58)
    },
    // KingPST (half-board)
    {
        S(43, -135), S(20, -95), S(-18, -75), S(-113, -59),
        S(47, -90), S(17, -63), S(-32, -51), S(-38, -44),
        S(-23, -58), S(-16, -42), S(-23, -33), S(-23, -26),
        S(-45, -22), S(-18, -4), S(-6, -2), S(-7, -2),
        S(-18, 21), S(-1, 34), S(10, 37), S(9, 34),
        S(5, 44), S(16, 61), S(19, 61), S(19, 56),
        S(14, 47), S(20, 59), S(24, 61), S(25, 62),
        S(17, 47), S(21, 54), S(25, 60), S(27, 63)
    },
    {
        {},
        {},
        {S(-76, -68), S(-56, -48), S(-36, -28), S(-20, -11), S(-2, 5), S(9, 20), S(19, 20), S(27, 20), S(27, 20)},
        {S(-40, -43), S(-22, -28), S(-4, -10), S(13, 6), S(27, 24), S(30, 39), S(35, 42), S(35, 43), S(35, 43), S(37, 43), S(37, 43), S(37, 43), S(37, 43), S(37, 43)},
        {S(-30, -22), S(-15, -10), S(-3, -3), S(10, 5), S(10, 8), S(19, 18), S(22, 27), S(27, 34), S(35, 41), S(38, 48), S(50, 50), S(50, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-8, -62), S(-8, -62), S(-8, -50), S(-8, -38), S(-7, -26), S(0, -14), S(2, -2), S(6, 10), S(10, 22), S(18, 23), S(20, 27), S(22, 35), S(27, 35), S(29, 35), S(34, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(45, 35), S(45, 35), S(47, 35), S(59, 47)},
        {},
    },
    {S(0, 0), S(-43, 14), S(-43, 25), S(-43, 33), S(0, 130), S(65, 345), S(210, 604), S(0, 0)},
    {S(0, 0), S(-10, 0), S(26, 3), S(31, 6), S(46, 35), S(46, 94), S(46, 216), S(0, 0)},
    S(69, 17), // RookOpenFileBonus
    S(33, 17), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(37, 26), // KnightOutpostBonus
    S(41, 21), // BishopOutpostBonus
    S(-41, 0), // TrappedRookByKingPenalty
    S(19, 27), // RookBehindOurPasserBonus
    S(21, 84), // RookBehindTheirPasserBonus
    S(19, 15), // MinorBehindPawnBonus
    S(29, 1), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-7, -3), // KingProtector
    S(24, 16), // BishopPair
    {
        {S(0, 0), S(74, 0), S(80, 0), S(50, 0), S(34, 0), S(51, 0), S(130, 0)},
        {S(-29, 0), S(126, 0), S(79, 0), S(39, 0), S(0, 0), S(41, 0), S(0, 0)},
        {S(0, 0), S(105, 0), S(32, 0), S(35, 0), S(18, 0), S(40, 0), S(71, 0)},
        {S(0, 0), S(24, 0), S(5, 0), S(8, 0), S(48, 0), S(20, 0), S(55, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(61, 0), S(3, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(7, 0), S(7, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(79, 0), S(36, 0), S(11, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(38, 0), S(27, 0), S(6, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(116, 0), S(0, 0), S(10, 0), S(13, 0), S(9, 0)}, // BlockedStorm
    S(-25, 0), // UndefendedKingZoneSq
    S(12, 0), // KingMobilityFactor
    S(21, 28), // KingAttackByKnight
    S(4, 22), // KingAttackByBishop
    S(21, 33), // KingAttackByRook
    S(21, 33), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 0), S(27, 619), S(29, 0), S(29, 0), S(0, 0)}, // KingSafeCheck
    S(5, 60), // KingRingWeakWeight
    S(2, 112), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-14, -22), // DoubledPawnPenalty
    S(-18, -5), // BackwardPawnPenalty
    S(-24, -13), // WeakUnopposedPenalty
    S(-31, -51), // DoubledIsolatedPenalty
    {S(0, -24), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-33, -20), // PawnIslandPenalty
    {S(20, 0), S(3, 0)}, // CentralPawnBonus
    S(26, 14), // BishopLongDiagonalBonus
    S(-16, 0), // BishopXrayPawns
    S(0, 6), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 6), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -4), // InitiativeConstant
    S(50, 60), // SliderOnQueenBishop
    S(60, 40), // SliderOnQueenRook
    S(7, 3), // RestrictedPiece
    S(85, 6), // ThreatByPawnPush
    S(-19, -5), // WeakQueenDefender
    S(34, 5), // KnightOnQueen
    S(-109, -93), // PawnlessFlank
    S(0, 44), // QueenInfiltration
    S(0, -1), // KingPawnDistEg
    S(0, 20), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 22), // KBNKPushClose
    S(0, 30), // KQKRPushToEdge
    S(0, 15), // KQKRPushClose
    S(0, 1), // KPsKFortressScale
    S(0, 0), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
    S(0, 20), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
