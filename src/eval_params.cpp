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
    S(262, 52), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(193, 0), S(148, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(209, 44), S(0, 0)},
    S(123, 25), // ThreatByKing
    S(100, 20), // Hanging
    S(0, 17), // WeakQueen
    S(21, 33), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 43), S(0, 43), S(0, 43), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 39), S(0, 70), S(0, 77), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-41, -40), S(-41, -58), S(-116, -63), S(-327, -129), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 21), S(53, 65), S(165, 141), S(165, 428), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-45, 13), S(46, 28), S(87, 28), S(87, 28), S(0, 0)},
    S(0, 22), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -7), // BishopPawns
    S(85, 0), // Tempo
    {S(0, 0), S(198, 220), S(909, 552), S(937, 482), S(1474, 757), S(2635, 1718), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-48, -56), S(-46, -56), S(-38, -58), S(-15, -60), S(-11, -54), S(-37, -53), S(-49, -56), S(-53, -68),
        S(-90, -53), S(-83, -61), S(-46, -67), S(-37, -65), S(-27, -56), S(-49, -61), S(-72, -70), S(-85, -63),
        S(-65, -42), S(-63, -53), S(-47, -72), S(-28, -72), S(-17, -71), S(-35, -67), S(-59, -52), S(-67, -52),
        S(-4, -2), S(-28, -24), S(0, -44), S(35, -58), S(39, -55), S(3, -44), S(-33, -18), S(-6, -7),
        S(67, 79), S(36, 87), S(73, 45), S(157, 23), S(160, 24), S(67, 43), S(17, 89), S(56, 86),
        S(149, 139), S(-81, 192), S(55, 149), S(241, 99), S(217, 99), S(47, 154), S(-216, 208), S(96, 139),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-71, -35), S(-67, -10), S(-47, -14), S(-55, 19),
        S(-59, -24), S(-40, -16), S(-17, -10), S(-10, 7),
        S(-3, 6), S(17, 7), S(38, -10), S(42, 21),
        S(2, 3), S(49, 5), S(60, 29), S(76, 27),
        S(30, 21), S(71, 11), S(93, 26), S(86, 29),
        S(-17, -5), S(35, 8), S(95, 21), S(95, 26),
        S(-100, -33), S(-30, -8), S(40, -15), S(46, 15),
        S(-242, -67), S(-116, -31), S(-20, -14), S(13, 0)
    },
    // BishopPST (half-board)
    {
        S(19, -24), S(21, -26), S(-29, -8), S(-26, 0),
        S(10, -24), S(16, -25), S(39, -5), S(0, 7),
        S(22, -14), S(44, 7), S(25, -19), S(11, 17),
        S(35, -12), S(54, -5), S(53, 19), S(40, -6),
        S(17, -13), S(43, 33), S(54, 16), S(50, -4),
        S(7, -9), S(6, 15), S(15, 5), S(28, 7),
        S(-44, -17), S(-51, 0), S(-46, 7), S(-43, -1),
        S(-65, 2), S(-86, 11), S(-132, 17), S(-90, 26)
    },
    // RookPST (half-board)
    {
        S(-47, -12), S(-46, 3), S(-9, -5), S(-8, -10),
        S(-66, -28), S(-50, -21), S(-30, -12), S(-26, -15),
        S(-68, -25), S(-53, -13), S(-51, -19), S(-47, -27),
        S(-37, 3), S(-33, 4), S(-22, 6), S(13, -7),
        S(-32, 22), S(-9, 22), S(22, 13), S(42, -1),
        S(6, 13), S(16, 12), S(51, 16), S(68, 11),
        S(27, 0), S(36, 9), S(65, 0), S(81, 11),
        S(38, 10), S(45, 12), S(61, 2), S(72, -3)
    },
    // QueenPST (half-board)
    {
        S(21, -79), S(19, -76), S(23, -68), S(42, -49),
        S(8, -36), S(20, -39), S(52, -37), S(60, -32),
        S(-7, -42), S(38, -16), S(36, 6), S(28, -16),
        S(14, -16), S(41, 15), S(36, 29), S(16, 22),
        S(-2, 5), S(2, 12), S(1, 59), S(-7, 65),
        S(-25, 12), S(-40, 23), S(-48, 66), S(-40, 90),
        S(-97, 10), S(-114, 33), S(-87, 54), S(-75, 73),
        S(-94, -10), S(-69, -19), S(200, -39), S(53, -15)
    },
    // KingPST (half-board)
    {
        S(205, -86), S(210, -57), S(67, -25), S(-41, -34),
        S(227, -39), S(185, -21), S(55, 6), S(-18, 14),
        S(64, -17), S(104, -3), S(19, 20), S(-53, 33),
        S(-20, 2), S(-2, 29), S(-64, 50), S(-150, 49),
        S(25, 10), S(-44, 44), S(-119, 63), S(-228, 70),
        S(7, -1), S(-60, 38), S(-138, 57), S(-237, 74),
        S(202, -57), S(-64, -7), S(-27, 12), S(-156, 26),
        S(-240, -155), S(231, -64), S(241, -51), S(-201, 0)
    },
    {
        {},
        {},
        {S(-87, -90), S(-48, -52), S(-8, -12), S(13, -8), S(28, 8), S(39, 27), S(56, 34), S(56, 36), S(56, 36)},
        {S(-16, 61), S(19, 88), S(53, 123), S(82, 138), S(110, 138), S(113, 148), S(118, 161), S(121, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163)},
        {S(-24, 254), S(4, 284), S(19, 294), S(20, 303), S(20, 305), S(20, 323), S(20, 323), S(33, 333), S(33, 337), S(57, 347), S(57, 352), S(67, 353), S(67, 357), S(67, 357), S(89, 357)},
        {S(18, 291), S(43, 316), S(66, 341), S(75, 351), S(100, 351), S(125, 351), S(138, 362), S(149, 363), S(155, 377), S(171, 377), S(171, 380), S(174, 380), S(174, 380), S(176, 395), S(176, 402), S(180, 403), S(186, 403), S(202, 403), S(203, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(236, 403), S(237, 403)},
        {},
    },
    {S(0, 0), S(-39, 42), S(-39, 42), S(-39, 85), S(0, 143), S(32, 288), S(512, 378), S(0, 0)},
    {S(0, 0), S(-8, -7), S(51, 9), S(51, 9), S(65, 13), S(65, 79), S(245, 99), S(0, 0)},
    S(114, 12), // RookOpenFileBonus
    S(29, 10), // RookSemiOpenFileBonus
    S(0, 2), // RookOnQueenFile
    S(103, 10), // KnightOutpostBonus
    S(103, 11), // BishopOutpostBonus
    S(-99, 0), // TrappedRookByKingPenalty
    S(67, 3), // RookBehindOurPasserBonus
    S(-141, 105), // RookBehindTheirPasserBonus
    S(35, 4), // MinorBehindPawnBonus
    S(27, 0), // MinorOnKingRing
    S(5, 0), // RookOnKingRing
    S(-16, -2), // KingProtector
    S(8, 3), // BishopPair
    {
        {S(0, 0), S(106, 0), S(98, 0), S(67, 0), S(55, 0), S(15, 0), S(0, 0)},
        {S(-89, 0), S(193, 0), S(86, 0), S(0, 0), S(0, 0), S(86, 0), S(167, 0)},
        {S(-6, 0), S(202, 0), S(65, 0), S(37, 0), S(64, 0), S(67, 0), S(89, 0)},
        {S(-19, 0), S(156, 0), S(74, 0), S(40, 0), S(15, 0), S(124, 0), S(74, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(10, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(93, 0), S(1, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(8, 0), S(274, 0), S(2, 0), S(1, 0), S(2, 0), S(0, 0)},
        {S(0, 0), S(32, 0), S(333, 0), S(30, 0), S(2, 0), S(4, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(218, 0), S(7, 0), S(0, 0), S(44, 0), S(0, 0)}, // BlockedStorm
    S(-31, 0), // UndefendedKingZoneSq
    S(10, 49), // KingMobilityFactor
    S(30, 41), // KingAttackByKnight
    S(11, 45), // KingAttackByBishop
    S(32, 49), // KingAttackByRook
    S(32, 49), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(36, 299), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(23, 6), // KingRingWeakWeight
    S(28, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -6), // DoubledPawnPenalty
    S(-12, -6), // BackwardPawnPenalty
    S(-27, -16), // WeakUnopposedPenalty
    S(0, -39), // DoubledIsolatedPenalty
    {S(-103, -27), S(-95, -109)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-4, -21), // PawnIslandPenalty
    {S(29, 0), S(11, 0)}, // CentralPawnBonus
    S(62, 25), // BishopLongDiagonalBonus
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
    S(95, 0), // KnightOnQueen
    S(-322, -134), // PawnlessFlank
    S(0, 0), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 9), // KBNKCornerEg
    S(0, 300), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
