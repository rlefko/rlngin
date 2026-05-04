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
    S(244, 23), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(186, 10), S(222, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(274, 14), S(0, 0)},
    S(110, 0), // ThreatByKing
    S(112, 24), // Hanging
    S(22, 0), // WeakQueen
    S(18, 19), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 34), S(0, 35), S(0, 35), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 18), S(0, 36), S(0, 66), S(0, 78), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-36, -19), S(-36, -39), S(-54, -61), S(-358, -105), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(32, 10), S(33, 45), S(131, 95), S(131, 324), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-43, 18), S(48, 28), S(51, 28), S(51, 28), S(0, 0)},
    S(0, 28), // RookOn7thBonus
    S(-1, 0), // BadBishop
    S(-2, -7), // BishopPawns
    S(47, 0), // Tempo
    {S(0, 0), S(169, 154), S(894, 490), S(926, 419), S(1364, 650), S(2452, 1516), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-27, -9), S(-12, -13), S(-20, -15), S(3, -5), S(11, 0), S(-10, -9), S(-9, -14), S(-33, -27),
        S(-60, -14), S(-53, -23), S(-24, -28), S(-14, -29), S(9, -19), S(-29, -21), S(-41, -31), S(-53, -24),
        S(-42, -2), S(-39, -14), S(-16, -35), S(14, -36), S(15, -34), S(-8, -31), S(-33, -16), S(-42, -16),
        S(8, 30), S(0, 8), S(9, -12), S(39, -27), S(49, -24), S(17, -17), S(-10, 8), S(0, 21),
        S(4, 55), S(11, 54), S(45, 6), S(74, 6), S(77, 8), S(42, 8), S(7, 53), S(-14, 54),
        S(10, 39), S(-8, 37), S(30, 25), S(61, 20), S(61, 18), S(30, 24), S(-6, 37), S(3, 41),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-118, -21), S(-135, -7), S(-78, -14), S(-78, 3),
        S(-90, -7), S(-68, -3), S(-38, -6), S(-29, -3),
        S(-54, -1), S(3, -1), S(10, -11), S(16, 19),
        S(-12, 13), S(46, 17), S(56, 23), S(50, 28),
        S(46, 11), S(71, 16), S(93, 22), S(75, 28),
        S(0, -6), S(37, 7), S(66, 16), S(87, 28),
        S(-32, -40), S(3, -11), S(43, 0), S(51, 17),
        S(-72, -104), S(-14, -37), S(19, -4), S(33, 5)
    },
    // BishopPST (half-board)
    {
        S(0, -25), S(5, -21), S(-88, 2), S(-44, 1),
        S(9, -22), S(23, -23), S(15, -8), S(-26, 3),
        S(0, -10), S(44, 9), S(12, -7), S(-14, 19),
        S(10, -7), S(36, 1), S(39, 14), S(23, 2),
        S(-16, -1), S(44, 20), S(49, 13), S(34, 5),
        S(24, 0), S(18, 17), S(37, 10), S(29, 12),
        S(-24, -19), S(-52, -6), S(-14, 9), S(-9, -1),
        S(-39, 4), S(-45, 11), S(-34, 12), S(-27, 9)
    },
    // RookPST (half-board)
    {
        S(-44, -16), S(-26, -8), S(-6, -7), S(5, -12),
        S(-65, -28), S(-37, -26), S(-22, -12), S(-30, -15),
        S(-63, -19), S(-33, -13), S(-42, -12), S(-44, -14),
        S(-42, 1), S(-19, 6), S(-30, 11), S(-9, 0),
        S(-12, 9), S(13, 22), S(20, 15), S(24, 3),
        S(20, 12), S(19, 19), S(41, 15), S(54, 11),
        S(25, 0), S(21, 4), S(57, -2), S(57, 7),
        S(28, 9), S(33, 13), S(50, 10), S(58, 7)
    },
    // QueenPST (half-board)
    {
        S(5, -74), S(17, -61), S(20, -58), S(51, -49),
        S(24, -56), S(40, -52), S(57, -46), S(69, -36),
        S(24, -45), S(54, -27), S(59, -2), S(19, 4),
        S(51, -26), S(62, 8), S(28, 28), S(17, 34),
        S(37, -21), S(19, 12), S(23, 50), S(3, 52),
        S(16, -9), S(-24, 24), S(-29, 58), S(-8, 83),
        S(-79, 4), S(-129, 30), S(-74, 52), S(-51, 66),
        S(-99, -8), S(-88, 20), S(-57, 34), S(-49, 40)
    },
    // KingPST (half-board)
    {
        S(141, -94), S(150, -69), S(24, -41), S(-115, -50),
        S(170, -56), S(121, -37), S(6, -14), S(-65, -5),
        S(44, -39), S(43, -19), S(-9, -1), S(-47, 3),
        S(6, -15), S(4, 7), S(-21, 23), S(-45, 21),
        S(-10, 11), S(-14, 38), S(-29, 46), S(-47, 44),
        S(-19, 18), S(-20, 42), S(-23, 53), S(-35, 49),
        S(-23, -2), S(-21, 21), S(-24, 31), S(-29, 40),
        S(-23, -41), S(-21, -8), S(-24, 2), S(-28, 28)
    },
    {
        {},
        {},
        {S(-95, -95), S(-55, -55), S(-16, -18), S(1, -6), S(20, 6), S(35, 19), S(53, 23), S(60, 26), S(60, 26)},
        {S(-16, 53), S(19, 88), S(53, 119), S(77, 129), S(95, 133), S(104, 146), S(112, 149), S(113, 152), S(124, 152), S(124, 154), S(124, 155), S(124, 155), S(128, 155), S(128, 155)},
        {S(-37, 261), S(-7, 291), S(20, 299), S(20, 311), S(20, 311), S(21, 323), S(23, 325), S(34, 331), S(45, 333), S(53, 340), S(53, 347), S(53, 348), S(54, 350), S(54, 353), S(54, 353)},
        {S(11, 291), S(36, 316), S(61, 341), S(86, 342), S(104, 347), S(120, 350), S(131, 357), S(146, 357), S(147, 372), S(156, 375), S(157, 380), S(171, 380), S(171, 390), S(171, 393), S(175, 403), S(175, 403), S(180, 403), S(191, 403), S(191, 403), S(204, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403)},
        {},
    },
    {S(0, 0), S(-37, 27), S(-37, 29), S(-31, 68), S(4, 131), S(113, 325), S(553, 512), S(0, 0)},
    {S(0, 0), S(-1, -7), S(42, 6), S(45, 9), S(76, 22), S(81, 83), S(394, 124), S(0, 0)},
    S(128, 8), // RookOpenFileBonus
    S(55, 8), // RookSemiOpenFileBonus
    S(6, 0), // RookOnQueenFile
    S(69, 8), // KnightOutpostBonus
    S(78, 14), // BishopOutpostBonus
    S(-81, 0), // TrappedRookByKingPenalty
    S(82, 6), // RookBehindOurPasserBonus
    S(-51, 87), // RookBehindTheirPasserBonus
    S(23, 4), // MinorBehindPawnBonus
    S(26, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-18, -3), // KingProtector
    S(7, 0), // BishopPair
    {
        {S(0, 0), S(149, 0), S(144, 0), S(97, 0), S(76, 0), S(71, 0), S(2, 0)},
        {S(-96, 0), S(206, 0), S(127, 0), S(3, 0), S(0, 0), S(23, 0), S(108, 0)},
        {S(-2, 0), S(181, 0), S(63, 0), S(42, 0), S(30, 0), S(123, 0), S(83, 0)},
        {S(0, 0), S(98, 0), S(92, 0), S(76, 0), S(50, 0), S(50, 0), S(30, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(3, 0), S(7, 0), S(0, 0), S(6, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(117, 0), S(11, 0), S(9, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(101, 0), S(304, 0), S(66, 0), S(23, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(12, 0), S(260, 0), S(66, 0), S(11, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(129, 0), S(10, 0), S(0, 0), S(1, 0), S(0, 0)}, // BlockedStorm
    S(-51, 0), // UndefendedKingZoneSq
    S(8, 78), // KingMobilityFactor
    S(31, 34), // KingAttackByKnight
    S(12, 58), // KingAttackByBishop
    S(31, 34), // KingAttackByRook
    S(31, 34), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(33, 426), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(15, 12), // KingRingWeakWeight
    S(24, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -15), // DoubledPawnPenalty
    S(-13, -6), // BackwardPawnPenalty
    S(-21, -15), // WeakUnopposedPenalty
    S(-10, -31), // DoubledIsolatedPenalty
    {S(-67, -24), S(0, -44)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-12, -13), // PawnIslandPenalty
    {S(14, 0), S(0, 0)}, // CentralPawnBonus
    S(44, 18), // BishopLongDiagonalBonus
    S(-17, 0), // BishopXrayPawns
    S(0, 43), // InitiativePasser
    S(0, 51), // InitiativePawnCount
    S(0, 1), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 38), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(71, 5), // SliderOnQueenBishop
    S(47, 0), // SliderOnQueenRook
    S(4, 3), // RestrictedPiece
    S(36, 0), // ThreatByPawnPush
    S(-32, -14), // WeakQueenDefender
    S(79, 0), // KnightOnQueen
    S(-280, -108), // PawnlessFlank
    S(0, 7), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 78), // KBNKCornerEg
    S(0, 523), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
