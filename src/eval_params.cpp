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
    S(241, 400), // ThreatByPawn (eg clamped to SPSA 0..400 bound)
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(67, 0), // ThreatByKing
    S(28, 26), // Hanging
    S(50, 50), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 28), S(0, 39), S(0, 39), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 30), S(0, 59), S(0, 76), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-13, -31), S(-14, -47), S(-14, -77), S(-143, -123), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 27), S(0, 62), S(14, 118), S(131, 382), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(22, 28), S(22, 28), S(75, 28), S(75, 28), S(0, 0)},
    S(0, 41), // RookOn7thBonus
    S(-14, 0), // BadBishop
    S(-3, -9), // BishopPawns
    S(43, 0), // Tempo
    {S(0, 0), S(160, 237), S(546, 686), S(634, 758), S(858, 1308), S(1803, 2388), S(0, 0)}, // PieceScore (Pawn.mg anchored at 160; lower bound enforced by the tuner)
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-29, -15), S(-15, -2), S(-18, -2), S(-20, -1), S(-3, 11), S(-8, 2), S(-17, 0), S(-34, -22),
        S(-55, -27), S(-49, -19), S(-35, -23), S(-52, -41), S(-29, -32), S(-33, -17), S(-41, -21), S(-61, -27),
        S(-27, -10), S(-29, -11), S(-12, -27), S(4, -45), S(8, -42), S(-10, -23), S(-26, -14), S(-29, -15),
        S(-1, 19), S(-6, 12), S(10, -8), S(58, -7), S(78, -7), S(16, -9), S(-5, 12), S(0, 18),
        S(12, 29), S(15, 26), S(34, 17), S(64, 15), S(65, 16), S(35, 17), S(15, 24), S(12, 31),
        S(14, 24), S(19, 25), S(37, 23), S(55, 23), S(55, 24), S(37, 23), S(19, 25), S(14, 25),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-102, -37), S(-106, -31), S(-87, -25), S(-64, -16),
        S(-86, -28), S(-66, -16), S(-64, -18), S(-49, -7),
        S(-72, -22), S(-34, -11), S(-9, -11), S(4, 14),
        S(-2, -4), S(11, 5), S(22, 20), S(11, 28),
        S(22, 5), S(30, 14), S(43, 28), S(53, 33),
        S(27, -5), S(43, 7), S(64, 24), S(75, 34),
        S(25, -13), S(36, -2), S(55, 8), S(72, 29),
        S(16, -21), S(33, -6), S(49, 7), S(61, 17)
    },
    // BishopPST (half-board)
    {
        S(-8, -19), S(7, -13), S(-76, -11), S(-66, -16),
        S(6, -18), S(-10, -39), S(-2, -9), S(-45, -7),
        S(-13, -18), S(9, 1), S(-7, -7), S(12, 5),
        S(0, -16), S(8, 2), S(31, 14), S(20, 11),
        S(-18, -16), S(4, 12), S(27, 17), S(34, 19),
        S(11, -2), S(14, 17), S(23, 13), S(37, 24),
        S(-8, -10), S(-12, 1), S(9, 18), S(21, 22),
        S(-11, -5), S(-6, 8), S(4, 11), S(9, 14)
    },
    // RookPST (half-board)
    {
        S(-72, -31), S(-37, -33), S(-29, -29), S(-2, -36),
        S(-66, -37), S(-29, -37), S(-23, -30), S(-11, -30),
        S(-42, -28), S(-7, -19), S(-10, -17), S(0, -20),
        S(-31, -4), S(2, 6), S(2, 10), S(2, 3),
        S(-13, 19), S(16, 31), S(26, 30), S(24, 23),
        S(3, 25), S(25, 32), S(39, 35), S(49, 34),
        S(-3, 0), S(2, 4), S(28, 11), S(45, 16),
        S(15, 15), S(24, 22), S(39, 26), S(43, 24)
    },
    // QueenPST (half-board)
    {
        S(4, -78), S(17, -74), S(7, -72), S(24, -74),
        S(17, -68), S(19, -68), S(33, -58), S(30, -43),
        S(26, -44), S(35, -23), S(31, -2), S(20, 5),
        S(42, -17), S(44, 10), S(31, 39), S(-12, 71),
        S(11, -18), S(16, 13), S(7, 39), S(0, 65),
        S(5, 0), S(-14, 23), S(-10, 49), S(1, 73),
        S(-38, 7), S(-100, 27), S(-43, 44), S(-24, 60),
        S(-53, 7), S(-61, 26), S(-47, 40), S(-33, 49)
    },
    // KingPST (half-board)
    {
        S(49, -125), S(30, -96), S(-6, -77), S(-90, -66),
        S(54, -85), S(24, -63), S(-23, -54), S(-39, -44),
        S(-4, -57), S(-7, -40), S(-20, -31), S(-25, -29),
        S(-17, -19), S(-9, -2), S(-6, 2), S(-3, -10),
        S(-6, 21), S(1, 32), S(4, 35), S(4, 30),
        S(3, 45), S(10, 57), S(11, 59), S(9, 54),
        S(5, 49), S(9, 59), S(10, 61), S(9, 61),
        S(4, 51), S(7, 57), S(9, 61), S(9, 62)
    },
    {
        {},
        {},
        {S(-56, -68), S(-43, -48), S(-34, -28), S(-16, -9), S(2, 4), S(7, 20), S(17, 20), S(29, 20), S(29, 20)},
        {S(-39, -41), S(-22, -28), S(-4, -10), S(14, 5), S(27, 22), S(27, 36), S(28, 42), S(32, 43), S(32, 43), S(34, 43), S(37, 43), S(38, 43), S(38, 43), S(43, 43)},
        {S(-18, -24), S(-15, -9), S(-2, -4), S(6, 11), S(6, 14), S(15, 24), S(20, 30), S(26, 36), S(32, 43), S(38, 49), S(45, 52), S(50, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-28, -73), S(-16, -62), S(-16, -50), S(-10, -38), S(2, -26), S(2, -14), S(2, -2), S(2, 10), S(5, 22), S(14, 22), S(16, 29), S(16, 35), S(26, 35), S(30, 35), S(34, 35), S(34, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(47, 35), S(59, 47)},
        {},
    },
    {S(0, 0), S(-39, 25), S(-39, 25), S(-39, 25), S(0, 131), S(81, 344), S(256, 592), S(0, 0)},
    {S(0, 0), S(-6, -2), S(30, 3), S(33, 4), S(45, 34), S(51, 90), S(113, 183), S(0, 0)},
    S(61, 18), // RookOpenFileBonus
    S(29, 17), // RookSemiOpenFileBonus
    S(7, 0), // RookOnQueenFile
    S(29, 31), // KnightOutpostBonus
    S(47, 25), // BishopOutpostBonus
    S(-36, 0), // TrappedRookByKingPenalty
    S(31, 18), // RookBehindOurPasserBonus
    S(16, 84), // RookBehindTheirPasserBonus
    S(22, 9), // MinorBehindPawnBonus
    S(24, 3), // MinorOnKingRing
    S(1, 0), // RookOnKingRing
    S(-6, -4), // KingProtector
    S(17, 20), // BishopPair
    {
        {S(0, 0), S(76, 0), S(76, 0), S(50, 0), S(40, 0), S(46, 0), S(112, 0)},
        {S(-35, 0), S(123, 0), S(71, 0), S(35, 0), S(0, 0), S(34, 0), S(0, 0)},
        {S(0, 0), S(111, 0), S(37, 0), S(42, 0), S(25, 0), S(47, 0), S(86, 0)},
        {S(0, 0), S(0, 0), S(6, 0), S(11, 0), S(49, 0), S(23, 0), S(57, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(56, 0), S(6, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(3, 0), S(1, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(56, 0), S(34, 0), S(0, 0), S(0, 0), S(1, 0)},
        {S(0, 0), S(0, 0), S(49, 0), S(26, 0), S(2, 0), S(0, 0), S(31, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(106, 0), S(3, 0), S(12, 0), S(12, 0), S(7, 0)}, // BlockedStorm
    S(-25, -1), // UndefendedKingZoneSq
    S(15, 0), // KingMobilityFactor
    S(21, 24), // KingAttackByKnight
    S(1, 9), // KingAttackByBishop
    S(21, 31), // KingAttackByRook
    S(21, 31), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(29, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(2, 74), // KingRingWeakWeight
    S(0, 98), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-36, -23), // DoubledPawnPenalty
    S(-14, -2), // BackwardPawnPenalty
    S(-21, -14), // WeakUnopposedPenalty
    S(-16, -54), // DoubledIsolatedPenalty
    {S(0, -22), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-31, -21), // PawnIslandPenalty
    {S(19, 0), S(0, 0)}, // CentralPawnBonus
    S(23, 12), // BishopLongDiagonalBonus
    S(-13, 0), // BishopXrayPawns
    S(0, 18), // InitiativePasser
    S(0, 7), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 47), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(28, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(7, 1), // RestrictedPiece
    S(91, 16), // ThreatByPawnPush
    S(-18, -2), // WeakQueenDefender
    S(28, 2), // KnightOnQueen
    S(-120, -101), // PawnlessFlank
    S(0, 55), // QueenInfiltration
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
    S(0, 20), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
