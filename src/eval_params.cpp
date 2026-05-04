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
    S(251, 40), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(194, 0), S(187, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(234, 26), S(0, 0)},
    S(115, 5), // ThreatByKing
    S(108, 24), // Hanging
    S(0, 0), // WeakQueen
    S(24, 25), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 23), S(0, 39), S(0, 39), S(0, 39), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 37), S(0, 68), S(0, 77), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-36, -24), S(-36, -49), S(-80, -59), S(-359, -107), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 14), S(53, 50), S(131, 114), S(131, 346), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-48, 19), S(47, 28), S(47, 28), S(47, 28), S(0, 0)},
    S(0, 26), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -7), // BishopPawns
    S(72, 0), // Tempo
    {S(0, 0), S(164, 162), S(889, 496), S(914, 431), S(1382, 663), S(2434, 1535), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-24, -13), S(-18, -13), S(-18, -18), S(-4, -13), S(3, -5), S(-13, -13), S(-19, -17), S(-30, -28),
        S(-56, -14), S(-55, -22), S(-20, -31), S(-12, -28), S(3, -20), S(-27, -23), S(-44, -34), S(-57, -25),
        S(-33, -1), S(-35, -14), S(-16, -32), S(9, -31), S(16, -33), S(-7, -30), S(-30, -16), S(-39, -13),
        S(7, 38), S(-1, 14), S(10, -7), S(38, -18), S(47, -18), S(14, -12), S(-11, 14), S(1, 27),
        S(1, 43), S(13, 44), S(29, 8), S(52, 7), S(54, 10), S(30, 8), S(12, 45), S(-3, 47),
        S(6, 34), S(12, 35), S(30, 24), S(49, 21), S(49, 20), S(30, 24), S(12, 36), S(5, 35),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-113, -25), S(-122, -7), S(-76, -15), S(-78, 14),
        S(-91, -10), S(-70, -6), S(-41, -5), S(-35, 2),
        S(-41, 2), S(-2, -1), S(12, -12), S(19, 20),
        S(-25, 12), S(29, 11), S(46, 23), S(52, 29),
        S(32, 16), S(64, 14), S(82, 20), S(64, 29),
        S(5, -6), S(33, 2), S(71, 17), S(77, 31),
        S(-20, -37), S(13, -7), S(42, -10), S(51, 16),
        S(-49, -88), S(-5, -34), S(25, -5), S(37, 4)
    },
    // BishopPST (half-board)
    {
        S(-5, -24), S(0, -26), S(-78, -1), S(-50, 1),
        S(-7, -27), S(9, -23), S(16, -10), S(-27, 3),
        S(-2, -17), S(38, 8), S(-2, -9), S(-12, 16),
        S(5, -12), S(32, -1), S(31, 14), S(21, 0),
        S(-9, -9), S(40, 25), S(48, 13), S(39, -3),
        S(14, -3), S(17, 19), S(31, 11), S(27, 6),
        S(-24, -16), S(-34, -2), S(-4, 7), S(-9, -7),
        S(-22, 10), S(-27, 17), S(-22, 12), S(-18, 14)
    },
    // RookPST (half-board)
    {
        S(-50, -15), S(-36, -3), S(-8, -4), S(-2, -10),
        S(-68, -26), S(-41, -25), S(-16, -13), S(-31, -12),
        S(-63, -21), S(-41, -13), S(-48, -16), S(-47, -15),
        S(-40, -2), S(-29, 2), S(-28, 13), S(5, -3),
        S(-25, 14), S(4, 20), S(25, 12), S(32, 1),
        S(13, 9), S(16, 18), S(46, 15), S(63, 9),
        S(30, -1), S(29, 3), S(54, -3), S(66, 7),
        S(32, 8), S(34, 10), S(45, 6), S(64, 6)
    },
    // QueenPST (half-board)
    {
        S(13, -81), S(29, -68), S(27, -59), S(53, -46),
        S(13, -40), S(31, -45), S(54, -42), S(65, -31),
        S(2, -37), S(44, -21), S(49, 5), S(26, -3),
        S(34, -19), S(60, 8), S(36, 23), S(22, 28),
        S(20, -10), S(18, 5), S(22, 53), S(15, 54),
        S(5, -5), S(-21, 12), S(-23, 51), S(-10, 84),
        S(-72, 0), S(-109, 29), S(-73, 63), S(-59, 77),
        S(-84, -19), S(-88, 0), S(-54, 28), S(-59, 20)
    },
    // KingPST (half-board)
    {
        S(135, -98), S(147, -68), S(0, -37), S(-128, -49),
        S(172, -57), S(130, -35), S(-3, -9), S(-73, -2),
        S(46, -36), S(55, -18), S(-3, 2), S(-45, 8),
        S(2, -17), S(6, 6), S(-16, 25), S(-47, 23),
        S(-10, 0), S(-8, 29), S(-25, 36), S(-45, 37),
        S(-17, 8), S(-16, 33), S(-24, 50), S(-35, 51),
        S(-23, -4), S(-21, 15), S(-26, 24), S(-27, 36),
        S(-27, -10), S(-23, 7), S(-26, 13), S(-26, 26)
    },
    {
        {},
        {},
        {S(-95, -95), S(-55, -55), S(-15, -15), S(10, -7), S(26, 9), S(39, 26), S(60, 31), S(60, 35), S(60, 35)},
        {S(-16, 51), S(19, 86), S(54, 121), S(85, 132), S(107, 133), S(111, 148), S(122, 156), S(125, 157), S(128, 159), S(128, 163), S(128, 163), S(128, 163), S(128, 163), S(128, 163)},
        {S(-37, 254), S(-7, 284), S(19, 293), S(20, 306), S(20, 306), S(20, 323), S(21, 324), S(35, 333), S(40, 334), S(53, 345), S(53, 351), S(53, 353), S(53, 357), S(53, 357), S(53, 357)},
        {S(11, 291), S(36, 316), S(61, 341), S(86, 345), S(94, 349), S(119, 349), S(136, 356), S(151, 356), S(156, 370), S(168, 374), S(168, 379), S(172, 380), S(172, 384), S(174, 397), S(174, 402), S(174, 403), S(179, 403), S(193, 403), S(193, 403), S(213, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403)},
        {},
    },
    {S(0, 0), S(-37, 33), S(-37, 33), S(-37, 71), S(9, 131), S(147, 333), S(572, 506), S(0, 0)},
    {S(0, 0), S(-5, -9), S(43, 8), S(45, 9), S(68, 20), S(68, 83), S(365, 114), S(0, 0)},
    S(121, 9), // RookOpenFileBonus
    S(43, 6), // RookSemiOpenFileBonus
    S(8, 1), // RookOnQueenFile
    S(89, 8), // KnightOutpostBonus
    S(87, 12), // BishopOutpostBonus
    S(-96, 0), // TrappedRookByKingPenalty
    S(71, 8), // RookBehindOurPasserBonus
    S(-71, 95), // RookBehindTheirPasserBonus
    S(27, 2), // MinorBehindPawnBonus
    S(30, 0), // MinorOnKingRing
    S(13, 0), // RookOnKingRing
    S(-18, -3), // KingProtector
    S(10, 0), // BishopPair
    {
        {S(0, 0), S(137, 0), S(136, 0), S(86, 0), S(71, 0), S(31, 0), S(0, 0)},
        {S(-86, 0), S(218, 0), S(116, 0), S(12, 0), S(0, 0), S(34, 0), S(111, 0)},
        {S(-2, 0), S(190, 0), S(59, 0), S(33, 0), S(29, 0), S(106, 0), S(80, 0)},
        {S(0, 0), S(119, 0), S(84, 0), S(66, 0), S(33, 0), S(85, 0), S(33, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(5, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(105, 0), S(9, 0), S(5, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(137, 0), S(330, 0), S(67, 0), S(21, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(44, 0), S(300, 0), S(69, 0), S(4, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(161, 0), S(11, 0), S(0, 0), S(13, 0), S(0, 0)}, // BlockedStorm
    S(-44, 0), // UndefendedKingZoneSq
    S(9, 65), // KingMobilityFactor
    S(31, 32), // KingAttackByKnight
    S(11, 50), // KingAttackByBishop
    S(31, 32), // KingAttackByRook
    S(31, 32), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 1), S(37, 409), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(20, 23), // KingRingWeakWeight
    S(29, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -9), // DoubledPawnPenalty
    S(-14, -6), // BackwardPawnPenalty
    S(-24, -16), // WeakUnopposedPenalty
    S(0, -37), // DoubledIsolatedPenalty
    {S(-70, -29), S(0, -33)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-7, -15), // PawnIslandPenalty
    {S(21, 0), S(0, 0)}, // CentralPawnBonus
    S(60, 17), // BishopLongDiagonalBonus
    S(-21, 0), // BishopXrayPawns
    S(0, 42), // InitiativePasser
    S(0, 49), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(54, 7), // SliderOnQueenBishop
    S(45, 0), // SliderOnQueenRook
    S(6, 1), // RestrictedPiece
    S(34, 0), // ThreatByPawnPush
    S(-31, -14), // WeakQueenDefender
    S(89, 0), // KnightOnQueen
    S(-279, -120), // PawnlessFlank
    S(0, 11), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 78), // KBNKCornerEg
    S(0, 500), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
