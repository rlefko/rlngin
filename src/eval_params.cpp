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
    S(230, 38), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(191, 15), S(170, 30), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(228, 8), S(0, 0)},
    S(151, 0), // ThreatByKing
    S(76, 28), // Hanging
    S(26, 4), // WeakQueen
    S(61, 16), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 30), S(0, 48), S(0, 48), S(0, 50), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 26), S(0, 48), S(0, 80), S(0, 91), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-1, -46), S(-29, -66), S(-96, -66), S(-304, -109), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(7, 23), S(57, 62), S(230, 118), S(230, 405), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(18, 28), S(129, 28), S(180, 28), S(188, 28), S(0, 0)},
    S(0, 27), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-2, -9), // BishopPawns
    S(42, 0), // Tempo
    {S(0, 0), S(245, 260), S(1008, 648), S(1059, 587), S(1610, 934), S(2722, 2140), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-48, -70), S(-41, -57), S(-64, -60), S(-30, -69), S(-21, -65), S(-66, -56), S(-30, -68), S(-60, -78),
        S(-100, -66), S(-94, -63), S(-77, -81), S(-79, -86), S(-32, -65), S(-112, -65), S(-86, -81), S(-101, -73),
        S(-70, -51), S(-104, -50), S(-99, -83), S(-54, -90), S(-45, -90), S(-70, -77), S(-78, -67), S(-72, -72),
        S(-32, -8), S(9, -52), S(-11, -69), S(41, -98), S(47, -84), S(-9, -91), S(-23, -52), S(-33, -26),
        S(107, 68), S(48, 76), S(91, 14), S(71, 7), S(151, 21), S(214, 9), S(87, 62), S(-24, 81),
        S(234, 175), S(34, 236), S(199, 222), S(131, 220), S(235, 197), S(234, 217), S(-139, 300), S(-26, 218),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-298, 22), S(-90, -10), S(-100, -8), S(-50, -15),
        S(-62, -22), S(-75, -1), S(-27, -8), S(8, -4),
        S(-46, -8), S(20, -2), S(29, -16), S(66, 12),
        S(26, 1), S(94, 16), S(87, 29), S(99, 26),
        S(95, 0), S(92, 23), S(165, 22), S(125, 47),
        S(-42, -4), S(130, -2), S(106, 12), S(180, 24),
        S(-26, -64), S(-47, 10), S(126, -32), S(38, 38),
        S(-300, -80), S(-132, -34), S(-243, 20), S(42, 6)
    },
    // BishopPST (half-board)
    {
        S(-57, -16), S(67, -24), S(-62, -4), S(-12, 1),
        S(14, -30), S(42, -26), S(42, -11), S(-8, 10),
        S(-1, -6), S(52, -3), S(13, 0), S(6, 19),
        S(-16, -27), S(37, 11), S(47, 18), S(65, 6),
        S(-14, -8), S(80, 12), S(93, 13), S(49, 19),
        S(69, -11), S(67, -11), S(73, 5), S(123, -8),
        S(-67, -35), S(-134, 8), S(15, 2), S(-23, 14),
        S(-145, 2), S(-71, -3), S(-125, 26), S(-191, 42)
    },
    // RookPST (half-board)
    {
        S(-31, -29), S(-40, -10), S(-4, -16), S(5, -24),
        S(-104, -29), S(-40, -28), S(15, -33), S(-24, -38),
        S(-87, -25), S(-76, -20), S(-32, -25), S(-76, -19),
        S(-105, 9), S(-36, 15), S(-38, 10), S(8, -8),
        S(-38, 23), S(25, 24), S(42, 21), S(20, 3),
        S(6, 28), S(66, 18), S(86, 16), S(84, 14),
        S(1, 18), S(-8, 19), S(131, 5), S(98, 2),
        S(14, 21), S(-33, 29), S(60, 15), S(108, 24)
    },
    // QueenPST (half-board)
    {
        S(-94, -7), S(-14, -77), S(-20, -59), S(12, -39),
        S(-30, -39), S(40, -95), S(28, -46), S(35, -38),
        S(-101, -1), S(19, -26), S(31, -3), S(3, -18),
        S(-24, -29), S(18, 23), S(17, 19), S(11, 38),
        S(52, -50), S(8, 32), S(43, 34), S(-3, 39),
        S(56, -14), S(30, 0), S(-7, 65), S(-31, 104),
        S(-58, -12), S(-172, 67), S(-57, 72), S(-62, 97),
        S(30, 3), S(42, -13), S(63, 3), S(122, -7)
    },
    // KingPST (half-board)
    {
        S(237, -138), S(225, -81), S(47, -47), S(-31, -55),
        S(265, -78), S(203, -50), S(45, -31), S(15, -16),
        S(-15, -47), S(69, -23), S(60, -7), S(-57, 10),
        S(-65, -6), S(73, -4), S(-83, 37), S(-135, 30),
        S(-59, 22), S(43, 42), S(-132, 54), S(-284, 68),
        S(49, 38), S(-81, 56), S(-193, 100), S(-293, 94),
        S(165, -48), S(75, 91), S(89, 66), S(-73, 82),
        S(-277, -246), S(299, 33), S(-60, 74), S(-117, -6)
    },
    {
        {},
        {},
        {S(-43, -204), S(-36, -34), S(-3, -6), S(15, 12), S(27, 22), S(41, 39), S(56, 43), S(56, 43), S(56, 43)},
        {S(-19, 80), S(48, 104), S(59, 131), S(83, 136), S(101, 145), S(108, 154), S(122, 159), S(122, 159), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(128, 165), S(128, 165)},
        {S(-34, 259), S(8, 289), S(20, 290), S(20, 300), S(20, 303), S(22, 318), S(22, 327), S(26, 327), S(34, 337), S(34, 348), S(43, 349), S(43, 357), S(43, 357), S(43, 357), S(43, 357)},
        {S(69, 35), S(101, 249), S(120, 258), S(120, 318), S(122, 338), S(135, 348), S(144, 348), S(145, 373), S(154, 373), S(163, 373), S(168, 373), S(174, 375), S(174, 379), S(174, 389), S(174, 394), S(181, 402), S(182, 402), S(187, 403), S(212, 403), S(212, 403), S(218, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 404), S(230, 404)},
        {},
    },
    {S(0, 0), S(-24, 40), S(-22, 43), S(-22, 84), S(0, 179), S(43, 354), S(456, 380), S(0, 0)},
    {S(0, 0), S(-4, -9), S(49, 5), S(52, 13), S(54, 30), S(123, 78), S(132, 139), S(0, 0)},
    S(132, 17), // RookOpenFileBonus
    S(62, 17), // RookSemiOpenFileBonus
    S(7, 2), // RookOnQueenFile
    S(82, 13), // KnightOutpostBonus
    S(104, 26), // BishopOutpostBonus
    S(-72, 0), // TrappedRookByKingPenalty
    S(46, 19), // RookBehindOurPasserBonus
    S(-81, 99), // RookBehindTheirPasserBonus
    S(38, 5), // MinorBehindPawnBonus
    S(15, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-14, -4), // KingProtector
    S(13, 20), // BishopPair
    {
        {S(-26, 0), S(72, 0), S(70, 0), S(46, 0), S(54, 0), S(24, 0), S(64, 0)},
        {S(-99, 0), S(172, 0), S(92, 0), S(0, 0), S(0, 0), S(64, 0), S(32, 0)},
        {S(-59, 0), S(172, 0), S(73, 0), S(0, 0), S(64, 0), S(64, 0), S(48, 0)},
        {S(-72, 0), S(134, 0), S(56, 0), S(13, 0), S(28, 0), S(48, 0), S(16, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(78, 0), S(0, 0), S(0, 0), S(0, 0), S(7, 0)},
        {S(0, 0), S(0, 0), S(78, 0), S(23, 0), S(12, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(182, 0), S(0, 0), S(2, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(118, 0), S(80, 0), S(0, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(55, 0), S(15, 0), S(4, 0), S(0, 0), S(0, 0)}, // BlockedStorm
    S(-47, 0), // UndefendedKingZoneSq
    S(5, 18), // KingMobilityFactor
    S(24, 15), // KingAttackByKnight
    S(9, 29), // KingAttackByBishop
    S(24, 17), // KingAttackByRook
    S(24, 18), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 8), S(33, 72), S(29, 8), S(29, 8), S(0, 0)}, // KingSafeCheck
    S(22, 0), // KingRingWeakWeight
    S(32, 3), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -14), // DoubledPawnPenalty
    S(-3, -9), // BackwardPawnPenalty
    S(-36, -22), // WeakUnopposedPenalty
    S(-11, -53), // DoubledIsolatedPenalty
    {S(-86, -28), S(-126, -85)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-13, -17), // PawnIslandPenalty
    {S(39, 0), S(23, 0)}, // CentralPawnBonus
    S(52, 13), // BishopLongDiagonalBonus
    S(-12, 0), // BishopXrayPawns
    S(0, 51), // InitiativePasser
    S(0, 56), // InitiativePawnCount
    S(0, 1), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 27), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(95, 0), // SliderOnQueenBishop
    S(39, 1), // SliderOnQueenRook
    S(12, 1), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
