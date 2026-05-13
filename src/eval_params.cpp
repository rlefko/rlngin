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
    S(272, 393), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 95), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(46, 3), // ThreatByKing
    S(38, 18), // Hanging
    S(50, 50), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 31), S(0, 40), S(0, 40), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 11), S(0, 29), S(0, 54), S(0, 70), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-14, -29), S(-14, -42), S(-14, -71), S(-156, -116), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 58), S(25, 107), S(99, 384), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(22, 28), S(22, 28), S(91, 28), S(101, 28), S(0, 0)},
    S(0, 42), // RookOn7thBonus
    S(-19, 0), // BadBishop
    S(-4, -8), // BishopPawns
    S(38, 0), // Tempo
    {S(0, 0), S(140, 220), S(629, 609), S(747, 671), S(1015, 1174), S(2031, 2174), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-24, -13), S(-7, -3), S(-16, -3), S(-8, -25), S(1, -20), S(-5, 0), S(-11, -1), S(-28, -21),
        S(-49, -24), S(-39, -16), S(-30, -20), S(-43, -40), S(-22, -32), S(-27, -15), S(-33, -18), S(-55, -24),
        S(-24, -7), S(-29, -8), S(-12, -25), S(7, -42), S(10, -39), S(-10, -21), S(-26, -11), S(-25, -13),
        S(-3, 20), S(-8, 13), S(5, -4), S(62, -10), S(71, -6), S(11, -2), S(-8, 12), S(-2, 18),
        S(8, 30), S(10, 27), S(27, 18), S(58, 15), S(59, 15), S(28, 18), S(10, 25), S(8, 32),
        S(7, 25), S(11, 26), S(27, 23), S(47, 22), S(47, 23), S(27, 23), S(11, 26), S(7, 26),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-102, -35), S(-110, -25), S(-83, -25), S(-56, -19),
        S(-82, -27), S(-61, -15), S(-58, -16), S(-42, -6),
        S(-68, -20), S(-31, -7), S(-10, -4), S(8, 15),
        S(-1, -3), S(10, 8), S(21, 22), S(17, 27),
        S(21, 4), S(29, 15), S(43, 28), S(49, 35),
        S(26, -8), S(42, 7), S(61, 25), S(72, 35),
        S(22, -16), S(32, -5), S(49, 7), S(66, 29),
        S(12, -25), S(27, -9), S(41, 5), S(53, 15)
    },
    // BishopPST (half-board)
    {
        S(-4, -18), S(11, -12), S(-71, -13), S(-61, -17),
        S(10, -18), S(-6, -34), S(2, -8), S(-40, -7),
        S(-11, -14), S(11, 2), S(-7, -3), S(9, 11),
        S(-1, -16), S(9, 3), S(27, 16), S(19, 13),
        S(-17, -17), S(14, 8), S(28, 17), S(35, 17),
        S(11, -3), S(15, 16), S(22, 12), S(38, 24),
        S(-11, -11), S(-15, 0), S(6, 19), S(18, 22),
        S(-17, -8), S(-11, 6), S(-2, 10), S(5, 13)
    },
    // RookPST (half-board)
    {
        S(-66, -27), S(-29, -31), S(-18, -29), S(7, -36),
        S(-57, -37), S(-21, -37), S(-14, -32), S(-4, -31),
        S(-35, -28), S(0, -20), S(-5, -18), S(7, -22),
        S(-25, -5), S(6, 6), S(4, 10), S(6, 2),
        S(-11, 18), S(16, 30), S(24, 29), S(22, 22),
        S(1, 25), S(22, 33), S(32, 36), S(44, 35),
        S(-7, 1), S(-3, 6), S(20, 13), S(33, 17),
        S(6, 17), S(13, 24), S(22, 28), S(23, 26)
    },
    // QueenPST (half-board)
    {
        S(-2, -61), S(11, -56), S(2, -51), S(10, -39),
        S(9, -51), S(10, -50), S(22, -35), S(22, -23),
        S(17, -30), S(25, -9), S(23, 11), S(15, 13),
        S(30, -6), S(35, 17), S(24, 44), S(14, 56),
        S(0, -14), S(10, 11), S(2, 37), S(-1, 59),
        S(2, -5), S(-8, 10), S(-9, 40), S(2, 63),
        S(-25, -12), S(-54, -2), S(-29, 26), S(-14, 46),
        S(-42, -16), S(-42, 2), S(-34, 20), S(-24, 31)
    },
    // KingPST (half-board)
    {
        S(76, -125), S(55, -94), S(19, -75), S(-66, -63),
        S(77, -86), S(51, -64), S(3, -54), S(-8, -47),
        S(12, -58), S(10, -41), S(-7, -31), S(-14, -26),
        S(-14, -20), S(-6, -3), S(-8, 3), S(-16, 4),
        S(-13, 19), S(-8, 31), S(-9, 35), S(-13, 32),
        S(-9, 42), S(-4, 55), S(-5, 58), S(-9, 54),
        S(-10, 46), S(-6, 56), S(-6, 59), S(-9, 61),
        S(-11, 48), S(-8, 54), S(-7, 59), S(-9, 61)
    },
    {
        {},
        {},
        {S(-62, -60), S(-42, -40), S(-22, -23), S(-8, -4), S(7, 10), S(19, 20), S(30, 20), S(37, 20), S(37, 20)},
        {S(-40, -35), S(-22, -23), S(-4, -5), S(14, 8), S(27, 25), S(29, 36), S(32, 40), S(32, 43), S(33, 43), S(36, 43), S(38, 43), S(47, 43), S(51, 43), S(51, 43)},
        {S(-29, -13), S(-14, -7), S(-5, 2), S(6, 13), S(6, 19), S(15, 27), S(20, 32), S(26, 38), S(32, 45), S(40, 50), S(44, 55), S(50, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-25, -54), S(-18, -42), S(-16, -32), S(-16, -21), S(-9, -9), S(-1, 3), S(5, 7), S(12, 13), S(18, 19), S(23, 23), S(26, 25), S(29, 30), S(30, 35), S(34, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(47, 35), S(59, 47)},
        {},
    },
    {S(0, 0), S(-39, 17), S(-39, 17), S(-39, 17), S(0, 111), S(108, 303), S(303, 537), S(0, 0)},
    {S(0, 0), S(-8, -1), S(24, 3), S(32, 3), S(59, 25), S(68, 76), S(90, 166), S(0, 0)},
    S(67, 17), // RookOpenFileBonus
    S(29, 17), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(39, 25), // KnightOutpostBonus
    S(51, 23), // BishopOutpostBonus
    S(-37, 0), // TrappedRookByKingPenalty
    S(23, 25), // RookBehindOurPasserBonus
    S(3, 78), // RookBehindTheirPasserBonus
    S(20, 10), // MinorBehindPawnBonus
    S(27, 0), // MinorOnKingRing
    S(1, 0), // RookOnKingRing
    S(-8, -2), // KingProtector
    S(19, 16), // BishopPair
    {
        {S(0, 0), S(78, 0), S(81, 0), S(54, 0), S(38, 0), S(40, 0), S(133, 0)},
        {S(-37, 0), S(136, 0), S(81, 0), S(43, 0), S(0, 0), S(45, 0), S(0, 0)},
        {S(-8, 0), S(108, 0), S(34, 0), S(33, 0), S(10, 0), S(30, 0), S(76, 0)},
        {S(0, 0), S(50, 0), S(16, 0), S(10, 0), S(42, 0), S(11, 0), S(72, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(61, 0), S(4, 0), S(0, 0), S(1, 0)},
        {S(0, 0), S(0, 0), S(0, 0), S(1, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(73, 0), S(37, 0), S(7, 0), S(3, 0), S(4, 0)},
        {S(0, 0), S(0, 0), S(39, 0), S(34, 0), S(11, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(115, 0), S(0, 0), S(11, 0), S(3, 0), S(0, 0)}, // BlockedStorm
    S(-25, -2), // UndefendedKingZoneSq
    S(13, 4), // KingMobilityFactor
    S(21, 11), // KingAttackByKnight
    S(1, 32), // KingAttackByBishop
    S(21, 11), // KingAttackByRook
    S(21, 11), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(29, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(3, 81), // KingRingWeakWeight
    S(2, 37), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-19, -31), // DoubledPawnPenalty
    S(-16, -3), // BackwardPawnPenalty
    S(-25, -13), // WeakUnopposedPenalty
    S(-28, -46), // DoubledIsolatedPenalty
    {S(-1, -17), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-28, -21), // PawnIslandPenalty
    {S(21, 0), S(1, 0)}, // CentralPawnBonus
    S(25, 12), // BishopLongDiagonalBonus
    S(-17, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 10), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(60, 60), // SliderOnQueenBishop
    S(60, 48), // SliderOnQueenRook
    S(8, 0), // RestrictedPiece
    S(80, 34), // ThreatByPawnPush
    S(-13, -6), // WeakQueenDefender
    S(35, 0), // KnightOnQueen
    S(-165, -89), // PawnlessFlank
    S(0, 65), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 0), // KBNKPushClose
    S(0, 6), // KQKRPushToEdge
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
