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
    S(161, 33), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(110, 0), S(87, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(139, 0), S(0, 0)},
    S(87, 0), // ThreatByKing
    S(62, 19), // Hanging
    S(17, 0), // WeakQueen
    S(24, 16), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 18), S(0, 30), S(0, 37), S(0, 37), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 11), S(0, 30), S(0, 56), S(0, 66), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-23, -24), S(-23, -41), S(-23, -65), S(-258, -93), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(10, 49), S(124, 82), S(124, 378), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(35, 28), S(35, 28), S(101, 28), S(101, 28), S(0, 0)},
    S(0, 44), // RookOn7thBonus
    S(-15, 0), // BadBishop
    S(-6, -6), // BishopPawns
    S(46, 0), // Tempo
    {S(0, 0), S(211, 208), S(768, 569), S(851, 532), S(1259, 849), S(2262, 1782), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-45, -10), S(-26, -6), S(-20, -4), S(-38, -34), S(-24, -25), S(-2, 1), S(-26, -6), S(-49, -20),
        S(-78, -32), S(-66, -20), S(-45, -23), S(-70, -48), S(-47, -34), S(-41, -20), S(-58, -27), S(-88, -31),
        S(-37, -12), S(-46, -15), S(-22, -29), S(1, -43), S(3, -41), S(-20, -25), S(-45, -19), S(-40, -20),
        S(3, 20), S(-8, 16), S(11, -1), S(91, -3), S(109, 5), S(19, 1), S(-13, 14), S(5, 18),
        S(16, 32), S(23, 34), S(47, 21), S(97, 25), S(100, 27), S(49, 22), S(22, 30), S(14, 35),
        S(18, 28), S(27, 28), S(51, 27), S(81, 33), S(82, 34), S(51, 27), S(26, 27), S(18, 29),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-154, -35), S(-176, -25), S(-139, -25), S(-91, -16),
        S(-116, -27), S(-95, -12), S(-95, -18), S(-77, -5),
        S(-106, -21), S(-50, -9), S(-27, -9), S(9, 14),
        S(-7, -3), S(16, 8), S(37, 25), S(34, 29),
        S(28, 3), S(54, 14), S(69, 33), S(77, 37),
        S(36, -11), S(64, 7), S(94, 25), S(106, 35),
        S(37, -15), S(54, -8), S(77, 1), S(100, 31),
        S(30, -24), S(49, -9), S(69, 6), S(84, 15)
    },
    // BishopPST (half-board)
    {
        S(-5, -20), S(13, -8), S(-94, -12), S(-90, -14),
        S(6, -14), S(-16, -39), S(-5, -4), S(-59, -8),
        S(-22, -16), S(7, 6), S(-11, -13), S(8, 13),
        S(-7, -13), S(16, 6), S(42, 14), S(33, 7),
        S(-28, -18), S(28, 9), S(42, 20), S(53, 17),
        S(9, -1), S(21, 19), S(33, 6), S(55, 27),
        S(-16, -12), S(-26, -7), S(12, 20), S(29, 26),
        S(-24, -16), S(-16, 7), S(-2, 8), S(6, 10)
    },
    // RookPST (half-board)
    {
        S(-106, -24), S(-49, -31), S(-43, -26), S(-10, -31),
        S(-83, -34), S(-18, -35), S(-25, -30), S(-13, -27),
        S(-53, -27), S(-1, -21), S(-10, -17), S(6, -20),
        S(-27, -8), S(12, 3), S(7, 10), S(14, 0),
        S(-25, 23), S(20, 34), S(41, 28), S(43, 20),
        S(-7, 27), S(36, 35), S(60, 36), S(80, 29),
        S(-18, -2), S(-4, 0), S(37, 7), S(63, 11),
        S(-3, 18), S(15, 24), S(40, 25), S(44, 21)
    },
    // QueenPST (half-board)
    {
        S(-3, -87), S(37, -79), S(13, -74), S(29, -58),
        S(13, -76), S(24, -74), S(44, -56), S(37, -39),
        S(13, -44), S(41, -24), S(46, -8), S(28, -1),
        S(41, -13), S(54, 9), S(45, 35), S(26, 42),
        S(4, -22), S(20, 10), S(14, 38), S(12, 68),
        S(6, 0), S(-14, 21), S(-17, 56), S(-6, 80),
        S(-47, 4), S(-88, 16), S(-52, 50), S(-34, 69),
        S(-72, 12), S(-75, 33), S(-65, 48), S(-51, 54)
    },
    // KingPST (half-board)
    {
        S(123, -148), S(83, -101), S(17, -75), S(-120, -56),
        S(130, -105), S(88, -71), S(-7, -51), S(-41, -40),
        S(26, -68), S(16, -44), S(-23, -28), S(-30, -20),
        S(-16, -26), S(-7, -3), S(-17, 6), S(-28, 6),
        S(-16, 19), S(-11, 34), S(-13, 38), S(-18, 34),
        S(-12, 44), S(-5, 61), S(-7, 61), S(-12, 55),
        S(-13, 48), S(-8, 59), S(-10, 60), S(-12, 62),
        S(-15, 48), S(-11, 55), S(-10, 60), S(-10, 62)
    },
    {
        {},
        {},
        {S(-112, -98), S(-72, -58), S(-35, -21), S(-13, 0), S(9, 15), S(28, 26), S(44, 26), S(53, 26), S(53, 26)},
        {S(-18, 83), S(17, 88), S(51, 108), S(71, 122), S(94, 144), S(109, 151), S(113, 155), S(116, 155), S(116, 155), S(121, 155), S(124, 155), S(124, 155), S(124, 155), S(124, 155)},
        {S(-57, 282), S(-28, 298), S(-9, 305), S(9, 311), S(9, 314), S(19, 323), S(24, 332), S(36, 335), S(46, 341), S(52, 347), S(54, 352), S(54, 355), S(54, 355), S(54, 355), S(54, 355)},
        {S(99, 278), S(99, 303), S(104, 306), S(110, 317), S(111, 342), S(121, 366), S(125, 375), S(139, 375), S(142, 385), S(153, 385), S(157, 388), S(162, 397), S(165, 403), S(167, 403), S(172, 403), S(173, 403), S(179, 403), S(179, 403), S(179, 403), S(185, 403), S(196, 403), S(196, 403), S(210, 403), S(219, 403), S(228, 403), S(228, 403), S(228, 403), S(239, 428)},
        {},
    },
    {S(0, 0), S(-39, 12), S(-39, 20), S(-39, 22), S(0, 103), S(141, 292), S(436, 474), S(0, 0)},
    {S(0, 0), S(-14, 3), S(26, 11), S(38, 11), S(65, 31), S(82, 70), S(82, 173), S(0, 0)},
    S(96, 11), // RookOpenFileBonus
    S(39, 11), // RookSemiOpenFileBonus
    S(11, 0), // RookOnQueenFile
    S(49, 24), // KnightOutpostBonus
    S(65, 13), // BishopOutpostBonus
    S(-52, 0), // TrappedRookByKingPenalty
    S(27, 26), // RookBehindOurPasserBonus
    S(-23, 83), // RookBehindTheirPasserBonus
    S(34, 13), // MinorBehindPawnBonus
    S(37, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-14, 0), // KingProtector
    S(54, 10), // BishopPair
    {
        {S(0, 0), S(96, 0), S(107, 0), S(65, 0), S(45, 0), S(49, 0), S(172, 0)},
        {S(-56, 0), S(168, 0), S(89, 0), S(41, 0), S(0, 0), S(39, 0), S(0, 0)},
        {S(0, 0), S(136, 0), S(45, 0), S(45, 0), S(17, 0), S(43, 0), S(92, 0)},
        {S(0, 0), S(97, 0), S(28, 0), S(22, 0), S(54, 0), S(22, 0), S(70, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(90, 0), S(19, 0), S(0, 0), S(16, 0)},
        {S(0, 0), S(0, 0), S(15, 0), S(16, 0), S(0, 0), S(0, 0), S(9, 0)},
        {S(0, 0), S(0, 0), S(138, 0), S(60, 0), S(18, 0), S(0, 0), S(2, 0)},
        {S(0, 0), S(0, 0), S(72, 0), S(33, 0), S(1, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(165, 0), S(11, 0), S(15, 0), S(13, 0), S(17, 0)}, // BlockedStorm
    S(-31, -4), // UndefendedKingZoneSq
    S(14, 55), // KingMobilityFactor
    S(28, 0), // KingAttackByKnight
    S(5, 0), // KingAttackByBishop
    S(28, 8), // KingAttackByRook
    S(28, 8), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(34, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(4, 0), // KingRingWeakWeight
    S(5, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-1, -25), // DoubledPawnPenalty
    S(-19, -5), // BackwardPawnPenalty
    S(-33, -10), // WeakUnopposedPenalty
    S(-41, -44), // DoubledIsolatedPenalty
    {S(0, -23), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-40, -16), // PawnIslandPenalty
    {S(32, 0), S(4, 0)}, // CentralPawnBonus
    S(28, 20), // BishopLongDiagonalBonus
    S(-19, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(55, 30), // SliderOnQueenBishop
    S(22, 0), // SliderOnQueenRook
    S(10, 0), // RestrictedPiece
    S(29, 0), // ThreatByPawnPush
    S(-22, -7), // WeakQueenDefender
    S(41, 0), // KnightOnQueen
    S(-250, -72), // PawnlessFlank
    S(0, 36), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 50), // KBNKCornerEg
    S(0, 300), // LucenaEg
    S(0, 50), // KXKPushToEdge
    S(0, 50), // KXKPushClose
    S(0, 50), // KBNKPushClose
    S(0, 50), // KQKRPushToEdge
    S(0, 50), // KQKRPushClose
    S(0, 0), // KPsKFortressScale
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
