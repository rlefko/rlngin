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
    S(290, 24), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(109, 0), S(129, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(170, 0), S(0, 0)},
    S(179, 50), // ThreatByKing
    S(51, 0), // Hanging
    S(0, 0), // WeakQueen
    S(26, 31), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 28), S(0, 44), S(0, 45), S(0, 48), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 22), S(0, 33), S(0, 63), S(0, 74), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-28, -19), S(-28, -64), S(-28, -78), S(-273, -78), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(73, 38), S(73, 75), S(162, 126), S(162, 260), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-80, -1), S(49, 16), S(61, 16), S(61, 16), S(0, 0)},
    S(0, 28), // RookOn7thBonus
    S(-36, 0), // BadBishop
    S(-5, -8), // BishopPawns
    S(168, 0), // Tempo
    {S(0, 0), S(254, 258), S(803, 584), S(882, 546), S(1297, 789), S(2552, 1632), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-49, -53), S(-41, -46), S(-61, -57), S(-68, -61), S(-64, -55), S(-55, -45), S(-39, -48), S(-53, -64),
        S(-59, -51), S(-66, -56), S(-53, -64), S(-50, -69), S(-43, -58), S(-55, -54), S(-65, -64), S(-61, -59),
        S(-34, -29), S(-37, -36), S(-25, -63), S(-2, -66), S(1, -65), S(-24, -60), S(-37, -36), S(-39, -39),
        S(15, 36), S(6, 21), S(26, -3), S(67, -17), S(68, -18), S(26, -4), S(-1, 19), S(10, 28),
        S(26, 81), S(18, 85), S(55, 53), S(103, 41), S(102, 41), S(54, 50), S(17, 81), S(23, 78),
        S(45, 112), S(-12, 117), S(59, 82), S(144, 66), S(133, 65), S(58, 81), S(-24, 115), S(35, 109),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-99, -58), S(-114, -47), S(-65, -39), S(-51, 1),
        S(-72, -49), S(-60, -39), S(-41, -24), S(-24, -5),
        S(-34, -30), S(8, -13), S(36, -7), S(44, 28),
        S(-2, -3), S(35, 9), S(57, 29), S(70, 41),
        S(36, 10), S(74, 30), S(73, 37), S(77, 50),
        S(4, -5), S(32, 11), S(52, 32), S(58, 49),
        S(-39, -13), S(-5, -5), S(26, 14), S(34, 32),
        S(-114, -26), S(-38, -10), S(1, 9), S(13, 16)
    },
    // BishopPST (half-board)
    {
        S(-7, -55), S(-21, -50), S(-83, -33), S(-58, -13),
        S(10, -55), S(13, -46), S(7, -22), S(-30, -3),
        S(14, -44), S(54, 1), S(6, -16), S(5, 20),
        S(22, -27), S(36, 0), S(36, 21), S(8, 9),
        S(7, -10), S(40, 38), S(34, 31), S(20, 15),
        S(17, 10), S(19, 29), S(10, 31), S(1, 21),
        S(-7, 11), S(-13, 17), S(-17, 27), S(-21, 20),
        S(-22, 22), S(-28, 26), S(-46, 26), S(-36, 29)
    },
    // RookPST (half-board)
    {
        S(-62, 6), S(-39, 20), S(2, 32), S(7, 18),
        S(-93, -71), S(-64, -49), S(-35, -16), S(-40, -16),
        S(-70, -60), S(-55, -41), S(-54, -40), S(-55, -30),
        S(-31, -16), S(-21, -14), S(-12, 2), S(-4, -9),
        S(13, 8), S(20, 16), S(34, 11), S(48, -2),
        S(46, 12), S(33, 10), S(50, 20), S(63, 20),
        S(56, 19), S(27, 7), S(32, 9), S(49, 39),
        S(54, 43), S(37, 45), S(30, 33), S(30, 22)
    },
    // QueenPST (half-board)
    {
        S(-22, -87), S(-5, -75), S(15, -67), S(74, -62),
        S(-23, -75), S(7, -57), S(46, -44), S(65, -42),
        S(-6, -46), S(39, -7), S(49, 16), S(11, 7),
        S(18, -4), S(48, 33), S(26, 45), S(-7, 55),
        S(8, 28), S(5, 48), S(6, 86), S(-27, 90),
        S(-15, 30), S(-46, 24), S(-50, 60), S(-50, 98),
        S(-26, 10), S(-52, 11), S(-33, 20), S(-44, 58),
        S(-49, -58), S(-24, -43), S(60, -37), S(-20, -24)
    },
    // KingPST (half-board)
    {
        S(139, -142), S(147, -82), S(-31, -37), S(-191, -70),
        S(182, -78), S(109, -44), S(-6, -8), S(-69, 0),
        S(82, -50), S(64, -19), S(4, 7), S(-37, 17),
        S(35, -24), S(21, 10), S(-24, 39), S(-69, 41),
        S(20, 5), S(-2, 37), S(-57, 63), S(-151, 79),
        S(33, 2), S(7, 29), S(-61, 63), S(-150, 88),
        S(59, -24), S(59, 8), S(13, 40), S(-91, 66),
        S(-151, -68), S(145, -7), S(145, 32), S(-144, 55)
    },
    {
        {},
        {},
        {S(-126, -101), S(-94, -61), S(-56, -21), S(-20, -7), S(3, 5), S(26, 36), S(64, 43), S(65, 43), S(65, 44)},
        {S(-21, 32), S(6, 63), S(41, 96), S(76, 127), S(111, 127), S(125, 146), S(128, 163), S(128, 163), S(128, 163), S(128, 163), S(128, 163), S(128, 163), S(128, 163), S(128, 167)},
        {S(-58, 227), S(-30, 253), S(-4, 282), S(0, 310), S(0, 310), S(2, 323), S(2, 323), S(23, 333), S(31, 335), S(54, 345), S(54, 356), S(54, 357), S(54, 365), S(54, 365), S(66, 365)},
        {S(-6, 291), S(18, 300), S(41, 317), S(58, 333), S(82, 333), S(107, 349), S(132, 355), S(152, 355), S(167, 356), S(167, 381), S(167, 391), S(167, 391), S(167, 391), S(168, 401), S(168, 401), S(168, 401), S(168, 401), S(168, 401), S(168, 401), S(174, 401), S(180, 401), S(187, 401), S(204, 401), S(204, 401), S(212, 401), S(212, 401), S(228, 403), S(228, 403)},
        {},
    },
    {S(0, 0), S(-53, 45), S(-53, 45), S(-53, 78), S(38, 104), S(140, 302), S(568, 415), S(0, 0)},
    {S(0, 0), S(-3, -8), S(51, 9), S(51, 9), S(75, 9), S(106, 71), S(318, 85), S(0, 0)},
    S(131, 27), // RookOpenFileBonus
    S(34, 13), // RookSemiOpenFileBonus
    S(1, 0), // RookOnQueenFile
    S(106, 34), // KnightOutpostBonus
    S(121, 20), // BishopOutpostBonus
    S(-149, 0), // TrappedRookByKingPenalty
    S(56, 34), // RookBehindOurPasserBonus
    S(-144, 127), // RookBehindTheirPasserBonus
    S(25, 11), // MinorBehindPawnBonus
    S(46, 5), // MinorOnKingRing
    S(33, 0), // RookOnKingRing
    S(-24, -2), // KingProtector
    S(1, 33), // BishopPair
    {
        {S(0, 0), S(163, 0), S(168, 0), S(77, 0), S(94, 0), S(14, 0), S(0, 0)},
        {S(-79, 0), S(228, 0), S(124, 0), S(4, 0), S(0, 0), S(66, 0), S(267, 0)},
        {S(-4, 0), S(222, 0), S(37, 0), S(27, 0), S(2, 0), S(151, 0), S(112, 0)},
        {S(0, 0), S(60, 0), S(67, 0), S(69, 0), S(63, 0), S(104, 0), S(131, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(40, 0), S(97, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(83, 0), S(236, 0), S(107, 0), S(2, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(24, 0), S(250, 0), S(29, 0), S(0, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(220, 0), S(19, 0), S(0, 0), S(34, 0), S(0, 0)}, // BlockedStorm
    S(-16, -2), // UndefendedKingZoneSq
    S(10, 50), // KingMobilityFactor
    S(36, 32), // KingAttackByKnight
    S(19, 35), // KingAttackByBishop
    S(38, 33), // KingAttackByRook
    S(44, 33), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(15, 1), S(27, 306), S(16, 1), S(18, 1), S(0, 0)}, // KingSafeCheck
    S(24, 15), // KingRingWeakWeight
    S(20, 0), // KingNoQueenDiscount
    S(0, -1), // IsolatedPawnPenalty
    S(0, -23), // DoubledPawnPenalty
    S(-37, -4), // BackwardPawnPenalty
    S(-41, -14), // WeakUnopposedPenalty
    S(-16, -45), // DoubledIsolatedPenalty
    {S(-73, -51), S(0, -70)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(0, -15), // PawnIslandPenalty
    {S(28, 0), S(0, 0)}, // CentralPawnBonus
    S(68, 20), // BishopLongDiagonalBonus
    S(-28, -9), // BishopXrayPawns
    S(0, 82), // InitiativePasser
    S(0, 33), // InitiativePawnCount
    S(0, 0), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 122), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(37, 0), // SliderOnQueenBishop
    S(38, 0), // SliderOnQueenRook
    S(4, 1), // RestrictedPiece
    S(47, 0), // ThreatByPawnPush
    S(-6, -1), // WeakQueenDefender
    S(139, 0), // KnightOnQueen
    S(-348, -156), // PawnlessFlank
    S(0, 0), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 10), // KBNKCornerEg
    S(0, 447), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
