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
    S(183, 25), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(138, 0), S(113, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(175, 0), S(0, 0)},
    S(100, 2), // ThreatByKing
    S(77, 15), // Hanging
    S(32, 0), // WeakQueen
    S(25, 13), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 14), S(0, 25), S(0, 31), S(0, 32), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 29), S(0, 58), S(0, 70), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-30, -16), S(-30, -31), S(-30, -47), S(-303, -69), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(11, 15), S(29, 38), S(108, 75), S(108, 366), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(18, 28), S(37, 28), S(85, 28), S(145, 28), S(0, 0)},
    S(0, 34), // RookOn7thBonus
    S(-9, 0), // BadBishop
    S(-4, -6), // BishopPawns
    S(52, 0), // Tempo
    {S(0, 0), S(200, 167), S(825, 518), S(899, 468), S(1322, 732), S(2355, 1609), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-39, -11), S(-18, -9), S(-10, -8), S(-25, -29), S(-13, -18), S(5, -1), S(-18, -9), S(-41, -18),
        S(-70, -28), S(-57, -21), S(-35, -23), S(-59, -40), S(-36, -26), S(-32, -18), S(-49, -27), S(-80, -27),
        S(-35, -10), S(-43, -14), S(-18, -26), S(7, -34), S(11, -33), S(-15, -21), S(-41, -17), S(-36, -16),
        S(0, 18), S(-12, 13), S(6, -2), S(85, -5), S(103, 5), S(14, 1), S(-16, 12), S(3, 17),
        S(9, 24), S(14, 28), S(38, 15), S(88, 24), S(91, 27), S(40, 17), S(14, 24), S(8, 29),
        S(10, 21), S(18, 22), S(42, 21), S(72, 33), S(73, 35), S(43, 22), S(18, 22), S(11, 23),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-151, -29), S(-174, -19), S(-135, -19), S(-88, -8),
        S(-114, -21), S(-93, -7), S(-88, -15), S(-69, -5),
        S(-101, -17), S(-43, -6), S(-18, -9), S(16, 12),
        S(-5, 1), S(22, 8), S(42, 22), S(41, 25),
        S(31, 5), S(59, 13), S(74, 28), S(83, 31),
        S(37, -13), S(62, 7), S(90, 23), S(105, 33),
        S(32, -19), S(48, -8), S(71, 0), S(89, 30),
        S(22, -32), S(40, -12), S(59, 6), S(71, 15)
    },
    // BishopPST (half-board)
    {
        S(5, -17), S(18, -2), S(-88, -7), S(-82, -8),
        S(13, -12), S(-8, -32), S(1, -1), S(-52, -5),
        S(-16, -13), S(13, 6), S(-3, -13), S(14, 12),
        S(-2, -11), S(22, 5), S(46, 14), S(36, 6),
        S(-27, -16), S(32, 8), S(44, 18), S(52, 12),
        S(9, 0), S(19, 17), S(30, 5), S(49, 24),
        S(-19, -12), S(-29, -9), S(3, 18), S(18, 25),
        S(-33, -14), S(-25, 10), S(-11, 9), S(-6, 11)
    },
    // RookPST (half-board)
    {
        S(-99, -24), S(-41, -26), S(-35, -21), S(0, -27),
        S(-77, -29), S(-18, -28), S(-24, -24), S(-12, -21),
        S(-46, -25), S(-1, -17), S(-16, -12), S(2, -15),
        S(-26, -7), S(9, 3), S(-1, 8), S(10, 1),
        S(-26, 15), S(17, 30), S(35, 23), S(36, 15),
        S(-4, 20), S(30, 30), S(50, 32), S(70, 24),
        S(-9, -4), S(3, -1), S(40, 5), S(62, 10),
        S(2, 18), S(16, 24), S(36, 23), S(38, 21)
    },
    // QueenPST (half-board)
    {
        S(7, -79), S(46, -70), S(25, -68), S(40, -58),
        S(19, -70), S(32, -70), S(53, -57), S(48, -44),
        S(21, -46), S(44, -26), S(51, -13), S(33, -12),
        S(45, -18), S(55, 3), S(42, 24), S(27, 26),
        S(14, -20), S(20, 12), S(8, 36), S(4, 63),
        S(4, 3), S(-19, 23), S(-24, 53), S(-12, 74),
        S(-57, 9), S(-98, 20), S(-62, 48), S(-43, 67),
        S(-85, 19), S(-88, 41), S(-75, 50), S(-61, 54)
    },
    // KingPST (half-board)
    {
        S(153, -135), S(109, -95), S(36, -70), S(-111, -57),
        S(159, -96), S(109, -66), S(5, -49), S(-44, -38),
        S(39, -63), S(27, -43), S(-16, -30), S(-34, -22),
        S(-7, -26), S(-6, -5), S(-20, 1), S(-31, 0),
        S(-15, 19), S(-15, 33), S(-22, 35), S(-33, 29),
        S(-16, 45), S(-14, 62), S(-19, 62), S(-27, 54),
        S(-21, 48), S(-18, 60), S(-21, 62), S(-25, 63),
        S(-25, 48), S(-22, 55), S(-22, 60), S(-23, 63)
    },
    {
        {},
        {},
        {S(-109, -92), S(-69, -52), S(-33, -15), S(-8, 1), S(14, 14), S(32, 25), S(48, 26), S(54, 26), S(54, 26)},
        {S(-18, 88), S(17, 97), S(52, 117), S(74, 127), S(97, 144), S(112, 149), S(116, 153), S(118, 155), S(118, 155), S(123, 155), S(124, 155), S(124, 155), S(124, 155), S(124, 155)},
        {S(-55, 288), S(-25, 300), S(-6, 308), S(15, 312), S(15, 315), S(24, 324), S(28, 332), S(40, 335), S(49, 340), S(54, 345), S(54, 351), S(54, 355), S(54, 355), S(54, 355), S(54, 355)},
        {S(61, 315), S(61, 316), S(61, 341), S(86, 341), S(101, 343), S(112, 362), S(119, 366), S(134, 368), S(134, 383), S(147, 384), S(154, 386), S(162, 391), S(166, 395), S(166, 399), S(168, 401), S(169, 403), S(177, 403), S(177, 403), S(178, 403), S(186, 403), S(204, 403), S(204, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(229, 428)},
        {},
    },
    {S(0, 0), S(-39, 18), S(-39, 24), S(-39, 40), S(0, 111), S(140, 300), S(505, 463), S(0, 0)},
    {S(0, 0), S(-14, -1), S(24, 7), S(41, 7), S(75, 22), S(89, 64), S(147, 157), S(0, 0)},
    S(105, 10), // RookOpenFileBonus
    S(46, 10), // RookSemiOpenFileBonus
    S(11, 0), // RookOnQueenFile
    S(53, 17), // KnightOutpostBonus
    S(68, 12), // BishopOutpostBonus
    S(-56, 0), // TrappedRookByKingPenalty
    S(36, 12), // RookBehindOurPasserBonus
    S(-29, 77), // RookBehindTheirPasserBonus
    S(33, 10), // MinorBehindPawnBonus
    S(40, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-15, 0), // KingProtector
    S(48, 0), // BishopPair
    {
        {S(0, 0), S(114, 0), S(128, 0), S(83, 0), S(60, 0), S(70, 0), S(130, 0)},
        {S(-70, 0), S(187, 0), S(103, 0), S(39, 0), S(0, 0), S(41, 0), S(15, 0)},
        {S(0, 0), S(152, 0), S(55, 0), S(51, 0), S(21, 0), S(66, 0), S(119, 0)},
        {S(0, 0), S(107, 0), S(50, 0), S(34, 0), S(52, 0), S(24, 0), S(52, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(86, 0), S(15, 0), S(0, 0), S(14, 0)},
        {S(0, 0), S(0, 0), S(70, 0), S(22, 0), S(0, 0), S(0, 0), S(6, 0)},
        {S(0, 0), S(0, 0), S(220, 0), S(71, 0), S(22, 0), S(2, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(150, 0), S(40, 0), S(6, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(165, 0), S(5, 0), S(11, 0), S(3, 0), S(8, 0)}, // BlockedStorm
    S(-41, 0), // UndefendedKingZoneSq
    S(12, 86), // KingMobilityFactor
    S(28, 0), // KingAttackByKnight
    S(6, 0), // KingAttackByBishop
    S(28, 8), // KingAttackByRook
    S(28, 8), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(34, 619), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(6, 0), // KingRingWeakWeight
    S(8, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -18), // DoubledPawnPenalty
    S(-21, -6), // BackwardPawnPenalty
    S(-32, -8), // WeakUnopposedPenalty
    S(-38, -38), // DoubledIsolatedPenalty
    {S(0, -23), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-33, -20), // PawnIslandPenalty
    {S(31, 0), S(5, 0)}, // CentralPawnBonus
    S(32, 19), // BishopLongDiagonalBonus
    S(-18, 0), // BishopXrayPawns
    S(0, 30), // InitiativePasser
    S(0, 35), // InitiativePawnCount
    S(0, 8), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 46), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(59, 24), // SliderOnQueenBishop
    S(26, 0), // SliderOnQueenRook
    S(10, 2), // RestrictedPiece
    S(32, 0), // ThreatByPawnPush
    S(-25, -3), // WeakQueenDefender
    S(59, 0), // KnightOnQueen
    S(-277, -68), // PawnlessFlank
    S(0, 13), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 309), // KBNKCornerEg
    S(0, 841), // LucenaEg
    S(0, 50), // KXKPushToEdge
    S(0, 50), // KXKPushClose
    S(0, 50), // KBNKPushClose
    S(0, 50), // KQKRPushToEdge
    S(0, 50), // KQKRPushClose
    S(0, 1), // KPsKFortressScale
    S(0, 1), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
