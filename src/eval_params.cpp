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
    S(360, 280), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 73), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(65, 3), // ThreatByKing
    S(29, 34), // Hanging
    S(50, 50), // WeakQueen
    S(0, 24), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 17), S(0, 27), S(0, 31), S(0, 31), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 11), S(0, 34), S(0, 60), S(0, 83), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-7, -16), S(-10, -38), S(-10, -71), S(-95, -122), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 24), S(0, 49), S(14, 111), S(254, 337), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(17, 28), S(35, 28), S(69, 28), S(69, 28), S(0, 0)},
    S(0, 51), // RookOn7thBonus
    S(-20, 0), // BadBishop
    S(-4, -6), // BishopPawns
    S(51, 0), // Tempo
    {S(0, 0), S(159, 209), S(511, 697), S(615, 772), S(874, 1301), S(1674, 2434), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-29, -8), S(-22, -1), S(-27, 1), S(-18, -27), S(-13, -23), S(1, 2), S(-20, 2), S(-46, -11),
        S(-68, -21), S(-60, -12), S(-40, -21), S(-68, -36), S(-40, -29), S(-37, -16), S(-54, -20), S(-73, -25),
        S(-26, -3), S(-33, -7), S(-22, -31), S(3, -53), S(-7, -48), S(-21, -20), S(-33, -15), S(-31, -13),
        S(-4, 20), S(-7, 13), S(3, -5), S(79, -11), S(101, -6), S(15, -1), S(-11, 15), S(0, 21),
        S(6, 29), S(14, 24), S(43, 12), S(92, 21), S(94, 23), S(44, 15), S(14, 25), S(8, 31),
        S(9, 22), S(19, 21), S(43, 22), S(77, 33), S(78, 35), S(44, 22), S(18, 19), S(10, 24),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-120, -41), S(-136, -36), S(-122, -12), S(-86, -10),
        S(-89, -29), S(-72, -12), S(-78, -11), S(-57, -7),
        S(-102, -12), S(-31, -9), S(-25, -6), S(1, 19),
        S(-26, -6), S(31, 0), S(24, 30), S(18, 31),
        S(-13, 16), S(32, 11), S(38, 37), S(34, 47),
        S(19, -22), S(71, -3), S(84, 23), S(82, 39),
        S(36, -21), S(50, -10), S(78, 0), S(111, 29),
        S(46, -31), S(54, -12), S(72, 6), S(88, 15)
    },
    // BishopPST (half-board)
    {
        S(22, -29), S(21, -20), S(-73, -13), S(-67, -30),
        S(21, -12), S(4, -59), S(18, -17), S(-52, -4),
        S(-10, -21), S(12, -5), S(-11, -7), S(8, 15),
        S(-3, -18), S(12, 6), S(29, 15), S(6, 14),
        S(-25, -17), S(21, 5), S(32, 18), S(27, 19),
        S(9, 9), S(16, 17), S(14, 13), S(58, 29),
        S(-14, -2), S(-25, 6), S(1, 19), S(24, 27),
        S(-37, -13), S(-23, 8), S(-6, 12), S(1, 21)
    },
    // RookPST (half-board)
    {
        S(-74, -29), S(-43, -35), S(-26, -33), S(0, -34),
        S(-62, -30), S(-13, -35), S(-14, -33), S(-12, -28),
        S(-43, -12), S(-4, -27), S(-22, -18), S(5, -33),
        S(-36, 4), S(7, 7), S(9, 12), S(5, -5),
        S(-48, 27), S(14, 30), S(33, 28), S(25, 22),
        S(-10, 23), S(17, 26), S(45, 32), S(67, 33),
        S(-18, -14), S(-6, -2), S(36, 3), S(49, 9),
        S(13, 27), S(28, 19), S(35, 20), S(42, 22)
    },
    // QueenPST (half-board)
    {
        S(-5, -79), S(16, -68), S(17, -64), S(31, -51),
        S(23, -72), S(19, -72), S(30, -57), S(23, -33),
        S(-7, -29), S(50, -24), S(18, -2), S(-5, 8),
        S(47, -16), S(67, -19), S(37, 21), S(-28, 79),
        S(33, -31), S(34, -2), S(7, 43), S(-27, 78),
        S(8, -8), S(-9, 1), S(8, 63), S(-5, 88),
        S(-33, -23), S(-86, 28), S(-39, 58), S(-34, 67),
        S(-92, -18), S(-66, 34), S(-45, 45), S(-11, 42)
    },
    // KingPST (half-board)
    {
        S(38, -126), S(25, -92), S(-12, -76), S(-105, -58),
        S(51, -90), S(15, -63), S(-26, -47), S(-28, -42),
        S(-23, -56), S(-14, -36), S(-22, -35), S(-26, -23),
        S(-65, -26), S(-30, -6), S(-11, -1), S(-1, -10),
        S(-19, 16), S(-3, 32), S(-2, 39), S(7, 35),
        S(3, 39), S(14, 64), S(22, 70), S(22, 51),
        S(12, 46), S(21, 58), S(28, 59), S(30, 47),
        S(16, 47), S(26, 54), S(44, 62), S(37, 56)
    },
    {
        {},
        {},
        {S(-76, -68), S(-56, -48), S(-38, -32), S(-18, -12), S(0, 0), S(0, 20), S(16, 20), S(26, 20), S(29, 20)},
        {S(-40, -32), S(-22, -26), S(-4, -8), S(14, 10), S(32, 20), S(35, 38), S(35, 42), S(35, 42), S(37, 43), S(37, 43), S(37, 43), S(37, 43), S(37, 43), S(37, 61)},
        {S(-20, -26), S(-13, -11), S(2, 0), S(16, 1), S(16, 1), S(22, 16), S(22, 30), S(23, 35), S(36, 41), S(37, 48), S(50, 53), S(50, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-20, -61), S(-8, -61), S(-8, -50), S(2, -38), S(10, -26), S(10, -14), S(10, -2), S(10, 10), S(12, 22), S(24, 22), S(28, 23), S(28, 35), S(33, 35), S(33, 35), S(33, 35), S(34, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(45, 35), S(45, 35), S(51, 35), S(60, 47)},
        {},
    },
    {S(0, 0), S(-43, 6), S(-43, 27), S(-43, 28), S(0, 128), S(66, 347), S(217, 595), S(0, 0)},
    {S(0, 0), S(-5, -3), S(25, 5), S(34, 5), S(46, 40), S(46, 99), S(46, 227), S(0, 0)},
    S(68, 22), // RookOpenFileBonus
    S(37, 22), // RookSemiOpenFileBonus
    S(0, 0), // RookOnQueenFile
    S(48, 25), // KnightOutpostBonus
    S(55, 14), // BishopOutpostBonus
    S(-49, 0), // TrappedRookByKingPenalty
    S(6, 31), // RookBehindOurPasserBonus
    S(33, 81), // RookBehindTheirPasserBonus
    S(10, 20), // MinorBehindPawnBonus
    S(22, 1), // MinorOnKingRing
    S(2, 0), // RookOnKingRing
    S(-7, -3), // KingProtector
    S(14, 18), // BishopPair
    {
        {S(0, 0), S(67, 0), S(83, 0), S(51, 0), S(34, 0), S(72, 0), S(185, 0)},
        {S(-33, 0), S(120, 0), S(79, 0), S(37, 0), S(5, 0), S(54, 0), S(0, 0)},
        {S(-2, 0), S(99, 0), S(19, 0), S(22, 0), S(12, 0), S(37, 0), S(120, 0)},
        {S(0, 0), S(0, 0), S(12, 0), S(7, 0), S(40, 0), S(2, 0), S(49, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(48, 0), S(4, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(10, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(85, 0), S(45, 0), S(5, 0), S(15, 0), S(4, 0)},
        {S(0, 0), S(0, 0), S(3, 0), S(4, 0), S(0, 0), S(0, 0), S(25, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(111, 0), S(0, 0), S(5, 0), S(16, 0), S(0, 0)}, // BlockedStorm
    S(-23, 0), // UndefendedKingZoneSq
    S(12, 0), // KingMobilityFactor
    S(21, 33), // KingAttackByKnight
    S(8, 21), // KingAttackByBishop
    S(21, 33), // KingAttackByRook
    S(21, 33), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 0), S(27, 619), S(29, 0), S(29, 0), S(0, 0)}, // KingSafeCheck
    S(6, 58), // KingRingWeakWeight
    S(5, 130), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-6, -18), // DoubledPawnPenalty
    S(-19, 0), // BackwardPawnPenalty
    S(-24, -11), // WeakUnopposedPenalty
    S(-25, -55), // DoubledIsolatedPenalty
    {S(0, -21), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-37, -16), // PawnIslandPenalty
    {S(44, 0), S(21, 0)}, // CentralPawnBonus
    S(35, 11), // BishopLongDiagonalBonus
    S(-10, -1), // BishopXrayPawns
    S(0, 2), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 0), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -6), // InitiativeConstant
    S(51, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(5, 5), // RestrictedPiece
    S(66, 3), // ThreatByPawnPush
    S(-28, -1), // WeakQueenDefender
    S(18, 12), // KnightOnQueen
    S(-98, -81), // PawnlessFlank
    S(4, 40), // QueenInfiltration
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
    S(0, 24), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
