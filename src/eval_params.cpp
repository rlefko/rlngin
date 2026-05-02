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
    S(245, 39), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(197, 0), S(201, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(242, 17), S(0, 0)},
    S(115, 14), // ThreatByKing
    S(97, 21), // Hanging
    S(16, 0), // WeakQueen
    S(25, 23), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 40), S(0, 41), S(0, 41), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 39), S(0, 70), S(0, 83), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-28, -29), S(-28, -57), S(-61, -76), S(-361, -110), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(41, 18), S(41, 57), S(171, 120), S(171, 348), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-60, 23), S(61, 28), S(77, 28), S(77, 28), S(0, 0)},
    S(0, 32), // RookOn7thBonus
    S(0, -1), // BadBishop
    S(-2, -7), // BishopPawns
    S(112, 0), // Tempo
    {S(0, 0), S(219, 220), S(907, 560), S(944, 501), S(1422, 774), S(2642, 1740), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-50, -57), S(-45, -55), S(-51, -60), S(-38, -53), S(-30, -45), S(-47, -54), S(-44, -58), S(-58, -72),
        S(-85, -59), S(-88, -64), S(-57, -72), S(-48, -70), S(-33, -61), S(-62, -64), S(-79, -75), S(-85, -68),
        S(-56, -43), S(-64, -55), S(-48, -75), S(-28, -75), S(-22, -76), S(-34, -72), S(-59, -55), S(-62, -55),
        S(4, -1), S(-10, -20), S(11, -45), S(42, -59), S(52, -60), S(17, -48), S(-20, -21), S(-2, -10),
        S(47, 87), S(28, 93), S(86, 49), S(162, 34), S(167, 35), S(91, 49), S(20, 93), S(38, 89),
        S(132, 150), S(-77, 185), S(55, 150), S(231, 108), S(208, 105), S(51, 157), S(-113, 195), S(70, 146),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-89, -30), S(-107, -15), S(-57, -15), S(-57, 9),
        S(-70, -19), S(-51, -10), S(-22, -6), S(-15, 1),
        S(-24, -2), S(21, -2), S(27, -12), S(42, 20),
        S(-3, 9), S(55, 14), S(73, 24), S(76, 31),
        S(50, 14), S(89, 14), S(108, 22), S(91, 31),
        S(-8, -3), S(38, 4), S(90, 15), S(92, 30),
        S(-83, -29), S(-25, -7), S(42, -6), S(50, 18),
        S(-228, -68), S(-124, -28), S(-19, -1), S(13, 10)
    },
    // BishopPST (half-board)
    {
        S(11, -30), S(17, -29), S(-69, -4), S(-30, -3),
        S(14, -33), S(29, -28), S(31, -12), S(-12, 3),
        S(13, -22), S(61, 5), S(16, -12), S(1, 19),
        S(28, -15), S(50, -2), S(47, 13), S(35, -2),
        S(9, -10), S(52, 25), S(60, 12), S(46, 0),
        S(27, -4), S(23, 17), S(30, 9), S(27, 9),
        S(-25, -11), S(-43, -1), S(-37, 13), S(-45, -2),
        S(-48, 16), S(-86, 20), S(-152, 22), S(-102, 19)
    },
    // RookPST (half-board)
    {
        S(-48, -15), S(-36, -2), S(-10, -1), S(-3, -7),
        S(-70, -29), S(-47, -26), S(-23, -12), S(-33, -13),
        S(-65, -25), S(-44, -14), S(-53, -15), S(-53, -14),
        S(-39, -2), S(-28, 3), S(-27, 13), S(1, 0),
        S(-16, 14), S(11, 24), S(29, 13), S(35, 4),
        S(23, 11), S(29, 17), S(47, 15), S(62, 10),
        S(31, 1), S(27, 4), S(51, -4), S(64, 4),
        S(31, 12), S(32, 12), S(45, 8), S(57, 7)
    },
    // QueenPST (half-board)
    {
        S(4, -79), S(15, -71), S(14, -61), S(46, -54),
        S(7, -51), S(23, -55), S(43, -44), S(57, -32),
        S(0, -38), S(41, -29), S(43, 4), S(13, 2),
        S(26, -21), S(55, 11), S(29, 26), S(13, 35),
        S(18, -3), S(13, 16), S(16, 64), S(1, 68),
        S(-7, 10), S(-44, 30), S(-44, 68), S(-30, 98),
        S(-88, 11), S(-118, 29), S(-73, 60), S(-78, 80),
        S(-87, -28), S(-50, -19), S(146, -31), S(10, -10)
    },
    // KingPST (half-board)
    {
        S(205, -99), S(209, -63), S(59, -29), S(-93, -43),
        S(232, -52), S(185, -29), S(44, 2), S(-41, 10),
        S(84, -29), S(98, -8), S(12, 18), S(-53, 26),
        S(-11, -4), S(-1, 25), S(-62, 48), S(-143, 48),
        S(10, 14), S(-32, 49), S(-119, 64), S(-229, 70),
        S(35, 5), S(-37, 46), S(-139, 75), S(-228, 79),
        S(157, -55), S(-7, -3), S(-14, 21), S(-157, 53),
        S(-229, -157), S(243, -56), S(243, -57), S(-222, 30)
    },
    {
        {},
        {},
        {S(-94, -92), S(-54, -52), S(-16, -13), S(6, -3), S(26, 11), S(42, 28), S(61, 35), S(63, 39), S(63, 39)},
        {S(-14, 52), S(21, 87), S(56, 118), S(86, 132), S(106, 136), S(114, 149), S(124, 158), S(128, 158), S(128, 162), S(128, 163), S(128, 163), S(128, 163), S(128, 163), S(128, 163)},
        {S(-41, 257), S(-11, 287), S(19, 296), S(20, 308), S(20, 308), S(20, 323), S(20, 324), S(37, 332), S(43, 333), S(54, 345), S(54, 351), S(54, 354), S(54, 357), S(54, 357), S(54, 357)},
        {S(14, 291), S(37, 316), S(62, 341), S(87, 341), S(101, 344), S(122, 344), S(135, 355), S(149, 359), S(157, 370), S(165, 377), S(167, 382), S(172, 382), S(172, 388), S(173, 396), S(180, 403), S(180, 403), S(180, 403), S(194, 403), S(194, 403), S(210, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(244, 403)},
        {},
    },
    {S(0, 0), S(-37, 34), S(-37, 34), S(-37, 86), S(0, 144), S(52, 284), S(520, 390), S(0, 0)},
    {S(0, 0), S(-5, -9), S(51, 9), S(51, 9), S(65, 20), S(68, 87), S(334, 141), S(0, 0)},
    S(131, 11), // RookOpenFileBonus
    S(51, 7), // RookSemiOpenFileBonus
    S(6, 0), // RookOnQueenFile
    S(84, 11), // KnightOutpostBonus
    S(89, 14), // BishopOutpostBonus
    S(-99, 0), // TrappedRookByKingPenalty
    S(72, 10), // RookBehindOurPasserBonus
    S(-80, 103), // RookBehindTheirPasserBonus
    S(26, 4), // MinorBehindPawnBonus
    S(30, 0), // MinorOnKingRing
    S(11, 0), // RookOnKingRing
    S(-20, -2), // KingProtector
    S(11, 3), // BishopPair
    {
        {S(0, 0), S(149, 0), S(147, 0), S(89, 0), S(74, 0), S(46, 0), S(0, 0)},
        {S(-110, 0), S(206, 0), S(107, 0), S(0, 0), S(0, 0), S(50, 0), S(195, 0)},
        {S(-6, 0), S(198, 0), S(54, 0), S(24, 0), S(23, 0), S(119, 0), S(128, 0)},
        {S(0, 0), S(114, 0), S(91, 0), S(73, 0), S(43, 0), S(64, 0), S(107, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(4, 0), S(0, 0)},
        {S(0, 0), S(8, 0), S(97, 0), S(0, 0), S(3, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(67, 0), S(300, 0), S(59, 0), S(20, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(8, 0), S(274, 0), S(57, 0), S(2, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(170, 0), S(11, 0), S(0, 0), S(18, 0), S(0, 0)}, // BlockedStorm
    S(-42, 0), // UndefendedKingZoneSq
    S(9, 54), // KingMobilityFactor
    S(30, 33), // KingAttackByKnight
    S(12, 43), // KingAttackByBishop
    S(30, 33), // KingAttackByRook
    S(30, 33), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(38, 306), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(21, 14), // KingRingWeakWeight
    S(29, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -11), // DoubledPawnPenalty
    S(-14, -6), // BackwardPawnPenalty
    S(-28, -18), // WeakUnopposedPenalty
    S(0, -40), // DoubledIsolatedPenalty
    {S(-84, -32), S(-58, -118)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-7, -15), // PawnIslandPenalty
    {S(34, 0), S(4, 0)}, // CentralPawnBonus
    S(59, 20), // BishopLongDiagonalBonus
    S(-22, 0), // BishopXrayPawns
    S(0, 42), // InitiativePasser
    S(0, 49), // InitiativePawnCount
    S(0, 1), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 54), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(65, 1), // SliderOnQueenBishop
    S(43, 0), // SliderOnQueenRook
    S(5, 2), // RestrictedPiece
    S(37, 0), // ThreatByPawnPush
    S(-33, -13), // WeakQueenDefender
    S(91, 0), // KnightOnQueen
    S(-276, -126), // PawnlessFlank
    S(0, 2), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 10), // KBNKCornerEg
    S(0, 359), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
