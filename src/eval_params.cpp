#include "eval_params.h"

// Compiled-in defaults for the tunable parameters. The in-memory
// `evalParams` instance is initialized from this struct, and
// `resetEvalParams()` snaps it back to these values after a tune run.
//
// Values below come from a Texel tune covering every scalar in
// `EvalParams` (1067 mg/eg halves) over 6.4M qsearch-leaf positions
// extracted from the PR #36 self-play PGN (64,000 games at nodes=25000)
// with mate-scored plies filtered out at extraction. Initial loss was
// 0.0875643; this snapshot was taken after the running tune's pass 3
// of the upgraded leg, then loaded through the canonicalization
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
//   - Periodic K refit (every 5 passes) and optional periodic leaf
//     refresh against the evolving params.
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
    S(244, 60), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(249, 8), S(188, 1), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(279, 23), S(0, 0)},
    S(177, 38), // ThreatByKing
    S(15, 0), // Hanging
    S(72, 28), // WeakQueen
    S(54, 15), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 38), S(0, 54), S(0, 57), S(0, 57), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 24), S(0, 59), S(0, 86), S(0, 113), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-18, -48), S(-18, -61), S(-18, -98), S(-215, -209), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(24, 44), S(24, 73), S(180, 152), S(294, 500), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(42, 18), S(136, 18), S(136, 28), S(245, 28), S(0, 0)},
    S(0, 47), // RookOn7thBonus
    S(-3, -14), // BadBishopPenalty
    S(36, 0), // Tempo
    {S(0, 0), S(175, 316), S(941, 762), S(987, 727), S(1449, 1202), S(2622, 2585), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-68, -66), S(-19, -85), S(-45, -82), S(-32, -107), S(27, -79), S(-4, -66), S(34, -74), S(-110, -77),
        S(-86, -78), S(-70, -97), S(-62, -105), S(-47, -111), S(-10, -101), S(-59, -87), S(-19, -108), S(-83, -100),
        S(-85, -52), S(-94, -76), S(-80, -120), S(-4, -143), S(-34, -120), S(-27, -100), S(-64, -99), S(-43, -90),
        S(-33, -27), S(-28, -64), S(46, -101), S(27, -102), S(51, -114), S(55, -98), S(-54, -63), S(-81, -37),
        S(31, 49), S(-39, 47), S(84, 5), S(45, 7), S(37, -12), S(31, 45), S(-104, 73), S(-27, 71),
        S(142, 284), S(139, 328), S(207, 314), S(244, 288), S(256, 281), S(150, 369), S(-1, 350), S(-97, 325),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-315, 18), S(-104, 17), S(-87, -14), S(-98, -29), S(-47, 11), S(-11, -46), S(-105, 1), S(-148, 81),
        S(-192, -1), S(6, -54), S(-77, -9), S(8, 10), S(-33, 6), S(-55, 8), S(-6, 52), S(-36, 10),
        S(-12, -8), S(25, -28), S(-1, 1), S(-24, 30), S(55, 17), S(25, 16), S(41, -3), S(-64, 58),
        S(68, -26), S(107, -2), S(72, 46), S(82, 36), S(80, 41), S(106, 51), S(57, 63), S(40, 31),
        S(197, -27), S(61, 26), S(98, 29), S(157, 18), S(105, 53), S(229, 15), S(105, 20), S(164, -31),
        S(-65, -24), S(32, 4), S(113, 29), S(140, 41), S(276, -13), S(170, -41), S(139, -8), S(95, -20),
        S(-130, -50), S(-119, 27), S(5, -6), S(50, 43), S(19, 4), S(184, -14), S(126, -38), S(-29, -115),
        S(-533, -103), S(-151, 14), S(-67, 0), S(-116, 35), S(155, -73), S(-358, -6), S(35, -34), S(-382, -137)
    },
    // BishopPST
    {
        S(-2, -63), S(92, -76), S(-44, 17), S(-23, -33), S(-122, 36), S(-107, 9), S(58, -8), S(6, -40),
        S(81, -95), S(41, -29), S(84, -47), S(-35, 20), S(-2, 4), S(-32, 18), S(39, -46), S(84, -58),
        S(57, -2), S(63, -12), S(-4, 29), S(1, 28), S(-8, 60), S(-16, -14), S(33, 24), S(29, -18),
        S(76, -26), S(53, 14), S(52, 6), S(63, 3), S(48, -10), S(4, 31), S(-13, 10), S(37, -9),
        S(0, 42), S(101, 16), S(78, 21), S(20, -4), S(19, -16), S(102, -6), S(52, 22), S(-19, 11),
        S(30, 1), S(59, 32), S(-75, 25), S(77, 10), S(65, -5), S(170, -20), S(127, -1), S(127, -38),
        S(-89, -9), S(-173, -13), S(4, 34), S(-53, 34), S(-54, 40), S(46, 11), S(-200, 20), S(-136, -16),
        S(-122, -10), S(-75, 33), S(-166, 41), S(-179, 90), S(-190, 8), S(-191, 30), S(76, -30), S(-20, -110)
    },
    // RookPST
    {
        S(-87, -2), S(-81, 1), S(-35, -2), S(-31, -13), S(-15, -27), S(-10, -13), S(-8, -23), S(-90, -17),
        S(-71, -2), S(-96, -15), S(-41, 0), S(-63, -14), S(-84, -8), S(-32, -32), S(76, -70), S(-200, 11),
        S(-149, 3), S(-61, -7), S(-22, -7), S(-146, -4), S(-79, -18), S(-11, -34), S(19, -38), S(-147, 9),
        S(-96, 20), S(-139, 48), S(-7, 0), S(-79, 34), S(-87, 2), S(-101, 34), S(15, -2), S(5, -36),
        S(-9, 40), S(-27, 38), S(12, 35), S(-60, 12), S(-14, 12), S(25, 1), S(83, 18), S(84, 6),
        S(-7, 26), S(12, 19), S(17, 23), S(105, 3), S(142, -5), S(183, -10), S(129, 13), S(43, 21),
        S(-13, -4), S(-78, 8), S(61, -6), S(206, -24), S(235, -38), S(191, -22), S(-17, 9), S(154, -23),
        S(105, -25), S(29, 10), S(61, 10), S(77, 7), S(201, -14), S(70, 30), S(1, 19), S(-15, 32)
    },
    // QueenPST
    {
        S(16, -86), S(-77, 18), S(-64, -23), S(-42, 4), S(31, -48), S(-75, -59), S(-37, -146), S(-187, -154),
        S(-80, -20), S(40, -40), S(-24, 2), S(-6, 4), S(-2, -29), S(48, -82), S(78, -187), S(-1, -142),
        S(-83, -15), S(-13, -9), S(-31, 54), S(-16, -37), S(10, -15), S(17, 27), S(38, -29), S(-24, -25),
        S(6, -9), S(-34, 10), S(-27, 30), S(16, 57), S(20, 21), S(22, 83), S(6, 76), S(1, -16),
        S(16, -7), S(42, -41), S(-25, 79), S(-8, 58), S(2, 102), S(57, 63), S(26, 84), S(42, 9),
        S(-11, -79), S(50, -53), S(-42, 54), S(7, 30), S(8, 113), S(96, 109), S(132, 73), S(158, -67),
        S(-87, -21), S(-156, 52), S(-116, 130), S(-23, 86), S(-67, 149), S(42, 69), S(-28, 46), S(108, -69),
        S(-139, 24), S(-146, 3), S(117, -41), S(15, 48), S(87, -19), S(59, -24), S(175, -115), S(136, -102)
    },
    // KingPST
    {
        S(-16, -89), S(92, -73), S(54, -52), S(-56, -55), S(-102, -72), S(-127, -60), S(108, -103), S(156, -147),
        S(60, -103), S(41, -56), S(13, -16), S(15, -19), S(30, -25), S(-29, -36), S(122, -61), S(178, -73),
        S(9, -56), S(158, -22), S(11, -18), S(-64, 9), S(-100, 1), S(13, -4), S(155, -37), S(-45, -27),
        S(-114, -2), S(169, -30), S(-108, 33), S(-175, 29), S(-275, 36), S(-116, 24), S(-77, 24), S(16, -42),
        S(86, 11), S(73, 67), S(-131, 66), S(-213, 70), S(-236, 47), S(-186, 64), S(-66, 65), S(-87, 24),
        S(71, 16), S(63, 83), S(5, 107), S(-99, 100), S(-103, 74), S(-71, 38), S(-33, 43), S(27, 38),
        S(86, -101), S(91, 28), S(-10, 72), S(59, 85), S(79, 95), S(13, 64), S(56, 64), S(24, 37),
        S(-135, -240), S(100, -54), S(126, -2), S(49, 74), S(3, 161), S(114, 93), S(185, 73), S(21, -195)
    },
    {
        {},
        {},
        {S(-111, -171), S(-47, -47), S(-13, -4), S(3, 12), S(31, 24), S(50, 43), S(56, 43), S(56, 43), S(56, 43)},
        {S(-6, 74), S(53, 81), S(83, 100), S(96, 124), S(107, 136), S(108, 154), S(113, 161), S(113, 165), S(113, 165), S(113, 165), S(114, 165), S(114, 165), S(164, 165), S(164, 165)},
        {S(-6, 226), S(4, 286), S(8, 292), S(20, 298), S(20, 299), S(22, 316), S(22, 321), S(26, 326), S(26, 335), S(26, 352), S(38, 354), S(39, 358), S(39, 358), S(39, 358), S(85, 358)},
        {S(106, -21), S(118, 163), S(118, 226), S(134, 252), S(134, 304), S(135, 340), S(141, 353), S(141, 362), S(149, 370), S(153, 370), S(153, 372), S(165, 372), S(165, 393), S(165, 393), S(174, 403), S(174, 403), S(174, 403), S(175, 403), S(195, 403), S(208, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 404), S(230, 404)},
        {},
    },
    {S(0, 0), S(-44, 60), S(-9, 76), S(-9, 76), S(0, 254), S(50, 428), S(211, 543), S(0, 0)},
    {S(0, 0), S(1, -17), S(34, 11), S(59, 14), S(70, 47), S(198, 108), S(392, 135), S(0, 0)},
    S(141, 18), // RookOpenFileBonus
    S(67, 18), // RookSemiOpenFileBonus
    S(93, 30), // KnightOutpostBonus
    S(119, 33), // BishopOutpostBonus
    S(-37, 0), // TrappedRookByKingPenalty
    S(97, 0), // RookBehindOurPasserBonus
    S(0, 124), // RookBehindTheirPasserBonus
    S(22, 18), // MinorBehindPawnBonus
    S(11, 2), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-24, 0), // KingProtector
    S(18, 41), // BishopPair
    {S(135, 0), S(75, 0)}, // PawnShieldBonus
    {S(0, 0), S(0, 0), S(88, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(40, 0), S(132, 0), S(42, 0), S(16, 0)}, // UnblockedPawnStorm
    S(-25, 0), // SemiOpenFileNearKing
    S(-146, 0), // OpenFileNearKing
    S(-50, 0), // UndefendedKingZoneSq
    {S(-37, -70), S(0, -19), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)}, // KingSafeSqPenalty
    S(24, 13), // KingAttackByKnight
    S(9, 13), // KingAttackByBishop
    S(24, 13), // KingAttackByRook
    S(24, 18), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 4), S(29, 0), S(29, 8), S(29, 8), S(0, 0)}, // KingSafeCheck
    S(13, 0), // KingRingWeakWeight
    S(29, 35), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -40), // DoubledPawnPenalty
    S(0, -12), // BackwardPawnPenalty
    S(-28, -21), // WeakUnopposedPenalty
    S(0, -35), // DoubledIsolatedPenalty
    {S(-66, -32), S(-41, -98)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-11, -35), // PawnIslandPenalty
    {S(25, 0), S(19, 0)}, // CentralPawnBonus
    S(66, 25), // BishopLongDiagonalBonus
    S(0, 40), // InitiativePasser
    S(0, 55), // InitiativePawnCount
    S(0, 1), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 68), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(79, 0), // SliderOnQueenBishop
    S(69, 0), // SliderOnQueenRook
    S(17, 2), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
