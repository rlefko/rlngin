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
    S(325, 325), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 99), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(97, 0), // ThreatByKing
    S(25, 31), // Hanging
    S(50, 50), // WeakQueen
    S(0, 17), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 31), S(0, 39), S(0, 39), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 32), S(0, 56), S(0, 77), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-10, -26), S(-10, -56), S(-10, -84), S(-134, -128), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 32), S(0, 54), S(0, 130), S(224, 356), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(8, 28), S(18, 28), S(51, 28), S(98, 28), S(0, 0)},
    S(0, 52), // RookOn7thBonus
    S(-17, 0), // BadBishop
    S(-3, -9), // BishopPawns
    S(46, 0), // Tempo
    {S(0, 0), S(150, 228), S(513, 708), S(611, 782), S(811, 1347), S(1703, 2467), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-31, -15), S(-9, -5), S(-30, 4), S(-5, -15), S(3, -12), S(-16, 7), S(-11, 2), S(-34, -17),
        S(-57, -22), S(-48, -17), S(-36, -22), S(-47, -40), S(-29, -34), S(-32, -12), S(-40, -18), S(-64, -22),
        S(-28, -6), S(-29, -5), S(-13, -30), S(6, -43), S(3, -42), S(-9, -25), S(-26, -10), S(-30, -11),
        S(-4, 18), S(-6, 14), S(2, -7), S(56, -6), S(64, -3), S(8, -5), S(-9, 13), S(-3, 19),
        S(10, 29), S(12, 26), S(33, 13), S(63, 14), S(64, 14), S(34, 15), S(13, 26), S(11, 31),
        S(14, 26), S(18, 19), S(36, 19), S(56, 23), S(56, 24), S(36, 19), S(17, 17), S(14, 27),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-103, -43), S(-108, -43), S(-90, -29), S(-69, -17),
        S(-90, -31), S(-68, -17), S(-67, -15), S(-49, -15),
        S(-75, -23), S(-41, -16), S(-20, -5), S(-3, 15),
        S(-9, -5), S(6, 4), S(17, 28), S(4, 31),
        S(15, 7), S(29, 17), S(38, 34), S(38, 41),
        S(30, -4), S(45, 7), S(64, 27), S(81, 35),
        S(33, -11), S(40, -2), S(64, 10), S(78, 29),
        S(26, -19), S(40, -5), S(56, 8), S(66, 15)
    },
    // BishopPST (half-board)
    {
        S(3, -25), S(8, -20), S(-79, -14), S(-73, -21),
        S(6, -28), S(-5, -49), S(-3, -12), S(-48, -7),
        S(-15, -24), S(9, -2), S(-11, 5), S(3, 7),
        S(-9, -17), S(-3, 1), S(23, 17), S(19, 17),
        S(-19, -19), S(19, 8), S(29, 17), S(47, 22),
        S(11, -10), S(19, 13), S(26, 11), S(44, 24),
        S(-6, -4), S(-5, 0), S(11, 18), S(17, 24),
        S(-7, -1), S(-2, 5), S(4, 12), S(6, 23)
    },
    // RookPST (half-board)
    {
        S(-70, -30), S(-39, -39), S(-25, -32), S(5, -38),
        S(-65, -28), S(-30, -35), S(-24, -30), S(-11, -36),
        S(-46, -13), S(-8, -17), S(-11, -19), S(15, -37),
        S(-34, 9), S(-1, 10), S(7, 9), S(6, -2),
        S(-30, 27), S(5, 30), S(27, 27), S(29, 26),
        S(-3, 23), S(15, 26), S(35, 30), S(60, 37),
        S(-10, -15), S(0, -3), S(34, 4), S(50, 17),
        S(12, 18), S(26, 15), S(43, 19), S(52, 20)
    },
    // QueenPST (half-board)
    {
        S(-2, -77), S(0, -72), S(5, -63), S(15, -47),
        S(15, -61), S(17, -62), S(29, -50), S(32, -41),
        S(22, -37), S(40, -19), S(26, -9), S(15, 3),
        S(48, -4), S(46, 12), S(41, 28), S(6, 61),
        S(22, -5), S(22, 15), S(8, 39), S(-15, 70),
        S(9, -4), S(-17, 17), S(-10, 36), S(-6, 89),
        S(-16, -38), S(-67, 1), S(-30, 37), S(-24, 64),
        S(-80, -2), S(-56, 17), S(-39, 35), S(-30, 49)
    },
    // KingPST (half-board)
    {
        S(37, -119), S(18, -86), S(-17, -72), S(-92, -58),
        S(43, -81), S(12, -61), S(-28, -48), S(-22, -43),
        S(-18, -52), S(-14, -35), S(-18, -32), S(-23, -28),
        S(-52, -16), S(-19, -4), S(2, -2), S(3, -13),
        S(-22, 20), S(0, 32), S(12, 34), S(19, 30),
        S(-9, 38), S(14, 56), S(19, 60), S(22, 49),
        S(6, 39), S(16, 54), S(21, 57), S(24, 52),
        S(11, 43), S(16, 51), S(21, 55), S(24, 55)
    },
    {
        {},
        {},
        {S(-69, -68), S(-49, -48), S(-30, -28), S(-18, -8), S(-3, 6), S(5, 20), S(15, 20), S(29, 20), S(32, 20)},
        {S(-40, -46), S(-22, -28), S(-4, -10), S(12, 8), S(17, 19), S(24, 37), S(24, 38), S(28, 42), S(28, 43), S(28, 43), S(33, 43), S(33, 43), S(33, 43), S(38, 43)},
        {S(-19, -23), S(-12, -8), S(-3, 2), S(8, 2), S(8, 10), S(22, 20), S(22, 24), S(22, 35), S(32, 47), S(36, 48), S(48, 52), S(48, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-8, -74), S(-8, -62), S(-8, -50), S(-7, -38), S(2, -26), S(2, -14), S(2, -2), S(5, 10), S(13, 22), S(16, 26), S(22, 30), S(22, 35), S(29, 35), S(29, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(47, 35), S(54, 35), S(55, 35), S(67, 47)},
        {},
    },
    {S(0, 0), S(-40, 3), S(-40, 29), S(-40, 29), S(0, 135), S(81, 341), S(229, 612), S(0, 0)},
    {S(0, 0), S(-9, -4), S(27, 3), S(31, 3), S(44, 41), S(46, 111), S(80, 203), S(0, 0)},
    S(61, 18), // RookOpenFileBonus
    S(39, 14), // RookSemiOpenFileBonus
    S(0, 3), // RookOnQueenFile
    S(35, 29), // KnightOutpostBonus
    S(41, 29), // BishopOutpostBonus
    S(-38, 0), // TrappedRookByKingPenalty
    S(9, 31), // RookBehindOurPasserBonus
    S(5, 81), // RookBehindTheirPasserBonus
    S(13, 17), // MinorBehindPawnBonus
    S(19, 6), // MinorOnKingRing
    S(2, 0), // RookOnKingRing
    S(-8, -3), // KingProtector
    S(12, 23), // BishopPair
    {
        {S(0, 0), S(71, 0), S(76, 0), S(52, 0), S(40, 0), S(69, 0), S(126, 0)},
        {S(-29, 0), S(128, 0), S(81, 0), S(41, 0), S(10, 0), S(55, 0), S(0, 0)},
        {S(0, 0), S(123, 0), S(33, 0), S(40, 0), S(26, 0), S(59, 0), S(72, 0)},
        {S(0, 0), S(0, 0), S(4, 0), S(8, 0), S(64, 0), S(23, 0), S(69, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(46, 0), S(2, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(10, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(56, 0), S(32, 0), S(11, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(48, 0), S(23, 0), S(12, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(97, 0), S(0, 0), S(5, 0), S(16, 0), S(0, 0)}, // BlockedStorm
    S(-23, 0), // UndefendedKingZoneSq
    S(12, 0), // KingMobilityFactor
    S(21, 34), // KingAttackByKnight
    S(4, 15), // KingAttackByBishop
    S(21, 46), // KingAttackByRook
    S(21, 61), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(26, 0), S(26, 619), S(27, 0), S(27, 0), S(0, 0)}, // KingSafeCheck
    S(11, 54), // KingRingWeakWeight
    S(0, 111), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-38, -14), // DoubledPawnPenalty
    S(-16, 0), // BackwardPawnPenalty
    S(-29, -5), // WeakUnopposedPenalty
    S(-12, -62), // DoubledIsolatedPenalty
    {S(0, -17), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-24, -26), // PawnIslandPenalty
    {S(10, 0), S(2, 0)}, // CentralPawnBonus
    S(21, 5), // BishopLongDiagonalBonus
    S(-13, -2), // BishopXrayPawns
    S(0, 23), // InitiativePasser
    S(0, 9), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(16, 60), // SliderOnQueenBishop
    S(60, 30), // SliderOnQueenRook
    S(4, 3), // RestrictedPiece
    S(53, 37), // ThreatByPawnPush
    S(-17, -7), // WeakQueenDefender
    S(13, 19), // KnightOnQueen
    S(-114, -99), // PawnlessFlank
    S(0, 55), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 0), // KBNKPushClose
    S(0, 0), // KQKRPushToEdge
    S(0, 0), // KQKRPushClose
    S(0, 1), // KPsKFortressScale
    S(0, 0), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
    S(0, 19), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
