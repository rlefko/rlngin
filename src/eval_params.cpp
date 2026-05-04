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
    S(245, 23), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(186, 10), S(222, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(274, 14), S(0, 0)},
    S(110, 0), // ThreatByKing
    S(112, 25), // Hanging
    S(22, 0), // WeakQueen
    S(19, 20), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 35), S(0, 35), S(0, 35), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 17), S(0, 35), S(0, 65), S(0, 77), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-36, -18), S(-36, -38), S(-54, -61), S(-358, -105), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(32, 9), S(33, 45), S(131, 95), S(131, 324), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-43, 19), S(48, 28), S(51, 28), S(51, 28), S(0, 0)},
    S(0, 27), // RookOn7thBonus
    S(-2, 0), // BadBishop
    S(-1, -6), // BishopPawns
    S(48, 0), // Tempo
    {S(0, 0), S(170, 155), S(895, 491), S(925, 418), S(1365, 651), S(2452, 1517), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-26, -9), S(-13, -12), S(-19, -16), S(4, -4), S(10, -1), S(-11, -10), S(-10, -15), S(-32, -26),
        S(-59, -13), S(-52, -24), S(-25, -27), S(-13, -28), S(8, -18), S(-28, -22), S(-42, -30), S(-54, -25),
        S(-43, -3), S(-38, -15), S(-15, -34), S(15, -36), S(14, -35), S(-9, -30), S(-34, -15), S(-41, -15),
        S(7, 29), S(-1, 9), S(10, -13), S(40, -26), S(48, -23), S(16, -16), S(-9, 9), S(1, 22),
        S(3, 56), S(10, 53), S(46, 6), S(75, 7), S(76, 7), S(42, 9), S(6, 52), S(-13, 53),
        S(10, 40), S(-7, 38), S(30, 25), S(60, 19), S(61, 18), S(30, 24), S(-5, 37), S(3, 41),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-118, -20), S(-134, -8), S(-78, -13), S(-77, 2),
        S(-89, -6), S(-68, -3), S(-38, -6), S(-30, -2),
        S(-53, 0), S(2, 0), S(11, -10), S(15, 18),
        S(-11, 14), S(45, 16), S(55, 22), S(49, 27),
        S(45, 11), S(70, 16), S(92, 23), S(74, 28),
        S(1, -6), S(37, 7), S(66, 16), S(86, 27),
        S(-32, -40), S(4, -12), S(43, 1), S(51, 16),
        S(-71, -103), S(-14, -37), S(19, -5), S(33, 5)
    },
    // BishopPST (half-board)
    {
        S(1, -25), S(4, -21), S(-87, 1), S(-44, 1),
        S(9, -21), S(22, -22), S(14, -7), S(-25, 4),
        S(1, -9), S(43, 8), S(13, -6), S(-13, 18),
        S(10, -7), S(37, 2), S(39, 13), S(23, 2),
        S(-15, -1), S(43, 19), S(48, 13), S(34, 5),
        S(23, 0), S(17, 16), S(36, 10), S(28, 11),
        S(-23, -18), S(-51, -5), S(-14, 9), S(-9, 0),
        S(-39, 3), S(-45, 10), S(-34, 12), S(-27, 9)
    },
    // RookPST (half-board)
    {
        S(-45, -17), S(-27, -9), S(-5, -6), S(6, -11),
        S(-64, -27), S(-37, -25), S(-23, -12), S(-29, -14),
        S(-62, -19), S(-34, -13), S(-42, -11), S(-43, -13),
        S(-42, 1), S(-19, 6), S(-29, 10), S(-9, 1),
        S(-11, 10), S(12, 22), S(19, 15), S(24, 3),
        S(19, 12), S(19, 19), S(41, 14), S(53, 11),
        S(25, 1), S(22, 3), S(56, -1), S(56, 6),
        S(28, 9), S(33, 12), S(50, 10), S(58, 7)
    },
    // QueenPST (half-board)
    {
        S(5, -74), S(17, -61), S(21, -58), S(50, -48),
        S(24, -56), S(40, -51), S(57, -45), S(70, -35),
        S(25, -45), S(55, -26), S(58, -3), S(20, 3),
        S(51, -25), S(61, 8), S(28, 27), S(18, 35),
        S(37, -21), S(20, 13), S(22, 49), S(3, 52),
        S(15, -9), S(-24, 24), S(-28, 58), S(-9, 82),
        S(-79, 4), S(-128, 30), S(-74, 51), S(-51, 65),
        S(-98, -8), S(-88, 20), S(-57, 34), S(-49, 40)
    },
    // KingPST (half-board)
    {
        S(142, -93), S(149, -68), S(23, -42), S(-115, -49),
        S(169, -55), S(120, -36), S(7, -15), S(-64, -6),
        S(45, -38), S(43, -18), S(-9, -2), S(-46, 3),
        S(7, -14), S(4, 7), S(-21, 23), S(-45, 21),
        S(-10, 12), S(-14, 37), S(-29, 45), S(-47, 44),
        S(-19, 18), S(-20, 41), S(-23, 52), S(-35, 48),
        S(-23, -1), S(-21, 20), S(-24, 30), S(-29, 39),
        S(-23, -40), S(-21, -8), S(-24, 3), S(-28, 27)
    },
    {
        {},
        {},
        {S(-95, -95), S(-55, -55), S(-15, -18), S(2, -5), S(21, 7), S(34, 18), S(53, 22), S(60, 27), S(60, 27)},
        {S(-16, 53), S(19, 88), S(52, 118), S(78, 130), S(96, 134), S(104, 146), S(113, 150), S(114, 153), S(124, 153), S(124, 155), S(124, 155), S(124, 155), S(128, 155), S(128, 155)},
        {S(-37, 260), S(-7, 290), S(20, 298), S(20, 311), S(20, 311), S(22, 323), S(24, 326), S(35, 331), S(44, 332), S(53, 340), S(53, 347), S(53, 348), S(53, 351), S(54, 353), S(54, 353)},
        {S(11, 291), S(36, 316), S(61, 341), S(86, 342), S(103, 347), S(121, 349), S(131, 357), S(147, 357), S(148, 372), S(156, 374), S(158, 381), S(171, 381), S(171, 390), S(171, 393), S(175, 403), S(175, 403), S(180, 403), S(191, 403), S(191, 403), S(204, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403)},
        {},
    },
    {S(0, 0), S(-37, 28), S(-37, 30), S(-31, 69), S(4, 132), S(114, 326), S(553, 513), S(0, 0)},
    {S(0, 0), S(-2, -8), S(41, 7), S(46, 10), S(77, 23), S(81, 84), S(394, 124), S(0, 0)},
    S(129, 8), // RookOpenFileBonus
    S(54, 8), // RookSemiOpenFileBonus
    S(7, 0), // RookOnQueenFile
    S(70, 9), // KnightOutpostBonus
    S(79, 14), // BishopOutpostBonus
    S(-82, 0), // TrappedRookByKingPenalty
    S(83, 6), // RookBehindOurPasserBonus
    S(-51, 86), // RookBehindTheirPasserBonus
    S(24, 5), // MinorBehindPawnBonus
    S(25, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-17, -3), // KingProtector
    S(8, 0), // BishopPair
    {
        {S(0, 0), S(149, 0), S(143, 0), S(96, 0), S(76, 0), S(71, 0), S(2, 0)},
        {S(-96, 0), S(205, 0), S(126, 0), S(4, 0), S(0, 0), S(23, 0), S(108, 0)},
        {S(-1, 0), S(180, 0), S(63, 0), S(43, 0), S(30, 0), S(123, 0), S(83, 0)},
        {S(0, 0), S(99, 0), S(92, 0), S(75, 0), S(51, 0), S(50, 0), S(30, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(3, 0), S(7, 0), S(0, 0), S(5, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(117, 0), S(10, 0), S(10, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(101, 0), S(304, 0), S(66, 0), S(23, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(12, 0), S(260, 0), S(66, 0), S(10, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(129, 0), S(11, 0), S(0, 0), S(0, 0), S(0, 0)}, // BlockedStorm
    S(-51, 0), // UndefendedKingZoneSq
    S(9, 79), // KingMobilityFactor
    S(31, 34), // KingAttackByKnight
    S(11, 58), // KingAttackByBishop
    S(31, 34), // KingAttackByRook
    S(31, 34), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(34, 426), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(14, 13), // KingRingWeakWeight
    S(25, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -14), // DoubledPawnPenalty
    S(-14, -6), // BackwardPawnPenalty
    S(-22, -15), // WeakUnopposedPenalty
    S(-10, -30), // DoubledIsolatedPenalty
    {S(-66, -25), S(0, -45)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-13, -14), // PawnIslandPenalty
    {S(15, 0), S(0, 0)}, // CentralPawnBonus
    S(45, 19), // BishopLongDiagonalBonus
    S(-16, 0), // BishopXrayPawns
    S(0, 44), // InitiativePasser
    S(0, 52), // InitiativePawnCount
    S(0, 2), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 38), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(71, 6), // SliderOnQueenBishop
    S(47, 0), // SliderOnQueenRook
    S(5, 2), // RestrictedPiece
    S(37, 0), // ThreatByPawnPush
    S(-33, -14), // WeakQueenDefender
    S(79, 0), // KnightOnQueen
    S(-280, -108), // PawnlessFlank
    S(1, 7), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 78), // KBNKCornerEg
    S(0, 523), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
