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
    S(216, 41), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(188, 12), S(196, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(250, 31), S(0, 0)},
    S(116, 0), // ThreatByKing
    S(105, 27), // Hanging
    S(57, 0), // WeakQueen
    S(29, 22), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 43), S(0, 43), S(0, 43), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 42), S(0, 75), S(0, 88), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-28, -32), S(-28, -50), S(-105, -52), S(-360, -114), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 17), S(53, 53), S(169, 112), S(171, 372), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-41, 21), S(56, 28), S(103, 28), S(103, 28), S(0, 0)},
    S(0, 25), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -7), // BishopPawns
    S(44, 0), // Tempo
    {S(0, 0), S(211, 232), S(926, 579), S(958, 513), S(1489, 814), S(2677, 1850), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-44, -49), S(-54, -53), S(-37, -59), S(-13, -67), S(28, -54), S(-25, -51), S(-25, -58), S(-44, -73),
        S(-103, -45), S(-113, -54), S(-54, -70), S(-62, -64), S(-9, -60), S(-80, -57), S(-50, -75), S(-102, -70),
        S(-78, -37), S(-102, -48), S(-93, -71), S(-54, -69), S(-25, -79), S(-46, -69), S(-57, -59), S(-96, -57),
        S(-19, -2), S(-37, -44), S(-34, -55), S(6, -74), S(33, -73), S(-6, -65), S(-92, -25), S(-24, -26),
        S(105, 47), S(74, 61), S(103, 15), S(191, -14), S(203, -14), S(92, -2), S(37, 83), S(40, 81),
        S(226, 153), S(-23, 216), S(140, 185), S(300, 151), S(217, 142), S(13, 221), S(-254, 264), S(44, 203),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-151, -59), S(-83, 0), S(-55, -17), S(-85, 15),
        S(-93, -8), S(-97, 8), S(-20, -9), S(-12, 6),
        S(-12, 3), S(31, 3), S(37, -8), S(35, 20),
        S(3, 10), S(93, 3), S(81, 32), S(85, 27),
        S(77, 27), S(103, 2), S(135, 17), S(94, 35),
        S(-71, 11), S(77, 2), S(136, 16), S(145, 25),
        S(-64, -46), S(-11, 14), S(103, -39), S(40, 36),
        S(-296, -142), S(-166, -22), S(-75, 5), S(7, 8)
    },
    // BishopPST (half-board)
    {
        S(21, -32), S(45, -36), S(-58, -2), S(-39, 2),
        S(1, -31), S(15, -23), S(58, -10), S(-10, 8),
        S(25, -13), S(62, 5), S(13, -15), S(5, 22),
        S(27, -10), S(77, -11), S(58, 20), S(39, 1),
        S(6, -10), S(44, 35), S(104, 6), S(43, -1),
        S(59, -22), S(31, 16), S(46, 4), S(84, 2),
        S(-19, -36), S(-123, 7), S(0, 9), S(-77, -4),
        S(-59, 5), S(-112, 25), S(-205, 27), S(-159, 37)
    },
    // RookPST (half-board)
    {
        S(-35, -21), S(-34, -7), S(-6, -11), S(2, -18),
        S(-81, -25), S(-41, -34), S(-15, -27), S(-37, -18),
        S(-85, -22), S(-53, -12), S(-77, -16), S(-66, -18),
        S(-8, -12), S(-49, 7), S(-66, 19), S(22, -12),
        S(-59, 27), S(-6, 34), S(26, 16), S(30, 0),
        S(35, 9), S(-5, 29), S(55, 21), S(98, 3),
        S(23, 7), S(0, 15), S(100, -4), S(106, 5),
        S(33, 23), S(30, 27), S(45, 15), S(110, -3)
    },
    // QueenPST (half-board)
    {
        S(28, -109), S(29, -91), S(4, -71), S(26, -50),
        S(-5, -21), S(6, -41), S(49, -47), S(54, -43),
        S(-36, -24), S(42, -30), S(29, 5), S(16, -16),
        S(28, -25), S(54, 5), S(37, 15), S(23, 13),
        S(1, 5), S(31, -8), S(-3, 76), S(0, 72),
        S(24, -25), S(9, 0), S(-5, 53), S(-25, 110),
        S(-105, 8), S(-169, 77), S(-146, 112), S(-114, 119),
        S(-36, -29), S(-110, 22), S(238, -73), S(56, -10)
    },
    // KingPST (half-board)
    {
        S(239, -108), S(249, -79), S(92, -48), S(-11, -60),
        S(278, -68), S(231, -46), S(50, -14), S(-34, -7),
        S(9, -18), S(146, -23), S(33, 3), S(-63, 14),
        S(-76, 4), S(35, 16), S(-32, 42), S(-207, 45),
        S(66, 2), S(14, 53), S(-121, 64), S(-300, 80),
        S(-54, 17), S(-35, 83), S(-228, 109), S(-295, 105),
        S(266, -70), S(-131, 20), S(11, 82), S(-189, 83),
        S(-294, -231), S(300, 5), S(300, -126), S(-274, 68)
    },
    {
        {},
        {},
        {S(-68, -77), S(-36, -37), S(2, -10), S(14, 1), S(27, 17), S(35, 34), S(51, 39), S(56, 39), S(56, 39)},
        {S(2, 100), S(37, 101), S(66, 130), S(87, 140), S(106, 143), S(108, 154), S(115, 162), S(116, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(128, 163), S(128, 163)},
        {S(-23, 262), S(7, 292), S(20, 300), S(20, 310), S(20, 311), S(20, 325), S(20, 327), S(33, 336), S(36, 337), S(45, 349), S(45, 353), S(45, 354), S(45, 357), S(45, 357), S(45, 357)},
        {S(48, 291), S(68, 316), S(93, 341), S(104, 341), S(112, 351), S(133, 351), S(141, 362), S(153, 362), S(158, 375), S(168, 377), S(168, 382), S(174, 382), S(174, 390), S(174, 398), S(176, 403), S(176, 403), S(176, 403), S(193, 403), S(193, 403), S(207, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(240, 403), S(240, 403)},
        {},
    },
    {S(0, 0), S(-37, 38), S(-37, 39), S(-29, 75), S(0, 160), S(0, 334), S(453, 390), S(0, 0)},
    {S(0, 0), S(-10, -7), S(49, 9), S(57, 9), S(78, 25), S(78, 96), S(263, 144), S(0, 0)},
    S(113, 17), // RookOpenFileBonus
    S(44, 17), // RookSemiOpenFileBonus
    S(6, 0), // RookOnQueenFile
    S(77, 13), // KnightOutpostBonus
    S(81, 16), // BishopOutpostBonus
    S(-76, 0), // TrappedRookByKingPenalty
    S(62, 9), // RookBehindOurPasserBonus
    S(-78, 91), // RookBehindTheirPasserBonus
    S(33, 5), // MinorBehindPawnBonus
    S(20, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-18, -2), // KingProtector
    S(13, 5), // BishopPair
    {
        {S(-3, 0), S(97, 0), S(112, 0), S(80, 0), S(50, 0), S(40, 0), S(2, 0)},
        {S(-94, 0), S(182, 0), S(77, 0), S(0, 0), S(0, 0), S(52, 0), S(155, 0)},
        {S(-18, 0), S(180, 0), S(62, 0), S(18, 0), S(49, 0), S(77, 0), S(96, 0)},
        {S(-6, 0), S(149, 0), S(88, 0), S(59, 0), S(27, 0), S(98, 0), S(32, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(14, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(51, 0), S(35, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(226, 0), S(20, 0), S(0, 0), S(9, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(277, 0), S(42, 0), S(3, 0), S(7, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(190, 0), S(13, 0), S(0, 0), S(38, 0), S(0, 0)}, // BlockedStorm
    S(-37, 0), // UndefendedKingZoneSq
    S(7, 43), // KingMobilityFactor
    S(26, 31), // KingAttackByKnight
    S(10, 26), // KingAttackByBishop
    S(26, 31), // KingAttackByRook
    S(26, 31), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(30, 1), S(32, 280), S(30, 1), S(30, 1), S(0, 0)}, // KingSafeCheck
    S(20, 9), // KingRingWeakWeight
    S(26, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-2, -15), // DoubledPawnPenalty
    S(-7, -5), // BackwardPawnPenalty
    S(-27, -19), // WeakUnopposedPenalty
    S(-2, -40), // DoubledIsolatedPenalty
    {S(-83, -23), S(-108, -95)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-19, -19), // PawnIslandPenalty
    {S(34, 0), S(33, 0)}, // CentralPawnBonus
    S(67, 24), // BishopLongDiagonalBonus
    S(-21, 0), // BishopXrayPawns
    S(0, 46), // InitiativePasser
    S(0, 54), // InitiativePawnCount
    S(0, 2), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 37), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(59, 15), // SliderOnQueenBishop
    S(42, 0), // SliderOnQueenRook
    S(8, 1), // RestrictedPiece
    S(28, 0), // ThreatByPawnPush
    S(-42, -16), // WeakQueenDefender
    S(61, 0), // KnightOnQueen
    S(-234, -143), // PawnlessFlank
    S(2, 0), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 132), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
