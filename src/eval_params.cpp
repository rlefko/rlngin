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
    S(268, 355), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 88), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(45, 4), // ThreatByKing
    S(39, 19), // Hanging
    S(50, 50), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 32), S(0, 39), S(0, 39), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 11), S(0, 30), S(0, 55), S(0, 72), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-14, -27), S(-14, -43), S(-14, -74), S(-161, -115), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 57), S(23, 108), S(107, 375), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(22, 28), S(22, 28), S(84, 28), S(96, 28), S(0, 0)},
    S(0, 41), // RookOn7thBonus
    S(-19, 0), // BadBishop
    S(-4, -8), // BishopPawns
    S(42, 0), // Tempo
    {S(0, 0), S(138, 222), S(608, 625), S(725, 688), S(975, 1207), S(1992, 2219), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-24, -14), S(-8, -2), S(-18, -2), S(-8, -25), S(1, -21), S(-6, 1), S(-12, 0), S(-29, -22),
        S(-51, -25), S(-43, -17), S(-33, -21), S(-46, -42), S(-24, -33), S(-30, -15), S(-37, -19), S(-57, -25),
        S(-24, -8), S(-30, -8), S(-12, -26), S(6, -44), S(9, -41), S(-10, -22), S(-27, -11), S(-26, -13),
        S(-2, 20), S(-7, 13), S(6, -4), S(65, -11), S(74, -7), S(12, -2), S(-7, 13), S(-1, 19),
        S(11, 29), S(11, 28), S(28, 17), S(60, 15), S(61, 16), S(29, 18), S(11, 26), S(11, 31),
        S(10, 23), S(15, 24), S(30, 23), S(49, 23), S(49, 24), S(30, 23), S(15, 24), S(10, 24),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-106, -33), S(-115, -23), S(-87, -23), S(-61, -17),
        S(-86, -27), S(-65, -13), S(-62, -15), S(-47, -6),
        S(-72, -20), S(-34, -8), S(-13, -4), S(5, 15),
        S(-3, -2), S(10, 8), S(21, 23), S(14, 29),
        S(22, 4), S(29, 15), S(43, 29), S(49, 36),
        S(27, -8), S(44, 6), S(62, 25), S(74, 35),
        S(24, -17), S(35, -5), S(52, 7), S(68, 29),
        S(14, -26), S(30, -9), S(44, 5), S(55, 14)
    },
    // BishopPST (half-board)
    {
        S(-7, -18), S(9, -12), S(-75, -12), S(-65, -16),
        S(9, -17), S(-8, -36), S(0, -8), S(-44, -6),
        S(-13, -14), S(9, 3), S(-9, -4), S(6, 12),
        S(-2, -15), S(8, 4), S(26, 19), S(18, 14),
        S(-19, -16), S(14, 9), S(28, 19), S(35, 19),
        S(12, -4), S(17, 16), S(24, 12), S(37, 25),
        S(-9, -12), S(-13, -1), S(7, 19), S(19, 24),
        S(-14, -8), S(-9, 6), S(0, 11), S(6, 15)
    },
    // RookPST (half-board)
    {
        S(-70, -29), S(-32, -32), S(-23, -29), S(4, -37),
        S(-61, -38), S(-24, -38), S(-17, -32), S(-7, -31),
        S(-39, -29), S(-2, -19), S(-8, -17), S(5, -21),
        S(-28, -4), S(6, 7), S(5, 10), S(4, 3),
        S(-12, 19), S(17, 30), S(25, 29), S(24, 22),
        S(3, 24), S(23, 32), S(35, 35), S(49, 33),
        S(-4, 0), S(-1, 4), S(24, 12), S(40, 15),
        S(10, 16), S(19, 23), S(30, 27), S(33, 25)
    },
    // QueenPST (half-board)
    {
        S(-3, -65), S(11, -61), S(0, -54), S(9, -45),
        S(10, -55), S(10, -54), S(22, -39), S(22, -27),
        S(19, -31), S(25, -10), S(22, 12), S(14, 13),
        S(33, -7), S(37, 17), S(26, 45), S(16, 57),
        S(4, -16), S(12, 13), S(4, 37), S(1, 61),
        S(4, -5), S(-8, 12), S(-7, 41), S(3, 66),
        S(-26, -9), S(-57, 0), S(-33, 29), S(-17, 50),
        S(-44, -11), S(-43, 5), S(-36, 22), S(-28, 34)
    },
    // KingPST (half-board)
    {
        S(73, -127), S(51, -97), S(13, -76), S(-72, -65),
        S(74, -87), S(45, -64), S(-4, -55), S(-12, -49),
        S(8, -58), S(4, -41), S(-11, -31), S(-16, -27),
        S(-17, -20), S(-8, -3), S(-9, 2), S(-16, 3),
        S(-13, 19), S(-7, 31), S(-6, 34), S(-9, 31),
        S(-7, 43), S(-1, 56), S(0, 57), S(-3, 53),
        S(-8, 47), S(-3, 57), S(-3, 59), S(-3, 60),
        S(-8, 49), S(-4, 55), S(-3, 59), S(-3, 61)
    },
    {
        {},
        {},
        {S(-62, -64), S(-42, -44), S(-22, -24), S(-8, -5), S(6, 10), S(18, 20), S(29, 20), S(36, 20), S(36, 20)},
        {S(-40, -36), S(-22, -24), S(-4, -6), S(13, 8), S(26, 25), S(28, 36), S(31, 40), S(31, 43), S(32, 43), S(35, 43), S(38, 43), S(45, 43), S(45, 43), S(45, 43)},
        {S(-30, -10), S(-15, -5), S(-5, 1), S(6, 13), S(6, 18), S(15, 27), S(19, 32), S(26, 38), S(32, 44), S(39, 50), S(43, 55), S(50, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-25, -57), S(-15, -45), S(-15, -33), S(-15, -21), S(-8, -9), S(0, -4), S(6, 3), S(12, 13), S(18, 19), S(24, 21), S(26, 24), S(28, 30), S(30, 34), S(33, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(47, 35), S(59, 47)},
        {},
    },
    {S(0, 0), S(-39, 17), S(-39, 17), S(-39, 17), S(0, 112), S(100, 312), S(293, 551), S(0, 0)},
    {S(0, 0), S(-7, -1), S(28, 3), S(34, 3), S(57, 27), S(61, 81), S(110, 168), S(0, 0)},
    S(66, 16), // RookOpenFileBonus
    S(29, 16), // RookSemiOpenFileBonus
    S(7, 0), // RookOnQueenFile
    S(36, 26), // KnightOutpostBonus
    S(49, 24), // BishopOutpostBonus
    S(-37, 0), // TrappedRookByKingPenalty
    S(27, 22), // RookBehindOurPasserBonus
    S(3, 80), // RookBehindTheirPasserBonus
    S(19, 11), // MinorBehindPawnBonus
    S(27, 1), // MinorOnKingRing
    S(2, 0), // RookOnKingRing
    S(-8, -2), // KingProtector
    S(17, 18), // BishopPair
    {
        {S(0, 0), S(78, 0), S(79, 0), S(53, 0), S(39, 0), S(41, 0), S(127, 0)},
        {S(-36, 0), S(133, 0), S(81, 0), S(42, 0), S(0, 0), S(41, 0), S(0, 0)},
        {S(-6, 0), S(109, 0), S(34, 0), S(36, 0), S(14, 0), S(36, 0), S(85, 0)},
        {S(0, 0), S(46, 0), S(13, 0), S(10, 0), S(43, 0), S(16, 0), S(76, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(59, 0), S(4, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(66, 0), S(33, 0), S(5, 0), S(1, 0), S(3, 0)},
        {S(0, 0), S(0, 0), S(44, 0), S(32, 0), S(9, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(113, 0), S(0, 0), S(11, 0), S(4, 0), S(0, 0)}, // BlockedStorm
    S(-25, -2), // UndefendedKingZoneSq
    S(13, 3), // KingMobilityFactor
    S(21, 11), // KingAttackByKnight
    S(0, 29), // KingAttackByBishop
    S(21, 11), // KingAttackByRook
    S(21, 11), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(29, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(2, 79), // KingRingWeakWeight
    S(1, 63), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-19, -31), // DoubledPawnPenalty
    S(-14, -3), // BackwardPawnPenalty
    S(-24, -13), // WeakUnopposedPenalty
    S(-26, -47), // DoubledIsolatedPenalty
    {S(0, -20), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-30, -19), // PawnIslandPenalty
    {S(21, 0), S(1, 0)}, // CentralPawnBonus
    S(25, 13), // BishopLongDiagonalBonus
    S(-16, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 10), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(24, 60), // SliderOnQueenBishop
    S(60, 59), // SliderOnQueenRook
    S(8, 1), // RestrictedPiece
    S(65, 32), // ThreatByPawnPush
    S(-13, -5), // WeakQueenDefender
    S(35, 0), // KnightOnQueen
    S(-153, -92), // PawnlessFlank
    S(0, 63), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 0), // KBNKPushClose
    S(0, 4), // KQKRPushToEdge
    S(0, 0), // KQKRPushClose
    S(0, 1), // KPsKFortressScale
    S(0, 0), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
    S(0, 21), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
