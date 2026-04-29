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
// time. Loss after six full passes was ~0.1076; this snapshot was
// taken mid-run, then loaded through the canonicalization pipeline:
// project violators onto current bounds, then center each piece's
// PST around zero by mean and push the mean into PieceScore (eval
// is bit-identical to the un-centered checkpoint, but the per-term
// values stop drifting in the PST/material gauge null direction).
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
    S(232, 40), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(216, 13), S(192, 11), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(247, 0), S(0, 0)},
    S(201, 29), // ThreatByKing
    S(0, 0), // Hanging
    S(56, 20), // WeakQueen
    S(61, 17), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 32), S(0, 48), S(0, 48), S(0, 48), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 23), S(0, 56), S(0, 87), S(0, 111), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-18, -51), S(-18, -60), S(-50, -76), S(-205, -167), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(33, 26), S(33, 74), S(228, 140), S(238, 444), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(78, 28), S(104, 28), S(120, 28), S(189, 28), S(0, 0)},
    S(0, 35), // RookOn7thBonus
    S(-10, -11), // BadBishopPenalty
    S(40, 0), // Tempo
    {S(0, 0), S(240, 295), S(977, 747), S(1027, 698), S(1532, 1102), S(2611, 2487), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-35, -72), S(-50, -75), S(-44, -75), S(-37, -86), S(-39, -51), S(-10, -64), S(20, -73), S(-115, -89),
        S(-71, -85), S(-113, -83), S(-71, -93), S(-76, -91), S(-11, -90), S(-80, -84), S(-34, -96), S(-82, -94),
        S(-64, -63), S(-82, -67), S(-83, -102), S(-25, -114), S(-37, -99), S(-23, -104), S(-76, -89), S(-63, -87),
        S(2, -40), S(11, -55), S(26, -97), S(20, -100), S(44, -89), S(27, -113), S(-45, -56), S(-73, -39),
        S(66, 59), S(20, 61), S(99, 12), S(112, 1), S(88, 1), S(94, 48), S(-57, 92), S(-20, 76),
        S(173, 233), S(50, 281), S(182, 275), S(235, 313), S(231, 218), S(157, 314), S(-66, 331), S(-66, 276),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-399, -4), S(-78, -11), S(-115, -6), S(-46, -23), S(-43, -8), S(-67, -32), S(-109, -8), S(-144, 43),
        S(-148, -55), S(34, -14), S(-77, -1), S(8, -8), S(-26, 2), S(1, -30), S(-66, 33), S(-67, -16),
        S(-3, -17), S(21, -14), S(20, -7), S(12, 16), S(63, 18), S(42, -18), S(43, -12), S(-62, 22),
        S(23, 16), S(55, 24), S(90, 22), S(116, 28), S(72, 40), S(85, 42), S(85, 17), S(-2, 28),
        S(145, -12), S(93, 19), S(118, 34), S(156, 34), S(87, 34), S(237, -4), S(81, 31), S(108, -20),
        S(-125, 2), S(34, 22), S(129, 8), S(120, 19), S(232, 13), S(205, -7), S(183, 10), S(35, -22),
        S(-78, -8), S(-75, 33), S(49, 24), S(110, -1), S(-49, 22), S(244, -40), S(114, -40), S(23, -57),
        S(-513, -101), S(-179, -16), S(-127, 42), S(-104, 29), S(207, -15), S(-346, -28), S(31, -16), S(-466, -71)
    },
    // BishopPST
    {
        S(-24, -61), S(38, -20), S(-31, -11), S(-37, -7), S(-88, 4), S(-84, -11), S(12, -24), S(-16, -32),
        S(51, -39), S(49, -26), S(58, -21), S(-23, 19), S(-1, -2), S(12, 6), S(42, -38), S(22, -100),
        S(11, 3), S(71, 32), S(-10, 11), S(33, 14), S(-9, 38), S(2, -3), S(15, 0), S(25, -32),
        S(22, -18), S(55, 2), S(60, 26), S(66, 12), S(26, 19), S(32, 17), S(5, 16), S(53, -5),
        S(14, 26), S(80, 25), S(39, 24), S(44, 16), S(71, -9), S(140, -2), S(76, 19), S(-1, -21),
        S(84, 1), S(77, 30), S(-117, 29), S(27, -2), S(107, 21), S(228, -12), S(185, 15), S(100, -15),
        S(-87, -3), S(-135, 13), S(-2, 22), S(-11, 14), S(-60, 16), S(24, -21), S(-198, 16), S(-174, -32),
        S(-88, -10), S(-65, 49), S(-188, -7), S(-185, 78), S(-196, 40), S(-237, -6), S(46, -30), S(-10, -94)
    },
    // RookPST
    {
        S(-76, -15), S(-74, 0), S(-52, -4), S(-36, -11), S(-10, -13), S(-4, -14), S(-26, -3), S(-41, -29),
        S(-138, -14), S(-124, -7), S(-80, -12), S(-24, -20), S(-37, -19), S(-19, 2), S(13, -44), S(-215, 5),
        S(-188, 9), S(-92, -27), S(-85, -13), S(-117, -12), S(-64, -26), S(-9, -18), S(-21, -24), S(-90, -27),
        S(-105, 12), S(-94, 26), S(-70, 10), S(-46, 10), S(-38, -3), S(-56, 26), S(48, 6), S(-42, -14),
        S(-64, 22), S(10, 25), S(1, 39), S(-11, 22), S(23, 11), S(74, 3), S(140, 4), S(53, -22),
        S(-70, 50), S(-26, 21), S(1, 35), S(82, 11), S(183, -17), S(272, -22), S(186, 11), S(28, 9),
        S(-26, -7), S(-39, 8), S(17, 9), S(167, -12), S(172, -22), S(256, -30), S(32, 20), S(107, -21),
        S(74, -11), S(60, 8), S(122, 10), S(42, 17), S(154, 4), S(23, 22), S(-46, 31), S(10, 20)
    },
    // QueenPST
    {
        S(-45, -56), S(-82, -20), S(-41, -48), S(-13, -20), S(10, -53), S(-136, -33), S(22, -184), S(-136, -120),
        S(-77, -50), S(-17, -46), S(11, -42), S(-3, -12), S(11, -43), S(40, -56), S(69, -153), S(-62, -140),
        S(-64, 11), S(1, -21), S(14, -4), S(-17, -10), S(15, 4), S(0, 51), S(39, 10), S(-33, -23),
        S(0, -35), S(-55, 32), S(-12, 36), S(3, 59), S(47, 31), S(61, 49), S(15, 33), S(-21, -6),
        S(9, -13), S(1, -3), S(18, 77), S(-5, 54), S(35, 92), S(69, 41), S(-3, 110), S(49, -45),
        S(-2, -53), S(-3, -43), S(-15, 36), S(2, 36), S(3, 103), S(67, 135), S(71, 51), S(105, -17),
        S(-124, -43), S(-141, 30), S(-97, 76), S(-4, 68), S(-80, 151), S(33, 39), S(-89, 64), S(123, -79),
        S(-144, 34), S(-135, 81), S(160, -15), S(10, 50), S(130, -25), S(86, 26), S(218, -81), S(155, -84)
    },
    // KingPST
    {
        S(37, -128), S(153, -96), S(91, -48), S(-115, -39), S(-71, -55), S(-174, -25), S(128, -89), S(183, -152),
        S(129, -62), S(102, -47), S(74, -18), S(50, -18), S(-21, -8), S(16, -15), S(131, -44), S(217, -80),
        S(54, -63), S(115, -25), S(72, 3), S(-60, 32), S(-39, 16), S(50, 3), S(165, -29), S(-36, -32),
        S(-85, -29), S(150, -31), S(-111, 34), S(-154, 56), S(-254, 67), S(-47, 55), S(-16, 39), S(-35, -21),
        S(35, 0), S(54, 44), S(-154, 52), S(-248, 75), S(-271, 72), S(-189, 81), S(-29, 87), S(-90, 11),
        S(68, 1), S(12, 52), S(-86, 56), S(-190, 97), S(-194, 91), S(-50, 97), S(-4, 100), S(-8, 19),
        S(131, -76), S(88, 5), S(-69, 25), S(8, 110), S(68, 64), S(74, 101), S(93, 65), S(-11, -18),
        S(-186, -295), S(145, -69), S(131, 47), S(-2, 59), S(-48, 138), S(95, 30), S(190, 26), S(-30, -258)
    },
    {
        {},
        {},
        {S(-111, -175), S(-47, -48), S(-4, -14), S(7, 10), S(28, 25), S(44, 43), S(56, 43), S(56, 43), S(56, 43)},
        {S(-17, 78), S(33, 87), S(67, 110), S(85, 126), S(102, 145), S(112, 150), S(112, 159), S(112, 161), S(122, 163), S(122, 163), S(122, 165), S(122, 165), S(148, 165), S(148, 165)},
        {S(-36, 242), S(8, 280), S(13, 291), S(20, 299), S(20, 299), S(25, 313), S(25, 320), S(34, 327), S(36, 334), S(36, 343), S(43, 347), S(43, 355), S(43, 357), S(43, 357), S(43, 357)},
        {S(58, -21), S(110, 211), S(110, 290), S(112, 316), S(132, 337), S(132, 342), S(143, 344), S(146, 370), S(158, 370), S(158, 372), S(160, 372), S(172, 380), S(174, 387), S(174, 387), S(174, 392), S(174, 401), S(174, 403), S(192, 403), S(212, 403), S(212, 403), S(212, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 404), S(230, 404)},
        {},
    },
    {S(0, 0), S(-22, 45), S(-21, 58), S(-2, 74), S(3, 233), S(72, 410), S(281, 481), S(0, 0)},
    {S(0, 0), S(4, -14), S(54, 3), S(66, 7), S(67, 34), S(139, 66), S(376, 127), S(0, 0)},
    S(138, 18), // RookOpenFileBonus
    S(57, 18), // RookSemiOpenFileBonus
    S(95, 23), // KnightOutpostBonus
    S(96, 26), // BishopOutpostBonus
    S(-81, 0), // TrappedRookByKingPenalty
    S(72, 0), // RookBehindOurPasserBonus
    S(-8, 103), // RookBehindTheirPasserBonus
    S(27, 12), // MinorBehindPawnBonus
    S(26, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-20, -4), // KingProtector
    S(22, 24), // BishopPair
    {S(136, 0), S(65, 0)}, // PawnShieldBonus
    {S(0, 0), S(6, 0), S(88, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(52, 0), S(140, 0), S(0, 0), S(0, 0)}, // UnblockedPawnStorm
    S(-33, 0), // SemiOpenFileNearKing
    S(-182, 0), // OpenFileNearKing
    S(-45, 0), // UndefendedKingZoneSq
    {S(-17, -70), S(0, -17), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)}, // KingSafeSqPenalty
    S(24, 13), // KingAttackByKnight
    S(6, 13), // KingAttackByBishop
    S(24, 13), // KingAttackByRook
    S(24, 18), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 4), S(29, 8), S(29, 8), S(29, 8), S(0, 0)}, // KingSafeCheck
    S(16, 0), // KingRingWeakWeight
    S(39, 35), // KingNoQueenDiscount
    S(-14, -1), // IsolatedPawnPenalty
    S(-2, -36), // DoubledPawnPenalty
    S(0, -12), // BackwardPawnPenalty
    S(-24, -21), // WeakUnopposedPenalty
    S(0, -25), // DoubledIsolatedPenalty
    {S(-98, -28), S(-42, -103)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-7, -26), // PawnIslandPenalty
    {S(29, 0), S(26, 0)}, // CentralPawnBonus
    S(66, 17), // BishopLongDiagonalBonus
    S(0, 43), // InitiativePasser
    S(0, 48), // InitiativePawnCount
    S(0, 10), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 12), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -13), // InitiativeConstant
    S(77, 0), // SliderOnQueenBishop
    S(65, 0), // SliderOnQueenRook
    S(13, 3), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
