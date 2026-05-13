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
    S(288, 481), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(61, 0), // ThreatByKing
    S(33, 23), // Hanging
    S(50, 50), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 31), S(0, 39), S(0, 39), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 11), S(0, 32), S(0, 58), S(0, 76), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-14, -30), S(-14, -46), S(-14, -76), S(-148, -120), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 27), S(0, 60), S(14, 116), S(125, 387), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(21, 28), S(21, 28), S(74, 28), S(74, 28), S(0, 0)},
    S(0, 40), // RookOn7thBonus
    S(-17, 0), // BadBishop
    S(-3, -9), // BishopPawns
    S(38, 0), // Tempo
    {S(0, 0), S(131, 233), S(545, 678), S(647, 749), S(870, 1296), S(1836, 2361), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-28, -14), S(-12, -2), S(-20, -1), S(-12, -14), S(-3, -9), S(-9, 3), S(-17, 1), S(-33, -22),
        S(-54, -26), S(-47, -18), S(-35, -22), S(-51, -41), S(-29, -32), S(-32, -16), S(-41, -19), S(-60, -26),
        S(-26, -9), S(-30, -10), S(-14, -27), S(6, -46), S(9, -42), S(-11, -23), S(-28, -12), S(-28, -14),
        S(-1, 20), S(-6, 12), S(8, -5), S(64, -10), S(74, -6), S(14, -3), S(-6, 12), S(0, 19),
        S(11, 30), S(13, 28), S(31, 18), S(64, 15), S(65, 16), S(32, 18), S(13, 26), S(11, 32),
        S(11, 25), S(16, 24), S(34, 23), S(55, 23), S(55, 24), S(34, 23), S(16, 24), S(11, 26),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-106, -34), S(-115, -25), S(-89, -24), S(-63, -17),
        S(-87, -26), S(-67, -14), S(-65, -16), S(-50, -6),
        S(-74, -20), S(-36, -9), S(-14, -8), S(2, 15),
        S(-3, -3), S(8, 7), S(19, 22), S(11, 29),
        S(23, 4), S(28, 14), S(42, 29), S(49, 36),
        S(29, -7), S(44, 6), S(65, 24), S(76, 34),
        S(26, -16), S(38, -5), S(56, 6), S(72, 28),
        S(17, -26), S(34, -9), S(49, 5), S(60, 16)
    },
    // BishopPST (half-board)
    {
        S(-8, -18), S(7, -13), S(-76, -11), S(-67, -16),
        S(8, -18), S(-11, -37), S(-2, -9), S(-46, -6),
        S(-13, -16), S(8, 2), S(-11, -4), S(6, 9),
        S(-2, -15), S(7, 4), S(27, 17), S(17, 13),
        S(-18, -16), S(12, 9), S(28, 18), S(35, 19),
        S(12, -3), S(16, 16), S(26, 13), S(38, 24),
        S(-8, -11), S(-10, 0), S(12, 19), S(22, 23),
        S(-11, -7), S(-5, 7), S(4, 11), S(9, 14)
    },
    // RookPST (half-board)
    {
        S(-72, -31), S(-37, -33), S(-28, -29), S(-1, -36),
        S(-66, -37), S(-27, -38), S(-21, -32), S(-11, -30),
        S(-43, -28), S(-6, -19), S(-10, -17), S(1, -20),
        S(-32, -3), S(2, 7), S(1, 11), S(1, 4),
        S(-13, 19), S(15, 32), S(24, 31), S(23, 23),
        S(4, 25), S(25, 33), S(37, 37), S(49, 34),
        S(-3, 0), S(2, 6), S(29, 12), S(46, 16),
        S(16, 15), S(25, 21), S(40, 25), S(44, 24)
    },
    // QueenPST (half-board)
    {
        S(1, -72), S(16, -72), S(6, -70), S(17, -68),
        S(13, -65), S(15, -65), S(29, -54), S(28, -41),
        S(23, -41), S(31, -20), S(26, 2), S(17, 7),
        S(38, -15), S(41, 11), S(27, 40), S(6, 61),
        S(7, -17), S(12, 13), S(4, 40), S(0, 65),
        S(3, 0), S(-11, 18), S(-9, 47), S(1, 71),
        S(-33, 1), S(-66, 10), S(-36, 38), S(-21, 58),
        S(-48, 1), S(-50, 18), S(-41, 35), S(-32, 47)
    },
    // KingPST (half-board)
    {
        S(54, -126), S(33, -97), S(-5, -77), S(-88, -67),
        S(57, -85), S(27, -64), S(-21, -55), S(-35, -47),
        S(-3, -57), S(-7, -40), S(-20, -31), S(-25, -28),
        S(-19, -20), S(-10, -2), S(-8, 2), S(-10, 1),
        S(-7, 19), S(0, 31), S(2, 34), S(0, 31),
        S(2, 42), S(9, 56), S(9, 58), S(7, 53),
        S(3, 47), S(8, 58), S(9, 60), S(8, 61),
        S(3, 49), S(7, 56), S(10, 60), S(10, 62)
    },
    {
        {},
        {},
        {S(-63, -65), S(-43, -45), S(-23, -27), S(-9, -8), S(6, 6), S(15, 20), S(25, 20), S(34, 20), S(36, 20)},
        {S(-40, -36), S(-22, -27), S(-4, -9), S(13, 5), S(25, 22), S(27, 35), S(29, 41), S(29, 43), S(32, 43), S(32, 43), S(36, 43), S(36, 43), S(38, 43), S(42, 43)},
        {S(-26, -16), S(-11, -11), S(-4, -1), S(6, 12), S(6, 15), S(15, 24), S(20, 29), S(26, 36), S(32, 42), S(38, 49), S(44, 52), S(50, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-22, -74), S(-16, -62), S(-16, -50), S(-16, -38), S(-8, -26), S(2, -14), S(2, -2), S(8, 10), S(12, 22), S(19, 22), S(19, 27), S(22, 34), S(27, 35), S(30, 35), S(33, 35), S(33, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(47, 35), S(59, 47)},
        {},
    },
    {S(0, 0), S(-39, 19), S(-39, 19), S(-39, 21), S(0, 127), S(85, 336), S(260, 586), S(0, 0)},
    {S(0, 0), S(-7, -1), S(29, 4), S(32, 5), S(49, 33), S(52, 91), S(117, 186), S(0, 0)},
    S(62, 17), // RookOpenFileBonus
    S(29, 17), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(33, 29), // KnightOutpostBonus
    S(47, 26), // BishopOutpostBonus
    S(-36, 0), // TrappedRookByKingPenalty
    S(31, 18), // RookBehindOurPasserBonus
    S(13, 83), // RookBehindTheirPasserBonus
    S(20, 10), // MinorBehindPawnBonus
    S(26, 2), // MinorOnKingRing
    S(2, 0), // RookOnKingRing
    S(-8, -3), // KingProtector
    S(17, 21), // BishopPair
    {
        {S(0, 0), S(77, 0), S(76, 0), S(52, 0), S(40, 0), S(48, 0), S(121, 0)},
        {S(-36, 0), S(126, 0), S(73, 0), S(37, 0), S(0, 0), S(33, 0), S(0, 0)},
        {S(0, 0), S(113, 0), S(37, 0), S(40, 0), S(25, 0), S(49, 0), S(93, 0)},
        {S(0, 0), S(24, 0), S(7, 0), S(10, 0), S(47, 0), S(21, 0), S(72, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(56, 0), S(3, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(5, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(58, 0), S(32, 0), S(1, 0), S(0, 0), S(1, 0)},
        {S(0, 0), S(0, 0), S(45, 0), S(28, 0), S(4, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(107, 0), S(2, 0), S(10, 0), S(12, 0), S(9, 0)}, // BlockedStorm
    S(-25, -1), // UndefendedKingZoneSq
    S(15, 0), // KingMobilityFactor
    S(21, 12), // KingAttackByKnight
    S(1, 4), // KingAttackByBishop
    S(21, 31), // KingAttackByRook
    S(21, 31), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(29, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(3, 86), // KingRingWeakWeight
    S(0, 102), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-25, -29), // DoubledPawnPenalty
    S(-14, -2), // BackwardPawnPenalty
    S(-22, -14), // WeakUnopposedPenalty
    S(-22, -50), // DoubledIsolatedPenalty
    {S(0, -22), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-32, -18), // PawnIslandPenalty
    {S(19, 0), S(3, 0)}, // CentralPawnBonus
    S(25, 12), // BishopLongDiagonalBonus
    S(-15, 0), // BishopXrayPawns
    S(0, 19), // InitiativePasser
    S(0, 8), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(25, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(7, 2), // RestrictedPiece
    S(102, 22), // ThreatByPawnPush
    S(-17, 0), // WeakQueenDefender
    S(31, 0), // KnightOnQueen
    S(-123, -101), // PawnlessFlank
    S(0, 57), // QueenInfiltration
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
    S(0, 17), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
