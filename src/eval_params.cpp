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
    S(290, 442), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(70, 2), // ThreatByKing
    S(54, 21), // Hanging
    S(50, 50), // WeakQueen
    S(27, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 28), S(0, 33), S(0, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 33), S(0, 59), S(0, 74), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-20, -23), S(-23, -38), S(-23, -66), S(-220, -93), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(2, 51), S(89, 86), S(118, 377), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(31, 28), S(31, 28), S(101, 28), S(101, 28), S(0, 0)},
    S(0, 42), // RookOn7thBonus
    S(-18, 0), // BadBishop
    S(-5, -7), // BishopPawns
    S(38, 0), // Tempo
    {S(0, 0), S(194, 211), S(746, 603), S(818, 568), S(1218, 897), S(2214, 1860), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-44, -9), S(-28, -4), S(-22, -4), S(-38, -31), S(-23, -23), S(-3, 2), S(-26, -4), S(-47, -18),
        S(-77, -30), S(-68, -17), S(-49, -22), S(-70, -45), S(-48, -31), S(-45, -18), S(-61, -24), S(-88, -29),
        S(-37, -10), S(-46, -13), S(-23, -29), S(-2, -42), S(1, -41), S(-20, -25), S(-44, -18), S(-39, -18),
        S(5, 20), S(-5, 16), S(8, -1), S(82, -4), S(102, 3), S(17, 1), S(-9, 16), S(7, 18),
        S(20, 28), S(27, 32), S(49, 21), S(97, 24), S(100, 26), S(52, 22), S(26, 28), S(19, 31),
        S(21, 24), S(31, 27), S(54, 26), S(84, 34), S(85, 35), S(55, 26), S(30, 27), S(22, 25),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-141, -35), S(-161, -23), S(-125, -22), S(-81, -14),
        S(-106, -26), S(-87, -10), S(-87, -14), S(-70, -3),
        S(-100, -19), S(-47, -6), S(-29, -6), S(8, 16),
        S(-13, -3), S(11, 9), S(27, 26), S(25, 32),
        S(19, 4), S(42, 15), S(58, 33), S(66, 39),
        S(32, -13), S(60, 7), S(88, 23), S(106, 34),
        S(31, -20), S(48, -10), S(75, 0), S(101, 31),
        S(25, -30), S(46, -12), S(69, 6), S(86, 15)
    },
    // BishopPST (half-board)
    {
        S(-5, -18), S(15, -8), S(-86, -10), S(-78, -12),
        S(5, -14), S(-15, -38), S(-3, -4), S(-52, -6),
        S(-22, -15), S(5, 8), S(-13, -10), S(6, 16),
        S(-8, -13), S(12, 7), S(31, 18), S(27, 10),
        S(-31, -18), S(22, 10), S(35, 20), S(48, 18),
        S(9, -2), S(22, 18), S(34, 5), S(56, 26),
        S(-17, -12), S(-25, -6), S(15, 19), S(38, 23),
        S(-25, -14), S(-15, 8), S(4, 8), S(15, 9)
    },
    // RookPST (half-board)
    {
        S(-99, -25), S(-46, -31), S(-41, -26), S(-11, -32),
        S(-78, -33), S(-17, -36), S(-24, -31), S(-13, -28),
        S(-50, -27), S(-2, -22), S(-15, -16), S(4, -22),
        S(-31, -7), S(9, 2), S(2, 8), S(9, -1),
        S(-28, 23), S(19, 32), S(38, 28), S(35, 20),
        S(-11, 27), S(35, 33), S(59, 34), S(81, 25),
        S(-19, -1), S(-5, 1), S(37, 6), S(64, 9),
        S(3, 20), S(21, 24), S(46, 25), S(51, 22)
    },
    // QueenPST (half-board)
    {
        S(4, -90), S(44, -87), S(22, -77), S(32, -60),
        S(17, -79), S(28, -78), S(47, -62), S(40, -41),
        S(19, -47), S(40, -22), S(42, -2), S(28, 2),
        S(40, -14), S(48, 11), S(39, 40), S(21, 50),
        S(1, -22), S(15, 12), S(5, 40), S(4, 72),
        S(6, -2), S(-15, 23), S(-20, 57), S(-7, 83),
        S(-46, 4), S(-89, 19), S(-52, 51), S(-31, 72),
        S(-72, 14), S(-70, 35), S(-63, 48), S(-51, 57)
    },
    // KingPST (half-board)
    {
        S(96, -150), S(59, -102), S(2, -76), S(-118, -57),
        S(104, -104), S(66, -70), S(-16, -52), S(-34, -42),
        S(15, -67), S(9, -45), S(-22, -31), S(-20, -23),
        S(-20, -26), S(-10, -4), S(-12, 2), S(-18, 4),
        S(-13, 20), S(-8, 35), S(-8, 38), S(-11, 34),
        S(-6, 46), S(1, 62), S(0, 63), S(-4, 57),
        S(-6, 50), S(-2, 61), S(-2, 62), S(-4, 64),
        S(-7, 50), S(-4, 56), S(-3, 61), S(-2, 64)
    },
    {
        {},
        {},
        {S(-112, -98), S(-72, -58), S(-37, -25), S(-17, -2), S(1, 14), S(18, 26), S(33, 26), S(45, 26), S(45, 26)},
        {S(-11, 77), S(19, 83), S(53, 104), S(68, 121), S(88, 144), S(101, 151), S(105, 155), S(108, 155), S(108, 155), S(112, 155), S(116, 155), S(116, 155), S(116, 155), S(118, 155)},
        {S(-47, 277), S(-20, 287), S(-6, 301), S(11, 306), S(11, 310), S(18, 321), S(23, 331), S(33, 334), S(43, 340), S(48, 346), S(54, 351), S(54, 355), S(54, 355), S(54, 355), S(54, 355)},
        {S(116, 238), S(116, 263), S(116, 288), S(121, 306), S(122, 331), S(131, 351), S(133, 366), S(145, 366), S(146, 384), S(155, 386), S(160, 386), S(163, 397), S(166, 403), S(167, 403), S(174, 403), S(174, 403), S(178, 403), S(180, 403), S(181, 403), S(188, 403), S(191, 403), S(191, 403), S(203, 403), S(203, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 428)},
        {},
    },
    {S(0, 0), S(-39, 9), S(-39, 19), S(-39, 19), S(0, 115), S(115, 313), S(366, 513), S(0, 0)},
    {S(0, 0), S(-12, 1), S(28, 7), S(37, 7), S(56, 31), S(74, 74), S(74, 201), S(0, 0)},
    S(85, 14), // RookOpenFileBonus
    S(35, 14), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(50, 24), // KnightOutpostBonus
    S(62, 13), // BishopOutpostBonus
    S(-47, 0), // TrappedRookByKingPenalty
    S(24, 26), // RookBehindOurPasserBonus
    S(8, 83), // RookBehindTheirPasserBonus
    S(31, 12), // MinorBehindPawnBonus
    S(35, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-12, 0), // KingProtector
    S(43, 11), // BishopPair
    {
        {S(0, 0), S(86, 0), S(95, 0), S(59, 0), S(40, 0), S(55, 0), S(159, 0)},
        {S(-37, 0), S(161, 0), S(88, 0), S(48, 0), S(0, 0), S(54, 0), S(0, 0)},
        {S(0, 0), S(121, 0), S(39, 0), S(41, 0), S(19, 0), S(37, 0), S(76, 0)},
        {S(0, 0), S(91, 0), S(22, 0), S(20, 0), S(50, 0), S(20, 0), S(64, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(80, 0), S(11, 0), S(0, 0), S(11, 0)},
        {S(0, 0), S(0, 0), S(12, 0), S(16, 0), S(0, 0), S(0, 0), S(4, 0)},
        {S(0, 0), S(0, 0), S(102, 0), S(52, 0), S(13, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(44, 0), S(36, 0), S(3, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(145, 0), S(11, 0), S(11, 0), S(17, 0), S(28, 0)}, // BlockedStorm
    S(-25, -3), // UndefendedKingZoneSq
    S(14, 3), // KingMobilityFactor
    S(26, 7), // KingAttackByKnight
    S(4, 43), // KingAttackByBishop
    S(26, 7), // KingAttackByRook
    S(26, 7), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(33, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(5, 0), // KingRingWeakWeight
    S(9, 64), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-1, -29), // DoubledPawnPenalty
    S(-13, -6), // BackwardPawnPenalty
    S(-30, -13), // WeakUnopposedPenalty
    S(-38, -44), // DoubledIsolatedPenalty
    {S(-4, -27), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-35, -18), // PawnIslandPenalty
    {S(26, 0), S(3, 0)}, // CentralPawnBonus
    S(27, 19), // BishopLongDiagonalBonus
    S(-19, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 18), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(60, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(9, 2), // RestrictedPiece
    S(45, 34), // ThreatByPawnPush
    S(-21, -2), // WeakQueenDefender
    S(42, 2), // KnightOnQueen
    S(-206, -74), // PawnlessFlank
    S(0, 48), // QueenInfiltration
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
    S(0, 22), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
