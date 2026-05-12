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
    S(236, 225), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(154, 76), S(61, 244), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(134, 293), S(0, 0)},
    S(81, 1), // ThreatByKing
    S(59, 20), // Hanging
    S(503, 82), // WeakQueen
    S(22, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 28), S(0, 36), S(0, 37), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 31), S(0, 58), S(0, 70), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-23, -25), S(-23, -41), S(-23, -67), S(-245, -94), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(3, 52), S(113, 81), S(126, 379), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(32, 28), S(32, 28), S(102, 28), S(102, 28), S(0, 0)},
    S(0, 44), // RookOn7thBonus
    S(-14, 0), // BadBishop
    S(-6, -7), // BishopPawns
    S(42, 0), // Tempo
    {S(0, 0), S(203, 210), S(755, 588), S(832, 554), S(1238, 877), S(2236, 1833), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-44, -9), S(-27, -5), S(-21, -3), S(-37, -32), S(-22, -24), S(-3, 3), S(-27, -4), S(-48, -18),
        S(-78, -29), S(-69, -18), S(-48, -22), S(-70, -47), S(-47, -33), S(-45, -18), S(-62, -25), S(-88, -30),
        S(-37, -11), S(-47, -14), S(-22, -30), S(0, -45), S(3, -43), S(-20, -25), S(-45, -20), S(-40, -19),
        S(4, 20), S(-6, 15), S(11, -2), S(88, -4), S(107, 3), S(19, 0), S(-11, 14), S(6, 18),
        S(19, 29), S(25, 32), S(48, 20), S(98, 23), S(101, 25), S(50, 21), S(24, 28), S(17, 32),
        S(21, 25), S(28, 26), S(53, 26), S(83, 32), S(84, 33), S(53, 26), S(27, 25), S(21, 26),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-145, -37), S(-166, -25), S(-131, -23), S(-85, -14),
        S(-109, -29), S(-88, -11), S(-88, -16), S(-71, -4),
        S(-101, -20), S(-46, -9), S(-27, -8), S(10, 16),
        S(-12, -4), S(11, 7), S(28, 24), S(28, 29),
        S(23, 3), S(46, 13), S(62, 32), S(70, 37),
        S(37, -11), S(62, 6), S(91, 24), S(105, 36),
        S(35, -18), S(49, -9), S(74, 2), S(102, 33),
        S(26, -26), S(46, -9), S(69, 10), S(86, 19)
    },
    // BishopPST (half-board)
    {
        S(-3, -18), S(16, -8), S(-88, -11), S(-82, -12),
        S(7, -15), S(-14, -39), S(-2, -4), S(-53, -7),
        S(-21, -16), S(8, 5), S(-11, -11), S(9, 14),
        S(-10, -16), S(15, 3), S(35, 14), S(29, 7),
        S(-30, -20), S(21, 7), S(38, 20), S(49, 18),
        S(10, -3), S(21, 17), S(34, 6), S(56, 27),
        S(-17, -11), S(-28, -6), S(11, 21), S(31, 25),
        S(-26, -13), S(-16, 8), S(0, 8), S(9, 10)
    },
    // RookPST (half-board)
    {
        S(-102, -25), S(-48, -31), S(-43, -25), S(-11, -32),
        S(-81, -32), S(-18, -36), S(-25, -31), S(-12, -28),
        S(-51, -28), S(-3, -22), S(-13, -17), S(6, -22),
        S(-31, -9), S(8, 0), S(3, 6), S(10, -1),
        S(-27, 21), S(18, 33), S(41, 27), S(40, 18),
        S(-8, 27), S(35, 35), S(60, 35), S(82, 27),
        S(-18, -1), S(-3, 0), S(40, 6), S(63, 11),
        S(-1, 20), S(19, 25), S(44, 25), S(47, 22)
    },
    // QueenPST (half-board)
    {
        S(1, -90), S(37, -80), S(15, -71), S(28, -56),
        S(16, -78), S(23, -73), S(44, -56), S(36, -36),
        S(16, -45), S(39, -23), S(42, -3), S(28, 1),
        S(35, -14), S(47, 8), S(37, 39), S(21, 44),
        S(1, -26), S(17, 8), S(6, 42), S(7, 70),
        S(6, -5), S(-12, 19), S(-15, 55), S(-5, 81),
        S(-44, 2), S(-85, 18), S(-51, 51), S(-31, 71),
        S(-69, 8), S(-71, 33), S(-63, 48), S(-54, 57)
    },
    // KingPST (half-board)
    {
        S(116, -148), S(76, -101), S(13, -74), S(-115, -57),
        S(123, -103), S(82, -69), S(-8, -50), S(-35, -39),
        S(23, -65), S(13, -42), S(-21, -27), S(-25, -19),
        S(-16, -23), S(-8, -1), S(-15, 7), S(-24, 7),
        S(-12, 21), S(-12, 35), S(-12, 39), S(-18, 34),
        S(-8, 45), S(-5, 61), S(-6, 62), S(-11, 56),
        S(-10, 47), S(-7, 58), S(-9, 60), S(-10, 62),
        S(-11, 48), S(-9, 54), S(-8, 59), S(-7, 62)
    },
    {
        {},
        {},
        {S(-112, -98), S(-72, -58), S(-36, -22), S(-15, 0), S(6, 15), S(23, 26), S(40, 26), S(51, 26), S(51, 26)},
        {S(-14, 82), S(18, 86), S(53, 106), S(70, 121), S(92, 144), S(105, 151), S(111, 155), S(114, 155), S(114, 155), S(118, 155), S(122, 155), S(124, 155), S(124, 155), S(124, 155)},
        {S(-47, 273), S(-21, 293), S(-5, 302), S(11, 309), S(11, 312), S(19, 323), S(25, 332), S(35, 335), S(46, 340), S(52, 346), S(54, 352), S(54, 355), S(54, 355), S(54, 355), S(54, 355)},
        {S(116, 238), S(116, 263), S(116, 288), S(116, 313), S(116, 338), S(125, 362), S(128, 372), S(140, 372), S(143, 385), S(153, 385), S(157, 389), S(162, 399), S(166, 403), S(170, 403), S(172, 403), S(176, 403), S(179, 403), S(179, 403), S(182, 403), S(188, 403), S(196, 403), S(196, 403), S(207, 403), S(207, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 428)},
        {},
    },
    {S(0, 0), S(-39, 9), S(-39, 19), S(-39, 20), S(0, 108), S(130, 301), S(410, 489), S(0, 0)},
    {S(0, 0), S(-14, 2), S(28, 8), S(39, 9), S(61, 31), S(79, 74), S(79, 189), S(0, 0)},
    S(90, 14), // RookOpenFileBonus
    S(37, 14), // RookSemiOpenFileBonus
    S(15, 0), // RookOnQueenFile
    S(53, 28), // KnightOutpostBonus
    S(67, 16), // BishopOutpostBonus
    S(-48, 0), // TrappedRookByKingPenalty
    S(26, 28), // RookBehindOurPasserBonus
    S(-15, 86), // RookBehindTheirPasserBonus
    S(33, 13), // MinorBehindPawnBonus
    S(34, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-13, 0), // KingProtector
    S(48, 11), // BishopPair
    {
        {S(0, 0), S(91, 0), S(102, 0), S(62, 0), S(44, 0), S(49, 0), S(165, 0)},
        {S(-49, 0), S(163, 0), S(88, 0), S(43, 0), S(0, 0), S(35, 0), S(0, 0)},
        {S(0, 0), S(130, 0), S(44, 0), S(44, 0), S(17, 0), S(38, 0), S(70, 0)},
        {S(0, 0), S(91, 0), S(25, 0), S(19, 0), S(51, 0), S(26, 0), S(71, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(85, 0), S(18, 0), S(0, 0), S(15, 0)},
        {S(0, 0), S(0, 0), S(10, 0), S(16, 0), S(0, 0), S(0, 0), S(7, 0)},
        {S(0, 0), S(0, 0), S(119, 0), S(56, 0), S(16, 0), S(0, 0), S(1, 0)},
        {S(0, 0), S(0, 0), S(53, 0), S(32, 0), S(3, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(158, 0), S(12, 0), S(13, 0), S(14, 0), S(21, 0)}, // BlockedStorm
    S(-27, -4), // UndefendedKingZoneSq
    S(15, 55), // KingMobilityFactor
    S(27, 0), // KingAttackByKnight
    S(4, 0), // KingAttackByBishop
    S(28, 7), // KingAttackByRook
    S(28, 8), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(34, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(4, 0), // KingRingWeakWeight
    S(6, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-2, -26), // DoubledPawnPenalty
    S(-16, -5), // BackwardPawnPenalty
    S(-32, -12), // WeakUnopposedPenalty
    S(-38, -45), // DoubledIsolatedPenalty
    {S(-4, -25), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-38, -16), // PawnIslandPenalty
    {S(30, 0), S(3, 0)}, // CentralPawnBonus
    S(28, 19), // BishopLongDiagonalBonus
    S(-19, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 46), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(53, 89), // SliderOnQueenBishop
    S(100, 58), // SliderOnQueenRook
    S(10, 2), // RestrictedPiece
    S(27, 32), // ThreatByPawnPush
    S(-21, -4), // WeakQueenDefender
    S(42, 0), // KnightOnQueen
    S(-232, -73), // PawnlessFlank
    S(0, 44), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 39), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 25), // KBNKPushClose
    S(0, 30), // KQKRPushToEdge
    S(0, 15), // KQKRPushClose
    S(0, 0), // KPsKFortressScale
    S(0, 1), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
    S(0, 54), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
