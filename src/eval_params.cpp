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
    S(278, 50), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(197, 0), S(148, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(225, 31), S(0, 0)},
    S(139, 29), // ThreatByKing
    S(104, 17), // Hanging
    S(0, 17), // WeakQueen
    S(27, 32), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 43), S(0, 43), S(0, 43), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 39), S(0, 73), S(0, 82), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-39, -40), S(-39, -58), S(-115, -60), S(-335, -133), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 21), S(53, 67), S(165, 143), S(165, 428), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-49, 17), S(46, 28), S(87, 28), S(87, 28), S(0, 0)},
    S(0, 20), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -7), // BishopPawns
    S(85, 0), // Tempo
    {S(0, 0), S(200, 223), S(918, 554), S(946, 486), S(1489, 767), S(2660, 1743), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-50, -57), S(-46, -58), S(-37, -60), S(-9, -62), S(-6, -56), S(-35, -54), S(-47, -57), S(-55, -69),
        S(-96, -53), S(-82, -63), S(-52, -68), S(-41, -66), S(-28, -57), S(-55, -63), S(-71, -70), S(-93, -63),
        S(-73, -41), S(-70, -54), S(-67, -72), S(-38, -72), S(-31, -71), S(-55, -69), S(-67, -53), S(-74, -50),
        S(-12, -5), S(-40, -28), S(-6, -51), S(26, -65), S(31, -61), S(-2, -50), S(-57, -22), S(-10, -11),
        S(91, 70), S(60, 78), S(97, 34), S(177, 16), S(184, 19), S(87, 32), S(35, 83), S(76, 79),
        S(173, 154), S(-57, 215), S(71, 168), S(265, 114), S(241, 110), S(41, 177), S(-240, 231), S(72, 154),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-86, -48), S(-78, -7), S(-58, -11), S(-62, 20),
        S(-68, -21), S(-59, -13), S(-24, -9), S(-17, 10),
        S(-6, 5), S(15, 10), S(38, -7), S(43, 23),
        S(3, 4), S(70, 5), S(69, 30), S(77, 30),
        S(39, 20), S(81, 10), S(104, 27), S(91, 32),
        S(-32, -2), S(44, 10), S(116, 20), S(116, 25),
        S(-109, -34), S(-33, -7), S(61, -16), S(59, 14),
        S(-269, -92), S(-143, -40), S(-23, -15), S(18, -1)
    },
    // BishopPST (half-board)
    {
        S(16, -24), S(16, -25), S(-32, -7), S(-29, 3),
        S(7, -23), S(19, -27), S(36, -4), S(1, 8),
        S(22, -11), S(47, 8), S(23, -19), S(13, 19),
        S(42, -13), S(59, -5), S(60, 18), S(43, -7),
        S(30, -15), S(54, 30), S(71, 13), S(63, -9),
        S(20, -10), S(9, 16), S(30, 2), S(49, 4),
        S(-47, -16), S(-78, 5), S(-25, 6), S(-66, -2),
        S(-76, 5), S(-109, 14), S(-159, 16), S(-109, 27)
    },
    // RookPST (half-board)
    {
        S(-47, -12), S(-42, 2), S(-11, -5), S(-8, -11),
        S(-66, -28), S(-50, -21), S(-30, -12), S(-26, -15),
        S(-68, -25), S(-55, -14), S(-55, -19), S(-43, -27),
        S(-43, 4), S(-41, 5), S(-38, 9), S(12, -7),
        S(-44, 22), S(-12, 22), S(21, 12), S(45, -1),
        S(8, 13), S(16, 12), S(58, 15), S(72, 11),
        S(29, 1), S(40, 10), S(75, 0), S(89, 11),
        S(38, 10), S(47, 12), S(69, 2), S(80, -3)
    },
    // QueenPST (half-board)
    {
        S(20, -88), S(22, -77), S(18, -70), S(37, -48),
        S(7, -37), S(19, -40), S(51, -40), S(57, -33),
        S(-16, -43), S(36, -17), S(35, 5), S(28, -17),
        S(13, -17), S(46, 12), S(41, 28), S(21, 21),
        S(10, 0), S(11, 11), S(6, 60), S(0, 68),
        S(-10, 5), S(-25, 21), S(-49, 69), S(-49, 97),
        S(-98, 13), S(-139, 52), S(-112, 77), S(-100, 96),
        S(-91, -11), S(-94, -16), S(223, -64), S(53, -14)
    },
    // KingPST (half-board)
    {
        S(227, -92), S(228, -61), S(81, -29), S(-27, -39),
        S(249, -44), S(203, -23), S(63, 4), S(-11, 12),
        S(38, -12), S(126, -4), S(29, 18), S(-55, 32),
        S(-46, 6), S(20, 28), S(-58, 49), S(-176, 52),
        S(47, 11), S(-22, 44), S(-129, 66), S(-254, 73),
        S(-19, 4), S(-58, 43), S(-164, 68), S(-263, 77),
        S(224, -58), S(-90, 8), S(-29, 15), S(-178, 31),
        S(-266, -180), S(253, -53), S(263, -76), S(-227, 23)
    },
    {
        {},
        {},
        {S(-79, -89), S(-43, -49), S(-3, -10), S(16, -7), S(29, 9), S(39, 28), S(56, 35), S(56, 36), S(56, 36)},
        {S(-13, 59), S(21, 88), S(54, 123), S(83, 138), S(108, 139), S(113, 148), S(118, 161), S(121, 163), S(121, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163)},
        {S(-20, 254), S(5, 284), S(20, 294), S(20, 304), S(20, 305), S(20, 323), S(20, 323), S(32, 333), S(33, 337), S(47, 349), S(49, 353), S(57, 354), S(66, 357), S(67, 357), S(88, 357)},
        {S(18, 291), S(43, 316), S(68, 341), S(75, 351), S(100, 351), S(125, 351), S(140, 362), S(148, 363), S(155, 377), S(171, 377), S(173, 380), S(174, 380), S(174, 380), S(176, 395), S(176, 400), S(178, 403), S(186, 403), S(202, 403), S(203, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(236, 403), S(237, 403)},
        {},
    },
    {S(0, 0), S(-39, 42), S(-39, 42), S(-36, 85), S(0, 148), S(8, 306), S(512, 382), S(0, 0)},
    {S(0, 0), S(-11, -7), S(51, 9), S(51, 9), S(67, 15), S(67, 83), S(245, 107), S(0, 0)},
    S(113, 13), // RookOpenFileBonus
    S(27, 11), // RookSemiOpenFileBonus
    S(0, 3), // RookOnQueenFile
    S(97, 13), // KnightOutpostBonus
    S(95, 15), // BishopOutpostBonus
    S(-104, 0), // TrappedRookByKingPenalty
    S(67, 4), // RookBehindOurPasserBonus
    S(-117, 102), // RookBehindTheirPasserBonus
    S(35, 4), // MinorBehindPawnBonus
    S(26, 0), // MinorOnKingRing
    S(9, 0), // RookOnKingRing
    S(-17, -2), // KingProtector
    S(8, 4), // BishopPair
    {
        {S(0, 0), S(111, 0), S(105, 0), S(77, 0), S(56, 0), S(31, 0), S(0, 0)},
        {S(-87, 0), S(197, 0), S(80, 0), S(4, 0), S(0, 0), S(78, 0), S(167, 0)},
        {S(-8, 0), S(201, 0), S(65, 0), S(42, 0), S(62, 0), S(75, 0), S(89, 0)},
        {S(-12, 0), S(155, 0), S(78, 0), S(48, 0), S(27, 0), S(120, 0), S(58, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(10, 0), S(0, 0)},
        {S(0, 0), S(16, 0), S(69, 0), S(11, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(8, 0), S(266, 0), S(4, 0), S(2, 0), S(6, 0), S(0, 0)},
        {S(0, 0), S(24, 0), S(309, 0), S(38, 0), S(4, 0), S(4, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(202, 0), S(5, 0), S(0, 0), S(44, 0), S(0, 0)}, // BlockedStorm
    S(-31, 0), // UndefendedKingZoneSq
    S(9, 49), // KingMobilityFactor
    S(29, 41), // KingAttackByKnight
    S(12, 45), // KingAttackByBishop
    S(30, 41), // KingAttackByRook
    S(30, 49), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(36, 299), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(24, 14), // KingRingWeakWeight
    S(28, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -6), // DoubledPawnPenalty
    S(-11, -6), // BackwardPawnPenalty
    S(-27, -16), // WeakUnopposedPenalty
    S(0, -39), // DoubledIsolatedPenalty
    {S(-95, -26), S(-119, -101)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-4, -21), // PawnIslandPenalty
    {S(37, 0), S(23, 0)}, // CentralPawnBonus
    S(64, 28), // BishopLongDiagonalBonus
    S(-20, 0), // BishopXrayPawns
    S(0, 58), // InitiativePasser
    S(0, 52), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 65), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(45, 16), // SliderOnQueenBishop
    S(53, 2), // SliderOnQueenRook
    S(8, 0), // RestrictedPiece
    S(30, 0), // ThreatByPawnPush
    S(-41, -6), // WeakQueenDefender
    S(103, 0), // KnightOnQueen
    S(-298, -139), // PawnlessFlank
    S(0, 0), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 9), // KBNKCornerEg
    S(0, 276), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
