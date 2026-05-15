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
    S(392, 280), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 77), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(74, 8), // ThreatByKing
    S(26, 34), // Hanging
    S(50, 50), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 17), S(0, 26), S(0, 31), S(0, 31), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 11), S(0, 33), S(0, 61), S(0, 83), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-7, -16), S(-10, -38), S(-10, -70), S(-116, -111), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 43), S(33, 98), S(278, 325), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(29, 28), S(32, 28), S(69, 28), S(69, 28), S(0, 0)},
    S(0, 38), // RookOn7thBonus
    S(-10, -9), // BadBishop
    S(-5, -6), // BishopPawns
    S(52, 0), // Tempo
    {S(0, 0), S(165, 205), S(510, 691), S(606, 773), S(866, 1292), S(1664, 2429), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-33, -7), S(-26, 1), S(-31, 4), S(-17, -27), S(-9, -21), S(-5, 6), S(-19, 0), S(-49, -10),
        S(-70, -18), S(-64, -13), S(-39, -25), S(-68, -34), S(-39, -29), S(-37, -17), S(-55, -20), S(-77, -24),
        S(-27, -5), S(-34, -7), S(-17, -33), S(1, -57), S(-8, -54), S(-14, -22), S(-31, -19), S(-28, -15),
        S(-8, 20), S(-13, 15), S(3, -1), S(83, -15), S(103, -7), S(15, -3), S(-13, 13), S(-5, 19),
        S(7, 25), S(11, 27), S(41, 16), S(92, 20), S(95, 23), S(44, 18), S(16, 33), S(8, 30),
        S(12, 19), S(20, 20), S(49, 21), S(80, 33), S(81, 35), S(50, 21), S(19, 19), S(13, 21),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-114, -51), S(-129, -55), S(-117, -6), S(-98, -11),
        S(-84, -33), S(-66, -11), S(-79, -13), S(-55, -17),
        S(-104, -13), S(-31, -6), S(-26, -14), S(-5, 21),
        S(-31, 0), S(18, 5), S(15, 35), S(5, 40),
        S(-18, 20), S(34, 12), S(31, 39), S(21, 53),
        S(27, -27), S(70, -5), S(83, 21), S(92, 32),
        S(40, -24), S(57, -12), S(84, 1), S(118, 27),
        S(53, -32), S(62, -12), S(79, 6), S(96, 17)
    },
    // BishopPST (half-board)
    {
        S(30, -34), S(22, -18), S(-72, -12), S(-65, -22),
        S(22, -12), S(8, -62), S(18, -11), S(-51, 5),
        S(-2, -27), S(16, -2), S(-13, -13), S(5, 21),
        S(2, -30), S(9, 11), S(25, 21), S(2, 11),
        S(-32, -13), S(15, 24), S(28, 21), S(22, 12),
        S(6, 12), S(13, 20), S(13, 8), S(46, 26),
        S(-26, 3), S(-23, -3), S(-4, 19), S(20, 28),
        S(-32, -8), S(-20, 9), S(-6, 12), S(2, 16)
    },
    // RookPST (half-board)
    {
        S(-76, -22), S(-46, -33), S(-25, -34), S(7, -39),
        S(-63, -27), S(-15, -35), S(-12, -34), S(-3, -34),
        S(-45, -15), S(-10, -30), S(-20, -19), S(4, -33),
        S(-35, 9), S(7, 9), S(11, 13), S(-1, -2),
        S(-53, 38), S(7, 34), S(23, 27), S(26, 21),
        S(-14, 24), S(21, 20), S(43, 35), S(72, 24),
        S(-19, 2), S(-12, 5), S(40, 8), S(64, 26),
        S(7, 29), S(12, 22), S(34, 21), S(45, 21)
    },
    // QueenPST (half-board)
    {
        S(-5, -76), S(13, -61), S(16, -63), S(37, -62),
        S(4, -58), S(21, -62), S(34, -65), S(28, -41),
        S(-2, -16), S(59, -27), S(22, -10), S(-9, 9),
        S(50, -23), S(58, 0), S(38, 23), S(-32, 84),
        S(28, -33), S(29, 19), S(8, 43), S(-22, 78),
        S(22, -17), S(-23, 19), S(24, 33), S(18, 75),
        S(-35, -48), S(-84, 24), S(-28, 45), S(-20, 63),
        S(-118, 15), S(-71, 38), S(-37, 41), S(7, 34)
    },
    // KingPST (half-board)
    {
        S(56, -133), S(39, -92), S(-6, -73), S(-91, -54),
        S(65, -92), S(30, -66), S(-11, -48), S(-18, -40),
        S(-10, -55), S(-4, -36), S(-11, -35), S(-20, -20),
        S(-71, -18), S(-10, -7), S(-8, -3), S(2, -10),
        S(-20, 19), S(2, 34), S(10, 32), S(4, 29),
        S(-12, 36), S(12, 63), S(15, 71), S(10, 51),
        S(4, 46), S(11, 59), S(8, 61), S(-19, 49),
        S(7, 47), S(10, 54), S(9, 58), S(0, 56)
    },
    {
        {},
        {},
        {S(-78, -74), S(-58, -54), S(-38, -34), S(-18, -14), S(0, 1), S(0, 20), S(20, 20), S(29, 20), S(38, 20)},
        {S(-40, -34), S(-22, -26), S(-4, -8), S(14, 7), S(32, 19), S(35, 32), S(35, 36), S(35, 41), S(37, 41), S(37, 43), S(37, 43), S(37, 43), S(37, 43), S(37, 61)},
        {S(-25, -26), S(-13, -11), S(2, 1), S(16, 1), S(16, 1), S(22, 16), S(22, 28), S(22, 31), S(34, 42), S(42, 42), S(47, 52), S(50, 52), S(50, 55), S(50, 55), S(50, 55)},
        {S(-27, -74), S(-15, -62), S(-9, -50), S(3, -38), S(10, -26), S(10, -14), S(10, -2), S(12, 10), S(12, 22), S(21, 22), S(31, 23), S(33, 35), S(33, 35), S(33, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(45, 35), S(45, 35), S(51, 43), S(63, 47)},
        {},
    },
    {S(0, 0), S(-43, 1), S(-43, 27), S(-43, 28), S(0, 126), S(66, 347), S(226, 582), S(0, 0)},
    {S(0, 0), S(-4, -2), S(24, 6), S(33, 6), S(46, 41), S(46, 96), S(46, 229), S(0, 0)},
    S(73, 20), // RookOpenFileBonus
    S(41, 20), // RookSemiOpenFileBonus
    S(0, 0), // RookOnQueenFile
    S(56, 21), // KnightOutpostBonus
    S(64, 8), // BishopOutpostBonus
    S(-46, 0), // TrappedRookByKingPenalty
    S(7, 38), // RookBehindOurPasserBonus
    S(38, 74), // RookBehindTheirPasserBonus
    S(8, 24), // MinorBehindPawnBonus
    S(22, 0), // MinorOnKingRing
    S(3, 0), // RookOnKingRing
    S(-7, -3), // KingProtector
    S(18, 11), // BishopPair
    {
        {S(0, 0), S(66, 0), S(85, 0), S(48, 0), S(42, 0), S(64, 0), S(230, 0)},
        {S(-33, 0), S(113, 0), S(79, 0), S(36, 0), S(8, 0), S(57, 0), S(0, 0)},
        {S(-3, 0), S(99, 0), S(16, 0), S(14, 0), S(4, 0), S(39, 0), S(148, 0)},
        {S(0, 0), S(0, 0), S(18, 0), S(9, 0), S(47, 0), S(0, 0), S(59, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(45, 0), S(5, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(0, 0), S(13, 0), S(0, 0), S(0, 0), S(4, 0)},
        {S(0, 0), S(0, 0), S(89, 0), S(32, 0), S(6, 0), S(17, 0), S(3, 0)},
        {S(0, 0), S(0, 0), S(15, 0), S(2, 0), S(16, 0), S(0, 0), S(36, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(96, 0), S(0, 0), S(0, 0), S(17, 0), S(3, 0)}, // BlockedStorm
    S(-25, 0), // UndefendedKingZoneSq
    S(12, 0), // KingMobilityFactor
    S(21, 27), // KingAttackByKnight
    S(8, 31), // KingAttackByBishop
    S(21, 33), // KingAttackByRook
    S(21, 46), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 0), S(28, 619), S(29, 0), S(29, 0), S(0, 0)}, // KingSafeCheck
    S(6, 53), // KingRingWeakWeight
    S(5, 137), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -24), // DoubledPawnPenalty
    S(-21, 0), // BackwardPawnPenalty
    S(-29, -7), // WeakUnopposedPenalty
    S(-26, -50), // DoubledIsolatedPenalty
    {S(-1, -20), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-42, -13), // PawnIslandPenalty
    {S(51, 0), S(17, 0)}, // CentralPawnBonus
    S(33, 22), // BishopLongDiagonalBonus
    S(-9, 0), // BishopXrayPawns
    S(0, 3), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 0), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -7), // InitiativeConstant
    S(35, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(5, 5), // RestrictedPiece
    S(70, 12), // ThreatByPawnPush
    S(-18, -7), // WeakQueenDefender
    S(19, 8), // KnightOnQueen
    S(-98, -77), // PawnlessFlank
    S(8, 33), // QueenInfiltration
    S(0, -3), // KingPawnDistEg
    S(0, 20), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 23), // KBNKPushClose
    S(0, 30), // KQKRPushToEdge
    S(0, 15), // KQKRPushClose
    S(0, 1), // KPsKFortressScale
    S(0, 0), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
    S(0, 23), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
