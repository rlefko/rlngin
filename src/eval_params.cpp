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
    S(224, 37), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(194, 12), S(204, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(250, 31), S(0, 0)},
    S(116, 0), // ThreatByKing
    S(111, 27), // Hanging
    S(65, 0), // WeakQueen
    S(29, 22), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 43), S(0, 43), S(0, 43), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 42), S(0, 77), S(0, 89), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-28, -32), S(-28, -53), S(-105, -53), S(-368, -115), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 18), S(53, 55), S(179, 113), S(179, 380), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-37, 21), S(64, 28), S(119, 28), S(119, 28), S(0, 0)},
    S(0, 25), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -7), // BishopPawns
    S(48, 0), // Tempo
    {S(0, 0), S(220, 235), S(944, 585), S(974, 518), S(1497, 827), S(2697, 1872), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-47, -51), S(-57, -55), S(-42, -60), S(-16, -69), S(25, -56), S(-30, -53), S(-28, -60), S(-46, -75),
        S(-105, -47), S(-116, -55), S(-59, -71), S(-65, -65), S(-12, -61), S(-83, -59), S(-53, -77), S(-105, -72),
        S(-81, -37), S(-105, -50), S(-96, -73), S(-56, -71), S(-28, -81), S(-49, -71), S(-60, -61), S(-98, -59),
        S(-20, -4), S(-40, -45), S(-36, -57), S(3, -76), S(30, -75), S(-11, -67), S(-96, -27), S(-27, -27),
        S(112, 47), S(71, 61), S(102, 16), S(188, -11), S(208, -12), S(97, -4), S(48, 81), S(47, 81),
        S(231, 157), S(-18, 218), S(161, 187), S(300, 157), S(214, 148), S(18, 227), S(-241, 274), S(57, 205),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-149, -54), S(-82, 0), S(-55, -19), S(-83, 16),
        S(-93, -11), S(-95, 9), S(-22, -10), S(-10, 5),
        S(-8, 2), S(33, 2), S(38, -10), S(30, 22),
        S(1, 11), S(93, 4), S(81, 31), S(85, 27),
        S(75, 26), S(109, 1), S(137, 18), S(90, 36),
        S(-69, 12), S(72, 3), S(136, 17), S(143, 26),
        S(-62, -45), S(-9, 13), S(105, -38), S(34, 37),
        S(-298, -145), S(-148, -25), S(-81, 6), S(1, 13)
    },
    // BishopPST (half-board)
    {
        S(20, -32), S(45, -35), S(-60, 0), S(-42, 3),
        S(1, -31), S(15, -25), S(58, -10), S(-10, 8),
        S(25, -14), S(63, 6), S(13, -15), S(3, 24),
        S(31, -10), S(80, -11), S(58, 20), S(39, 1),
        S(6, -10), S(47, 35), S(103, 6), S(43, -1),
        S(65, -22), S(35, 16), S(46, 4), S(84, 2),
        S(-19, -36), S(-123, 7), S(4, 9), S(-77, -3),
        S(-59, 9), S(-112, 25), S(-205, 27), S(-159, 37)
    },
    // RookPST (half-board)
    {
        S(-37, -22), S(-35, -8), S(-6, -12), S(2, -19),
        S(-81, -26), S(-42, -35), S(-15, -28), S(-39, -19),
        S(-85, -23), S(-53, -13), S(-77, -17), S(-67, -17),
        S(-8, -12), S(-49, 6), S(-66, 18), S(22, -13),
        S(-58, 26), S(-5, 33), S(26, 15), S(30, -1),
        S(37, 8), S(-5, 29), S(51, 20), S(100, 2),
        S(23, 7), S(0, 15), S(102, -5), S(108, 4),
        S(33, 22), S(30, 28), S(45, 14), S(114, -4)
    },
    // QueenPST (half-board)
    {
        S(20, -109), S(29, -98), S(4, -71), S(26, -51),
        S(1, -29), S(6, -43), S(49, -47), S(54, -43),
        S(-34, -24), S(42, -30), S(31, 5), S(16, -16),
        S(26, -23), S(54, 7), S(37, 14), S(23, 14),
        S(1, 5), S(29, -8), S(-5, 76), S(0, 72),
        S(22, -21), S(9, 2), S(-5, 55), S(-26, 110),
        S(-109, 10), S(-169, 79), S(-146, 112), S(-114, 120),
        S(-28, -29), S(-102, 18), S(222, -65), S(56, -10)
    },
    // KingPST (half-board)
    {
        S(232, -108), S(244, -79), S(85, -48), S(-22, -60),
        S(273, -68), S(226, -46), S(45, -13), S(-39, -7),
        S(4, -18), S(141, -23), S(32, 3), S(-68, 15),
        S(-81, 4), S(30, 16), S(-37, 43), S(-204, 45),
        S(61, 2), S(9, 53), S(-130, 68), S(-295, 80),
        S(-35, 15), S(-40, 87), S(-217, 109), S(-296, 107),
        S(261, -70), S(-96, 17), S(22, 82), S(-186, 83),
        S(-291, -239), S(301, 5), S(301, -122), S(-255, 68)
    },
    {
        {},
        {},
        {S(-68, -146), S(-39, -37), S(0, -10), S(14, 1), S(27, 17), S(35, 34), S(51, 39), S(56, 39), S(56, 39)},
        {S(-28, 100), S(34, 100), S(63, 130), S(86, 140), S(104, 143), S(108, 153), S(115, 162), S(116, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(128, 163), S(128, 163)},
        {S(-25, 259), S(6, 292), S(20, 300), S(20, 310), S(20, 310), S(20, 325), S(20, 327), S(33, 336), S(38, 337), S(45, 349), S(45, 353), S(45, 354), S(45, 357), S(45, 357), S(45, 357)},
        {S(48, 112), S(49, 266), S(93, 341), S(104, 341), S(112, 351), S(133, 351), S(141, 360), S(153, 362), S(158, 373), S(168, 377), S(168, 382), S(174, 382), S(174, 389), S(174, 399), S(176, 403), S(176, 403), S(176, 403), S(193, 403), S(193, 403), S(207, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(240, 403), S(240, 403)},
        {},
    },
    {S(0, 0), S(-37, 38), S(-37, 39), S(-29, 75), S(0, 162), S(0, 344), S(453, 392), S(0, 0)},
    {S(0, 0), S(-10, -7), S(49, 9), S(57, 9), S(78, 25), S(78, 96), S(263, 144), S(0, 0)},
    S(117, 17), // RookOpenFileBonus
    S(46, 17), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(77, 13), // KnightOutpostBonus
    S(81, 17), // BishopOutpostBonus
    S(-76, 0), // TrappedRookByKingPenalty
    S(61, 9), // RookBehindOurPasserBonus
    S(-76, 93), // RookBehindTheirPasserBonus
    S(33, 7), // MinorBehindPawnBonus
    S(20, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-19, -2), // KingProtector
    S(11, 7), // BishopPair
    {
        {S(-5, 0), S(95, 0), S(112, 0), S(78, 0), S(48, 0), S(40, 0), S(2, 0)},
        {S(-98, 0), S(182, 0), S(77, 0), S(0, 0), S(0, 0), S(52, 0), S(155, 0)},
        {S(-22, 0), S(182, 0), S(62, 0), S(18, 0), S(49, 0), S(77, 0), S(96, 0)},
        {S(-10, 0), S(147, 0), S(88, 0), S(57, 0), S(27, 0), S(98, 0), S(32, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(14, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(51, 0), S(35, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(226, 0), S(16, 0), S(0, 0), S(9, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(269, 0), S(42, 0), S(4, 0), S(6, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(184, 0), S(12, 0), S(0, 0), S(38, 0), S(0, 0)}, // BlockedStorm
    S(-37, 0), // UndefendedKingZoneSq
    S(7, 43), // KingMobilityFactor
    S(26, 31), // KingAttackByKnight
    S(11, 26), // KingAttackByBishop
    S(26, 31), // KingAttackByRook
    S(26, 31), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(30, 1), S(31, 272), S(30, 1), S(30, 1), S(0, 0)}, // KingSafeCheck
    S(20, 9), // KingRingWeakWeight
    S(26, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-3, -15), // DoubledPawnPenalty
    S(-7, -5), // BackwardPawnPenalty
    S(-29, -19), // WeakUnopposedPenalty
    S(-2, -40), // DoubledIsolatedPenalty
    {S(-83, -23), S(-117, -95)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-19, -19), // PawnIslandPenalty
    {S(34, 0), S(33, 0)}, // CentralPawnBonus
    S(69, 24), // BishopLongDiagonalBonus
    S(-21, 0), // BishopXrayPawns
    S(0, 46), // InitiativePasser
    S(0, 54), // InitiativePawnCount
    S(0, 2), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 37), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(43, 7), // SliderOnQueenBishop
    S(34, 0), // SliderOnQueenRook
    S(8, 1), // RestrictedPiece
    S(35, 20), // ThreatByPawnPush
    S(-50, -10), // WeakQueenDefender
    S(16, 12), // KnightOnQueen
    S(-17, -78), // PawnlessFlank
    S(8, 12), // QueenInfiltration
    S(0, -16), // KingPawnDistEg
    S(0, 12), // KBNKCornerEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
