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
    S(270, 50), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(189, 0), S(148, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(217, 31), S(0, 0)},
    S(139, 29), // ThreatByKing
    S(96, 17), // Hanging
    S(0, 17), // WeakQueen
    S(27, 31), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 43), S(0, 43), S(0, 43), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 39), S(0, 73), S(0, 84), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-39, -38), S(-39, -60), S(-123, -60), S(-327, -125), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 25), S(53, 67), S(165, 143), S(165, 428), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-51, 17), S(46, 28), S(87, 28), S(87, 28), S(0, 0)},
    S(0, 20), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -7), // BishopPawns
    S(87, 0), // Tempo
    {S(0, 0), S(200, 224), S(919, 556), S(952, 488), S(1497, 771), S(2668, 1751), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-50, -57), S(-48, -58), S(-37, -59), S(-1, -64), S(2, -57), S(-33, -53), S(-45, -57), S(-55, -69),
        S(-98, -53), S(-90, -61), S(-60, -67), S(-41, -66), S(-32, -57), S(-59, -63), S(-63, -70), S(-93, -63),
        S(-75, -40), S(-78, -52), S(-75, -70), S(-42, -71), S(-35, -71), S(-63, -67), S(-67, -51), S(-76, -50),
        S(-14, -6), S(-48, -27), S(-10, -51), S(18, -65), S(27, -62), S(-6, -52), S(-65, -20), S(-18, -11),
        S(99, 66), S(68, 76), S(105, 26), S(185, 12), S(192, 15), S(95, 24), S(43, 79), S(74, 75),
        S(181, 162), S(-49, 223), S(79, 176), S(273, 122), S(245, 118), S(33, 185), S(-248, 239), S(64, 162),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-95, -56), S(-79, -5), S(-59, -9), S(-71, 24),
        S(-77, -21), S(-68, -11), S(-25, -5), S(-18, 12),
        S(-7, 9), S(18, 12), S(37, -6), S(40, 23),
        S(10, 0), S(77, 5), S(72, 30), S(80, 34),
        S(46, 21), S(84, 11), S(111, 27), S(98, 32),
        S(-41, 2), S(51, 10), S(123, 20), S(123, 27),
        S(-102, -34), S(-26, -7), S(68, -12), S(50, 14),
        S(-278, -100), S(-152, -40), S(-26, -15), S(13, -3)
    },
    // BishopPST (half-board)
    {
        S(18, -24), S(16, -25), S(-34, -6), S(-31, 3),
        S(5, -21), S(13, -25), S(38, -4), S(-2, 8),
        S(20, -11), S(45, 10), S(20, -17), S(12, 19),
        S(40, -13), S(65, -5), S(60, 18), S(42, -7),
        S(30, -15), S(54, 30), S(77, 13), S(65, -9),
        S(26, -12), S(11, 16), S(36, 3), S(55, 4),
        S(-45, -16), S(-88, 5), S(-19, 6), S(-76, -2),
        S(-78, 5), S(-111, 18), S(-169, 20), S(-119, 27)
    },
    // RookPST (half-board)
    {
        S(-47, -12), S(-42, 2), S(-12, -5), S(-8, -11),
        S(-66, -28), S(-50, -21), S(-32, -12), S(-26, -15),
        S(-66, -26), S(-55, -14), S(-55, -19), S(-43, -29),
        S(-35, 3), S(-41, 6), S(-46, 11), S(12, -5),
        S(-52, 26), S(-20, 23), S(17, 12), S(45, -1),
        S(16, 12), S(12, 14), S(58, 15), S(72, 11),
        S(29, 3), S(40, 10), S(83, -2), S(91, 11),
        S(38, 11), S(47, 12), S(69, 2), S(88, -3)
    },
    // QueenPST (half-board)
    {
        S(28, -96), S(22, -85), S(18, -66), S(35, -48),
        S(3, -37), S(19, -41), S(50, -40), S(55, -33),
        S(-24, -39), S(36, -17), S(35, 5), S(28, -17),
        S(13, -17), S(46, 11), S(41, 28), S(23, 21),
        S(12, -4), S(19, 7), S(6, 60), S(0, 68),
        S(-2, 1), S(-17, 17), S(-41, 69), S(-41, 97),
        S(-94, 13), S(-147, 60), S(-120, 85), S(-108, 104),
        S(-83, -11), S(-102, -12), S(231, -72), S(53, -14)
    },
    // KingPST (half-board)
    {
        S(234, -95), S(235, -64), S(82, -32), S(-26, -42),
        S(256, -45), S(210, -24), S(66, 3), S(-12, 9),
        S(29, -13), S(133, -5), S(29, 17), S(-56, 30),
        S(-55, 5), S(27, 27), S(-51, 48), S(-185, 51),
        S(54, 12), S(-15, 47), S(-130, 66), S(-263, 76),
        S(-28, 3), S(-51, 46), S(-173, 75), S(-272, 84),
        S(231, -59), S(-99, 15), S(-22, 16), S(-179, 38),
        S(-275, -189), S(260, -46), S(270, -85), S(-236, 30)
    },
    {
        {},
        {},
        {S(-77, -89), S(-39, -49), S(-3, -9), S(16, -5), S(29, 10), S(39, 28), S(56, 35), S(56, 36), S(56, 36)},
        {S(-13, 63), S(22, 88), S(56, 123), S(83, 138), S(108, 139), S(112, 148), S(117, 161), S(121, 163), S(121, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163)},
        {S(-20, 254), S(5, 284), S(20, 294), S(20, 304), S(20, 305), S(20, 323), S(20, 323), S(33, 333), S(33, 337), S(45, 349), S(47, 353), S(49, 356), S(58, 357), S(67, 357), S(80, 357)},
        {S(22, 291), S(43, 316), S(68, 341), S(75, 351), S(100, 351), S(125, 351), S(139, 362), S(148, 363), S(155, 377), S(171, 377), S(173, 380), S(174, 381), S(174, 381), S(176, 396), S(176, 402), S(178, 403), S(186, 403), S(202, 403), S(203, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(236, 403), S(237, 403)},
        {},
    },
    {S(0, 0), S(-39, 41), S(-39, 42), S(-32, 83), S(0, 149), S(0, 314), S(504, 378), S(0, 0)},
    {S(0, 0), S(-12, -7), S(50, 9), S(51, 9), S(69, 15), S(69, 79), S(245, 115), S(0, 0)},
    S(114, 13), // RookOpenFileBonus
    S(25, 11), // RookSemiOpenFileBonus
    S(0, 3), // RookOnQueenFile
    S(97, 13), // KnightOutpostBonus
    S(91, 16), // BishopOutpostBonus
    S(-104, 0), // TrappedRookByKingPenalty
    S(67, 4), // RookBehindOurPasserBonus
    S(-109, 100), // RookBehindTheirPasserBonus
    S(37, 4), // MinorBehindPawnBonus
    S(28, 0), // MinorOnKingRing
    S(17, 0), // RookOnKingRing
    S(-17, -2), // KingProtector
    S(6, 5), // BishopPair
    {
        {S(0, 0), S(113, 0), S(109, 0), S(77, 0), S(56, 0), S(39, 0), S(0, 0)},
        {S(-83, 0), S(201, 0), S(80, 0), S(4, 0), S(0, 0), S(70, 0), S(167, 0)},
        {S(-8, 0), S(193, 0), S(65, 0), S(42, 0), S(54, 0), S(75, 0), S(89, 0)},
        {S(-4, 0), S(151, 0), S(86, 0), S(56, 0), S(35, 0), S(120, 0), S(50, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(10, 0), S(0, 0)},
        {S(0, 0), S(16, 0), S(61, 0), S(19, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(8, 0), S(258, 0), S(12, 0), S(0, 0), S(8, 0), S(0, 0)},
        {S(0, 0), S(16, 0), S(301, 0), S(38, 0), S(0, 0), S(4, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(202, 0), S(6, 0), S(0, 0), S(42, 0), S(0, 0)}, // BlockedStorm
    S(-31, 0), // UndefendedKingZoneSq
    S(9, 41), // KingMobilityFactor
    S(29, 33), // KingAttackByKnight
    S(12, 37), // KingAttackByBishop
    S(30, 41), // KingAttackByRook
    S(30, 41), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(36, 299), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(24, 14), // KingRingWeakWeight
    S(28, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -5), // DoubledPawnPenalty
    S(-15, -6), // BackwardPawnPenalty
    S(-27, -17), // WeakUnopposedPenalty
    S(0, -39), // DoubledIsolatedPenalty
    {S(-95, -24), S(-119, -97)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-8, -21), // PawnIslandPenalty
    {S(37, 0), S(27, 0)}, // CentralPawnBonus
    S(65, 28), // BishopLongDiagonalBonus
    S(-20, 0), // BishopXrayPawns
    S(0, 58), // InitiativePasser
    S(0, 52), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 61), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(45, 16), // SliderOnQueenBishop
    S(54, 2), // SliderOnQueenRook
    S(10, 0), // RestrictedPiece
    S(30, 0), // ThreatByPawnPush
    S(-41, -6), // WeakQueenDefender
    S(99, 0), // KnightOnQueen
    S(-290, -143), // PawnlessFlank
    S(0, 0), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 9), // KBNKCornerEg
    S(0, 268), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
