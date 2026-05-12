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
    S(254, 48), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(189, 3), S(164, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(225, 31), S(0, 0)},
    S(127, 19), // ThreatByKing
    S(100, 21), // Hanging
    S(0, 33), // WeakQueen
    S(21, 32), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 27), S(0, 43), S(0, 43), S(0, 43), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 39), S(0, 73), S(0, 83), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-38, -37), S(-39, -56), S(-123, -56), S(-335, -121), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 22), S(53, 63), S(165, 135), S(165, 420), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-45, 17), S(46, 28), S(95, 28), S(95, 28), S(0, 0)},
    S(0, 21), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -7), // BishopPawns
    S(71, 0), // Tempo
    {S(0, 0), S(208, 228), S(925, 560), S(960, 493), S(1513, 779), S(2684, 1767), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-48, -54), S(-50, -58), S(-35, -60), S(-5, -62), S(18, -59), S(-34, -53), S(-35, -57), S(-53, -69),
        S(-104, -47), S(-106, -58), S(-63, -63), S(-57, -64), S(-24, -57), S(-75, -57), S(-47, -74), S(-93, -64),
        S(-83, -38), S(-94, -52), S(-91, -68), S(-54, -71), S(-41, -72), S(-47, -69), S(-51, -54), S(-84, -52),
        S(-16, -6), S(-44, -31), S(-26, -51), S(2, -65), S(18, -61), S(-10, -54), S(-81, -24), S(-18, -14),
        S(115, 50), S(84, 72), S(121, 14), S(201, 0), S(208, 3), S(111, 8), S(59, 79), S(58, 79),
        S(197, 155), S(-33, 207), S(95, 188), S(289, 134), S(229, 134), S(17, 201), S(-264, 255), S(48, 178),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-111, -75), S(-83, -1), S(-51, -12), S(-87, 26),
        S(-93, -16), S(-84, -8), S(-21, -8), S(-12, 12),
        S(-7, 10), S(26, 9), S(45, -4), S(46, 23),
        S(-6, 9), S(93, 2), S(80, 29), S(88, 31),
        S(62, 24), S(100, 8), S(127, 25), S(98, 33),
        S(-57, 3), S(67, 11), S(139, 15), S(139, 27),
        S(-86, -37), S(-10, 6), S(84, -31), S(34, 27),
        S(-294, -119), S(-168, -39), S(-42, -2), S(-3, 10)
    },
    // BishopPST (half-board)
    {
        S(16, -27), S(32, -30), S(-44, -5), S(-35, 2),
        S(-3, -22), S(11, -24), S(54, -9), S(-4, 9),
        S(16, -10), S(49, 9), S(18, -18), S(13, 20),
        S(36, -12), S(81, -9), S(62, 19), S(41, -8),
        S(14, -8), S(54, 35), S(93, 10), S(61, -10),
        S(42, -13), S(19, 17), S(44, 1), S(71, 0),
        S(-29, -27), S(-104, 6), S(-3, 7), S(-92, 3),
        S(-62, 2), S(-127, 19), S(-185, 23), S(-135, 36)
    },
    // RookPST (half-board)
    {
        S(-41, -14), S(-44, 0), S(-8, -7), S(-6, -13),
        S(-68, -28), S(-46, -25), S(-24, -16), S(-30, -15),
        S(-82, -22), S(-59, -12), S(-71, -15), S(-59, -25),
        S(-19, -1), S(-39, 6), S(-62, 15), S(24, -10),
        S(-68, 28), S(-16, 25), S(17, 16), S(43, -1),
        S(32, 10), S(-4, 19), S(58, 15), S(88, 7),
        S(29, 3), S(24, 12), S(99, -2), S(107, 9),
        S(34, 16), S(35, 16), S(53, 6), S(104, -3)
    },
    // QueenPST (half-board)
    {
        S(44, -112), S(24, -87), S(18, -74), S(34, -48),
        S(-5, -21), S(11, -37), S(53, -40), S(57, -33),
        S(-40, -29), S(38, -19), S(31, 9), S(26, -17),
        S(19, -17), S(50, 11), S(43, 26), S(23, 19),
        S(8, 0), S(35, -9), S(6, 68), S(8, 69),
        S(14, -11), S(-1, 1), S(-33, 73), S(-33, 101),
        S(-110, 17), S(-163, 76), S(-136, 101), S(-124, 120),
        S(-67, -19), S(-118, 4), S(247, -88), S(49, -14)
    },
    // KingPST (half-board)
    {
        S(244, -102), S(243, -71), S(78, -35), S(-28, -49),
        S(270, -54), S(224, -31), S(52, 0), S(-30, 7),
        S(11, -12), S(147, -11), S(34, 16), S(-66, 28),
        S(-73, 4), S(41, 26), S(-37, 47), S(-203, 52),
        S(68, 3), S(-1, 48), S(-120, 67), S(-281, 79),
        S(-46, 10), S(-37, 57), S(-191, 90), S(-290, 99),
        S(245, -76), S(-117, 30), S(-8, 27), S(-165, 49),
        S(-293, -206), S(274, -31), S(284, -102), S(-254, 45)
    },
    {
        {},
        {},
        {S(-76, -83), S(-36, -45), S(3, -10), S(18, -5), S(29, 12), S(39, 30), S(56, 35), S(56, 36), S(56, 36)},
        {S(-11, 79), S(24, 88), S(57, 123), S(84, 138), S(108, 139), S(112, 150), S(117, 161), S(119, 163), S(119, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163)},
        {S(-18, 254), S(5, 284), S(20, 294), S(20, 304), S(20, 306), S(20, 323), S(20, 323), S(29, 335), S(34, 336), S(45, 349), S(45, 353), S(45, 356), S(48, 357), S(51, 357), S(64, 357)},
        {S(26, 291), S(47, 316), S(72, 341), S(83, 351), S(104, 351), S(125, 351), S(137, 362), S(149, 363), S(155, 378), S(169, 378), S(169, 382), S(174, 382), S(174, 383), S(176, 396), S(176, 403), S(178, 403), S(186, 403), S(201, 403), S(203, 403), S(225, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(236, 403), S(237, 403)},
        {},
    },
    {S(0, 0), S(-39, 41), S(-39, 41), S(-24, 75), S(0, 151), S(0, 322), S(492, 374), S(0, 0)},
    {S(0, 0), S(-14, -7), S(48, 9), S(54, 9), S(77, 17), S(77, 87), S(261, 131), S(0, 0)},
    S(115, 15), // RookOpenFileBonus
    S(33, 13), // RookSemiOpenFileBonus
    S(0, 2), // RookOnQueenFile
    S(93, 13), // KnightOutpostBonus
    S(89, 16), // BishopOutpostBonus
    S(-96, 0), // TrappedRookByKingPenalty
    S(67, 4), // RookBehindOurPasserBonus
    S(-93, 96), // RookBehindTheirPasserBonus
    S(37, 4), // MinorBehindPawnBonus
    S(26, 0), // MinorOnKingRing
    S(11, 0), // RookOnKingRing
    S(-17, -2), // KingProtector
    S(12, 7), // BishopPair
    {
        {S(0, 0), S(113, 0), S(115, 0), S(83, 0), S(58, 0), S(47, 0), S(0, 0)},
        {S(-89, 0), S(197, 0), S(72, 0), S(0, 0), S(0, 0), S(54, 0), S(167, 0)},
        {S(-9, 0), S(197, 0), S(73, 0), S(26, 0), S(64, 0), S(75, 0), S(89, 0)},
        {S(0, 0), S(163, 0), S(94, 0), S(68, 0), S(39, 0), S(104, 0), S(34, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(6, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(45, 0), S(31, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(242, 0), S(8, 0), S(0, 0), S(5, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(285, 0), S(30, 0), S(0, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(194, 0), S(10, 0), S(0, 0), S(34, 0), S(0, 0)}, // BlockedStorm
    S(-31, -1), // UndefendedKingZoneSq
    S(10, 47), // KingMobilityFactor
    S(27, 31), // KingAttackByKnight
    S(10, 29), // KingAttackByBishop
    S(30, 33), // KingAttackByRook
    S(30, 33), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(31, 0), S(36, 299), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(22, 22), // KingRingWeakWeight
    S(28, 0), // KingNoQueenDiscount
    S(-1, 0), // IsolatedPawnPenalty
    S(0, -6), // DoubledPawnPenalty
    S(-12, -6), // BackwardPawnPenalty
    S(-29, -18), // WeakUnopposedPenalty
    S(0, -43), // DoubledIsolatedPenalty
    {S(-90, -24), S(-119, -93)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-7, -21), // PawnIslandPenalty
    {S(41, 0), S(30, 0)}, // CentralPawnBonus
    S(68, 28), // BishopLongDiagonalBonus
    S(-22, 0), // BishopXrayPawns
    S(0, 25), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(45, 28), // SliderOnQueenBishop
    S(53, 0), // SliderOnQueenRook
    S(9, 0), // RestrictedPiece
    S(32, 0), // ThreatByPawnPush
    S(-41, -9), // WeakQueenDefender
    S(83, 0), // KnightOnQueen
    S(-274, -144), // PawnlessFlank
    S(0, 1), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 9), // KBNKCornerEg
    S(0, 252), // LucenaEg
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
