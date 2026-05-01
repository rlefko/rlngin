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
    S(270, 52), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(193, 0), S(148, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(217, 39), S(0, 0)},
    S(131, 29), // ThreatByKing
    S(100, 19), // Hanging
    S(0, 17), // WeakQueen
    S(25, 32), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 43), S(0, 43), S(0, 43), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 39), S(0, 72), S(0, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-39, -40), S(-39, -58), S(-116, -62), S(-335, -133), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 21), S(53, 67), S(165, 143), S(165, 428), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-53, 15), S(46, 28), S(87, 28), S(87, 28), S(0, 0)},
    S(0, 22), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -7), // BishopPawns
    S(85, 0), // Tempo
    {S(0, 0), S(200, 223), S(917, 554), S(946, 485), S(1490, 765), S(2651, 1735), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-50, -57), S(-44, -58), S(-37, -60), S(-13, -61), S(-10, -54), S(-39, -54), S(-48, -57), S(-57, -69),
        S(-96, -53), S(-83, -62), S(-50, -68), S(-37, -66), S(-28, -57), S(-53, -62), S(-72, -70), S(-91, -64),
        S(-69, -41), S(-69, -54), S(-59, -72), S(-34, -74), S(-27, -72), S(-47, -69), S(-65, -53), S(-73, -52),
        S(-4, -5), S(-36, -27), S(-2, -47), S(30, -63), S(35, -59), S(2, -46), S(-49, -20), S(-6, -10),
        S(83, 74), S(52, 82), S(89, 38), S(169, 20), S(176, 23), S(79, 40), S(27, 87), S(68, 83),
        S(165, 146), S(-65, 207), S(63, 160), S(257, 106), S(233, 102), S(49, 169), S(-232, 223), S(80, 146),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-77, -40), S(-73, -7), S(-49, -13), S(-61, 20),
        S(-67, -21), S(-50, -13), S(-19, -11), S(-14, 8),
        S(-5, 5), S(17, 8), S(39, -9), S(40, 22),
        S(4, 2), S(63, 4), S(66, 29), S(78, 30),
        S(32, 20), S(78, 9), S(97, 27), S(88, 31),
        S(-23, -2), S(37, 9), S(109, 20), S(109, 25),
        S(-108, -34), S(-32, -9), S(54, -16), S(60, 14),
        S(-260, -84), S(-134, -36), S(-22, -15), S(19, -1)
    },
    // BishopPST (half-board)
    {
        S(16, -24), S(20, -27), S(-32, -7), S(-29, 3),
        S(11, -23), S(17, -27), S(36, -4), S(1, 6),
        S(22, -13), S(45, 8), S(22, -19), S(9, 18),
        S(40, -13), S(59, -5), S(56, 18), S(39, -7),
        S(26, -15), S(50, 32), S(63, 15), S(59, -8),
        S(12, -10), S(7, 16), S(28, 2), S(41, 4),
        S(-47, -16), S(-70, 3), S(-33, 6), S(-58, -2),
        S(-76, 1), S(-101, 14), S(-151, 16), S(-101, 27)
    },
    // RookPST (half-board)
    {
        S(-47, -12), S(-46, 3), S(-10, -5), S(-8, -11),
        S(-66, -28), S(-50, -21), S(-30, -12), S(-26, -15),
        S(-68, -25), S(-53, -14), S(-55, -19), S(-47, -27),
        S(-39, 3), S(-37, 4), S(-30, 7), S(12, -7),
        S(-36, 22), S(-12, 22), S(21, 13), S(45, -1),
        S(8, 13), S(16, 12), S(54, 16), S(72, 11),
        S(29, 1), S(36, 10), S(67, 0), S(85, 11),
        S(40, 10), S(47, 12), S(61, 2), S(80, -3)
    },
    // QueenPST (half-board)
    {
        S(21, -80), S(19, -77), S(23, -69), S(42, -50),
        S(8, -37), S(20, -40), S(52, -40), S(60, -33),
        S(-7, -43), S(38, -17), S(36, 5), S(28, -17),
        S(14, -17), S(43, 14), S(40, 28), S(20, 21),
        S(3, 4), S(8, 11), S(5, 60), S(-7, 68),
        S(-17, 9), S(-32, 22), S(-48, 69), S(-48, 97),
        S(-97, 13), S(-130, 44), S(-103, 69), S(-91, 88),
        S(-98, -11), S(-85, -16), S(216, -56), S(54, -14)
    },
    // KingPST (half-board)
    {
        S(221, -91), S(222, -59), S(79, -27), S(-33, -34),
        S(243, -42), S(197, -21), S(63, 6), S(-9, 13),
        S(48, -15), S(120, -3), S(23, 20), S(-53, 33),
        S(-36, 3), S(14, 29), S(-64, 50), S(-166, 51),
        S(41, 12), S(-28, 45), S(-127, 65), S(-244, 74),
        S(-9, 3), S(-64, 42), S(-154, 61), S(-253, 78),
        S(218, -57), S(-80, 1), S(-35, 12), S(-172, 32),
        S(-256, -171), S(247, -60), S(257, -67), S(-217, 16)
    },
    {
        {},
        {},
        {S(-83, -89), S(-43, -50), S(-7, -10), S(14, -7), S(29, 9), S(39, 28), S(56, 34), S(56, 36), S(56, 36)},
        {S(-14, 59), S(19, 88), S(54, 123), S(82, 138), S(108, 138), S(113, 148), S(118, 161), S(121, 163), S(121, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163)},
        {S(-20, 254), S(4, 284), S(20, 294), S(20, 303), S(20, 305), S(20, 323), S(20, 323), S(32, 333), S(33, 337), S(49, 349), S(57, 352), S(65, 354), S(67, 357), S(67, 357), S(89, 357)},
        {S(18, 291), S(43, 316), S(68, 341), S(75, 351), S(100, 351), S(125, 351), S(138, 362), S(147, 363), S(155, 377), S(171, 377), S(173, 380), S(174, 380), S(174, 380), S(176, 395), S(176, 402), S(178, 403), S(186, 403), S(202, 403), S(203, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(236, 403), S(237, 403)},
        {},
    },
    {S(0, 0), S(-39, 42), S(-39, 42), S(-38, 85), S(0, 144), S(16, 298), S(512, 380), S(0, 0)},
    {S(0, 0), S(-10, -7), S(51, 9), S(51, 9), S(65, 15), S(65, 81), S(245, 107), S(0, 0)},
    S(114, 12), // RookOpenFileBonus
    S(29, 10), // RookSemiOpenFileBonus
    S(0, 3), // RookOnQueenFile
    S(99, 13), // KnightOutpostBonus
    S(95, 13), // BishopOutpostBonus
    S(-103, 0), // TrappedRookByKingPenalty
    S(67, 3), // RookBehindOurPasserBonus
    S(-125, 102), // RookBehindTheirPasserBonus
    S(35, 4), // MinorBehindPawnBonus
    S(26, 0), // MinorOnKingRing
    S(9, 0), // RookOnKingRing
    S(-16, -2), // KingProtector
    S(8, 5), // BishopPair
    {
        {S(0, 0), S(109, 0), S(103, 0), S(75, 0), S(55, 0), S(23, 0), S(0, 0)},
        {S(-91, 0), S(193, 0), S(80, 0), S(0, 0), S(0, 0), S(86, 0), S(167, 0)},
        {S(-8, 0), S(203, 0), S(65, 0), S(38, 0), S(58, 0), S(75, 0), S(89, 0)},
        {S(-16, 0), S(155, 0), S(74, 0), S(48, 0), S(27, 0), S(124, 0), S(66, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(10, 0), S(0, 0)},
        {S(0, 0), S(16, 0), S(77, 0), S(11, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(8, 0), S(274, 0), S(4, 0), S(2, 0), S(4, 0), S(0, 0)},
        {S(0, 0), S(32, 0), S(317, 0), S(38, 0), S(2, 0), S(4, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(210, 0), S(5, 0), S(0, 0), S(44, 0), S(0, 0)}, // BlockedStorm
    S(-31, 0), // UndefendedKingZoneSq
    S(10, 49), // KingMobilityFactor
    S(30, 41), // KingAttackByKnight
    S(12, 45), // KingAttackByBishop
    S(30, 49), // KingAttackByRook
    S(32, 49), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(36, 299), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(24, 6), // KingRingWeakWeight
    S(26, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -6), // DoubledPawnPenalty
    S(-12, -6), // BackwardPawnPenalty
    S(-29, -16), // WeakUnopposedPenalty
    S(0, -40), // DoubledIsolatedPenalty
    {S(-99, -26), S(-111, -109)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-4, -21), // PawnIslandPenalty
    {S(37, 0), S(19, 0)}, // CentralPawnBonus
    S(64, 26), // BishopLongDiagonalBonus
    S(-20, 0), // BishopXrayPawns
    S(0, 59), // InitiativePasser
    S(0, 52), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 65), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(45, 16), // SliderOnQueenBishop
    S(55, 2), // SliderOnQueenRook
    S(8, 0), // RestrictedPiece
    S(30, 0), // ThreatByPawnPush
    S(-39, -7), // WeakQueenDefender
    S(103, 0), // KnightOnQueen
    S(-306, -138), // PawnlessFlank
    S(0, 0), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 9), // KBNKCornerEg
    S(0, 284), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
