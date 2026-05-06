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
    S(170, 58), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(141, 25), S(161, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(199, 8), S(0, 0)},
    S(70, 6), // ThreatByKing
    S(88, 32), // Hanging
    S(160, 19), // WeakQueen
    S(19, 25), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 26), S(0, 34), S(0, 34), S(0, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 22), S(0, 41), S(0, 74), S(0, 85), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-29, -42), S(-29, -55), S(-29, -82), S(-281, -145), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(21, 20), S(21, 62), S(85, 123), S(193, 352), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-66, 39), S(29, 42), S(29, 44), S(29, 44), S(0, 0)},
    S(0, 49), // RookOn7thBonus
    S(-4, 0), // BadBishop
    S(-5, -11), // BishopPawns
    S(39, 0), // Tempo
    {S(0, 0), S(147, 215), S(804, 609), S(830, 531), S(1217, 844), S(2398, 1808), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-11, -21), S(-7, -13), S(-1, -18), S(30, -8), S(20, -10), S(6, -5), S(-11, -18), S(-12, -27),
        S(-48, -27), S(-30, -28), S(-12, -31), S(7, -30), S(31, -16), S(-16, -17), S(-26, -27), S(-51, -28),
        S(-15, -9), S(-23, -3), S(-11, -26), S(10, -28), S(11, -28), S(0, -22), S(-13, -13), S(-17, -13),
        S(3, 35), S(-11, 24), S(-3, 1), S(22, -24), S(21, -22), S(5, -1), S(-14, 20), S(2, 30),
        S(-7, 36), S(-1, 32), S(22, 14), S(33, 3), S(38, 8), S(22, 13), S(0, 30), S(-14, 34),
        S(-7, 23), S(-2, 24), S(20, 27), S(34, 21), S(35, 23), S(20, 26), S(-2, 25), S(-8, 25),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-107, -14), S(-89, -28), S(-71, -17), S(-57, -4),
        S(-83, -3), S(-44, -7), S(-48, -15), S(-29, -16),
        S(-75, -8), S(-15, -7), S(-10, -10), S(10, 7),
        S(5, 10), S(22, 3), S(43, 27), S(20, 36),
        S(36, 14), S(58, 12), S(69, 41), S(51, 34),
        S(23, -10), S(56, 13), S(73, 29), S(73, 42),
        S(-13, -37), S(11, -14), S(34, -1), S(55, 30),
        S(-57, -67), S(-10, -26), S(23, -2), S(43, 14)
    },
    // BishopPST (half-board)
    {
        S(-13, -26), S(-6, -2), S(-71, 2), S(-31, -10),
        S(-2, -36), S(11, -35), S(12, -12), S(-39, 14),
        S(6, -25), S(37, -25), S(-8, -15), S(-7, 22),
        S(33, -17), S(36, -7), S(23, 29), S(19, 9),
        S(13, -17), S(56, 5), S(50, 12), S(28, 12),
        S(25, 10), S(13, 32), S(7, 26), S(18, 20),
        S(-26, -11), S(-36, 4), S(-20, 21), S(-11, 15),
        S(-32, -2), S(-35, 5), S(-37, 7), S(-34, 14)
    },
    // RookPST (half-board)
    {
        S(-49, -14), S(-26, -19), S(-8, -16), S(14, -38),
        S(-65, -32), S(-37, -30), S(-29, -29), S(-7, -43),
        S(-72, -18), S(-33, -14), S(-21, -22), S(-24, -24),
        S(-39, 19), S(-11, 18), S(-11, 20), S(9, -3),
        S(-10, 5), S(32, 28), S(43, 17), S(62, 1),
        S(-6, 18), S(27, 26), S(49, 34), S(54, 18),
        S(4, -7), S(-5, 9), S(23, 9), S(52, 9),
        S(14, 3), S(13, 17), S(33, 17), S(48, 12)
    },
    // QueenPST (half-board)
    {
        S(-2, -87), S(20, -80), S(40, -94), S(42, -59),
        S(25, -87), S(23, -72), S(32, -47), S(41, -34),
        S(24, -60), S(44, -31), S(46, -11), S(18, -3),
        S(40, -25), S(57, 6), S(31, 33), S(33, 60),
        S(19, -7), S(30, 22), S(17, 40), S(-7, 72),
        S(-11, 20), S(-6, 43), S(-16, 59), S(0, 79),
        S(-76, 20), S(-102, 31), S(-64, 46), S(-40, 56),
        S(-98, 14), S(-82, 25), S(-55, 36), S(-29, 48)
    },
    // KingPST (half-board)
    {
        S(141, -153), S(119, -105), S(39, -65), S(-65, -66),
        S(114, -93), S(86, -56), S(9, -33), S(-10, -22),
        S(37, -61), S(15, -38), S(-5, -16), S(-29, -4),
        S(3, -27), S(-3, 3), S(-17, 16), S(-37, 21),
        S(-11, 10), S(-13, 38), S(-31, 41), S(-48, 34),
        S(-16, 34), S(-12, 56), S(-29, 53), S(-49, 38),
        S(-21, 36), S(-15, 56), S(-25, 59), S(-31, 50),
        S(-24, 37), S(-19, 49), S(-23, 56), S(-22, 61)
    },
    {
        {},
        {},
        {S(-90, -121), S(-50, -81), S(-25, -41), S(-9, -16), S(6, 6), S(13, 26), S(28, 26), S(40, 26), S(40, 26)},
        {S(13, 48), S(48, 73), S(69, 107), S(78, 126), S(94, 142), S(96, 155), S(109, 155), S(113, 155), S(119, 155), S(119, 155), S(124, 155), S(124, 155), S(140, 155), S(140, 155)},
        {S(-42, 269), S(-12, 276), S(7, 287), S(20, 287), S(20, 297), S(32, 313), S(36, 318), S(47, 322), S(48, 341), S(52, 346), S(54, 353), S(54, 353), S(54, 353), S(54, 353), S(54, 353)},
        {S(73, 257), S(73, 278), S(79, 303), S(94, 324), S(98, 349), S(111, 365), S(111, 373), S(123, 379), S(130, 379), S(142, 390), S(150, 390), S(163, 390), S(164, 393), S(164, 398), S(164, 398), S(164, 403), S(164, 403), S(178, 403), S(186, 403), S(188, 403), S(196, 403), S(201, 403), S(222, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403)},
        {},
    },
    {S(0, 0), S(-24, 14), S(-24, 28), S(-12, 57), S(34, 159), S(156, 388), S(529, 607), S(0, 0)},
    {S(0, 0), S(-5, -8), S(25, 16), S(33, 16), S(73, 26), S(110, 86), S(203, 112), S(0, 0)},
    S(96, 13), // RookOpenFileBonus
    S(51, 13), // RookSemiOpenFileBonus
    S(2, 0), // RookOnQueenFile
    S(65, 11), // KnightOutpostBonus
    S(77, 23), // BishopOutpostBonus
    S(-50, 0), // TrappedRookByKingPenalty
    S(36, 34), // RookBehindOurPasserBonus
    S(-85, 118), // RookBehindTheirPasserBonus
    S(19, 8), // MinorBehindPawnBonus
    S(24, 0), // MinorOnKingRing
    S(1, 0), // RookOnKingRing
    S(-17, -1), // KingProtector
    S(15, 26), // BishopPair
    {
        {S(-17, 0), S(115, 0), S(111, 0), S(67, 0), S(74, 0), S(97, 0), S(4, 0)},
        {S(-73, 0), S(184, 0), S(93, 0), S(2, 0), S(0, 0), S(134, 0), S(0, 0)},
        {S(0, 0), S(138, 0), S(68, 0), S(55, 0), S(15, 0), S(121, 0), S(246, 0)},
        {S(0, 0), S(144, 0), S(32, 0), S(54, 0), S(69, 0), S(16, 0), S(121, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(25, 0), S(67, 0), S(27, 0), S(28, 0), S(9, 0)},
        {S(0, 0), S(0, 0), S(88, 0), S(29, 0), S(18, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(194, 0), S(80, 0), S(18, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(50, 0), S(46, 0), S(5, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(157, 0), S(36, 0), S(18, 0), S(10, 0), S(0, 0)}, // BlockedStorm
    S(-41, 0), // UndefendedKingZoneSq
    S(11, 24), // KingMobilityFactor
    S(30, 12), // KingAttackByKnight
    S(8, 29), // KingAttackByBishop
    S(30, 23), // KingAttackByRook
    S(30, 73), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(35, 531), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(12, 0), // KingRingWeakWeight
    S(18, 13), // KingNoQueenDiscount
    S(0, -1), // IsolatedPawnPenalty
    S(0, -19), // DoubledPawnPenalty
    S(-8, -2), // BackwardPawnPenalty
    S(-22, -31), // WeakUnopposedPenalty
    S(-18, -41), // DoubledIsolatedPenalty
    {S(-21, -39), S(-4, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-28, 0), // PawnIslandPenalty
    {S(7, 0), S(0, 0)}, // CentralPawnBonus
    S(34, 18), // BishopLongDiagonalBonus
    S(-14, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 46), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(51, 20), // SliderOnQueenBishop
    S(34, 0), // SliderOnQueenRook
    S(5, 3), // RestrictedPiece
    S(24, 0), // ThreatByPawnPush
    S(-15, -35), // WeakQueenDefender
    S(65, 0), // KnightOnQueen
    S(-262, -102), // PawnlessFlank
    S(0, 41), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 200), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
