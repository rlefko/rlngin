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
    S(313, 325), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(59, 7), // ThreatByKing
    S(30, 24), // Hanging
    S(50, 50), // WeakQueen
    S(0, 3), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 30), S(0, 40), S(0, 40), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 31), S(0, 58), S(0, 79), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-10, -32), S(-10, -53), S(-10, -82), S(-116, -133), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 31), S(0, 56), S(7, 124), S(212, 351), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(11, 28), S(11, 28), S(65, 28), S(98, 28), S(0, 0)},
    S(0, 43), // RookOn7thBonus
    S(-11, 0), // BadBishop
    S(-3, -9), // BishopPawns
    S(44, 0), // Tempo
    {S(0, 0), S(150, 230), S(532, 697), S(618, 772), S(838, 1326), S(1742, 2438), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-30, -15), S(-11, -4), S(-28, 0), S(-8, -9), S(-1, -6), S(-17, 6), S(-14, 2), S(-33, -21),
        S(-55, -25), S(-51, -17), S(-38, -23), S(-49, -42), S(-30, -36), S(-35, -14), S(-43, -19), S(-62, -23),
        S(-28, -8), S(-29, -7), S(-12, -30), S(6, -42), S(4, -42), S(-7, -25), S(-27, -11), S(-29, -11),
        S(-2, 21), S(-2, 15), S(7, -8), S(55, -9), S(66, -6), S(13, -4), S(-3, 13), S(-2, 23),
        S(10, 30), S(13, 28), S(35, 16), S(65, 15), S(66, 14), S(35, 17), S(14, 30), S(10, 33),
        S(13, 25), S(17, 21), S(37, 22), S(57, 24), S(57, 25), S(37, 22), S(16, 20), S(13, 26),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-101, -44), S(-104, -44), S(-94, -22), S(-74, -13),
        S(-89, -31), S(-68, -18), S(-67, -16), S(-48, -13),
        S(-73, -22), S(-35, -14), S(-18, -6), S(1, 13),
        S(-8, -8), S(4, 2), S(20, 24), S(5, 29),
        S(15, 4), S(28, 15), S(37, 33), S(43, 35),
        S(28, -5), S(45, 6), S(67, 27), S(82, 33),
        S(33, -10), S(40, -1), S(64, 11), S(78, 28),
        S(33, -11), S(42, -2), S(56, 9), S(65, 15)
    },
    // BishopPST (half-board)
    {
        S(3, -22), S(10, -18), S(-76, -11), S(-71, -23),
        S(6, -28), S(-5, -44), S(2, -10), S(-48, -8),
        S(-15, -21), S(9, -4), S(-8, -1), S(7, 3),
        S(-10, -17), S(0, 3), S(27, 15), S(19, 12),
        S(-17, -15), S(15, 6), S(27, 18), S(39, 19),
        S(6, 4), S(17, 16), S(22, 15), S(37, 26),
        S(-7, -4), S(3, 5), S(9, 20), S(17, 24),
        S(-8, -1), S(0, 6), S(5, 11), S(9, 14)
    },
    // RookPST (half-board)
    {
        S(-72, -31), S(-41, -38), S(-26, -33), S(2, -39),
        S(-63, -35), S(-29, -38), S(-24, -33), S(-14, -34),
        S(-43, -20), S(-5, -26), S(-12, -18), S(12, -32),
        S(-31, -1), S(2, 6), S(6, 8), S(5, -1),
        S(-23, 21), S(10, 27), S(26, 22), S(27, 24),
        S(1, 24), S(19, 29), S(34, 28), S(56, 41),
        S(-3, -6), S(-1, 5), S(33, 12), S(43, 28),
        S(15, 19), S(23, 20), S(34, 24), S(42, 25)
    },
    // QueenPST (half-board)
    {
        S(1, -82), S(2, -86), S(8, -67), S(16, -57),
        S(20, -68), S(24, -63), S(29, -52), S(31, -42),
        S(26, -41), S(41, -18), S(27, -9), S(16, 0),
        S(48, -15), S(47, 1), S(38, 32), S(-3, 70),
        S(23, -21), S(18, 15), S(8, 40), S(-10, 72),
        S(5, 4), S(-17, 17), S(-6, 41), S(4, 84),
        S(-30, -25), S(-68, 16), S(-34, 44), S(-25, 63),
        S(-83, 7), S(-61, 23), S(-44, 39), S(-34, 50)
    },
    // KingPST (half-board)
    {
        S(39, -122), S(20, -90), S(-14, -74), S(-95, -62),
        S(43, -81), S(11, -58), S(-30, -49), S(-39, -43),
        S(-13, -53), S(-10, -36), S(-18, -32), S(-30, -27),
        S(-49, -15), S(-11, -3), S(3, -3), S(-6, -12),
        S(-21, 19), S(0, 31), S(7, 34), S(8, 32),
        S(3, 38), S(14, 55), S(18, 63), S(17, 52),
        S(12, 42), S(20, 55), S(24, 60), S(23, 55),
        S(15, 46), S(22, 54), S(31, 61), S(27, 59)
    },
    {
        {},
        {},
        {S(-68, -68), S(-48, -48), S(-32, -28), S(-18, -8), S(-2, 4), S(4, 20), S(13, 20), S(27, 20), S(27, 20)},
        {S(-40, -46), S(-22, -28), S(-4, -10), S(11, 8), S(16, 19), S(23, 37), S(28, 37), S(28, 43), S(32, 43), S(32, 43), S(37, 43), S(37, 43), S(39, 43), S(42, 43)},
        {S(-18, -17), S(-13, -5), S(-4, 3), S(8, 6), S(8, 10), S(22, 19), S(22, 24), S(22, 37), S(34, 43), S(34, 51), S(48, 53), S(49, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-8, -62), S(-8, -62), S(-8, -50), S(-7, -38), S(1, -26), S(2, -14), S(2, -2), S(2, 10), S(10, 22), S(14, 28), S(22, 31), S(22, 35), S(25, 35), S(28, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(44, 35), S(44, 35), S(47, 35), S(59, 47)},
        {},
    },
    {S(0, 0), S(-54, 15), S(-40, 25), S(-40, 29), S(0, 131), S(73, 348), S(218, 619), S(0, 0)},
    {S(0, 0), S(-8, -4), S(29, 3), S(31, 3), S(42, 43), S(45, 102), S(114, 179), S(0, 0)},
    S(61, 20), // RookOpenFileBonus
    S(35, 19), // RookSemiOpenFileBonus
    S(1, 0), // RookOnQueenFile
    S(34, 31), // KnightOutpostBonus
    S(43, 29), // BishopOutpostBonus
    S(-39, 0), // TrappedRookByKingPenalty
    S(12, 32), // RookBehindOurPasserBonus
    S(12, 80), // RookBehindTheirPasserBonus
    S(12, 17), // MinorBehindPawnBonus
    S(22, 4), // MinorOnKingRing
    S(3, 0), // RookOnKingRing
    S(-9, -3), // KingProtector
    S(18, 17), // BishopPair
    {
        {S(0, 0), S(74, 0), S(75, 0), S(53, 0), S(38, 0), S(67, 0), S(118, 0)},
        {S(-32, 0), S(124, 0), S(77, 0), S(39, 0), S(0, 0), S(39, 0), S(8, 0)},
        {S(0, 0), S(119, 0), S(38, 0), S(38, 0), S(18, 0), S(52, 0), S(68, 0)},
        {S(0, 0), S(1, 0), S(3, 0), S(12, 0), S(65, 0), S(29, 0), S(73, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(57, 0), S(3, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(5, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(61, 0), S(32, 0), S(6, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(50, 0), S(30, 0), S(10, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(107, 0), S(0, 0), S(9, 0), S(11, 0), S(3, 0)}, // BlockedStorm
    S(-25, 0), // UndefendedKingZoneSq
    S(15, 1), // KingMobilityFactor
    S(21, 28), // KingAttackByKnight
    S(2, 22), // KingAttackByBishop
    S(21, 31), // KingAttackByRook
    S(21, 42), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(31, 0), S(27, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(6, 67), // KingRingWeakWeight
    S(0, 104), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-39, -12), // DoubledPawnPenalty
    S(-17, 0), // BackwardPawnPenalty
    S(-25, -10), // WeakUnopposedPenalty
    S(-16, -60), // DoubledIsolatedPenalty
    {S(0, -22), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-25, -25), // PawnIslandPenalty
    {S(10, 0), S(2, 0)}, // CentralPawnBonus
    S(22, 10), // BishopLongDiagonalBonus
    S(-14, 0), // BishopXrayPawns
    S(0, 22), // InitiativePasser
    S(0, 9), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 44), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(20, 60), // SliderOnQueenBishop
    S(60, 51), // SliderOnQueenRook
    S(4, 3), // RestrictedPiece
    S(96, 8), // ThreatByPawnPush
    S(-16, -15), // WeakQueenDefender
    S(19, 13), // KnightOnQueen
    S(-112, -96), // PawnlessFlank
    S(0, 54), // QueenInfiltration
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
    S(0, 20), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
