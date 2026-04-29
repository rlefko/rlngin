#include "eval_params.h"

// Compiled-in defaults for the tunable parameters. The in-memory
// `evalParams` instance is initialized from this struct, and
// `resetEvalParams()` snaps it back to these values after a tune run.
//
// Values below come from a Texel tune covering every scalar in
// `EvalParams` (1065 mg/eg halves) over 5.47M unique qsearch-leaf
// positions extracted from a fresh 64,000-game self-play PGN at
// nodes=100000 with mate-scored plies filtered out and identical
// positions folded into a single weighted-average row at extraction
// time. Tune ran to its full thirty parallel passes plus thirty
// deterministic-finalizer passes, ending at loss 0.10736 (down
// from an initial 0.1086 against the prior compiled-in defaults).
// The values were then loaded through the canonicalization
// pipeline: project violators onto current bounds, then center each
// piece's PST around zero by mean and push the mean into PieceScore
// (eval is bit-identical to the un-centered checkpoint, but the
// per-term values stop drifting in the PST/material gauge null
// direction).
//
// Tuner improvements that produced this state:
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
//     unopposed, doubled-isolated, blocked, pawn-island,
//     bad-bishop) <= 0 each half.
//   - BishopPair / MinorOnKingRing / RookOnKingRing >= 0 against
//     universal chess priors.
//   - `RookOpenFile >= RookSemiOpenFile >= 0` per phase, and
//     `OpenFileNearKing <= SemiOpenFileNearKing <= 0` mirrored for
//     the king-zone files.
//   - King-attack and king-safe-check piece-weight chains:
//     `Queen >= Rook >= max(Bishop, Knight)` per half.
//   - `KingSafeSqPenalty` non-decreasing chain on both halves.
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
    S(226, 36), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(211, 14), S(196, 13), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(246, 0), S(0, 0)},
    S(215, 21), // ThreatByKing
    S(0, 0), // Hanging
    S(34, 28), // WeakQueen
    S(60, 16), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 29), S(0, 43), S(0, 46), S(0, 46), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 22), S(0, 49), S(0, 78), S(0, 88), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-33, -41), S(-33, -49), S(-93, -62), S(-312, -137), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(33, 23), S(33, 65), S(230, 124), S(230, 405), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(82, 24), S(89, 28), S(124, 28), S(124, 28), S(0, 0)},
    S(0, 32), // RookOn7thBonus
    S(-11, -11), // BadBishopPenalty
    S(36, 0), // Tempo
    {S(0, 0), S(261, 256), S(1002, 656), S(1056, 602), S(1610, 927), S(2692, 2153), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-40, -59), S(-62, -61), S(-58, -60), S(-46, -71), S(-53, -39), S(-17, -49), S(20, -61), S(-118, -74),
        S(-82, -69), S(-126, -68), S(-90, -75), S(-94, -75), S(-28, -74), S(-89, -68), S(-42, -81), S(-85, -79),
        S(-70, -51), S(-93, -53), S(-95, -85), S(-37, -96), S(-51, -81), S(-35, -85), S(-89, -73), S(-74, -71),
        S(-6, -27), S(7, -43), S(18, -80), S(9, -83), S(32, -73), S(18, -93), S(-49, -43), S(-75, -28),
        S(99, 56), S(82, 54), S(129, 17), S(137, 6), S(105, 12), S(160, 41), S(25, 79), S(8, 77),
        S(325, 163), S(-20, 238), S(137, 224), S(165, 258), S(205, 159), S(346, 191), S(-201, 287), S(-88, 236),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-554, 11), S(-67, -18), S(-106, -10), S(1, -32), S(-26, -11), S(-73, -27), S(-98, -14), S(-47, -18),
        S(-112, -62), S(43, -19), S(-62, -8), S(20, -15), S(-15, -3), S(20, -37), S(-77, 31), S(-63, -17),
        S(6, -20), S(32, -20), S(29, -10), S(24, 7), S(77, 10), S(53, -22), S(48, -16), S(-47, 15),
        S(30, 11), S(56, 19), S(101, 13), S(128, 18), S(81, 32), S(93, 33), S(87, 9), S(3, 22),
        S(43, 19), S(103, 14), S(130, 24), S(167, 28), S(96, 26), S(244, -11), S(92, 23), S(103, -12),
        S(-176, 12), S(39, 13), S(142, -1), S(122, 13), S(187, 20), S(210, -10), S(200, 3), S(-24, 4),
        S(-71, -6), S(-75, 28), S(71, 15), S(191, -20), S(-87, 27), S(320, -51), S(121, -41), S(-17, -13),
        S(-521, -90), S(-361, 15), S(-383, 73), S(-85, 28), S(229, 28), S(-107, -62), S(172, -22), S(-697, 38)
    },
    // BishopPST
    {
        S(-47, -50), S(36, -13), S(-30, -7), S(-36, -6), S(-78, 5), S(-85, -10), S(-17, -14), S(-6, -33),
        S(51, -35), S(43, -20), S(63, -21), S(-26, 19), S(1, -3), S(7, 7), S(43, -37), S(-25, -70),
        S(2, 5), S(72, 29), S(-12, 12), S(31, 12), S(-12, 36), S(4, -2), S(12, 2), S(33, -38),
        S(3, -11), S(52, 4), S(59, 22), S(62, 12), S(29, 17), S(30, 13), S(12, 8), S(55, -7),
        S(20, 20), S(78, 21), S(30, 24), S(55, 13), S(73, -8), S(149, -9), S(74, 17), S(6, -23),
        S(91, 1), S(78, 27), S(-108, 22), S(27, -4), S(122, 13), S(264, -21), S(199, 7), S(94, -10),
        S(-75, -3), S(-97, 3), S(8, 15), S(18, 2), S(-41, 6), S(37, -22), S(-181, 10), S(-174, -27),
        S(-89, 0), S(-18, 37), S(-85, -15), S(-120, 65), S(-265, 43), S(-198, -10), S(-239, 9), S(-59, -72)
    },
    // RookPST
    {
        S(-69, -16), S(-67, -4), S(-45, -6), S(-29, -14), S(0, -14), S(7, -15), S(-17, -5), S(-25, -30),
        S(-139, -12), S(-126, -5), S(-79, -12), S(-20, -18), S(-25, -20), S(-11, 1), S(-11, -31), S(-206, 5),
        S(-181, 6), S(-95, -23), S(-109, -5), S(-108, -11), S(-66, -21), S(-2, -18), S(-25, -19), S(-49, -29),
        S(-95, 12), S(-15, 10), S(-93, 16), S(-38, 7), S(17, -13), S(-15, 17), S(64, 3), S(-71, -2),
        S(-123, 33), S(14, 22), S(3, 35), S(51, 7), S(37, 6), S(91, 0), S(156, 0), S(-38, 3),
        S(-77, 49), S(-24, 20), S(4, 31), S(78, 12), S(210, -23), S(287, -23), S(252, 0), S(26, 10),
        S(-14, -9), S(-23, 6), S(19, 7), S(150, -7), S(126, -11), S(335, -44), S(126, 3), S(88, -15),
        S(-123, 28), S(55, 6), S(151, 4), S(10, 18), S(69, 20), S(-56, 29), S(-257, 62), S(79, 7)
    },
    // QueenPST
    {
        S(-97, -14), S(-42, -49), S(-26, -55), S(0, -27), S(19, -59), S(-147, -25), S(105, -227), S(-222, -7),
        S(-60, -58), S(-29, -40), S(20, -48), S(3, -17), S(19, -46), S(48, -59), S(60, -136), S(-131, -45),
        S(-57, 0), S(8, -30), S(28, -16), S(-13, -14), S(16, -4), S(5, 44), S(38, 5), S(-21, -32),
        S(6, -35), S(-65, 30), S(-6, 27), S(2, 51), S(53, 23), S(66, 37), S(20, 27), S(-11, -21),
        S(10, -20), S(-21, 14), S(36, 58), S(8, 37), S(46, 72), S(74, 30), S(-17, 108), S(71, -62),
        S(0, -48), S(-13, -34), S(4, 23), S(0, 34), S(22, 82), S(27, 145), S(70, 40), S(60, 34),
        S(-119, -48), S(-138, 19), S(-71, 49), S(28, 47), S(-92, 138), S(81, 9), S(-211, 122), S(126, -82),
        S(-86, 10), S(-80, 56), S(120, 1), S(35, 35), S(222, -60), S(18, 52), S(127, -37), S(13, 11)
    },
    // KingPST
    {
        S(172, -122), S(198, -74), S(137, -30), S(-88, -18), S(-28, -33), S(-138, -9), S(171, -67), S(224, -126),
        S(363, -80), S(208, -42), S(148, -12), S(90, -4), S(21, 5), S(56, -1), S(175, -29), S(250, -62),
        S(124, -54), S(63, -5), S(172, 4), S(-22, 38), S(53, 18), S(94, 11), S(192, -15), S(-7, -19),
        S(68, -30), S(116, -11), S(-74, 38), S(-44, 55), S(-81, 58), S(53, 53), S(176, 24), S(-100, 6),
        S(-201, 41), S(-3, 54), S(-40, 46), S(-199, 76), S(-223, 72), S(-78, 73), S(27, 83), S(-27, 15),
        S(121, -5), S(-184, 77), S(-405, 88), S(-395, 117), S(-326, 107), S(-90, 124), S(152, 95), S(-132, 45),
        S(343, -77), S(21, 21), S(-184, 42), S(-172, 113), S(44, 54), S(283, 63), S(19, 69), S(-123, -22),
        S(-562, -367), S(521, -97), S(67, 65), S(-131, -26), S(-214, -94), S(-297, -24), S(30, 55), S(-414, -332)
    },
    {
        {},
        {},
        {S(-107, -164), S(-48, -41), S(-2, -14), S(9, 8), S(28, 24), S(43, 43), S(56, 43), S(56, 43), S(56, 43)},
        {S(-15, 82), S(34, 91), S(67, 112), S(85, 128), S(103, 145), S(112, 150), S(112, 159), S(112, 161), S(122, 163), S(122, 163), S(122, 165), S(122, 165), S(128, 165), S(128, 165)},
        {S(-37, 250), S(6, 285), S(12, 294), S(20, 300), S(20, 300), S(26, 315), S(26, 322), S(36, 329), S(36, 336), S(36, 344), S(43, 347), S(43, 356), S(43, 357), S(43, 357), S(43, 357)},
        {S(5, 3), S(101, 225), S(109, 290), S(110, 318), S(130, 346), S(130, 348), S(143, 348), S(148, 370), S(160, 370), S(160, 372), S(160, 373), S(174, 380), S(174, 388), S(174, 388), S(176, 392), S(176, 400), S(176, 403), S(192, 403), S(212, 403), S(212, 403), S(218, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 404), S(230, 404)},
        {},
    },
    {S(0, 0), S(-16, 38), S(-11, 48), S(6, 69), S(12, 201), S(79, 353), S(437, 396), S(0, 0)},
    {S(0, 0), S(3, -13), S(53, 3), S(64, 6), S(65, 32), S(65, 73), S(68, 155), S(0, 0)},
    S(136, 18), // RookOpenFileBonus
    S(57, 18), // RookSemiOpenFileBonus
    S(95, 21), // KnightOutpostBonus
    S(95, 24), // BishopOutpostBonus
    S(-82, 0), // TrappedRookByKingPenalty
    S(67, 0), // RookBehindOurPasserBonus
    S(-17, 90), // RookBehindTheirPasserBonus
    S(29, 11), // MinorBehindPawnBonus
    S(27, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-20, -4), // KingProtector
    S(35, 11), // BishopPair
    {S(128, 0), S(63, 0)}, // PawnShieldBonus
    {S(0, 0), S(9, 0), S(87, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(50, 0), S(142, 0), S(0, 0), S(0, 0)}, // UnblockedPawnStorm
    S(-35, 0), // SemiOpenFileNearKing
    S(-186, 0), // OpenFileNearKing
    S(-46, 0), // UndefendedKingZoneSq
    {S(-12, -68), S(0, -14), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)}, // KingSafeSqPenalty
    S(24, 13), // KingAttackByKnight
    S(6, 13), // KingAttackByBishop
    S(24, 13), // KingAttackByRook
    S(24, 18), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 4), S(29, 8), S(29, 8), S(29, 8), S(0, 0)}, // KingSafeCheck
    S(16, 0), // KingRingWeakWeight
    S(39, 35), // KingNoQueenDiscount
    S(-8, -1), // IsolatedPawnPenalty
    S(-5, -33), // DoubledPawnPenalty
    S(0, -10), // BackwardPawnPenalty
    S(-26, -19), // WeakUnopposedPenalty
    S(0, -24), // DoubledIsolatedPenalty
    {S(-102, -26), S(-66, -94)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-16, -24), // PawnIslandPenalty
    {S(27, 0), S(23, 0)}, // CentralPawnBonus
    S(64, 15), // BishopLongDiagonalBonus
    S(0, 53), // InitiativePasser
    S(0, 48), // InitiativePawnCount
    S(0, 12), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 35), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(77, 0), // SliderOnQueenBishop
    S(67, 0), // SliderOnQueenRook
    S(13, 3), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
