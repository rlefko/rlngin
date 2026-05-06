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
    S(203, 44), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(164, 20), S(183, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(237, 11), S(0, 0)},
    S(93, 0), // ThreatByKing
    S(101, 29), // Hanging
    S(100, 1), // WeakQueen
    S(20, 23), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 27), S(0, 34), S(0, 34), S(0, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 17), S(0, 31), S(0, 61), S(0, 72), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-29, -55), S(-29, -59), S(-29, -89), S(-300, -167), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(25, 25), S(30, 67), S(162, 117), S(162, 349), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-54, 21), S(51, 28), S(51, 28), S(51, 35), S(0, 0)},
    S(1, 56), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-5, -11), // BishopPawns
    S(45, 0), // Tempo
    {S(0, 0), S(172, 217), S(857, 531), S(898, 462), S(1311, 757), S(2445, 1681), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-18, -20), S(-12, -7), S(-6, -14), S(32, -9), S(22, -8), S(-5, -8), S(-16, -15), S(-24, -27),
        S(-59, -25), S(-39, -22), S(-20, -23), S(-3, -31), S(21, -21), S(-18, -16), S(-36, -28), S(-61, -30),
        S(-19, -7), S(-23, -1), S(-16, -25), S(10, -32), S(14, -33), S(-9, -23), S(-17, -10), S(-20, -13),
        S(3, 34), S(-4, 25), S(-2, 0), S(40, -25), S(38, -23), S(5, 0), S(-7, 21), S(2, 27),
        S(-3, 38), S(5, 32), S(25, 14), S(39, -2), S(41, 1), S(25, 12), S(5, 30), S(-7, 35),
        S(-2, 29), S(3, 26), S(23, 25), S(36, 17), S(37, 19), S(23, 24), S(2, 25), S(-3, 28),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-122, -16), S(-105, -23), S(-87, -27), S(-83, -16),
        S(-98, -8), S(-63, -16), S(-49, -21), S(-28, -18),
        S(-81, -8), S(-11, -9), S(-9, -8), S(15, 9),
        S(-4, 5), S(24, 0), S(53, 28), S(37, 34),
        S(40, 3), S(66, 15), S(82, 41), S(64, 39),
        S(25, -9), S(63, 21), S(74, 30), S(78, 44),
        S(-10, -33), S(17, -10), S(39, 9), S(60, 33),
        S(-40, -52), S(-3, -21), S(24, 1), S(43, 14)
    },
    // BishopPST (half-board)
    {
        S(-13, -23), S(0, -10), S(-76, -4), S(-39, -1),
        S(6, -35), S(8, -29), S(13, -14), S(-42, 10),
        S(-3, -22), S(39, -17), S(-16, -11), S(-7, 19),
        S(29, -22), S(36, 1), S(32, 22), S(17, 7),
        S(-3, -7), S(53, 13), S(48, 14), S(32, 15),
        S(21, 9), S(9, 27), S(13, 21), S(25, 14),
        S(-25, -10), S(-38, 0), S(-10, 21), S(-5, 15),
        S(-32, -6), S(-35, 2), S(-27, 5), S(-24, 11)
    },
    // RookPST (half-board)
    {
        S(-48, -17), S(-28, -24), S(-10, -13), S(12, -34),
        S(-63, -35), S(-38, -30), S(-30, -31), S(-14, -40),
        S(-63, -21), S(-32, -21), S(-29, -19), S(-35, -24),
        S(-35, 10), S(-6, 15), S(-16, 21), S(7, -6),
        S(-20, 17), S(21, 31), S(38, 27), S(52, 10),
        S(-1, 27), S(28, 28), S(45, 36), S(56, 20),
        S(7, -4), S(3, 7), S(34, 11), S(59, 9),
        S(11, 13), S(16, 15), S(38, 17), S(49, 14)
    },
    // QueenPST (half-board)
    {
        S(-15, -90), S(0, -75), S(30, -72), S(41, -58),
        S(17, -78), S(15, -58), S(34, -38), S(50, -29),
        S(22, -59), S(48, -22), S(48, -5), S(8, 1),
        S(48, -28), S(66, 6), S(34, 32), S(35, 47),
        S(22, -16), S(22, 15), S(32, 38), S(9, 53),
        S(2, 8), S(-4, 31), S(-7, 51), S(10, 72),
        S(-74, 8), S(-101, 25), S(-51, 41), S(-35, 60),
        S(-101, 0), S(-82, 22), S(-54, 42), S(-43, 52)
    },
    // KingPST (half-board)
    {
        S(150, -140), S(131, -101), S(36, -64), S(-60, -69),
        S(146, -98), S(110, -57), S(16, -29), S(-21, -19),
        S(40, -69), S(28, -42), S(-3, -18), S(-40, -4),
        S(-6, -33), S(-7, 0), S(-21, 17), S(-43, 20),
        S(-19, 6), S(-18, 37), S(-29, 44), S(-49, 40),
        S(-24, 30), S(-21, 53), S(-28, 56), S(-47, 43),
        S(-27, 36), S(-24, 51), S(-26, 56), S(-31, 47),
        S(-29, 37), S(-27, 47), S(-26, 55), S(-26, 57)
    },
    {
        {},
        {},
        {S(-98, -115), S(-58, -75), S(-21, -35), S(-4, -15), S(12, 11), S(27, 26), S(46, 26), S(51, 26), S(51, 26)},
        {S(8, 41), S(36, 75), S(61, 107), S(79, 121), S(96, 135), S(98, 151), S(112, 151), S(112, 155), S(124, 155), S(124, 155), S(124, 155), S(124, 155), S(128, 155), S(128, 155)},
        {S(-45, 258), S(-15, 286), S(5, 287), S(20, 293), S(20, 301), S(36, 314), S(37, 319), S(44, 327), S(50, 342), S(54, 348), S(54, 353), S(54, 353), S(54, 353), S(54, 353), S(54, 353)},
        {S(36, 291), S(36, 316), S(61, 316), S(77, 333), S(89, 351), S(109, 357), S(109, 372), S(124, 374), S(131, 375), S(146, 384), S(157, 390), S(164, 390), S(164, 390), S(164, 396), S(172, 396), S(172, 403), S(174, 403), S(186, 403), S(186, 403), S(193, 403), S(199, 403), S(224, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(229, 403)},
        {},
    },
    {S(0, 0), S(-30, 15), S(-30, 33), S(-20, 46), S(29, 132), S(163, 352), S(555, 570), S(0, 0)},
    {S(0, 0), S(-6, -3), S(27, 19), S(38, 19), S(79, 30), S(138, 79), S(314, 105), S(0, 0)},
    S(121, 9), // RookOpenFileBonus
    S(60, 9), // RookSemiOpenFileBonus
    S(4, 4), // RookOnQueenFile
    S(72, 12), // KnightOutpostBonus
    S(75, 24), // BishopOutpostBonus
    S(-54, 0), // TrappedRookByKingPenalty
    S(51, 33), // RookBehindOurPasserBonus
    S(-80, 93), // RookBehindTheirPasserBonus
    S(25, 6), // MinorBehindPawnBonus
    S(22, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-15, 0), // KingProtector
    S(24, 19), // BishopPair
    {
        {S(-8, 0), S(125, 0), S(122, 0), S(70, 0), S(79, 0), S(98, 0), S(0, 0)},
        {S(-89, 0), S(197, 0), S(107, 0), S(0, 0), S(0, 0), S(52, 0), S(56, 0)},
        {S(0, 0), S(160, 0), S(63, 0), S(55, 0), S(21, 0), S(118, 0), S(183, 0)},
        {S(0, 0), S(120, 0), S(56, 0), S(64, 0), S(60, 0), S(19, 0), S(84, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(51, 0), S(25, 0), S(33, 0), S(17, 0)},
        {S(0, 0), S(0, 0), S(87, 0), S(32, 0), S(3, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(207, 0), S(91, 0), S(26, 0), S(8, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(152, 0), S(23, 0), S(6, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(148, 0), S(35, 0), S(0, 0), S(0, 0), S(0, 0)}, // BlockedStorm
    S(-55, 0), // UndefendedKingZoneSq
    S(10, 75), // KingMobilityFactor
    S(31, 34), // KingAttackByKnight
    S(12, 48), // KingAttackByBishop
    S(31, 34), // KingAttackByRook
    S(31, 34), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(34, 531), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(13, 0), // KingRingWeakWeight
    S(10, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -18), // DoubledPawnPenalty
    S(-21, -3), // BackwardPawnPenalty
    S(-30, -29), // WeakUnopposedPenalty
    S(-17, -38), // DoubledIsolatedPenalty
    {S(-32, -32), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-31, 0), // PawnIslandPenalty
    {S(6, 0), S(0, 0)}, // CentralPawnBonus
    S(47, 20), // BishopLongDiagonalBonus
    S(-16, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(76, 16), // SliderOnQueenBishop
    S(37, 0), // SliderOnQueenRook
    S(6, 0), // RestrictedPiece
    S(29, 0), // ThreatByPawnPush
    S(-24, -28), // WeakQueenDefender
    S(75, 0), // KnightOnQueen
    S(-288, -112), // PawnlessFlank
    S(8, 28), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 586), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
