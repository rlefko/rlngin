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
    S(368, 397), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(116, 203), S(218, 188), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(283, 289), S(0, 0)},
    S(53, 13), // ThreatByKing
    S(31, 31), // Hanging
    S(294, 100), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 27), S(0, 32), S(0, 32), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 34), S(0, 60), S(0, 79), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-8, -23), S(-8, -45), S(-8, -71), S(-146, -101), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 44), S(34, 95), S(296, 323), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(31, 28), S(31, 28), S(80, 28), S(80, 28), S(0, 0)},
    S(0, 42), // RookOn7thBonus
    S(-15, -3), // BadBishop
    S(-4, -8), // BishopPawns
    S(42, 0), // Tempo
    {S(0, 0), S(157, 212), S(503, 698), S(605, 772), S(874, 1294), S(1665, 2428), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-44, -6), S(-23, -5), S(-35, 2), S(-14, -26), S(0, -20), S(-14, 7), S(-26, 0), S(-44, -15),
        S(-71, -25), S(-71, -14), S(-39, -25), S(-57, -41), S(-29, -30), S(-39, -17), S(-60, -22), S(-84, -22),
        S(-35, -5), S(-39, -12), S(-25, -32), S(-2, -41), S(-9, -43), S(-18, -25), S(-39, -16), S(-36, -12),
        S(2, 18), S(-7, 17), S(2, -3), S(69, -7), S(93, 1), S(10, -1), S(-13, 17), S(-2, 20),
        S(15, 24), S(21, 26), S(50, 15), S(92, 20), S(94, 22), S(50, 17), S(25, 30), S(14, 27),
        S(17, 21), S(25, 21), S(52, 23), S(82, 32), S(83, 34), S(52, 23), S(24, 19), S(18, 23),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-117, -35), S(-124, -27), S(-108, -29), S(-83, -5),
        S(-91, -28), S(-72, -17), S(-74, -21), S(-54, -16),
        S(-96, -19), S(-34, -13), S(-23, -13), S(8, 6),
        S(-22, -3), S(10, 3), S(21, 29), S(4, 32),
        S(-1, 15), S(33, 16), S(40, 36), S(45, 40),
        S(24, -9), S(61, 2), S(82, 22), S(97, 29),
        S(37, -9), S(46, -6), S(73, 4), S(99, 26),
        S(46, -3), S(51, -3), S(67, 8), S(81, 15)
    },
    // BishopPST (half-board)
    {
        S(-7, -21), S(19, -13), S(-79, -14), S(-73, -15),
        S(19, -30), S(5, -59), S(8, -9), S(-40, -6),
        S(-11, -22), S(10, 5), S(-14, -5), S(11, 12),
        S(-20, -9), S(2, 10), S(31, 16), S(19, 9),
        S(-30, -16), S(14, 14), S(33, 20), S(42, 18),
        S(7, 4), S(15, 16), S(23, 6), S(63, 25),
        S(-19, -5), S(-22, 5), S(3, 20), S(33, 25),
        S(-24, -7), S(-13, 9), S(2, 11), S(23, 17)
    },
    // RookPST (half-board)
    {
        S(-81, -30), S(-45, -34), S(-38, -26), S(-1, -38),
        S(-73, -26), S(-20, -33), S(-26, -29), S(-12, -29),
        S(-49, -16), S(4, -29), S(-16, -17), S(14, -34),
        S(-32, 10), S(8, 2), S(16, 4), S(-2, -4),
        S(-48, 30), S(-4, 30), S(25, 33), S(30, 19),
        S(-12, 24), S(23, 25), S(34, 31), S(67, 36),
        S(-6, -8), S(0, -4), S(42, 5), S(68, 18),
        S(17, 23), S(27, 19), S(48, 18), S(44, 17)
    },
    // QueenPST (half-board)
    {
        S(-18, -89), S(13, -87), S(-2, -65), S(18, -55),
        S(5, -64), S(16, -67), S(29, -60), S(30, -41),
        S(3, -39), S(38, -24), S(21, -15), S(17, -11),
        S(53, -36), S(50, -12), S(38, 30), S(-4, 50),
        S(33, -31), S(18, 18), S(6, 50), S(-10, 84),
        S(13, 22), S(-11, 38), S(17, 49), S(19, 87),
        S(-47, -26), S(-91, 35), S(-23, 52), S(-17, 62),
        S(-111, 35), S(-71, 49), S(-37, 45), S(9, 39)
    },
    // KingPST (half-board)
    {
        S(54, -135), S(28, -88), S(-1, -72), S(-97, -55),
        S(57, -93), S(22, -63), S(-20, -44), S(-15, -40),
        S(-16, -57), S(-11, -40), S(-24, -31), S(-16, -24),
        S(-65, -18), S(-15, -12), S(-2, -1), S(-2, -11),
        S(-30, 19), S(-7, 32), S(-4, 31), S(-1, 38),
        S(-19, 37), S(5, 60), S(12, 61), S(14, 50),
        S(-1, 47), S(12, 57), S(20, 58), S(22, 47),
        S(6, 48), S(17, 54), S(35, 61), S(29, 55)
    },
    {
        {},
        {},
        {S(-100, -91), S(-60, -60), S(-33, -52), S(-22, -19), S(-10, 1), S(0, 18), S(12, 20), S(27, 20), S(27, 20)},
        {S(-37, -47), S(-18, -32), S(5, -21), S(16, 7), S(24, 25), S(31, 38), S(37, 39), S(37, 42), S(37, 42), S(37, 43), S(37, 43), S(37, 43), S(37, 43), S(37, 43)},
        {S(-27, -22), S(-15, -8), S(-6, -3), S(7, 5), S(8, 9), S(22, 21), S(22, 26), S(24, 36), S(40, 39), S(45, 43), S(50, 50), S(50, 53), S(50, 55), S(50, 55), S(50, 55)},
        {S(2, -62), S(2, -62), S(2, -62), S(2, -37), S(10, -33), S(10, -33), S(10, -9), S(10, 8), S(14, 22), S(24, 22), S(28, 24), S(28, 33), S(34, 35), S(34, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(51, 35), S(51, 35), S(51, 39), S(63, 64)},
        {},
    },
    {S(0, 0), S(-43, 4), S(-43, 26), S(-43, 26), S(0, 129), S(74, 343), S(227, 572), S(0, 0)},
    {S(0, 0), S(-8, 0), S(30, 6), S(30, 7), S(41, 39), S(41, 97), S(41, 219), S(0, 0)},
    S(69, 18), // RookOpenFileBonus
    S(37, 17), // RookSemiOpenFileBonus
    S(1, 0), // RookOnQueenFile
    S(47, 18), // KnightOutpostBonus
    S(52, 13), // BishopOutpostBonus
    S(-40, 0), // TrappedRookByKingPenalty
    S(14, 28), // RookBehindOurPasserBonus
    S(39, 75), // RookBehindTheirPasserBonus
    S(14, 17), // MinorBehindPawnBonus
    S(24, 0), // MinorOnKingRing
    S(1, 0), // RookOnKingRing
    S(-7, -3), // KingProtector
    S(19, 13), // BishopPair
    {
        {S(0, 0), S(79, 0), S(84, 0), S(55, 0), S(38, 0), S(65, 0), S(182, 0)},
        {S(-34, 0), S(119, 0), S(77, 0), S(38, 0), S(0, 0), S(30, 0), S(0, 0)},
        {S(-8, 0), S(101, 0), S(16, 0), S(25, 0), S(10, 0), S(19, 0), S(61, 0)},
        {S(0, 0), S(0, 0), S(4, 0), S(8, 0), S(53, 0), S(0, 0), S(53, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(71, 0), S(6, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(55, 0), S(19, 0), S(0, 0), S(0, 0), S(1, 0)},
        {S(0, 0), S(0, 0), S(90, 0), S(32, 0), S(15, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(5, 0), S(16, 0), S(23, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(126, 0), S(2, 0), S(3, 0), S(15, 0), S(0, 0)}, // BlockedStorm
    S(-23, -1), // UndefendedKingZoneSq
    S(11, 0), // KingMobilityFactor
    S(21, 36), // KingAttackByKnight
    S(5, 22), // KingAttackByBishop
    S(21, 36), // KingAttackByRook
    S(21, 36), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 0), S(28, 619), S(29, 0), S(29, 0), S(0, 0)}, // KingSafeCheck
    S(8, 39), // KingRingWeakWeight
    S(4, 160), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-11, -18), // DoubledPawnPenalty
    S(-17, 0), // BackwardPawnPenalty
    S(-25, -13), // WeakUnopposedPenalty
    S(-27, -50), // DoubledIsolatedPenalty
    {S(0, -24), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-33, -17), // PawnIslandPenalty
    {S(26, 0), S(13, 0)}, // CentralPawnBonus
    S(26, 19), // BishopLongDiagonalBonus
    S(-15, 0), // BishopXrayPawns
    S(0, 11), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 0), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(38, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(7, 4), // RestrictedPiece
    S(94, 15), // ThreatByPawnPush
    S(-11, -22), // WeakQueenDefender
    S(23, 11), // KnightOnQueen
    S(-85, -94), // PawnlessFlank
    S(0, 19), // QueenInfiltration
    S(0, -2), // KingPawnDistEg
    S(0, 37), // KBNKCornerEg
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
    S(0, 20), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
