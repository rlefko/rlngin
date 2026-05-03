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
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(205, 0), S(197, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(241, 32), S(0, 0)},
    S(107, 8), // ThreatByKing
    S(108, 26), // Hanging
    S(0, 0), // WeakQueen
    S(24, 27), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 23), S(0, 42), S(0, 42), S(0, 42), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 40), S(0, 71), S(0, 82), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-36, -27), S(-36, -52), S(-80, -67), S(-369, -119), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 16), S(53, 56), S(155, 119), S(155, 369), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-51, 22), S(58, 28), S(80, 28), S(80, 28), S(0, 0)},
    S(3, 27), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-2, -7), // BishopPawns
    S(72, 0), // Tempo
    {S(0, 0), S(188, 203), S(912, 539), S(942, 476), S(1429, 731), S(2586, 1659), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-41, -43), S(-37, -43), S(-35, -48), S(-20, -43), S(-12, -35), S(-32, -42), S(-37, -47), S(-47, -59),
        S(-75, -44), S(-75, -52), S(-38, -61), S(-30, -58), S(-14, -50), S(-44, -53), S(-65, -64), S(-73, -56),
        S(-48, -31), S(-50, -44), S(-29, -63), S(-2, -62), S(5, -64), S(-19, -60), S(-44, -46), S(-53, -43),
        S(6, 13), S(-6, -10), S(14, -34), S(46, -46), S(55, -46), S(18, -38), S(-16, -10), S(0, 2),
        S(20, 80), S(3, 84), S(51, 41), S(113, 30), S(112, 31), S(50, 40), S(1, 84), S(13, 82),
        S(77, 113), S(-56, 137), S(52, 104), S(173, 86), S(157, 80), S(49, 103), S(-82, 143), S(56, 113),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-102, -31), S(-114, -14), S(-64, -18), S(-64, 10),
        S(-79, -17), S(-58, -10), S(-27, -8), S(-24, -1),
        S(-31, -2), S(10, -3), S(24, -16), S(33, 17),
        S(-14, 8), S(42, 9), S(57, 21), S(63, 28),
        S(41, 16), S(77, 12), S(95, 20), S(76, 28),
        S(-4, -4), S(31, 4), S(76, 18), S(86, 32),
        S(-63, -27), S(-9, -2), S(36, -7), S(51, 17),
        S(-152, -73), S(-53, -27), S(5, -1), S(28, 6)
    },
    // BishopPST (half-board)
    {
        S(7, -29), S(10, -31), S(-69, -5), S(-37, -3),
        S(5, -32), S(22, -28), S(28, -14), S(-15, 1),
        S(12, -21), S(51, 6), S(9, -13), S(-1, 15),
        S(18, -16), S(45, -3), S(42, 13), S(31, -2),
        S(1, -12), S(49, 26), S(55, 14), S(45, -3),
        S(19, -4), S(17, 19), S(29, 13), S(25, 9),
        S(-31, -12), S(-48, 1), S(-23, 13), S(-28, -1),
        S(-39, 14), S(-60, 19), S(-81, 18), S(-58, 19)
    },
    // RookPST (half-board)
    {
        S(-50, -16), S(-38, -3), S(-9, -4), S(-3, -11),
        S(-70, -28), S(-43, -26), S(-19, -13), S(-33, -13),
        S(-67, -23), S(-42, -13), S(-51, -16), S(-52, -16),
        S(-43, -1), S(-31, 2), S(-30, 14), S(1, -2),
        S(-26, 16), S(4, 22), S(25, 13), S(30, 2),
        S(14, 10), S(19, 20), S(48, 15), S(65, 9),
        S(31, 1), S(28, 6), S(58, -3), S(72, 8),
        S(37, 10), S(38, 11), S(53, 7), S(72, 6)
    },
    // QueenPST (half-board)
    {
        S(-4, -83), S(13, -72), S(12, -62), S(38, -47),
        S(-5, -42), S(14, -46), S(38, -41), S(50, -30),
        S(-16, -37), S(30, -20), S(34, 8), S(8, -1),
        S(18, -16), S(44, 12), S(22, 27), S(4, 34),
        S(4, -1), S(3, 14), S(7, 65), S(0, 67),
        S(-11, 6), S(-33, 20), S(-26, 55), S(-11, 92),
        S(-79, 5), S(-103, 26), S(-40, 53), S(-31, 71),
        S(-66, -25), S(-26, -17), S(106, -13), S(29, -6)
    },
    // KingPST (half-board)
    {
        S(150, -93), S(166, -63), S(14, -31), S(-116, -45),
        S(194, -52), S(147, -29), S(9, -1), S(-70, 6),
        S(60, -30), S(58, -9), S(-10, 13), S(-65, 20),
        S(7, -9), S(-3, 17), S(-49, 39), S(-108, 38),
        S(8, 5), S(-20, 39), S(-82, 52), S(-169, 56),
        S(39, 0), S(-1, 36), S(-80, 61), S(-165, 69),
        S(113, -40), S(77, -1), S(15, 23), S(-105, 45),
        S(-169, -90), S(169, -26), S(175, 3), S(-160, 26)
    },
    {
        {},
        {},
        {S(-94, -95), S(-54, -55), S(-14, -15), S(10, -6), S(26, 10), S(39, 28), S(60, 32), S(60, 38), S(60, 38)},
        {S(-16, 51), S(19, 86), S(54, 119), S(83, 132), S(106, 133), S(111, 148), S(122, 157), S(127, 157), S(128, 159), S(128, 163), S(128, 163), S(128, 163), S(128, 163), S(128, 163)},
        {S(-37, 254), S(-7, 284), S(19, 293), S(20, 306), S(20, 306), S(20, 322), S(20, 323), S(35, 333), S(41, 334), S(53, 346), S(53, 352), S(53, 354), S(53, 357), S(53, 357), S(53, 357)},
        {S(11, 291), S(36, 316), S(61, 341), S(83, 345), S(94, 347), S(119, 349), S(135, 356), S(151, 356), S(156, 370), S(168, 373), S(168, 381), S(172, 381), S(172, 384), S(174, 398), S(174, 402), S(174, 402), S(180, 403), S(193, 403), S(193, 403), S(218, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 403)},
        {},
    },
    {S(0, 0), S(-37, 34), S(-37, 34), S(-37, 77), S(0, 138), S(110, 293), S(568, 430), S(0, 0)},
    {S(0, 0), S(-4, -10), S(47, 8), S(47, 9), S(65, 20), S(65, 87), S(338, 125), S(0, 0)},
    S(129, 9), // RookOpenFileBonus
    S(48, 6), // RookSemiOpenFileBonus
    S(7, 0), // RookOnQueenFile
    S(95, 8), // KnightOutpostBonus
    S(94, 11), // BishopOutpostBonus
    S(-100, 0), // TrappedRookByKingPenalty
    S(72, 10), // RookBehindOurPasserBonus
    S(-77, 103), // RookBehindTheirPasserBonus
    S(27, 3), // MinorBehindPawnBonus
    S(31, 0), // MinorOnKingRing
    S(15, 0), // RookOnKingRing
    S(-18, -3), // KingProtector
    S(15, 0), // BishopPair
    {
        {S(0, 0), S(142, 0), S(138, 0), S(87, 0), S(69, 0), S(24, 0), S(0, 0)},
        {S(-98, 0), S(218, 0), S(115, 0), S(6, 0), S(0, 0), S(43, 0), S(173, 0)},
        {S(-4, 0), S(194, 0), S(57, 0), S(29, 0), S(27, 0), S(103, 0), S(109, 0)},
        {S(0, 0), S(120, 0), S(89, 0), S(68, 0), S(30, 0), S(65, 0), S(49, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(5, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(105, 0), S(0, 0), S(5, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(107, 0), S(313, 0), S(60, 0), S(23, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(32, 0), S(288, 0), S(59, 0), S(6, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(170, 0), S(12, 0), S(0, 0), S(9, 0), S(0, 0)}, // BlockedStorm
    S(-44, 0), // UndefendedKingZoneSq
    S(9, 57), // KingMobilityFactor
    S(31, 32), // KingAttackByKnight
    S(12, 42), // KingAttackByBishop
    S(31, 32), // KingAttackByRook
    S(31, 32), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(38, 321), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(21, 23), // KingRingWeakWeight
    S(29, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -9), // DoubledPawnPenalty
    S(-14, -6), // BackwardPawnPenalty
    S(-25, -18), // WeakUnopposedPenalty
    S(-2, -40), // DoubledIsolatedPenalty
    {S(-84, -32), S(-13, -100)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-7, -16), // PawnIslandPenalty
    {S(21, 0), S(0, 0)}, // CentralPawnBonus
    S(63, 18), // BishopLongDiagonalBonus
    S(-21, 0), // BishopXrayPawns
    S(0, 41), // InitiativePasser
    S(0, 48), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(54, 8), // SliderOnQueenBishop
    S(47, 1), // SliderOnQueenRook
    S(6, 1), // RestrictedPiece
    S(34, 0), // ThreatByPawnPush
    S(-30, -16), // WeakQueenDefender
    S(94, 0), // KnightOnQueen
    S(-277, -127), // PawnlessFlank
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
