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
    S(196, 24), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(149, 0), S(137, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(200, 0), S(0, 0)},
    S(110, 1), // ThreatByKing
    S(86, 17), // Hanging
    S(42, 0), // WeakQueen
    S(23, 14), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 15), S(0, 26), S(0, 34), S(0, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 14), S(0, 30), S(0, 60), S(0, 72), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-30, -17), S(-30, -32), S(-30, -50), S(-318, -78), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(16, 13), S(31, 39), S(114, 78), S(114, 345), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-4, 28), S(48, 28), S(51, 28), S(51, 28), S(0, 0)},
    S(0, 33), // RookOn7thBonus
    S(-9, 0), // BadBishop
    S(-3, -6), // BishopPawns
    S(51, 0), // Tempo
    {S(0, 0), S(194, 162), S(841, 505), S(912, 453), S(1339, 701), S(2389, 1564), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-35, -9), S(-14, -9), S(-10, -9), S(-20, -25), S(-8, -14), S(4, -2), S(-14, -10), S(-37, -18),
        S(-65, -24), S(-54, -19), S(-29, -22), S(-52, -37), S(-28, -24), S(-26, -17), S(-46, -26), S(-73, -25),
        S(-35, -7), S(-42, -13), S(-16, -25), S(11, -32), S(16, -31), S(-13, -20), S(-37, -15), S(-36, -14),
        S(-2, 22), S(-14, 13), S(4, -2), S(78, -6), S(94, 3), S(12, -1), S(-20, 12), S(-1, 19),
        S(6, 27), S(12, 29), S(34, 10), S(83, 19), S(86, 22), S(36, 11), S(11, 25), S(5, 32),
        S(5, 21), S(15, 21), S(39, 18), S(68, 27), S(69, 29), S(40, 19), S(15, 21), S(6, 23),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-146, -28), S(-167, -17), S(-126, -21), S(-85, -9),
        S(-108, -20), S(-87, -7), S(-80, -15), S(-61, -7),
        S(-94, -15), S(-34, -8), S(-7, -12), S(20, 11),
        S(-3, 3), S(26, 8), S(48, 20), S(47, 23),
        S(34, 5), S(63, 12), S(79, 25), S(85, 29),
        S(31, -12), S(55, 6), S(82, 22), S(98, 32),
        S(24, -21), S(37, -9), S(59, 0), S(75, 28),
        S(12, -39), S(29, -15), S(46, 6), S(56, 14)
    },
    // BishopPST (half-board)
    {
        S(3, -18), S(20, -6), S(-86, -7), S(-76, -8),
        S(12, -13), S(-4, -31), S(5, -4), S(-47, -6),
        S(-11, -12), S(19, 5), S(0, -13), S(13, 12),
        S(0, -10), S(23, 3), S(46, 12), S(29, 5),
        S(-25, -13), S(35, 9), S(43, 16), S(43, 11),
        S(10, 1), S(18, 17), S(30, 4), S(40, 21),
        S(-20, -13), S(-34, -9), S(-5, 17), S(6, 22),
        S(-32, -12), S(-30, 11), S(-21, 9), S(-16, 10)
    },
    // RookPST (half-board)
    {
        S(-87, -24), S(-37, -22), S(-28, -19), S(3, -23),
        S(-74, -29), S(-23, -26), S(-24, -22), S(-18, -19),
        S(-50, -22), S(-10, -15), S(-23, -11), S(-11, -12),
        S(-33, -4), S(0, 4), S(-11, 9), S(3, 2),
        S(-23, 14), S(12, 28), S(26, 23), S(30, 15),
        S(6, 17), S(27, 28), S(46, 28), S(65, 23),
        S(4, -6), S(10, -1), S(43, 3), S(57, 10),
        S(12, 14), S(18, 22), S(38, 20), S(43, 18)
    },
    // QueenPST (half-board)
    {
        S(4, -75), S(38, -67), S(23, -64), S(47, -58),
        S(17, -66), S(36, -67), S(57, -56), S(56, -44),
        S(22, -49), S(48, -28), S(57, -13), S(34, -10),
        S(44, -20), S(54, 4), S(39, 24), S(25, 27),
        S(19, -21), S(24, 12), S(15, 39), S(11, 60),
        S(6, 1), S(-17, 24), S(-23, 54), S(-7, 77),
        S(-64, 10), S(-101, 23), S(-68, 51), S(-43, 68),
        S(-94, 17), S(-92, 40), S(-80, 49), S(-63, 52)
    },
    // KingPST (half-board)
    {
        S(156, -124), S(123, -90), S(39, -62), S(-109, -55),
        S(165, -86), S(117, -60), S(4, -42), S(-50, -33),
        S(43, -58), S(30, -38), S(-11, -25), S(-39, -18),
        S(-5, -24), S(-4, -3), S(-17, 3), S(-37, 2),
        S(-15, 17), S(-16, 32), S(-22, 35), S(-35, 29),
        S(-19, 40), S(-17, 57), S(-23, 60), S(-31, 52),
        S(-25, 40), S(-23, 52), S(-25, 56), S(-29, 58),
        S(-29, 39), S(-26, 46), S(-26, 53), S(-27, 56)
    },
    {
        {},
        {},
        {S(-109, -91), S(-69, -51), S(-29, -15), S(-4, 0), S(17, 13), S(35, 24), S(51, 26), S(56, 26), S(56, 26)},
        {S(-18, 79), S(17, 93), S(52, 115), S(75, 126), S(98, 141), S(113, 147), S(116, 152), S(119, 154), S(122, 154), S(124, 155), S(124, 155), S(124, 155), S(124, 155), S(124, 155)},
        {S(-52, 282), S(-22, 298), S(-1, 304), S(20, 311), S(20, 313), S(26, 324), S(29, 331), S(41, 334), S(48, 339), S(54, 345), S(54, 350), S(54, 354), S(54, 355), S(54, 355), S(54, 355)},
        {S(36, 316), S(36, 316), S(61, 341), S(86, 341), S(100, 343), S(113, 358), S(119, 364), S(137, 365), S(137, 381), S(150, 381), S(157, 383), S(164, 389), S(168, 394), S(168, 397), S(170, 401), S(174, 401), S(177, 403), S(180, 403), S(181, 403), S(191, 403), S(212, 403), S(218, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(251, 427)},
        {},
    },
    {S(0, 0), S(-39, 20), S(-39, 25), S(-39, 48), S(0, 116), S(137, 307), S(528, 478), S(0, 0)},
    {S(0, 0), S(-13, -2), S(20, 6), S(43, 6), S(81, 21), S(85, 68), S(301, 140), S(0, 0)},
    S(114, 8), // RookOpenFileBonus
    S(51, 8), // RookSemiOpenFileBonus
    S(11, 0), // RookOnQueenFile
    S(58, 14), // KnightOutpostBonus
    S(70, 11), // BishopOutpostBonus
    S(-60, 0), // TrappedRookByKingPenalty
    S(49, 9), // RookBehindOurPasserBonus
    S(-39, 81), // RookBehindTheirPasserBonus
    S(33, 8), // MinorBehindPawnBonus
    S(37, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-15, -1), // KingProtector
    S(41, 0), // BishopPair
    {
        {S(0, 0), S(121, 0), S(135, 0), S(88, 0), S(66, 0), S(65, 0), S(80, 0)},
        {S(-80, 0), S(192, 0), S(112, 0), S(27, 0), S(0, 0), S(32, 0), S(39, 0)},
        {S(-2, 0), S(158, 0), S(55, 0), S(46, 0), S(21, 0), S(84, 0), S(107, 0)},
        {S(0, 0), S(104, 0), S(70, 0), S(48, 0), S(45, 0), S(23, 0), S(53, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(69, 0), S(13, 0), S(0, 0), S(12, 0)},
        {S(0, 0), S(0, 0), S(92, 0), S(24, 0), S(1, 0), S(0, 0), S(6, 0)},
        {S(0, 0), S(0, 0), S(255, 0), S(72, 0), S(25, 0), S(4, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(176, 0), S(37, 0), S(6, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(154, 0), S(6, 0), S(11, 0), S(0, 0), S(0, 0)}, // BlockedStorm
    S(-45, 0), // UndefendedKingZoneSq
    S(12, 95), // KingMobilityFactor
    S(30, 0), // KingAttackByKnight
    S(8, 0), // KingAttackByBishop
    S(31, 34), // KingAttackByRook
    S(31, 34), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(34, 567), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(8, 0), // KingRingWeakWeight
    S(13, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-4, -16), // DoubledPawnPenalty
    S(-25, -5), // BackwardPawnPenalty
    S(-30, -10), // WeakUnopposedPenalty
    S(-28, -38), // DoubledIsolatedPenalty
    {S(-7, -22), S(0, -3)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-29, -20), // PawnIslandPenalty
    {S(28, 0), S(4, 0)}, // CentralPawnBonus
    S(37, 18), // BishopLongDiagonalBonus
    S(-16, 0), // BishopXrayPawns
    S(0, 31), // InitiativePasser
    S(0, 39), // InitiativePawnCount
    S(0, 7), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 47), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(65, 17), // SliderOnQueenBishop
    S(31, 0), // SliderOnQueenRook
    S(10, 2), // RestrictedPiece
    S(34, 0), // ThreatByPawnPush
    S(-26, -8), // WeakQueenDefender
    S(66, 0), // KnightOnQueen
    S(-290, -73), // PawnlessFlank
    S(0, 7), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 309), // KBNKCornerEg
    S(0, 841), // LucenaEg
    S(0, 50), // KXKPushToEdge
    S(0, 50), // KXKPushClose
    S(0, 50), // KBNKPushClose
    S(0, 50), // KQKRPushToEdge
    S(0, 50), // KQKRPushClose
    S(0, 1), // KPsKFortressScale
    S(0, 2), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
