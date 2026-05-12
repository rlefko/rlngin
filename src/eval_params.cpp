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
    S(169, 30), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(120, 0), S(97, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(155, 0), S(0, 0)},
    S(101, 1), // ThreatByKing
    S(66, 16), // Hanging
    S(5, 0), // WeakQueen
    S(24, 14), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 14), S(0, 25), S(0, 32), S(0, 32), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 10), S(0, 29), S(0, 56), S(0, 67), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-28, -17), S(-30, -31), S(-30, -50), S(-273, -73), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 18), S(21, 42), S(100, 76), S(100, 373), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(32, 28), S(32, 28), S(92, 28), S(92, 28), S(0, 0)},
    S(0, 35), // RookOn7thBonus
    S(-11, 0), // BadBishop
    S(-4, -7), // BishopPawns
    S(49, 0), // Tempo
    {S(0, 0), S(200, 174), S(795, 545), S(873, 499), S(1287, 775), S(2306, 1676), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-41, -12), S(-23, -9), S(-17, -7), S(-30, -29), S(-17, -19), S(-1, 0), S(-22, -8), S(-43, -18),
        S(-74, -30), S(-63, -21), S(-42, -23), S(-64, -41), S(-42, -27), S(-39, -19), S(-54, -27), S(-85, -29),
        S(-34, -12), S(-45, -15), S(-19, -28), S(3, -36), S(6, -35), S(-16, -23), S(-43, -18), S(-37, -18),
        S(3, 18), S(-8, 13), S(10, -2), S(90, -4), S(108, 5), S(19, 1), S(-12, 14), S(6, 16),
        S(12, 25), S(19, 30), S(43, 18), S(94, 25), S(97, 28), S(45, 20), S(19, 26), S(11, 30),
        S(14, 22), S(24, 23), S(48, 24), S(77, 34), S(78, 36), S(48, 25), S(23, 23), S(15, 23),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-154, -32), S(-176, -21), S(-137, -20), S(-90, -8),
        S(-116, -24), S(-96, -7), S(-94, -14), S(-76, -4),
        S(-104, -19), S(-49, -6), S(-24, -9), S(11, 12),
        S(-9, -1), S(18, 8), S(38, 24), S(37, 25),
        S(27, 4), S(57, 12), S(71, 29), S(79, 31),
        S(37, -14), S(65, 6), S(94, 22), S(107, 32),
        S(36, -20), S(51, -9), S(76, 0), S(96, 29),
        S(27, -32), S(46, -13), S(66, 6), S(79, 13)
    },
    // BishopPST (half-board)
    {
        S(-2, -17), S(15, -3), S(-94, -8), S(-88, -8),
        S(9, -13), S(-11, -34), S(-3, -1), S(-57, -5),
        S(-19, -14), S(8, 6), S(-7, -12), S(10, 12),
        S(-4, -12), S(19, 4), S(43, 15), S(34, 6),
        S(-25, -19), S(31, 8), S(44, 19), S(54, 13),
        S(8, -1), S(21, 17), S(34, 4), S(56, 24),
        S(-18, -16), S(-28, -10), S(8, 18), S(26, 24),
        S(-29, -17), S(-21, 7), S(-6, 8), S(2, 9)
    },
    // RookPST (half-board)
    {
        S(-108, -24), S(-48, -27), S(-43, -21), S(-6, -27),
        S(-85, -28), S(-18, -30), S(-24, -26), S(-11, -23),
        S(-51, -26), S(0, -19), S(-11, -15), S(8, -18),
        S(-28, -9), S(14, 2), S(6, 6), S(16, -2),
        S(-24, 17), S(18, 30), S(41, 22), S(41, 14),
        S(-8, 22), S(34, 29), S(57, 31), S(79, 23),
        S(-19, -2), S(-1, -1), S(37, 6), S(62, 10),
        S(-6, 20), S(12, 25), S(36, 24), S(42, 21)
    },
    // QueenPST (half-board)
    {
        S(7, -81), S(45, -73), S(22, -66), S(35, -55),
        S(19, -68), S(33, -71), S(51, -54), S(44, -40),
        S(18, -43), S(44, -24), S(47, -9), S(32, -10),
        S(43, -14), S(56, 4), S(43, 26), S(27, 29),
        S(8, -19), S(23, 9), S(12, 33), S(6, 61),
        S(7, 0), S(-16, 20), S(-24, 53), S(-13, 74),
        S(-52, 9), S(-94, 16), S(-63, 47), S(-44, 66),
        S(-80, 19), S(-81, 38), S(-74, 50), S(-62, 55)
    },
    // KingPST (half-board)
    {
        S(135, -137), S(87, -94), S(17, -70), S(-123, -56),
        S(141, -97), S(89, -66), S(-8, -49), S(-45, -39),
        S(27, -63), S(16, -43), S(-23, -30), S(-34, -22),
        S(-16, -26), S(-8, -5), S(-17, 0), S(-29, 0),
        S(-11, 18), S(-8, 32), S(-15, 35), S(-24, 30),
        S(-9, 44), S(-4, 61), S(-9, 61), S(-16, 55),
        S(-11, 48), S(-8, 59), S(-11, 61), S(-14, 62),
        S(-13, 47), S(-12, 54), S(-12, 59), S(-14, 62)
    },
    {
        {},
        {},
        {S(-109, -92), S(-69, -52), S(-32, -16), S(-10, 1), S(11, 14), S(28, 26), S(46, 26), S(54, 26), S(54, 26)},
        {S(-18, 94), S(17, 99), S(52, 117), S(71, 129), S(94, 146), S(109, 150), S(114, 154), S(116, 155), S(116, 155), S(122, 155), S(124, 155), S(124, 155), S(124, 155), S(124, 155)},
        {S(-57, 290), S(-27, 301), S(-8, 308), S(11, 313), S(11, 316), S(20, 324), S(25, 332), S(38, 334), S(47, 340), S(53, 345), S(54, 351), S(54, 355), S(54, 355), S(54, 355), S(54, 355)},
        {S(99, 278), S(99, 303), S(99, 328), S(108, 328), S(109, 342), S(119, 363), S(123, 370), S(137, 371), S(138, 385), S(148, 385), S(154, 387), S(158, 396), S(161, 401), S(163, 402), S(166, 403), S(170, 403), S(175, 403), S(177, 403), S(177, 403), S(183, 403), S(196, 403), S(196, 403), S(220, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(229, 428)},
        {},
    },
    {S(0, 0), S(-39, 16), S(-39, 23), S(-39, 32), S(0, 109), S(143, 290), S(463, 457), S(0, 0)},
    {S(0, 0), S(-13, -1), S(26, 8), S(39, 8), S(68, 25), S(86, 64), S(86, 183), S(0, 0)},
    S(97, 12), // RookOpenFileBonus
    S(41, 12), // RookSemiOpenFileBonus
    S(10, 0), // RookOnQueenFile
    S(51, 19), // KnightOutpostBonus
    S(69, 12), // BishopOutpostBonus
    S(-55, 0), // TrappedRookByKingPenalty
    S(26, 15), // RookBehindOurPasserBonus
    S(-23, 76), // RookBehindTheirPasserBonus
    S(33, 11), // MinorBehindPawnBonus
    S(40, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-15, 0), // KingProtector
    S(49, 0), // BishopPair
    {
        {S(0, 0), S(105, 0), S(118, 0), S(77, 0), S(55, 0), S(64, 0), S(178, 0)},
        {S(-61, 0), S(185, 0), S(100, 0), S(48, 0), S(0, 0), S(42, 0), S(0, 0)},
        {S(0, 0), S(149, 0), S(54, 0), S(51, 0), S(25, 0), S(50, 0), S(103, 0)},
        {S(0, 0), S(106, 0), S(33, 0), S(24, 0), S(57, 0), S(23, 0), S(65, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(91, 0), S(15, 0), S(0, 0), S(13, 0)},
        {S(0, 0), S(0, 0), S(44, 0), S(16, 0), S(0, 0), S(0, 0), S(4, 0)},
        {S(0, 0), S(0, 0), S(169, 0), S(62, 0), S(19, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(112, 0), S(44, 0), S(5, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(164, 0), S(7, 0), S(13, 0), S(12, 0), S(18, 0)}, // BlockedStorm
    S(-34, -2), // UndefendedKingZoneSq
    S(13, 55), // KingMobilityFactor
    S(28, 0), // KingAttackByKnight
    S(5, 0), // KingAttackByBishop
    S(28, 8), // KingAttackByRook
    S(28, 8), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(34, 619), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(6, 0), // KingRingWeakWeight
    S(8, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -20), // DoubledPawnPenalty
    S(-18, -6), // BackwardPawnPenalty
    S(-32, -8), // WeakUnopposedPenalty
    S(-38, -39), // DoubledIsolatedPenalty
    {S(0, -25), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-36, -18), // PawnIslandPenalty
    {S(34, 0), S(5, 0)}, // CentralPawnBonus
    S(30, 19), // BishopLongDiagonalBonus
    S(-20, 0), // BishopXrayPawns
    S(0, 30), // InitiativePasser
    S(0, 30), // InitiativePawnCount
    S(0, 10), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 42), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(54, 34), // SliderOnQueenBishop
    S(25, 0), // SliderOnQueenRook
    S(10, 2), // RestrictedPiece
    S(31, 0), // ThreatByPawnPush
    S(-23, -4), // WeakQueenDefender
    S(50, 0), // KnightOnQueen
    S(-255, -73), // PawnlessFlank
    S(0, 20), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 50), // KBNKCornerEg
    S(0, 300), // LucenaEg
    S(0, 50), // KXKPushToEdge
    S(0, 50), // KXKPushClose
    S(0, 50), // KBNKPushClose
    S(0, 50), // KQKRPushToEdge
    S(0, 50), // KQKRPushClose
    S(0, 0), // KPsKFortressScale
    S(0, 3), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
