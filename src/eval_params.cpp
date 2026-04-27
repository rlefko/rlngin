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
    {S(0, 0), S(175, 316), S(944, 763), S(990, 727), S(1455, 1202), S(2625, 2583), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-68, -66), S(-20, -85), S(-45, -83), S(-31, -108), S(27, -80), S(-4, -66), S(34, -75), S(-110, -78),
        S(-87, -80), S(-71, -97), S(-62, -106), S(-46, -111), S(-9, -101), S(-58, -88), S(-18, -109), S(-82, -100),
        S(-85, -53), S(-93, -76), S(-80, -121), S(-1, -144), S(-35, -121), S(-27, -101), S(-65, -100), S(-43, -91),
        S(-30, -28), S(-28, -65), S(49, -101), S(28, -103), S(54, -115), S(55, -98), S(-53, -63), S(-82, -38),
        S(31, 48), S(-38, 47), S(84, 4), S(45, 6), S(38, -13), S(31, 44), S(-100, 72), S(-27, 70),
        S(143, 283), S(139, 327), S(207, 313), S(244, 288), S(255, 281), S(149, 369), S(4, 349), S(-91, 324),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST
    {
        S(-318, 17), S(-107, 16), S(-90, -11), S(-99, -30), S(-48, 14), S(-12, -42), S(-108, 0), S(-151, 80),
        S(-191, -2), S(7, -50), S(-76, -4), S(9, 9), S(-36, 5), S(-58, 7), S(0, 57), S(-38, 9),
        S(-15, -9), S(24, -29), S(-4, 0), S(-26, 29), S(56, 22), S(22, 15), S(38, 0), S(-66, 63),
        S(65, -27), S(104, 2), S(70, 45), S(79, 35), S(78, 40), S(103, 50), S(55, 62), S(38, 30),
        S(195, -28), S(58, 25), S(97, 28), S(156, 17), S(102, 52), S(226, 14), S(102, 19), S(162, -32),
        S(-59, -21), S(36, 3), S(110, 28), S(138, 40), S(273, -14), S(173, -38), S(140, -3), S(98, -15),
        S(-132, -51), S(-122, 30), S(6, -7), S(54, 46), S(25, 7), S(181, -9), S(123, -39), S(-28, -110),
        S(-536, -104), S(-152, 17), S(-61, 5), S(-110, 38), S(158, -73), S(-361, -7), S(33, -35), S(-385, -138)
    },
    // BishopPST
    {
        S(-5, -63), S(93, -76), S(-47, 17), S(-24, -32), S(-121, 36), S(-110, 9), S(56, -8), S(7, -39),
        S(82, -95), S(39, -29), S(85, -46), S(-34, 21), S(-4, 4), S(-29, 18), S(44, -46), S(90, -58),
        S(58, -2), S(62, -12), S(-6, 29), S(2, 28), S(-3, 61), S(-19, -14), S(34, 25), S(32, -18),
        S(77, -25), S(58, 14), S(50, 6), S(64, 3), S(46, -10), S(2, 32), S(-7, 11), S(42, -9),
        S(-3, 42), S(100, 16), S(76, 21), S(21, -4), S(17, -16), S(101, -6), S(50, 22), S(-18, 11),
        S(27, 1), S(57, 32), S(-74, 25), S(80, 10), S(63, -4), S(168, -20), S(128, 0), S(126, -38),
        S(-90, -8), S(-174, -13), S(2, 34), S(-49, 34), S(-53, 40), S(44, 12), S(-203, 20), S(-133, -16),
        S(-124, -10), S(-70, 34), S(-162, 41), S(-182, 90), S(-193, 8), S(-187, 31), S(73, -29), S(-21, -110)
    },
    // RookPST
    {
        S(-86, -2), S(-70, -1), S(-28, -3), S(-33, -15), S(-14, -27), S(-9, -15), S(-14, -24), S(-96, -17),
        S(-73, -2), S(-95, -17), S(-43, -2), S(-69, -16), S(-69, -9), S(-38, -33), S(70, -71), S(-186, 9),
        S(-135, 1), S(-54, -9), S(-24, -9), S(-148, -6), S(-78, -20), S(-10, -36), S(15, -38), S(-151, 8),
        S(-82, 20), S(-141, 47), S(-11, 0), S(-83, 32), S(-85, 1), S(-105, 34), S(16, -2), S(1, -38),
        S(-13, 39), S(-29, 37), S(8, 33), S(-53, 10), S(-16, 11), S(26, -1), S(77, 18), S(78, 6),
        S(-9, 26), S(6, 17), S(13, 23), S(101, 3), S(136, -5), S(177, -10), S(136, 13), S(50, 19),
        S(-2, -5), S(-67, 6), S(68, -8), S(200, -24), S(229, -38), S(198, -22), S(-15, 9), S(152, -23),
        S(99, -25), S(31, 10), S(57, 9), S(71, 6), S(195, -14), S(64, 30), S(15, 17), S(-19, 32)
    },
    // QueenPST
    {
        S(21, -86), S(-80, 20), S(-66, -24), S(-43, 4), S(30, -47), S(-76, -59), S(-39, -147), S(-181, -155),
        S(-74, -19), S(38, -39), S(-24, 1), S(-9, 4), S(2, -28), S(53, -81), S(75, -186), S(4, -141),
        S(-81, -14), S(-15, -7), S(-27, 56), S(-16, -36), S(10, -14), S(16, 28), S(36, -28), S(-18, -26),
        S(4, -9), S(-36, 11), S(-23, 30), S(14, 56), S(20, 22), S(22, 84), S(5, 75), S(-1, -15),
        S(13, -5), S(39, -39), S(-23, 80), S(-9, 57), S(4, 103), S(54, 65), S(25, 83), S(40, 8),
        S(-11, -78), S(47, -52), S(-44, 54), S(6, 32), S(13, 114), S(96, 109), S(130, 74), S(155, -65),
        S(-83, -21), S(-152, 53), S(-119, 129), S(-21, 85), S(-69, 150), S(40, 69), S(-29, 47), S(110, -67),
        S(-142, 26), S(-149, 3), S(115, -40), S(13, 49), S(92, -20), S(57, -24), S(172, -114), S(133, -100)
    },
    // KingPST
    {
        S(-16, -90), S(92, -74), S(54, -52), S(-56, -56), S(-102, -73), S(-127, -61), S(109, -104), S(156, -147),
        S(61, -104), S(42, -57), S(13, -17), S(15, -20), S(30, -26), S(-29, -37), S(122, -61), S(179, -74),
        S(9, -54), S(159, -23), S(12, -19), S(-63, 8), S(-100, 0), S(13, -5), S(156, -38), S(-45, -27),
        S(-113, -2), S(170, -31), S(-107, 35), S(-174, 28), S(-274, 35), S(-116, 23), S(-77, 24), S(16, -43),
        S(87, 13), S(74, 66), S(-130, 68), S(-212, 69), S(-235, 46), S(-185, 63), S(-65, 65), S(-86, 28),
        S(72, 15), S(64, 87), S(6, 106), S(-98, 99), S(-102, 73), S(-70, 37), S(-32, 42), S(27, 37),
        S(87, -101), S(91, 27), S(-9, 74), S(59, 84), S(79, 94), S(14, 63), S(57, 63), S(24, 36),
        S(-134, -241), S(101, -55), S(126, -3), S(49, 73), S(3, 160), S(115, 92), S(186, 72), S(22, -196)
    },
    {
        {},
        {},
        {S(-111, -171), S(-47, -47), S(-13, -4), S(3, 12), S(31, 24), S(50, 43), S(56, 43), S(56, 43), S(56, 43)},
        {S(-6, 74), S(53, 81), S(83, 100), S(96, 124), S(107, 136), S(108, 154), S(113, 161), S(113, 165), S(113, 165), S(113, 165), S(114, 165), S(114, 165), S(164, 165), S(164, 165)},
        {S(-6, 226), S(4, 286), S(8, 292), S(20, 298), S(20, 299), S(22, 316), S(22, 321), S(26, 326), S(26, 335), S(26, 352), S(38, 354), S(39, 358), S(39, 358), S(39, 358), S(93, 358)},
        {S(106, -21), S(118, 155), S(118, 218), S(134, 244), S(134, 304), S(135, 340), S(141, 353), S(141, 362), S(149, 370), S(153, 370), S(153, 372), S(165, 372), S(165, 393), S(165, 393), S(174, 403), S(174, 403), S(174, 403), S(175, 403), S(195, 403), S(208, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 404), S(230, 404)},
        {},
    },
    {S(0, 0), S(-44, 60), S(-9, 76), S(-9, 76), S(0, 258), S(50, 428), S(211, 543), S(0, 0)},
    {S(0, 0), S(1, -17), S(34, 11), S(59, 14), S(70, 47), S(200, 108), S(384, 135), S(0, 0)},
    S(141, 18), // RookOpenFileBonus
    S(67, 18), // RookSemiOpenFileBonus
    S(93, 30), // KnightOutpostBonus
    S(119, 33), // BishopOutpostBonus
    S(-33, 0), // TrappedRookByKingPenalty
    S(97, 0), // RookBehindOurPasserBonus
    S(0, 124), // RookBehindTheirPasserBonus
    S(22, 18), // MinorBehindPawnBonus
    S(11, 2), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-24, 0), // KingProtector
    S(18, 41), // BishopPair
    {S(135, 0), S(75, 0)}, // PawnShieldBonus
    {S(0, 0), S(0, 0), S(84, 0), S(0, 0), S(0, 0)}, // BlockedPawnStorm
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
    S(0, 42), // InitiativePasser
    S(0, 55), // InitiativePawnCount
    S(0, 1), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 64), // InitiativeInfiltrate
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
