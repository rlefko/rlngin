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
    S(239, 67), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(227, 12), S(160, 37), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(239, 79), S(0, 0)},
    S(202, 40), // ThreatByKing
    S(7, 0), // Hanging
    S(72, 28), // WeakQueen
    S(56, 23), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 41), S(0, 49), S(0, 61), S(0, 61), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 26), S(0, 57), S(0, 86), S(0, 112), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-10, -44), S(-18, -61), S(-18, -98), S(-199, -223), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 37), S(12, 93), S(124, 178), S(262, 444), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(44, 18), S(96, 20), S(104, 20), S(229, 28), S(0, 0)},
    S(-34, 52), // RookOn7thBonus
    S(0, -14), // BadBishopPenalty
    S(32, 0), // Tempo
    {S(0, 0), S(175, 324), S(918, 768), S(930, 738), S(1374, 1237), S(2558, 2594), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-57, -63), S(-18, -79), S(-37, -88), S(-35, -102), S(16, -70), S(-3, -64), S(30, -70), S(-104, -81),
        S(-75, -80), S(-61, -94), S(-51, -109), S(-39, -110), S(-12, -99), S(-58, -86), S(-13, -106), S(-73, -101),
        S(-81, -54), S(-93, -75), S(-62, -125), S(8, -147), S(-23, -120), S(-22, -99), S(-65, -102), S(-37, -95),
        S(-37, -26), S(-29, -62), S(48, -102), S(25, -103), S(45, -115), S(64, -97), S(-52, -62), S(-79, -38),
        S(5, 52), S(-52, 40), S(58, 6), S(31, 0), S(19, -13), S(13, 41), S(-90, 67), S(-32, 62),
        S(156, 291), S(185, 329), S(181, 329), S(202, 303), S(206, 296), S(212, 356), S(5, 369), S(-91, 332),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-276, -10), S(-93, 27), S(-88, -18), S(-91, -29), S(-28, 7), S(4, -42), S(-97, 16), S(-85, 29),
        S(-185, -5), S(-3, -58), S(-74, -5), S(23, 9), S(-23, 8), S(-44, 18), S(-7, 56), S(-13, -2),
        S(-22, -10), S(16, -21), S(-7, -1), S(-29, 31), S(54, 21), S(21, 23), S(37, 5), S(-61, 62),
        S(59, -18), S(86, 6), S(60, 48), S(66, 44), S(82, 42), S(92, 57), S(56, 71), S(40, 44),
        S(172, -21), S(51, 33), S(77, 33), S(144, 22), S(76, 62), S(196, 25), S(88, 30), S(139, -23),
        S(-58, -24), S(47, 2), S(72, 35), S(124, 45), S(227, -1), S(197, -51), S(134, -12), S(94, -16),
        S(-139, -32), S(-120, 29), S(36, -14), S(49, 39), S(26, 0), S(159, -14), S(77, -26), S(-46, -111),
        S(-478, -123), S(-176, 10), S(-76, 4), S(-101, 31), S(162, -77), S(-295, -18), S(10, -62), S(-319, -189)
    },
    // BishopPST
    {
        S(-49, -53), S(69, -73), S(-37, 27), S(-2, -35), S(-89, 30), S(-102, 15), S(19, 6), S(-1, -50),
        S(66, -93), S(35, -33), S(81, -49), S(-24, 17), S(8, 2), S(-19, 14), S(44, -50), S(93, -60),
        S(42, -3), S(58, -12), S(-9, 28), S(-2, 26), S(-3, 57), S(-21, -9), S(40, 26), S(22, -20),
        S(69, -25), S(54, 11), S(41, 8), S(57, 9), S(29, -3), S(9, 30), S(-4, 12), S(46, -19),
        S(-11, 41), S(94, 21), S(55, 27), S(21, -1), S(12, -10), S(79, 0), S(45, 30), S(-26, 9),
        S(19, 1), S(60, 32), S(-50, 27), S(74, 8), S(66, -7), S(147, -10), S(120, -3), S(96, -32),
        S(-92, -7), S(-124, -19), S(-11, 38), S(-44, 24), S(-37, 34), S(59, 9), S(-135, 8), S(-111, -26),
        S(-121, 4), S(-66, 31), S(-157, 31), S(-138, 88), S(-149, 10), S(-182, 28), S(37, -32), S(-27, -96)
    },
    // RookPST
    {
        S(-64, -4), S(-54, -4), S(-20, -1), S(-22, -16), S(-7, -28), S(18, -19), S(1, -28), S(-73, -12),
        S(-62, -2), S(-65, -22), S(-30, -1), S(-60, -15), S(-69, -9), S(-18, -34), S(46, -57), S(-185, 8),
        S(-130, 3), S(-50, -10), S(-25, -5), S(-137, -7), S(-60, -24), S(-8, -35), S(-2, -37), S(-132, 6),
        S(-77, 23), S(-108, 45), S(-24, 3), S(-64, 31), S(-68, -1), S(-86, 29), S(18, 1), S(0, -29),
        S(-14, 43), S(-40, 40), S(19, 35), S(-49, 9), S(-27, 17), S(20, 0), S(38, 34), S(43, 23),
        S(-20, 29), S(-1, 20), S(-4, 22), S(76, 0), S(105, 0), S(146, -5), S(148, 13), S(46, 20),
        S(-2, -8), S(-53, 1), S(64, -8), S(169, -21), S(198, -32), S(194, -25), S(-2, 4), S(141, -24),
        S(68, -30), S(56, -1), S(24, 9), S(48, 4), S(164, -13), S(49, 25), S(20, 16), S(44, 21)
    },
    // QueenPST
    {
        S(24, -97), S(-45, -39), S(-44, -36), S(-19, -7), S(39, -49), S(-51, -92), S(-53, -123), S(-179, -147),
        S(-80, -29), S(54, -57), S(-10, -11), S(7, -3), S(4, -37), S(56, -81), S(38, -164), S(-1, -127),
        S(-79, -16), S(9, -47), S(-20, 47), S(-16, -42), S(3, -16), S(24, 22), S(30, -21), S(-16, -46),
        S(21, -22), S(-50, 25), S(-29, 25), S(-1, 68), S(8, 16), S(18, 86), S(-14, 91), S(-27, 23),
        S(-8, 16), S(6, -6), S(-21, 78), S(-24, 65), S(-6, 93), S(27, 86), S(10, 99), S(24, 16),
        S(-11, -80), S(2, -26), S(-27, 37), S(-17, 61), S(16, 104), S(112, 92), S(140, 64), S(126, -20),
        S(-73, -30), S(-140, 47), S(-68, 109), S(-31, 93), S(-59, 148), S(90, 52), S(-4, 45), S(108, -62),
        S(-91, -25), S(-82, -22), S(77, -18), S(23, 47), S(95, -4), S(51, -9), S(127, -76), S(88, -47)
    },
    // KingPST
    {
        S(6, -112), S(98, -72), S(68, -47), S(-33, -50), S(-80, -75), S(-121, -55), S(104, -105), S(134, -148),
        S(58, -108), S(35, -57), S(3, -12), S(15, -18), S(36, -25), S(-34, -38), S(104, -62), S(164, -75),
        S(-9, -55), S(100, -12), S(-9, -17), S(-54, 11), S(-126, 5), S(-4, -2), S(105, -36), S(-33, -34),
        S(-76, -5), S(111, -19), S(-78, 34), S(-121, 30), S(-221, 33), S(-118, 25), S(-95, 25), S(-10, -41),
        S(60, 12), S(23, 76), S(-77, 65), S(-159, 67), S(-186, 46), S(-132, 63), S(-44, 66), S(-57, 25),
        S(29, 25), S(85, 84), S(51, 104), S(-45, 97), S(-49, 75), S(-17, 35), S(21, 40), S(25, 47),
        S(84, -100), S(65, 37), S(-12, 73), S(49, 94), S(69, 104), S(11, 73), S(-2, 81), S(-2, 42),
        S(-137, -239), S(90, -69), S(108, -33), S(31, 19), S(-31, 106), S(72, 102), S(127, 82), S(19, -154)
    },
    {
        {},
        {},
        {S(-139, -187), S(-75, -49), S(-35, -5), S(-13, 11), S(21, 24), S(49, 39), S(56, 47), S(56, 47), S(56, 47)},
        {S(11, 39), S(48, 79), S(81, 96), S(92, 122), S(106, 133), S(108, 152), S(111, 161), S(111, 165), S(113, 165), S(114, 165), S(122, 165), S(122, 165), S(164, 165), S(164, 165)},
        {S(8, 162), S(12, 247), S(13, 272), S(20, 288), S(20, 290), S(22, 310), S(22, 313), S(22, 321), S(22, 330), S(26, 348), S(26, 352), S(34, 358), S(34, 358), S(34, 358), S(141, 358)},
        {S(106, -21), S(118, 107), S(118, 170), S(135, 228), S(137, 288), S(141, 330), S(145, 344), S(145, 355), S(149, 367), S(153, 369), S(153, 373), S(165, 373), S(165, 385), S(165, 393), S(173, 401), S(173, 401), S(173, 401), S(175, 403), S(195, 403), S(207, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 403), S(230, 428), S(230, 434)},
        {},
    },
    {S(0, 0), S(-40, 56), S(-7, 74), S(-7, 75), S(16, 261), S(82, 423), S(171, 550), S(0, 0)},
    {S(0, 0), S(5, -21), S(38, 6), S(67, 13), S(83, 47), S(216, 108), S(336, 135), S(0, 0)},
    S(145, 18), // RookOpenFileBonus
    S(71, 18), // RookSemiOpenFileBonus
    S(87, 33), // KnightOutpostBonus
    S(107, 35), // BishopOutpostBonus
    S(-32, 0), // TrappedRookByKingPenalty
    S(98, 0), // RookBehindOurPasserBonus
    S(40, 120), // RookBehindTheirPasserBonus
    S(26, 19), // MinorBehindPawnBonus
    S(2, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-20, 0), // KingProtector
    S(12, 55), // BishopPair
    {S(129, 0), S(67, 0)}, // PawnShieldBonus
    {S(0, 0), S(0, 0), S(72, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
    {S(0, 0), S(36, 0), S(122, 0), S(28, 0), S(2, 0)}, // UnblockedPawnStorm
    S(-28, 0), // SemiOpenFileNearKing
    S(-155, 0), // OpenFileNearKing
    S(-50, 0), // UndefendedKingZoneSq
    {S(-42, -68), S(0, -22), S(0, -12), S(0, -4), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)}, // KingSafeSqPenalty
    S(25, 1), // KingAttackByKnight
    S(11, 5), // KingAttackByBishop
    S(25, 5), // KingAttackByRook
    S(25, 6), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 0), S(29, 4), S(29, 4), S(29, 4), S(0, 0)}, // KingSafeCheck
    S(11, 0), // KingRingWeakWeight
    S(30, 7), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -45), // DoubledPawnPenalty
    S(-6, -14), // BackwardPawnPenalty
    S(-30, -20), // WeakUnopposedPenalty
    S(0, -35), // DoubledIsolatedPenalty
    {S(-62, -34), S(-26, -94)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-10, -35), // PawnIslandPenalty
    {S(20, 0), S(9, 0)}, // CentralPawnBonus
    S(74, 26), // BishopLongDiagonalBonus
    S(0, 55), // InitiativePasser
    S(0, 45), // InitiativePawnCount
    S(0, 9), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 63), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(83, 0), // SliderOnQueenBishop
    S(68, 0), // SliderOnQueenRook
    S(13, 0), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
