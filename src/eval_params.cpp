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
    S(255, 41), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(201, 0), S(194, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(239, 30), S(0, 0)},
    S(107, 8), // ThreatByKing
    S(108, 25), // Hanging
    S(0, 0), // WeakQueen
    S(24, 27), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 23), S(0, 42), S(0, 42), S(0, 42), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 40), S(0, 71), S(0, 81), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-36, -27), S(-36, -51), S(-92, -63), S(-377, -113), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 15), S(53, 53), S(139, 118), S(139, 361), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-59, 22), S(58, 28), S(64, 28), S(64, 28), S(0, 0)},
    S(0, 27), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-2, -7), // BishopPawns
    S(72, 0), // Tempo
    {S(0, 0), S(179, 191), S(904, 529), S(934, 465), S(1407, 715), S(2546, 1623), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-33, -35), S(-28, -35), S(-29, -40), S(-14, -34), S(-6, -27), S(-25, -34), S(-30, -39), S(-39, -51),
        S(-67, -36), S(-66, -44), S(-31, -53), S(-22, -50), S(-7, -42), S(-37, -45), S(-57, -56), S(-64, -48),
        S(-41, -23), S(-43, -36), S(-24, -54), S(3, -54), S(10, -56), S(-15, -52), S(-38, -38), S(-46, -35),
        S(7, 21), S(-4, -2), S(11, -25), S(41, -37), S(50, -37), S(16, -30), S(-14, -2), S(1, 10),
        S(10, 70), S(5, 74), S(39, 29), S(83, 24), S(84, 27), S(40, 26), S(3, 74), S(5, 72),
        S(39, 93), S(-30, 109), S(46, 80), S(135, 66), S(123, 64), S(45, 79), S(-40, 111), S(26, 89),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-104, -29), S(-117, -12), S(-69, -16), S(-68, 12),
        S(-85, -15), S(-62, -9), S(-31, -7), S(-26, -1),
        S(-35, 0), S(6, -2), S(21, -15), S(27, 17),
        S(-19, 9), S(39, 9), S(51, 22), S(60, 27),
        S(35, 17), S(69, 14), S(89, 21), S(70, 28),
        S(-2, -4), S(31, 4), S(72, 18), S(80, 31),
        S(-47, -31), S(-2, -4), S(38, -9), S(49, 17),
        S(-106, -79), S(-35, -29), S(11, -3), S(30, 4)
    },
    // BishopPST (half-board)
    {
        S(3, -28), S(6, -30), S(-73, -4), S(-42, -2),
        S(1, -31), S(17, -27), S(24, -13), S(-20, 2),
        S(5, -20), S(45, 7), S(6, -12), S(-6, 15),
        S(12, -15), S(38, -2), S(38, 14), S(25, 0),
        S(-3, -11), S(43, 27), S(51, 13), S(41, -2),
        S(15, -3), S(13, 20), S(26, 13), S(25, 8),
        S(-31, -12), S(-46, 0), S(-15, 10), S(-20, -4),
        S(-35, 15), S(-46, 20), S(-45, 15), S(-34, 16)
    },
    // RookPST (half-board)
    {
        S(-50, -16), S(-36, -3), S(-9, -4), S(-3, -11),
        S(-69, -27), S(-43, -26), S(-19, -14), S(-33, -13),
        S(-65, -23), S(-42, -13), S(-51, -16), S(-50, -16),
        S(-43, -1), S(-31, 3), S(-30, 14), S(2, -4),
        S(-26, 15), S(4, 22), S(24, 13), S(30, 2),
        S(12, 10), S(16, 19), S(46, 15), S(67, 9),
        S(30, 0), S(28, 4), S(57, -3), S(72, 6),
        S(37, 8), S(40, 11), S(49, 7), S(68, 6)
    },
    // QueenPST (half-board)
    {
        S(-4, -83), S(13, -70), S(14, -62), S(39, -47),
        S(-3, -42), S(16, -44), S(40, -41), S(51, -30),
        S(-14, -37), S(31, -20), S(34, 8), S(12, -2),
        S(18, -16), S(46, 12), S(24, 27), S(10, 30),
        S(4, -1), S(5, 14), S(10, 62), S(3, 65),
        S(-7, 2), S(-29, 16), S(-25, 55), S(-11, 91),
        S(-77, 3), S(-105, 26), S(-46, 54), S(-35, 71),
        S(-66, -26), S(-38, -17), S(66, -5), S(5, 0)
    },
    // KingPST (half-board)
    {
        S(141, -95), S(157, -66), S(8, -34), S(-122, -48),
        S(183, -55), S(139, -32), S(3, -4), S(-74, 3),
        S(50, -33), S(54, -13), S(-12, 8), S(-63, 15),
        S(1, -13), S(-3, 12), S(-47, 34), S(-94, 32),
        S(2, 2), S(-14, 35), S(-64, 45), S(-135, 48),
        S(25, 1), S(1, 33), S(-46, 57), S(-131, 63),
        S(67, -25), S(55, 4), S(11, 22), S(-81, 40),
        S(-135, -53), S(123, -13), S(129, 6), S(-126, 27)
    },
    {
        {},
        {},
        {S(-94, -95), S(-54, -55), S(-14, -15), S(10, -7), S(26, 10), S(39, 27), S(60, 32), S(60, 38), S(60, 38)},
        {S(-16, 51), S(19, 86), S(54, 120), S(83, 132), S(106, 133), S(111, 148), S(122, 157), S(128, 157), S(128, 159), S(128, 163), S(128, 163), S(128, 163), S(128, 163), S(128, 163)},
        {S(-37, 254), S(-7, 284), S(19, 293), S(20, 306), S(20, 306), S(20, 322), S(20, 323), S(35, 333), S(41, 334), S(53, 346), S(53, 351), S(53, 353), S(53, 357), S(53, 357), S(53, 357)},
        {S(11, 291), S(36, 316), S(61, 341), S(85, 343), S(94, 349), S(119, 349), S(135, 356), S(151, 356), S(156, 370), S(168, 373), S(168, 381), S(172, 381), S(172, 384), S(174, 398), S(174, 402), S(174, 402), S(178, 403), S(193, 403), S(193, 403), S(218, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 403)},
        {},
    },
    {S(0, 0), S(-37, 34), S(-37, 34), S(-37, 76), S(4, 136), S(134, 307), S(582, 450), S(0, 0)},
    {S(0, 0), S(-4, -10), S(45, 8), S(45, 9), S(65, 20), S(65, 86), S(338, 125), S(0, 0)},
    S(125, 9), // RookOpenFileBonus
    S(45, 6), // RookSemiOpenFileBonus
    S(7, 0), // RookOnQueenFile
    S(91, 8), // KnightOutpostBonus
    S(92, 11), // BishopOutpostBonus
    S(-98, 0), // TrappedRookByKingPenalty
    S(70, 10), // RookBehindOurPasserBonus
    S(-69, 100), // RookBehindTheirPasserBonus
    S(27, 3), // MinorBehindPawnBonus
    S(31, 0), // MinorOnKingRing
    S(15, 0), // RookOnKingRing
    S(-18, -3), // KingProtector
    S(14, 0), // BishopPair
    {
        {S(0, 0), S(142, 0), S(138, 0), S(87, 0), S(69, 0), S(20, 0), S(0, 0)},
        {S(-94, 0), S(218, 0), S(115, 0), S(8, 0), S(0, 0), S(27, 0), S(141, 0)},
        {S(-4, 0), S(192, 0), S(58, 0), S(33, 0), S(27, 0), S(103, 0), S(109, 0)},
        {S(0, 0), S(120, 0), S(87, 0), S(70, 0), S(30, 0), S(73, 0), S(49, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(5, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(105, 0), S(2, 0), S(7, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(123, 0), S(321, 0), S(64, 0), S(21, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(32, 0), S(288, 0), S(67, 0), S(4, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(166, 0), S(12, 0), S(0, 0), S(11, 0), S(0, 0)}, // BlockedStorm
    S(-42, 0), // UndefendedKingZoneSq
    S(9, 57), // KingMobilityFactor
    S(31, 32), // KingAttackByKnight
    S(12, 42), // KingAttackByBishop
    S(31, 32), // KingAttackByRook
    S(31, 32), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(37, 321), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(20, 23), // KingRingWeakWeight
    S(28, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -9), // DoubledPawnPenalty
    S(-14, -6), // BackwardPawnPenalty
    S(-25, -17), // WeakUnopposedPenalty
    S(-1, -38), // DoubledIsolatedPenalty
    {S(-79, -32), S(-1, -80)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-7, -16), // PawnIslandPenalty
    {S(21, 0), S(0, 0)}, // CentralPawnBonus
    S(62, 18), // BishopLongDiagonalBonus
    S(-21, 0), // BishopXrayPawns
    S(0, 37), // InitiativePasser
    S(0, 48), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 46), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(54, 6), // SliderOnQueenBishop
    S(47, 1), // SliderOnQueenRook
    S(6, 1), // RestrictedPiece
    S(34, 0), // ThreatByPawnPush
    S(-30, -15), // WeakQueenDefender
    S(94, 0), // KnightOnQueen
    S(-269, -127), // PawnlessFlank
    S(0, 5), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 14), // KBNKCornerEg
    S(0, 372), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
