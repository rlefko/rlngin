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
    S(241, 29), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(194, 10), S(214, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(262, 22), S(0, 0)},
    S(118, 1), // ThreatByKing
    S(114, 26), // Hanging
    S(14, 0), // WeakQueen
    S(18, 22), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 36), S(0, 39), S(0, 39), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 18), S(0, 36), S(0, 67), S(0, 78), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-36, -20), S(-36, -37), S(-63, -60), S(-358, -105), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(36, 11), S(38, 48), S(131, 96), S(131, 332), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-43, 21), S(46, 28), S(51, 28), S(51, 28), S(0, 0)},
    S(0, 22), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(0, -7), // BishopPawns
    S(50, 0), // Tempo
    {S(0, 0), S(170, 159), S(892, 494), S(922, 425), S(1374, 655), S(2460, 1532), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-22, -11), S(-14, -14), S(-17, -17), S(3, -6), S(9, -1), S(-15, -11), S(-11, -16), S(-33, -25),
        S(-59, -16), S(-52, -25), S(-25, -31), S(-15, -29), S(7, -21), S(-31, -23), S(-40, -32), S(-57, -26),
        S(-38, -2), S(-37, -15), S(-16, -34), S(11, -36), S(14, -34), S(-3, -29), S(-31, -15), S(-42, -15),
        S(6, 31), S(-2, 12), S(11, -12), S(41, -23), S(48, -24), S(16, -15), S(-8, 9), S(3, 22),
        S(2, 56), S(17, 52), S(38, 8), S(71, 9), S(75, 6), S(41, 8), S(12, 51), S(-12, 56),
        S(16, 36), S(-8, 39), S(26, 27), S(60, 18), S(57, 18), S(28, 26), S(-6, 39), S(11, 37),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-113, -29), S(-135, -3), S(-81, -10), S(-79, 3),
        S(-91, -7), S(-63, -3), S(-37, -5), S(-26, -3),
        S(-56, 3), S(6, -2), S(12, -12), S(16, 18),
        S(-13, 12), S(41, 19), S(58, 19), S(50, 28),
        S(41, 15), S(70, 16), S(88, 23), S(75, 22),
        S(-1, -3), S(38, 6), S(71, 17), S(82, 29),
        S(-31, -39), S(6, -14), S(40, -1), S(52, 15),
        S(-73, -103), S(-17, -38), S(24, -5), S(34, 4)
    },
    // BishopPST (half-board)
    {
        S(-7, -26), S(3, -19), S(-86, 3), S(-47, 1),
        S(9, -22), S(20, -23), S(13, -7), S(-26, 4),
        S(0, -11), S(45, 6), S(11, -7), S(-9, 17),
        S(10, -8), S(29, 0), S(36, 10), S(19, 1),
        S(-14, -4), S(42, 15), S(49, 8), S(32, 0),
        S(22, -1), S(13, 11), S(35, 7), S(27, 7),
        S(-27, -14), S(-50, -6), S(-14, 5), S(-14, -2),
        S(-31, 10), S(-45, 15), S(-34, 11), S(-31, 8)
    },
    // RookPST (half-board)
    {
        S(-42, -16), S(-27, -9), S(-8, -5), S(8, -12),
        S(-66, -25), S(-33, -25), S(-22, -11), S(-28, -14),
        S(-63, -18), S(-34, -12), S(-41, -12), S(-43, -14),
        S(-43, 3), S(-18, 7), S(-27, 7), S(-4, -1),
        S(-18, 11), S(10, 23), S(17, 15), S(23, 1),
        S(16, 12), S(18, 17), S(38, 14), S(56, 5),
        S(30, 3), S(22, 6), S(57, 2), S(63, 6),
        S(29, 10), S(34, 14), S(51, 9), S(61, 1)
    },
    // QueenPST (half-board)
    {
        S(12, -78), S(21, -61), S(18, -54), S(50, -48),
        S(20, -57), S(36, -50), S(56, -46), S(69, -35),
        S(17, -41), S(53, -27), S(59, -4), S(23, 0),
        S(51, -27), S(62, 4), S(31, 25), S(22, 33),
        S(26, -23), S(21, 9), S(21, 49), S(0, 53),
        S(14, -13), S(-23, 24), S(-26, 54), S(-10, 84),
        S(-85, 4), S(-127, 32), S(-78, 59), S(-59, 73),
        S(-100, -11), S(-88, 12), S(-49, 34), S(-45, 32)
    },
    // KingPST (half-board)
    {
        S(140, -93), S(146, -67), S(15, -39), S(-122, -46),
        S(166, -56), S(126, -37), S(6, -15), S(-64, -5),
        S(43, -39), S(48, -18), S(-1, -1), S(-45, 5),
        S(0, -15), S(4, 8), S(-21, 23), S(-48, 23),
        S(-10, 12), S(-14, 37), S(-29, 46), S(-47, 45),
        S(-19, 17), S(-20, 41), S(-31, 52), S(-39, 48),
        S(-31, -5), S(-21, 23), S(-20, 32), S(-29, 43),
        S(-31, -38), S(-21, 1), S(-24, 5), S(-28, 28)
    },
    {
        {},
        {},
        {S(-95, -95), S(-55, -55), S(-15, -17), S(7, -6), S(26, 8), S(38, 21), S(57, 25), S(60, 27), S(60, 27)},
        {S(-16, 53), S(19, 88), S(54, 120), S(80, 132), S(99, 136), S(105, 148), S(111, 153), S(112, 155), S(123, 155), S(124, 158), S(124, 159), S(124, 159), S(128, 159), S(128, 159)},
        {S(-37, 259), S(-7, 288), S(20, 298), S(20, 307), S(20, 311), S(22, 323), S(27, 325), S(35, 332), S(46, 334), S(53, 342), S(53, 348), S(53, 349), S(53, 351), S(53, 353), S(56, 353)},
        {S(11, 291), S(36, 316), S(61, 341), S(85, 342), S(106, 344), S(123, 347), S(133, 357), S(151, 357), S(151, 372), S(160, 374), S(160, 381), S(171, 381), S(171, 389), S(171, 396), S(174, 403), S(174, 403), S(179, 403), S(193, 403), S(193, 403), S(204, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403)},
        {},
    },
    {S(0, 0), S(-37, 28), S(-37, 32), S(-32, 69), S(5, 133), S(125, 325), S(561, 510), S(0, 0)},
    {S(0, 0), S(-3, -9), S(41, 8), S(46, 9), S(73, 24), S(81, 85), S(386, 132), S(0, 0)},
    S(123, 9), // RookOpenFileBonus
    S(49, 7), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(72, 8), // KnightOutpostBonus
    S(80, 18), // BishopOutpostBonus
    S(-84, 0), // TrappedRookByKingPenalty
    S(78, 6), // RookBehindOurPasserBonus
    S(-60, 90), // RookBehindTheirPasserBonus
    S(24, 3), // MinorBehindPawnBonus
    S(24, 0), // MinorOnKingRing
    S(7, 0), // RookOnKingRing
    S(-17, -3), // KingProtector
    S(5, 0), // BishopPair
    {
        {S(0, 0), S(147, 0), S(144, 0), S(95, 0), S(77, 0), S(63, 0), S(2, 0)},
        {S(-89, 0), S(208, 0), S(123, 0), S(6, 0), S(0, 0), S(23, 0), S(116, 0)},
        {S(0, 0), S(185, 0), S(63, 0), S(34, 0), S(30, 0), S(115, 0), S(83, 0)},
        {S(0, 0), S(114, 0), S(86, 0), S(76, 0), S(43, 0), S(64, 0), S(30, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(3, 0), S(4, 0), S(0, 0), S(4, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(109, 0), S(13, 0), S(4, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(109, 0), S(304, 0), S(66, 0), S(19, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(20, 0), S(268, 0), S(66, 0), S(9, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(137, 0), S(10, 0), S(0, 0), S(0, 0), S(0, 0)}, // BlockedStorm
    S(-42, 0), // UndefendedKingZoneSq
    S(9, 77), // KingMobilityFactor
    S(31, 32), // KingAttackByKnight
    S(11, 56), // KingAttackByBishop
    S(31, 34), // KingAttackByRook
    S(31, 34), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(35, 418), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(16, 13), // KingRingWeakWeight
    S(21, 0), // KingNoQueenDiscount
    S(-1, 0), // IsolatedPawnPenalty
    S(-1, -14), // DoubledPawnPenalty
    S(-13, -6), // BackwardPawnPenalty
    S(-23, -15), // WeakUnopposedPenalty
    S(-6, -31), // DoubledIsolatedPenalty
    {S(-68, -27), S(-2, -45)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-13, -14), // PawnIslandPenalty
    {S(17, 0), S(0, 0)}, // CentralPawnBonus
    S(47, 20), // BishopLongDiagonalBonus
    S(-13, 0), // BishopXrayPawns
    S(0, 47), // InitiativePasser
    S(0, 51), // InitiativePawnCount
    S(0, 2), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 33), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(70, 6), // SliderOnQueenBishop
    S(47, 0), // SliderOnQueenRook
    S(5, 2), // RestrictedPiece
    S(38, 0), // ThreatByPawnPush
    S(-33, -15), // WeakQueenDefender
    S(83, 0), // KnightOnQueen
    S(-272, -106), // PawnlessFlank
    S(1, 10), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 78), // KBNKCornerEg
    S(0, 515), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
