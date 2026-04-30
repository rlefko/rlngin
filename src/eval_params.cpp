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
    S(229, 38), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(192, 14), S(183, 21), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(244, 26), S(0, 0)},
    S(85, 2), // ThreatByKing
    S(107, 28), // Hanging
    S(55, 0), // WeakQueen
    S(40, 21), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 28), S(0, 45), S(0, 48), S(0, 48), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 22), S(0, 45), S(0, 80), S(0, 92), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-28, -33), S(-28, -57), S(-77, -60), S(-353, -114), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(41, 20), S(49, 59), S(209, 112), S(224, 388), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-38, 25), S(97, 28), S(163, 28), S(163, 28), S(0, 0)},
    S(0, 33), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -8), // BishopPawns
    S(42, 0), // Tempo
    {S(0, 0), S(247, 247), S(993, 620), S(1045, 552), S(1590, 890), S(2775, 2019), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-55, -56), S(-57, -55), S(-60, -60), S(-30, -69), S(5, -55), S(-48, -50), S(-45, -56), S(-57, -74),
        S(-112, -53), S(-109, -59), S(-75, -74), S(-76, -70), S(-28, -61), S(-110, -59), S(-56, -77), S(-119, -70),
        S(-82, -44), S(-109, -51), S(-114, -78), S(-62, -76), S(-40, -84), S(-74, -73), S(-69, -61), S(-87, -63),
        S(-23, -9), S(-18, -50), S(-33, -62), S(10, -85), S(32, -80), S(-19, -72), S(-62, -34), S(-32, -27),
        S(116, 55), S(62, 72), S(101, 17), S(146, 7), S(187, 7), S(159, -7), S(116, 71), S(44, 87),
        S(194, 164), S(16, 222), S(215, 189), S(193, 183), S(230, 170), S(114, 220), S(-154, 275), S(90, 197),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-234, -42), S(-85, -4), S(-63, -20), S(-71, 5),
        S(-79, -21), S(-81, 3), S(-17, -13), S(-3, 0),
        S(-16, -2), S(33, 3), S(40, -9), S(51, 20),
        S(14, 7), S(101, 9), S(90, 36), S(96, 32),
        S(89, 20), S(108, 10), S(142, 26), S(108, 46),
        S(-35, -5), S(115, -3), S(107, 29), S(170, 30),
        S(-30, -57), S(-58, 23), S(111, -36), S(29, 42),
        S(-300, -142), S(-169, -29), S(-162, 15), S(13, 11)
    },
    // BishopPST (half-board)
    {
        S(-21, -26), S(55, -36), S(-61, -2), S(-31, -1),
        S(6, -33), S(23, -29), S(57, -13), S(-7, 6),
        S(19, -12), S(62, 3), S(11, -13), S(8, 21),
        S(17, -13), S(59, -2), S(59, 22), S(48, 3),
        S(-4, -13), S(61, 29), S(96, 12), S(47, 9),
        S(65, -19), S(43, 11), S(67, 4), S(99, -2),
        S(-39, -38), S(-138, 7), S(18, 7), S(-51, -1),
        S(-146, 18), S(-57, 17), S(-160, 22), S(-182, 44)
    },
    // RookPST (half-board)
    {
        S(-29, -29), S(-31, -12), S(-1, -17), S(7, -23),
        S(-71, -32), S(-42, -37), S(-5, -33), S(-29, -26),
        S(-83, -28), S(-65, -14), S(-55, -22), S(-72, -18),
        S(-59, -4), S(-62, 10), S(-51, 15), S(13, -11),
        S(-51, 26), S(14, 32), S(33, 19), S(24, 4),
        S(11, 18), S(41, 25), S(52, 24), S(76, 17),
        S(9, 9), S(-14, 18), S(119, -3), S(97, 10),
        S(27, 23), S(23, 29), S(53, 18), S(137, 4)
    },
    // QueenPST (half-board)
    {
        S(2, -88), S(2, -74), S(0, -75), S(18, -48),
        S(24, -73), S(23, -77), S(45, -51), S(48, -44),
        S(-66, -12), S(35, -31), S(31, -4), S(12, -20),
        S(12, -27), S(38, 14), S(34, 15), S(13, 20),
        S(23, -20), S(14, 17), S(23, 58), S(1, 67),
        S(34, -18), S(11, 10), S(-5, 66), S(-39, 122),
        S(-115, 15), S(-178, 81), S(-162, 127), S(-82, 112),
        S(16, -26), S(2, -11), S(94, -10), S(107, -15)
    },
    // KingPST (half-board)
    {
        S(213, -118), S(214, -78), S(50, -46), S(-52, -59),
        S(248, -69), S(193, -45), S(32, -17), S(-19, -11),
        S(-36, -21), S(76, -18), S(16, 3), S(-101, 17),
        S(-111, 6), S(-3, 17), S(-91, 48), S(-226, 47),
        S(29, 3), S(35, 48), S(-180, 72), S(-295, 80),
        S(135, 4), S(-82, 82), S(-110, 100), S(-296, 109),
        S(241, -73), S(109, 10), S(116, 77), S(-91, 76),
        S(-300, -222), S(300, 10), S(90, -67), S(-84, 59)
    },
    {
        {},
        {},
        {S(-53, -178), S(-34, -34), S(4, -6), S(18, 5), S(30, 20), S(41, 37), S(56, 39), S(56, 43), S(56, 43)},
        {S(-27, 87), S(37, 97), S(64, 127), S(85, 138), S(103, 144), S(109, 153), S(120, 160), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(128, 163), S(128, 163)},
        {S(-31, 255), S(4, 287), S(19, 293), S(20, 304), S(20, 308), S(20, 320), S(22, 322), S(28, 332), S(32, 334), S(43, 346), S(43, 348), S(43, 354), S(43, 357), S(43, 357), S(59, 357)},
        {S(47, 62), S(47, 270), S(108, 273), S(110, 327), S(117, 343), S(135, 349), S(143, 359), S(149, 368), S(159, 373), S(168, 373), S(168, 380), S(174, 380), S(174, 387), S(174, 398), S(176, 403), S(176, 403), S(176, 403), S(196, 403), S(196, 403), S(202, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 403), S(230, 403), S(246, 403)},
        {},
    },
    {S(0, 0), S(-36, 40), S(-36, 40), S(-22, 77), S(3, 171), S(3, 346), S(463, 403), S(0, 0)},
    {S(0, 0), S(-7, -8), S(50, 10), S(56, 13), S(74, 28), S(107, 92), S(269, 181), S(0, 0)},
    S(129, 17), // RookOpenFileBonus
    S(52, 17), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(79, 11), // KnightOutpostBonus
    S(89, 17), // BishopOutpostBonus
    S(-78, 0), // TrappedRookByKingPenalty
    S(51, 13), // RookBehindOurPasserBonus
    S(-109, 105), // RookBehindTheirPasserBonus
    S(34, 6), // MinorBehindPawnBonus
    S(24, 0), // MinorOnKingRing
    S(7, 0), // RookOnKingRing
    S(-18, -3), // KingProtector
    S(9, 14), // BishopPair
    {
        {S(-18, 0), S(80, 0), S(98, 0), S(63, 0), S(47, 0), S(60, 0), S(69, 0)},
        {S(-117, 0), S(181, 0), S(79, 0), S(0, 0), S(0, 0), S(66, 0), S(76, 0)},
        {S(-50, 0), S(176, 0), S(63, 0), S(6, 0), S(51, 0), S(94, 0), S(68, 0)},
        {S(-45, 0), S(141, 0), S(64, 0), S(31, 0), S(26, 0), S(58, 0), S(10, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(3, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(42, 0), S(8, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(211, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(133, 0), S(52, 0), S(0, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(109, 0), S(0, 0), S(0, 0), S(19, 0), S(0, 0)}, // BlockedStorm
    S(-43, 0), // UndefendedKingZoneSq
    S(6, 18), // KingMobilityFactor
    S(24, 17), // KingAttackByKnight
    S(10, 28), // KingAttackByBishop
    S(24, 17), // KingAttackByRook
    S(26, 23), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 0), S(33, 192), S(29, 8), S(30, 8), S(0, 0)}, // KingSafeCheck
    S(21, 0), // KingRingWeakWeight
    S(25, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-6, -16), // DoubledPawnPenalty
    S(-7, -5), // BackwardPawnPenalty
    S(-32, -21), // WeakUnopposedPenalty
    S(-11, -49), // DoubledIsolatedPenalty
    {S(-93, -23), S(-144, -94)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-18, -22), // PawnIslandPenalty
    {S(33, 0), S(33, 0)}, // CentralPawnBonus
    S(62, 23), // BishopLongDiagonalBonus
    S(-24, 0), // BishopXrayPawns
    S(0, 42), // InitiativePasser
    S(0, 52), // InitiativePawnCount
    S(0, 2), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 27), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(51, 8), // SliderOnQueenBishop
    S(40, 0), // SliderOnQueenRook
    S(8, 1), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
