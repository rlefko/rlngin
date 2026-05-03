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
    S(257, 21), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(157, 0), S(161, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(202, 0), S(0, 0)},
    S(139, 38), // ThreatByKing
    S(57, 2), // Hanging
    S(0, 0), // WeakQueen
    S(24, 25), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 41), S(0, 43), S(0, 43), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 36), S(0, 65), S(0, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-28, -29), S(-28, -60), S(-28, -86), S(-321, -86), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(57, 27), S(57, 57), S(162, 114), S(162, 308), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-72, 9), S(57, 24), S(77, 24), S(77, 24), S(0, 0)},
    S(0, 31), // RookOn7thBonus
    S(-19, 0), // BadBishop
    S(-3, -8), // BishopPawns
    S(152, 0), // Tempo
    {S(0, 0), S(245, 239), S(855, 570), S(915, 521), S(1369, 784), S(2602, 1691), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-52, -55), S(-47, -53), S(-58, -61), S(-55, -56), S(-50, -49), S(-53, -53), S(-45, -56), S(-60, -69),
        S(-76, -57), S(-79, -64), S(-57, -70), S(-54, -73), S(-40, -61), S(-59, -61), S(-75, -73), S(-75, -66),
        S(-46, -39), S(-50, -50), S(-36, -70), S(-13, -72), S(-9, -74), S(-31, -69), S(-47, -50), S(-50, -51),
        S(14, 18), S(0, 0), S(23, -24), S(60, -38), S(66, -37), S(25, -27), S(-9, -2), S(8, 9),
        S(37, 85), S(13, 90), S(60, 51), S(126, 39), S(127, 40), S(59, 51), S(10, 88), S(30, 83),
        S(92, 133), S(-49, 152), S(61, 121), S(191, 89), S(176, 86), S(59, 120), S(-73, 158), S(72, 127),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-93, -44), S(-108, -31), S(-57, -26), S(-53, 5),
        S(-68, -35), S(-50, -25), S(-27, -14), S(-14, -3),
        S(-24, -18), S(16, -7), S(34, -11), S(50, 24),
        S(0, 1), S(45, 10), S(65, 27), S(76, 37),
        S(42, 12), S(85, 22), S(92, 31), S(87, 40),
        S(-2, -7), S(34, 7), S(62, 25), S(68, 40),
        S(-67, -19), S(-21, -9), S(22, 6), S(36, 26),
        S(-176, -42), S(-72, -22), S(-15, -3), S(11, 12)
    },
    // BishopPST (half-board)
    {
        S(10, -44), S(4, -41), S(-74, -13), S(-45, -5),
        S(19, -47), S(24, -37), S(22, -14), S(-15, 1),
        S(15, -33), S(55, 7), S(9, -12), S(8, 19),
        S(23, -21), S(43, 0), S(42, 20), S(18, 3),
        S(10, -12), S(47, 35), S(39, 23), S(29, 7),
        S(26, 6), S(26, 23), S(15, 19), S(10, 15),
        S(-14, 3), S(-24, 9), S(-30, 19), S(-42, 8),
        S(-37, 18), S(-55, 22), S(-101, 22), S(-75, 21)
    },
    // RookPST (half-board)
    {
        S(-56, -3), S(-39, 7), S(-10, 13), S(-6, 5),
        S(-81, -48), S(-54, -38), S(-31, -13), S(-38, -13),
        S(-62, -39), S(-43, -24), S(-50, -27), S(-49, -21),
        S(-29, -6), S(-15, -7), S(-14, 9), S(-4, -2),
        S(5, 9), S(22, 17), S(34, 12), S(45, 2),
        S(38, 9), S(33, 15), S(50, 19), S(63, 14),
        S(44, 10), S(25, 8), S(36, 1), S(43, 18),
        S(42, 26), S(29, 26), S(26, 18), S(30, 13)
    },
    // QueenPST (half-board)
    {
        S(-12, -86), S(-1, -74), S(7, -64), S(56, -63),
        S(-9, -66), S(9, -56), S(41, -46), S(61, -42),
        S(-4, -45), S(37, -16), S(46, 9), S(11, 5),
        S(20, -12), S(52, 20), S(28, 38), S(3, 48),
        S(10, 14), S(9, 33), S(16, 75), S(-11, 85),
        S(-17, 27), S(-44, 31), S(-48, 69), S(-40, 103),
        S(-48, 11), S(-78, 20), S(-33, 29), S(-44, 61),
        S(-71, -47), S(-26, -34), S(106, -34), S(2, -21)
    },
    // KingPST (half-board)
    {
        S(180, -121), S(180, -72), S(18, -31), S(-138, -52),
        S(211, -62), S(148, -33), S(23, 1), S(-42, 7),
        S(87, -37), S(69, -10), S(3, 18), S(-48, 24),
        S(24, -11), S(6, 21), S(-43, 47), S(-108, 46),
        S(13, 12), S(-21, 45), S(-92, 64), S(-194, 74),
        S(38, 3), S(-14, 36), S(-104, 64), S(-193, 87),
        S(112, -43), S(28, -7), S(-2, 21), S(-126, 49),
        S(-194, -113), S(198, -40), S(198, -13), S(-187, 26)
    },
    {
        {},
        {},
        {S(-108, -93), S(-70, -53), S(-34, -13), S(-8, -5), S(14, 11), S(33, 31), S(57, 39), S(63, 39), S(65, 39)},
        {S(-21, 40), S(14, 75), S(49, 110), S(84, 129), S(110, 130), S(118, 148), S(128, 160), S(128, 160), S(128, 163), S(128, 163), S(128, 163), S(128, 163), S(128, 163), S(128, 163)},
        {S(-50, 243), S(-20, 273), S(10, 290), S(14, 306), S(14, 306), S(14, 323), S(14, 323), S(32, 333), S(39, 333), S(54, 345), S(54, 353), S(54, 356), S(54, 357), S(54, 357), S(54, 357)},
        {S(2, 291), S(21, 316), S(46, 341), S(69, 341), S(93, 341), S(115, 341), S(131, 359), S(148, 359), S(162, 362), S(167, 377), S(167, 380), S(172, 382), S(172, 388), S(173, 398), S(174, 401), S(176, 401), S(176, 401), S(182, 401), S(182, 401), S(198, 401), S(204, 401), S(204, 401), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(244, 403)},
        {},
    },
    {S(0, 0), S(-45, 39), S(-45, 39), S(-45, 80), S(4, 124), S(92, 278), S(546, 391), S(0, 0)},
    {S(0, 0), S(-2, -10), S(51, 9), S(51, 9), S(71, 12), S(88, 75), S(334, 109), S(0, 0)},
    S(131, 16), // RookOpenFileBonus
    S(45, 7), // RookSemiOpenFileBonus
    S(0, 0), // RookOnQueenFile
    S(96, 20), // KnightOutpostBonus
    S(111, 12), // BishopOutpostBonus
    S(-127, 0), // TrappedRookByKingPenalty
    S(68, 14), // RookBehindOurPasserBonus
    S(-120, 111), // RookBehindTheirPasserBonus
    S(24, 8), // MinorBehindPawnBonus
    S(40, 0), // MinorOnKingRing
    S(23, 0), // RookOnKingRing
    S(-22, -2), // KingProtector
    S(1, 19), // BishopPair
    {
        {S(0, 0), S(153, 0), S(153, 0), S(79, 0), S(82, 0), S(22, 0), S(0, 0)},
        {S(-96, 0), S(216, 0), S(114, 0), S(0, 0), S(0, 0), S(58, 0), S(235, 0)},
        {S(-2, 0), S(210, 0), S(45, 0), S(26, 0), S(8, 0), S(135, 0), S(112, 0)},
        {S(0, 0), S(84, 0), S(85, 0), S(71, 0), S(47, 0), S(88, 0), S(107, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(40, 0), S(105, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(83, 0), S(260, 0), S(83, 0), S(16, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(24, 0), S(250, 0), S(45, 0), S(4, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(198, 0), S(29, 0), S(0, 0), S(26, 0), S(0, 0)}, // BlockedStorm
    S(-36, 0), // UndefendedKingZoneSq
    S(9, 50), // KingMobilityFactor
    S(32, 33), // KingAttackByKnight
    S(15, 43), // KingAttackByBishop
    S(34, 33), // KingAttackByRook
    S(35, 33), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(26, 1), S(34, 306), S(26, 1), S(26, 1), S(0, 0)}, // KingSafeCheck
    S(21, 14), // KingRingWeakWeight
    S(27, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -16), // DoubledPawnPenalty
    S(-20, -5), // BackwardPawnPenalty
    S(-37, -17), // WeakUnopposedPenalty
    S(-5, -42), // DoubledIsolatedPenalty
    {S(-93, -44), S(-18, -102)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(0, -17), // PawnIslandPenalty
    {S(27, 0), S(0, 0)}, // CentralPawnBonus
    S(68, 20), // BishopLongDiagonalBonus
    S(-26, -3), // BishopXrayPawns
    S(0, 70), // InitiativePasser
    S(0, 41), // InitiativePawnCount
    S(0, 0), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 90), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(53, 0), // SliderOnQueenBishop
    S(46, 0), // SliderOnQueenRook
    S(5, 2), // RestrictedPiece
    S(41, 0), // ThreatByPawnPush
    S(-18, -9), // WeakQueenDefender
    S(115, 0), // KnightOnQueen
    S(-308, -138), // PawnlessFlank
    S(0, 3), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 10), // KBNKCornerEg
    S(0, 399), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
