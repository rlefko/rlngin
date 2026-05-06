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
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(142, 24), S(160, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(200, 8), S(0, 0)},
    S(69, 6), // ThreatByKing
    S(88, 32), // Hanging
    S(160, 10), // WeakQueen
    S(19, 25), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 26), S(0, 34), S(0, 34), S(0, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 23), S(0, 41), S(0, 74), S(0, 86), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-29, -42), S(-29, -55), S(-29, -82), S(-285, -143), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(22, 19), S(22, 62), S(88, 121), S(189, 352), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-66, 39), S(30, 42), S(30, 47), S(30, 47), S(0, 0)},
    S(0, 48), // RookOn7thBonus
    S(-6, -4), // BadBishop
    S(-4, -11), // BishopPawns
    S(39, 0), // Tempo
    {S(0, 0), S(146, 214), S(803, 605), S(829, 531), S(1216, 835), S(2406, 1750), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-11, -20), S(-8, -12), S(-2, -17), S(28, -6), S(18, -8), S(6, -4), S(-12, -17), S(-14, -25),
        S(-48, -26), S(-31, -27), S(-12, -31), S(6, -29), S(30, -15), S(-16, -17), S(-27, -27), S(-52, -27),
        S(-15, -9), S(-24, -2), S(-12, -25), S(10, -27), S(11, -27), S(-1, -21), S(-14, -12), S(-17, -12),
        S(2, 36), S(-13, 25), S(-4, 2), S(20, -23), S(19, -21), S(4, 0), S(-16, 21), S(0, 31),
        S(-7, 36), S(-1, 32), S(23, 15), S(33, 3), S(38, 8), S(23, 14), S(0, 30), S(-14, 34),
        S(-7, 22), S(-1, 24), S(22, 27), S(36, 20), S(37, 22), S(21, 26), S(-1, 25), S(-8, 24),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-109, -14), S(-90, -28), S(-72, -17), S(-57, -4),
        S(-84, -3), S(-45, -7), S(-49, -15), S(-30, -16),
        S(-75, -8), S(-15, -7), S(-10, -10), S(9, 8),
        S(6, 10), S(22, 3), S(43, 28), S(21, 36),
        S(37, 14), S(58, 12), S(70, 41), S(53, 34),
        S(23, -10), S(56, 13), S(73, 29), S(73, 42),
        S(-13, -37), S(11, -15), S(34, -1), S(55, 30),
        S(-58, -67), S(-10, -26), S(23, -1), S(43, 15)
    },
    // BishopPST (half-board)
    {
        S(-12, -26), S(-7, -2), S(-72, 1), S(-32, -11),
        S(-3, -36), S(10, -36), S(11, -13), S(-40, 13),
        S(5, -25), S(36, -26), S(-9, -15), S(-8, 22),
        S(33, -18), S(35, -7), S(22, 29), S(18, 10),
        S(14, -18), S(56, 5), S(50, 12), S(29, 11),
        S(26, 9), S(13, 31), S(7, 26), S(18, 19),
        S(-25, -12), S(-36, 3), S(-21, 21), S(-11, 15),
        S(-29, -4), S(-33, 4), S(-36, 6), S(-34, 13)
    },
    // RookPST (half-board)
    {
        S(-50, -13), S(-27, -18), S(-9, -15), S(13, -36),
        S(-66, -31), S(-37, -30), S(-29, -28), S(-8, -41),
        S(-72, -17), S(-33, -14), S(-21, -21), S(-25, -23),
        S(-39, 19), S(-12, 18), S(-11, 21), S(9, -2),
        S(-11, 6), S(31, 28), S(44, 17), S(62, 1),
        S(-6, 19), S(26, 27), S(48, 34), S(53, 19),
        S(4, -6), S(-4, 10), S(23, 10), S(50, 10),
        S(10, 4), S(13, 19), S(34, 16), S(45, 12)
    },
    // QueenPST (half-board)
    {
        S(-2, -88), S(20, -81), S(40, -94), S(42, -59),
        S(25, -88), S(23, -72), S(32, -48), S(41, -34),
        S(24, -60), S(44, -32), S(46, -12), S(18, -4),
        S(40, -26), S(56, 6), S(32, 31), S(34, 58),
        S(19, -10), S(29, 20), S(16, 38), S(-7, 71),
        S(-11, 18), S(-6, 42), S(-17, 59), S(-1, 80),
        S(-78, 22), S(-104, 32), S(-66, 47), S(-43, 58),
        S(-100, 17), S(-84, 27), S(-58, 37), S(-34, 50)
    },
    // KingPST (half-board)
    {
        S(141, -154), S(119, -106), S(38, -65), S(-65, -67),
        S(113, -93), S(85, -57), S(8, -33), S(-11, -22),
        S(37, -61), S(14, -39), S(-7, -17), S(-31, -5),
        S(3, -27), S(-2, 3), S(-18, 15), S(-37, 20),
        S(-10, 9), S(-13, 37), S(-30, 40), S(-47, 33),
        S(-14, 33), S(-10, 55), S(-27, 52), S(-46, 36),
        S(-20, 35), S(-14, 54), S(-23, 57), S(-28, 48),
        S(-23, 36), S(-19, 47), S(-21, 54), S(-21, 59)
    },
    {
        {},
        {},
        {S(-90, -121), S(-50, -81), S(-24, -41), S(-8, -16), S(7, 6), S(14, 26), S(29, 26), S(40, 26), S(40, 26)},
        {S(12, 48), S(47, 73), S(68, 107), S(77, 126), S(92, 143), S(95, 155), S(108, 155), S(112, 155), S(118, 155), S(118, 155), S(124, 155), S(124, 155), S(137, 155), S(137, 155)},
        {S(-41, 269), S(-11, 276), S(8, 287), S(20, 288), S(20, 297), S(32, 313), S(37, 317), S(47, 322), S(48, 341), S(54, 345), S(54, 353), S(54, 353), S(54, 353), S(54, 353), S(54, 353)},
        {S(73, 256), S(73, 278), S(80, 303), S(94, 325), S(98, 350), S(112, 365), S(112, 373), S(123, 380), S(130, 380), S(142, 390), S(150, 390), S(164, 390), S(164, 395), S(164, 399), S(164, 399), S(164, 403), S(164, 403), S(177, 403), S(183, 403), S(191, 403), S(196, 403), S(202, 403), S(227, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(229, 403)},
        {},
    },
    {S(0, 0), S(-24, 13), S(-24, 28), S(-13, 61), S(34, 158), S(155, 388), S(531, 609), S(0, 0)},
    {S(0, 0), S(-4, -9), S(26, 15), S(34, 15), S(74, 25), S(111, 84), S(195, 108), S(0, 0)},
    S(95, 13), // RookOpenFileBonus
    S(51, 13), // RookSemiOpenFileBonus
    S(2, 0), // RookOnQueenFile
    S(65, 10), // KnightOutpostBonus
    S(77, 23), // BishopOutpostBonus
    S(-50, 0), // TrappedRookByKingPenalty
    S(36, 33), // RookBehindOurPasserBonus
    S(-85, 118), // RookBehindTheirPasserBonus
    S(20, 8), // MinorBehindPawnBonus
    S(24, 0), // MinorOnKingRing
    S(1, 0), // RookOnKingRing
    S(-17, -1), // KingProtector
    S(14, 26), // BishopPair
    {
        {S(-18, 0), S(115, 0), S(111, 0), S(67, 0), S(75, 0), S(100, 0), S(5, 0)},
        {S(-72, 0), S(184, 0), S(94, 0), S(2, 0), S(0, 0), S(133, 0), S(0, 0)},
        {S(0, 0), S(138, 0), S(68, 0), S(56, 0), S(15, 0), S(124, 0), S(251, 0)},
        {S(0, 0), S(144, 0), S(32, 0), S(56, 0), S(69, 0), S(16, 0), S(118, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(22, 0), S(66, 0), S(26, 0), S(28, 0), S(9, 0)},
        {S(0, 0), S(0, 0), S(89, 0), S(31, 0), S(18, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(189, 0), S(81, 0), S(18, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(53, 0), S(45, 0), S(5, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(158, 0), S(35, 0), S(19, 0), S(10, 0), S(0, 0)}, // BlockedStorm
    S(-41, 0), // UndefendedKingZoneSq
    S(11, 23), // KingMobilityFactor
    S(30, 11), // KingAttackByKnight
    S(8, 29), // KingAttackByBishop
    S(30, 24), // KingAttackByRook
    S(30, 77), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(35, 531), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(12, 0), // KingRingWeakWeight
    S(19, 13), // KingNoQueenDiscount
    S(0, -1), // IsolatedPawnPenalty
    S(0, -19), // DoubledPawnPenalty
    S(-8, -2), // BackwardPawnPenalty
    S(-22, -31), // WeakUnopposedPenalty
    S(-18, -41), // DoubledIsolatedPenalty
    {S(-20, -39), S(-5, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-27, 0), // PawnIslandPenalty
    {S(5, 0), S(0, 0)}, // CentralPawnBonus
    S(35, 17), // BishopLongDiagonalBonus
    S(-14, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 47), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(51, 19), // SliderOnQueenBishop
    S(34, 0), // SliderOnQueenRook
    S(5, 3), // RestrictedPiece
    S(24, 0), // ThreatByPawnPush
    S(-16, -33), // WeakQueenDefender
    S(64, 0), // KnightOnQueen
    S(-262, -101), // PawnlessFlank
    S(0, 43), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 208), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
