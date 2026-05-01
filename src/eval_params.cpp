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
    S(249, 47), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(189, 4), S(167, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(227, 30), S(0, 0)},
    S(123, 18), // ThreatByKing
    S(101, 22), // Hanging
    S(1, 33), // WeakQueen
    S(21, 31), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 27), S(0, 43), S(0, 43), S(0, 43), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 39), S(0, 73), S(0, 84), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-33, -38), S(-41, -55), S(-122, -55), S(-338, -121), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 23), S(53, 62), S(165, 134), S(165, 415), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-46, 17), S(46, 28), S(95, 28), S(95, 28), S(0, 0)},
    S(0, 20), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -7), // BishopPawns
    S(68, 0), // Tempo
    {S(0, 0), S(207, 229), S(923, 562), S(956, 497), S(1508, 785), S(2674, 1779), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-47, -54), S(-48, -59), S(-34, -60), S(-4, -61), S(16, -61), S(-36, -54), S(-35, -58), S(-53, -69),
        S(-101, -48), S(-105, -59), S(-64, -62), S(-56, -64), S(-24, -57), S(-74, -57), S(-50, -74), S(-93, -64),
        S(-83, -39), S(-93, -54), S(-91, -68), S(-53, -72), S(-42, -72), S(-50, -70), S(-52, -55), S(-83, -52),
        S(-15, -8), S(-45, -31), S(-25, -52), S(4, -66), S(18, -62), S(-10, -54), S(-82, -26), S(-19, -13),
        S(114, 50), S(80, 72), S(116, 13), S(201, 0), S(208, 1), S(110, 7), S(58, 76), S(60, 77),
        S(203, 156), S(-36, 209), S(102, 190), S(288, 135), S(232, 133), S(20, 200), S(-264, 255), S(49, 179),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-117, -70), S(-84, -2), S(-53, -11), S(-84, 23),
        S(-92, -17), S(-86, -7), S(-22, -8), S(-14, 12),
        S(-10, 10), S(25, 8), S(43, -3), S(44, 23),
        S(-5, 8), S(92, 3), S(82, 28), S(86, 30),
        S(61, 23), S(99, 8), S(125, 25), S(98, 32),
        S(-58, 2), S(67, 11), S(138, 13), S(138, 27),
        S(-86, -37), S(-16, 7), S(84, -30), S(36, 26),
        S(-292, -122), S(-161, -39), S(-45, -1), S(-5, 10)
    },
    // BishopPST (half-board)
    {
        S(16, -27), S(32, -29), S(-44, -6), S(-34, 1),
        S(-1, -24), S(11, -24), S(53, -9), S(-4, 9),
        S(18, -11), S(49, 8), S(18, -17), S(14, 19),
        S(34, -13), S(78, -7), S(63, 18), S(40, -7),
        S(17, -7), S(54, 33), S(93, 10), S(60, -10),
        S(44, -13), S(16, 17), S(44, 2), S(71, -1),
        S(-30, -26), S(-108, 6), S(-5, 8), S(-89, 4),
        S(-64, 3), S(-124, 16), S(-183, 23), S(-137, 34)
    },
    // RookPST (half-board)
    {
        S(-41, -15), S(-43, -1), S(-7, -8), S(-6, -14),
        S(-68, -28), S(-45, -26), S(-25, -17), S(-32, -16),
        S(-82, -22), S(-60, -12), S(-69, -16), S(-57, -26),
        S(-19, -2), S(-39, 6), S(-62, 14), S(20, -10),
        S(-66, 27), S(-15, 25), S(16, 16), S(43, -2),
        S(33, 10), S(-4, 19), S(56, 14), S(89, 6),
        S(29, 4), S(23, 12), S(97, -1), S(106, 9),
        S(34, 16), S(36, 16), S(54, 5), S(105, -3)
    },
    // QueenPST (half-board)
    {
        S(44, -113), S(23, -86), S(18, -74), S(33, -48),
        S(-3, -25), S(12, -39), S(53, -40), S(56, -33),
        S(-38, -28), S(37, -20), S(31, 8), S(25, -18),
        S(20, -16), S(49, 10), S(42, 26), S(23, 19),
        S(7, -1), S(34, -10), S(4, 68), S(8, 68),
        S(14, -12), S(-2, 2), S(-34, 74), S(-33, 100),
        S(-106, 15), S(-165, 75), S(-137, 101), S(-121, 118),
        S(-63, -19), S(-113, 4), S(240, -84), S(48, -12)
    },
    // KingPST (half-board)
    {
        S(245, -101), S(245, -71), S(81, -36), S(-25, -49),
        S(272, -54), S(224, -31), S(54, -1), S(-30, 7),
        S(14, -12), S(148, -11), S(35, 16), S(-65, 27),
        S(-72, 4), S(39, 26), S(-36, 47), S(-203, 52),
        S(67, 5), S(1, 48), S(-118, 67), S(-282, 79),
        S(-47, 8), S(-40, 58), S(-185, 93), S(-287, 100),
        S(245, -76), S(-124, 28), S(-11, 24), S(-165, 57),
        S(-287, -206), S(275, -36), S(288, -105), S(-260, 46)
    },
    {
        {},
        {},
        {S(-76, -82), S(-36, -42), S(4, -9), S(19, -4), S(30, 13), S(39, 31), S(56, 36), S(56, 39), S(56, 39)},
        {S(-8, 81), S(27, 90), S(58, 125), S(84, 138), S(107, 139), S(112, 150), S(117, 161), S(119, 163), S(119, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163)},
        {S(-19, 256), S(5, 286), S(19, 294), S(20, 304), S(20, 306), S(20, 323), S(20, 323), S(29, 335), S(34, 336), S(45, 349), S(45, 352), S(45, 355), S(45, 357), S(49, 357), S(60, 357)},
        {S(35, 291), S(58, 316), S(79, 341), S(86, 351), S(106, 351), S(125, 351), S(137, 362), S(150, 362), S(155, 377), S(169, 378), S(169, 382), S(174, 382), S(174, 382), S(176, 396), S(176, 403), S(176, 403), S(186, 403), S(200, 403), S(203, 403), S(221, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(238, 403), S(238, 403)},
        {},
    },
    {S(0, 0), S(-37, 40), S(-37, 40), S(-24, 76), S(0, 151), S(0, 324), S(487, 377), S(0, 0)},
    {S(0, 0), S(-14, -7), S(48, 9), S(54, 9), S(77, 17), S(77, 86), S(262, 127), S(0, 0)},
    S(115, 15), // RookOpenFileBonus
    S(34, 13), // RookSemiOpenFileBonus
    S(0, 2), // RookOnQueenFile
    S(92, 13), // KnightOutpostBonus
    S(89, 16), // BishopOutpostBonus
    S(-96, 0), // TrappedRookByKingPenalty
    S(67, 4), // RookBehindOurPasserBonus
    S(-91, 94), // RookBehindTheirPasserBonus
    S(36, 4), // MinorBehindPawnBonus
    S(25, 0), // MinorOnKingRing
    S(11, 0), // RookOnKingRing
    S(-17, -2), // KingProtector
    S(13, 7), // BishopPair
    {
        {S(0, 0), S(112, 0), S(114, 0), S(81, 0), S(57, 0), S(48, 0), S(0, 0)},
        {S(-89, 0), S(195, 0), S(70, 0), S(0, 0), S(0, 0), S(55, 0), S(171, 0)},
        {S(-9, 0), S(195, 0), S(72, 0), S(25, 0), S(64, 0), S(77, 0), S(84, 0)},
        {S(0, 0), S(160, 0), S(92, 0), S(68, 0), S(38, 0), S(102, 0), S(34, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(8, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(47, 0), S(35, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(238, 0), S(10, 0), S(0, 0), S(8, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(282, 0), S(30, 0), S(0, 0), S(1, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(192, 0), S(12, 0), S(0, 0), S(36, 0), S(0, 0)}, // BlockedStorm
    S(-30, -1), // UndefendedKingZoneSq
    S(9, 44), // KingMobilityFactor
    S(26, 31), // KingAttackByKnight
    S(10, 29), // KingAttackByBishop
    S(30, 31), // KingAttackByRook
    S(30, 32), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(30, 1), S(35, 298), S(31, 1), S(31, 1), S(0, 0)}, // KingSafeCheck
    S(22, 24), // KingRingWeakWeight
    S(26, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -5), // DoubledPawnPenalty
    S(-11, -6), // BackwardPawnPenalty
    S(-29, -18), // WeakUnopposedPenalty
    S(0, -43), // DoubledIsolatedPenalty
    {S(-88, -24), S(-118, -92)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-8, -21), // PawnIslandPenalty
    {S(41, 0), S(31, 0)}, // CentralPawnBonus
    S(69, 27), // BishopLongDiagonalBonus
    S(-22, 0), // BishopXrayPawns
    S(0, 59), // InitiativePasser
    S(0, 51), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(46, 30), // SliderOnQueenBishop
    S(51, 0), // SliderOnQueenRook
    S(9, 0), // RestrictedPiece
    S(32, 0), // ThreatByPawnPush
    S(-41, -10), // WeakQueenDefender
    S(77, 0), // KnightOnQueen
    S(-272, -145), // PawnlessFlank
    S(0, 0), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 8), // KBNKCornerEg
    S(0, 220), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
