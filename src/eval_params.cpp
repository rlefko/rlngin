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
    S(254, 382), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(77, 2), // ThreatByKing
    S(55, 21), // Hanging
    S(50, 50), // WeakQueen
    S(5, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 28), S(0, 34), S(0, 35), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 32), S(0, 59), S(0, 72), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-23, -24), S(-23, -39), S(-23, -65), S(-229, -95), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(2, 51), S(103, 82), S(134, 375), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(32, 28), S(32, 28), S(103, 28), S(103, 28), S(0, 0)},
    S(0, 44), // RookOn7thBonus
    S(-17, 0), // BadBishop
    S(-6, -7), // BishopPawns
    S(37, 0), // Tempo
    {S(0, 0), S(203, 210), S(755, 588), S(832, 554), S(1238, 877), S(2236, 1833), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-44, -9), S(-28, -4), S(-21, -4), S(-37, -32), S(-22, -24), S(-2, 2), S(-27, -4), S(-47, -18),
        S(-77, -29), S(-68, -17), S(-48, -22), S(-69, -46), S(-46, -32), S(-44, -18), S(-61, -24), S(-88, -29),
        S(-36, -11), S(-46, -14), S(-23, -30), S(-1, -44), S(1, -43), S(-20, -26), S(-44, -19), S(-38, -18),
        S(6, 19), S(-5, 15), S(10, -2), S(86, -5), S(106, 2), S(19, 0), S(-9, 14), S(9, 17),
        S(20, 28), S(26, 32), S(48, 21), S(98, 24), S(101, 26), S(51, 22), S(25, 28), S(18, 32),
        S(20, 24), S(28, 27), S(53, 26), S(85, 33), S(86, 34), S(54, 26), S(27, 27), S(20, 25),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-142, -38), S(-162, -25), S(-124, -23), S(-80, -14),
        S(-107, -28), S(-87, -10), S(-85, -15), S(-69, -3),
        S(-100, -19), S(-47, -7), S(-28, -6), S(9, 16),
        S(-14, -3), S(8, 9), S(25, 27), S(24, 33),
        S(24, 3), S(42, 14), S(60, 34), S(67, 40),
        S(34, -13), S(60, 6), S(91, 24), S(108, 34),
        S(34, -20), S(50, -10), S(76, 2), S(102, 32),
        S(25, -30), S(46, -12), S(69, 6), S(84, 15)
    },
    // BishopPST (half-board)
    {
        S(-3, -21), S(17, -8), S(-85, -11), S(-79, -11),
        S(8, -16), S(-13, -39), S(-2, -3), S(-50, -7),
        S(-20, -15), S(7, 7), S(-12, -11), S(9, 15),
        S(-8, -13), S(11, 6), S(31, 18), S(28, 8),
        S(-31, -18), S(19, 9), S(34, 22), S(48, 17),
        S(9, -3), S(21, 17), S(33, 5), S(57, 26),
        S(-18, -12), S(-26, -7), S(13, 21), S(36, 25),
        S(-29, -13), S(-17, 10), S(-1, 10), S(10, 11)
    },
    // RookPST (half-board)
    {
        S(-100, -25), S(-46, -31), S(-41, -26), S(-10, -33),
        S(-77, -32), S(-19, -36), S(-25, -30), S(-12, -29),
        S(-51, -27), S(-4, -21), S(-14, -16), S(4, -22),
        S(-33, -7), S(7, 1), S(0, 8), S(7, -1),
        S(-29, 23), S(17, 34), S(37, 29), S(36, 19),
        S(-11, 27), S(33, 34), S(61, 34), S(82, 26),
        S(-20, -2), S(-7, 1), S(40, 6), S(64, 8),
        S(0, 20), S(18, 25), S(45, 24), S(48, 22)
    },
    // QueenPST (half-board)
    {
        S(2, -93), S(44, -85), S(21, -74), S(32, -59),
        S(16, -77), S(28, -76), S(48, -62), S(40, -39),
        S(18, -45), S(40, -20), S(42, 0), S(29, 2),
        S(38, -16), S(45, 10), S(36, 43), S(19, 48),
        S(1, -21), S(15, 11), S(3, 47), S(4, 75),
        S(6, 0), S(-14, 21), S(-19, 59), S(-7, 82),
        S(-45, 3), S(-87, 18), S(-53, 52), S(-33, 74),
        S(-68, 9), S(-69, 34), S(-65, 49), S(-55, 60)
    },
    // KingPST (half-board)
    {
        S(104, -149), S(66, -102), S(7, -75), S(-116, -57),
        S(111, -104), S(72, -70), S(-13, -51), S(-35, -41),
        S(17, -65), S(11, -44), S(-23, -29), S(-24, -21),
        S(-22, -24), S(-11, -2), S(-15, 4), S(-19, 5),
        S(-16, 21), S(-10, 36), S(-8, 38), S(-10, 34),
        S(-8, 46), S(-1, 62), S(-3, 62), S(-6, 57),
        S(-6, 48), S(-2, 60), S(-2, 62), S(-4, 63),
        S(-8, 49), S(-4, 56), S(-3, 61), S(-4, 63)
    },
    {
        {},
        {},
        {S(-112, -98), S(-72, -58), S(-37, -23), S(-17, 0), S(3, 15), S(20, 26), S(36, 26), S(48, 26), S(48, 26)},
        {S(-12, 82), S(18, 87), S(53, 106), S(69, 120), S(89, 144), S(102, 151), S(107, 155), S(111, 155), S(111, 155), S(114, 155), S(118, 155), S(118, 155), S(124, 155), S(124, 155)},
        {S(-48, 278), S(-21, 290), S(-6, 302), S(11, 307), S(11, 311), S(19, 321), S(23, 331), S(34, 334), S(45, 340), S(50, 346), S(54, 352), S(54, 355), S(54, 355), S(54, 355), S(54, 355)},
        {S(116, 255), S(116, 263), S(116, 288), S(116, 313), S(119, 334), S(128, 358), S(130, 370), S(141, 373), S(144, 386), S(154, 386), S(159, 386), S(164, 397), S(167, 403), S(170, 403), S(173, 403), S(176, 403), S(178, 403), S(181, 403), S(181, 403), S(188, 403), S(191, 403), S(191, 403), S(207, 403), S(207, 403), S(228, 403), S(228, 403), S(229, 403), S(238, 428)},
        {},
    },
    {S(0, 0), S(-39, 8), S(-39, 18), S(-39, 19), S(0, 111), S(121, 308), S(390, 499), S(0, 0)},
    {S(0, 0), S(-13, 1), S(27, 7), S(37, 8), S(57, 31), S(76, 73), S(76, 197), S(0, 0)},
    S(87, 15), // RookOpenFileBonus
    S(37, 15), // RookSemiOpenFileBonus
    S(11, 0), // RookOnQueenFile
    S(54, 25), // KnightOutpostBonus
    S(67, 13), // BishopOutpostBonus
    S(-46, 0), // TrappedRookByKingPenalty
    S(24, 28), // RookBehindOurPasserBonus
    S(-4, 85), // RookBehindTheirPasserBonus
    S(32, 12), // MinorBehindPawnBonus
    S(33, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-12, 0), // KingProtector
    S(48, 11), // BishopPair
    {
        {S(0, 0), S(88, 0), S(98, 0), S(61, 0), S(40, 0), S(51, 0), S(163, 0)},
        {S(-41, 0), S(162, 0), S(88, 0), S(46, 0), S(0, 0), S(48, 0), S(0, 0)},
        {S(0, 0), S(125, 0), S(42, 0), S(42, 0), S(17, 0), S(35, 0), S(72, 0)},
        {S(0, 0), S(91, 0), S(23, 0), S(18, 0), S(50, 0), S(18, 0), S(68, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(82, 0), S(13, 0), S(0, 0), S(12, 0)},
        {S(0, 0), S(0, 0), S(10, 0), S(17, 0), S(0, 0), S(0, 0), S(6, 0)},
        {S(0, 0), S(0, 0), S(108, 0), S(54, 0), S(15, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(48, 0), S(35, 0), S(2, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(151, 0), S(10, 0), S(11, 0), S(15, 0), S(23, 0)}, // BlockedStorm
    S(-26, -3), // UndefendedKingZoneSq
    S(14, 55), // KingMobilityFactor
    S(26, 7), // KingAttackByKnight
    S(4, 35), // KingAttackByBishop
    S(26, 7), // KingAttackByRook
    S(26, 7), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(34, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(5, 0), // KingRingWeakWeight
    S(8, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-3, -28), // DoubledPawnPenalty
    S(-13, -6), // BackwardPawnPenalty
    S(-32, -12), // WeakUnopposedPenalty
    S(-39, -44), // DoubledIsolatedPenalty
    {S(-4, -25), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-37, -17), // PawnIslandPenalty
    {S(29, 0), S(3, 0)}, // CentralPawnBonus
    S(28, 20), // BishopLongDiagonalBonus
    S(-20, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 29), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(60, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(10, 2), // RestrictedPiece
    S(39, 42), // ThreatByPawnPush
    S(-20, -3), // WeakQueenDefender
    S(44, 2), // KnightOnQueen
    S(-223, -72), // PawnlessFlank
    S(0, 45), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 40), // KBNKCornerEg
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
    S(0, 24), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
