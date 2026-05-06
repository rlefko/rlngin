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
    S(153, 49), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(121, 10), S(107, 11), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(146, 0), S(0, 0)},
    S(65, 4), // ThreatByKing
    S(72, 23), // Hanging
    S(88, 3), // WeakQueen
    S(21, 18), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 30), S(0, 34), S(0, 35), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 39), S(0, 72), S(0, 85), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-29, -23), S(-29, -40), S(-29, -61), S(-261, -117), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(3, 21), S(5, 59), S(52, 110), S(225, 377), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-29, 44), S(-11, 44), S(23, 44), S(23, 44), S(0, 0)},
    S(0, 46), // RookOn7thBonus
    S(0, -15), // BadBishop
    S(-4, -11), // BishopPawns
    S(39, 0), // Tempo
    {S(0, 0), S(149, 214), S(763, 621), S(799, 579), S(1168, 893), S(2322, 1813), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-19, -19), S(-9, -14), S(-4, -13), S(19, -29), S(20, -24), S(15, -1), S(-7, -15), S(-24, -22),
        S(-59, -31), S(-41, -24), S(-19, -24), S(-21, -39), S(4, -23), S(-22, -14), S(-36, -24), S(-67, -31),
        S(-19, -14), S(-24, -9), S(-18, -26), S(11, -28), S(12, -28), S(-9, -21), S(-20, -17), S(-21, -17),
        S(2, 26), S(-15, 16), S(-12, -1), S(45, -11), S(56, -7), S(1, 2), S(-19, 16), S(4, 23),
        S(2, 32), S(3, 32), S(26, 17), S(58, 23), S(62, 25), S(28, 19), S(3, 29), S(0, 35),
        S(-2, 24), S(4, 24), S(27, 28), S(49, 33), S(50, 35), S(27, 28), S(4, 25), S(-1, 26),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-115, -25), S(-111, -30), S(-110, -16), S(-75, -7),
        S(-102, -17), S(-60, -10), S(-74, -19), S(-49, -12),
        S(-87, -20), S(-37, -8), S(-19, -9), S(15, 7),
        S(12, 5), S(17, 1), S(38, 24), S(19, 33),
        S(37, 12), S(59, 10), S(65, 31), S(65, 29),
        S(37, -5), S(64, 6), S(80, 25), S(86, 39),
        S(16, -23), S(28, -15), S(47, 0), S(66, 30),
        S(-4, -31), S(17, -14), S(37, 5), S(49, 16)
    },
    // BishopPST (half-board)
    {
        S(-3, -16), S(19, 5), S(-74, 2), S(-59, -13),
        S(13, -20), S(0, -39), S(4, -3), S(-53, 8),
        S(-6, -20), S(22, -18), S(-16, -9), S(8, 18),
        S(30, -17), S(33, -3), S(30, 21), S(20, 14),
        S(-10, -25), S(50, 1), S(49, 13), S(39, 14),
        S(26, 7), S(21, 27), S(12, 14), S(30, 22),
        S(-14, -8), S(-32, -3), S(-13, 20), S(-1, 17),
        S(-22, -7), S(-25, 8), S(-27, 6), S(-24, 8)
    },
    // RookPST (half-board)
    {
        S(-76, -22), S(-39, -25), S(-29, -24), S(10, -42),
        S(-75, -30), S(-27, -35), S(-32, -28), S(-7, -37),
        S(-52, -20), S(-11, -21), S(-10, -21), S(-1, -19),
        S(-21, 6), S(8, 7), S(-1, 13), S(16, -1),
        S(-17, 9), S(30, 32), S(42, 20), S(57, 13),
        S(-10, 20), S(31, 33), S(49, 41), S(62, 28),
        S(-9, -11), S(-2, 7), S(23, 11), S(43, 10),
        S(-5, 13), S(10, 29), S(26, 27), S(34, 23)
    },
    // QueenPST (half-board)
    {
        S(12, -89), S(32, -87), S(23, -90), S(33, -56),
        S(28, -90), S(24, -78), S(38, -53), S(39, -41),
        S(24, -56), S(46, -21), S(46, -14), S(34, -12),
        S(39, -20), S(56, 6), S(34, 29), S(27, 46),
        S(15, -14), S(36, 14), S(5, 40), S(-12, 83),
        S(-3, 13), S(-9, 37), S(-18, 60), S(-13, 93),
        S(-46, 10), S(-84, 25), S(-56, 51), S(-44, 66),
        S(-79, 9), S(-78, 28), S(-68, 43), S(-55, 62)
    },
    // KingPST (half-board)
    {
        S(135, -162), S(95, -109), S(36, -74), S(-64, -69),
        S(113, -103), S(76, -68), S(3, -53), S(-12, -42),
        S(21, -68), S(1, -45), S(-15, -32), S(-26, -21),
        S(-15, -25), S(-12, -2), S(-22, 4), S(-32, 8),
        S(-15, 19), S(-13, 36), S(-18, 36), S(-26, 29),
        S(-10, 46), S(-8, 62), S(-17, 61), S(-25, 54),
        S(-16, 52), S(-13, 67), S(-17, 69), S(-22, 66),
        S(-17, 57), S(-16, 66), S(-18, 71), S(-20, 71)
    },
    {
        {},
        {},
        {S(-92, -121), S(-52, -81), S(-30, -43), S(-15, -11), S(1, 5), S(11, 24), S(25, 26), S(26, 26), S(26, 26)},
        {S(21, 64), S(43, 71), S(66, 102), S(76, 120), S(91, 140), S(94, 152), S(107, 153), S(109, 155), S(114, 155), S(114, 155), S(114, 155), S(114, 155), S(125, 155), S(132, 155)},
        {S(-36, 276), S(-17, 278), S(2, 287), S(16, 291), S(17, 301), S(29, 314), S(35, 320), S(44, 327), S(45, 341), S(54, 341), S(54, 349), S(54, 353), S(54, 353), S(54, 353), S(54, 353)},
        {S(84, 246), S(84, 271), S(90, 290), S(94, 315), S(97, 332), S(111, 357), S(111, 364), S(124, 372), S(126, 380), S(138, 388), S(141, 393), S(156, 393), S(156, 402), S(156, 403), S(164, 403), S(164, 403), S(165, 403), S(166, 403), S(171, 403), S(179, 403), S(187, 403), S(187, 403), S(199, 403), S(199, 403), S(208, 403), S(228, 403), S(228, 403), S(238, 427)},
        {},
    },
    {S(0, 0), S(-22, 15), S(-22, 24), S(-22, 52), S(22, 153), S(147, 375), S(488, 583), S(0, 0)},
    {S(0, 0), S(-13, 1), S(21, 11), S(33, 11), S(79, 24), S(79, 78), S(122, 138), S(0, 0)},
    S(86, 15), // RookOpenFileBonus
    S(45, 15), // RookSemiOpenFileBonus
    S(6, 0), // RookOnQueenFile
    S(55, 13), // KnightOutpostBonus
    S(70, 17), // BishopOutpostBonus
    S(-45, 0), // TrappedRookByKingPenalty
    S(26, 29), // RookBehindOurPasserBonus
    S(-59, 105), // RookBehindTheirPasserBonus
    S(25, 11), // MinorBehindPawnBonus
    S(31, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-14, 0), // KingProtector
    S(28, 30), // BishopPair
    {
        {S(-13, 0), S(106, 0), S(110, 0), S(81, 0), S(67, 0), S(91, 0), S(25, 0)},
        {S(-56, 0), S(174, 0), S(89, 0), S(25, 0), S(0, 0), S(134, 0), S(1, 0)},
        {S(0, 0), S(125, 0), S(52, 0), S(57, 0), S(13, 0), S(90, 0), S(253, 0)},
        {S(0, 0), S(128, 0), S(25, 0), S(46, 0), S(66, 0), S(12, 0), S(95, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(74, 0), S(17, 0), S(16, 0), S(14, 0)},
        {S(0, 0), S(0, 0), S(79, 0), S(35, 0), S(13, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(208, 0), S(90, 0), S(21, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(38, 0), S(53, 0), S(16, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(169, 0), S(25, 0), S(27, 0), S(8, 0), S(14, 0)}, // BlockedStorm
    S(-33, 0), // UndefendedKingZoneSq
    S(13, 55), // KingMobilityFactor
    S(28, 35), // KingAttackByKnight
    S(5, 24), // KingAttackByBishop
    S(30, 35), // KingAttackByRook
    S(30, 35), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(35, 531), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(7, 0), // KingRingWeakWeight
    S(11, 4), // KingNoQueenDiscount
    S(0, -2), // IsolatedPawnPenalty
    S(0, -19), // DoubledPawnPenalty
    S(-12, -5), // BackwardPawnPenalty
    S(-27, -21), // WeakUnopposedPenalty
    S(-22, -46), // DoubledIsolatedPenalty
    {S(0, -27), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-42, -10), // PawnIslandPenalty
    {S(10, 0), S(8, 0)}, // CentralPawnBonus
    S(32, 16), // BishopLongDiagonalBonus
    S(-15, 0), // BishopXrayPawns
    S(0, 13), // InitiativePasser
    S(0, 18), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 29), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(48, 17), // SliderOnQueenBishop
    S(22, 0), // SliderOnQueenRook
    S(9, 3), // RestrictedPiece
    S(24, 0), // ThreatByPawnPush
    S(-12, -25), // WeakQueenDefender
    S(53, 6), // KnightOnQueen
    S(-248, -81), // PawnlessFlank
    S(0, 38), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 627), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
