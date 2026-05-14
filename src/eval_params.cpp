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
    S(279, 401), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 98), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(67, 0), // ThreatByKing
    S(26, 29), // Hanging
    S(50, 50), // WeakQueen
    S(0, 1), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 22), S(0, 28), S(0, 40), S(0, 40), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 30), S(0, 60), S(0, 76), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-10, -34), S(-10, -51), S(-10, -77), S(-149, -123), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 27), S(0, 62), S(18, 116), S(178, 369), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(15, 28), S(15, 28), S(73, 28), S(73, 28), S(0, 0)},
    S(0, 42), // RookOn7thBonus
    S(-14, 0), // BadBishop
    S(-3, -9), // BishopPawns
    S(46, 0), // Tempo
    {S(0, 0), S(150, 234), S(545, 690), S(629, 765), S(851, 1319), S(1773, 2416), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-31, -15), S(-18, -1), S(-21, -2), S(-26, 6), S(-8, 18), S(-11, 2), S(-19, 1), S(-36, -22),
        S(-55, -27), S(-51, -18), S(-36, -23), S(-56, -40), S(-32, -32), S(-34, -17), S(-43, -21), S(-61, -27),
        S(-28, -10), S(-30, -11), S(-13, -27), S(2, -45), S(6, -42), S(-11, -23), S(-27, -14), S(-29, -15),
        S(-1, 19), S(-6, 12), S(10, -7), S(57, -6), S(78, -6), S(16, -7), S(-5, 12), S(0, 18),
        S(12, 28), S(15, 25), S(34, 16), S(65, 15), S(67, 16), S(35, 17), S(15, 24), S(12, 30),
        S(15, 24), S(20, 25), S(37, 23), S(56, 22), S(56, 23), S(37, 23), S(20, 25), S(15, 24),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-103, -38), S(-107, -32), S(-87, -25), S(-64, -17),
        S(-86, -29), S(-66, -17), S(-62, -18), S(-47, -8),
        S(-71, -23), S(-33, -12), S(-6, -13), S(6, 12),
        S(-2, -4), S(10, 5), S(23, 19), S(12, 27),
        S(22, 5), S(30, 14), S(44, 28), S(53, 33),
        S(27, -5), S(44, 7), S(63, 25), S(74, 35),
        S(25, -13), S(35, -2), S(53, 9), S(68, 30),
        S(18, -20), S(32, -4), S(46, 9), S(57, 19)
    },
    // BishopPST (half-board)
    {
        S(-7, -17), S(7, -13), S(-75, -11), S(-65, -17),
        S(8, -18), S(-9, -39), S(0, -9), S(-44, -7),
        S(-11, -17), S(10, 0), S(-6, -7), S(15, 3),
        S(-1, -16), S(8, 2), S(32, 13), S(20, 10),
        S(-19, -14), S(1, 13), S(26, 18), S(34, 19),
        S(11, -3), S(12, 17), S(22, 13), S(37, 24),
        S(-9, -10), S(-13, 1), S(10, 18), S(21, 23),
        S(-11, -5), S(-5, 7), S(4, 11), S(10, 14)
    },
    // RookPST (half-board)
    {
        S(-72, -31), S(-37, -33), S(-28, -29), S(-2, -36),
        S(-65, -37), S(-29, -36), S(-23, -30), S(-11, -29),
        S(-42, -27), S(-8, -19), S(-11, -16), S(1, -20),
        S(-31, -4), S(3, 6), S(3, 10), S(2, 3),
        S(-13, 19), S(16, 31), S(26, 31), S(23, 24),
        S(4, 25), S(25, 33), S(39, 35), S(51, 34),
        S(-1, -1), S(2, 4), S(30, 11), S(45, 16),
        S(17, 14), S(25, 23), S(41, 26), S(45, 24)
    },
    // QueenPST (half-board)
    {
        S(7, -81), S(20, -78), S(13, -77), S(30, -80),
        S(20, -70), S(23, -73), S(38, -63), S(34, -46),
        S(30, -45), S(40, -27), S(34, -4), S(23, 2),
        S(47, -20), S(47, 7), S(32, 38), S(-15, 73),
        S(12, -17), S(18, 13), S(9, 40), S(-1, 68),
        S(3, 3), S(-16, 24), S(-10, 50), S(2, 73),
        S(-41, 13), S(-107, 32), S(-45, 48), S(-23, 62),
        S(-56, 13), S(-66, 32), S(-47, 43), S(-33, 51)
    },
    // KingPST (half-board)
    {
        S(44, -125), S(26, -96), S(-11, -77), S(-95, -66),
        S(50, -85), S(20, -62), S(-26, -55), S(-43, -43),
        S(-6, -57), S(-9, -40), S(-22, -32), S(-25, -30),
        S(-19, -19), S(-9, -3), S(-6, 1), S(-3, -8),
        S(-5, 20), S(1, 31), S(3, 34), S(4, 30),
        S(7, 44), S(13, 55), S(14, 57), S(13, 53),
        S(9, 48), S(14, 57), S(15, 59), S(15, 60),
        S(9, 49), S(13, 55), S(15, 59), S(16, 61)
    },
    {
        {},
        {},
        {S(-54, -68), S(-34, -48), S(-34, -28), S(-17, -9), S(2, 3), S(6, 20), S(15, 20), S(27, 20), S(27, 20)},
        {S(-39, -46), S(-22, -28), S(-4, -10), S(14, 3), S(27, 20), S(27, 35), S(28, 41), S(32, 43), S(32, 43), S(32, 43), S(37, 43), S(39, 43), S(39, 43), S(39, 43)},
        {S(-18, -25), S(-16, -10), S(-2, -6), S(6, 9), S(6, 14), S(16, 23), S(21, 29), S(26, 36), S(33, 42), S(38, 49), S(45, 52), S(50, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-28, -74), S(-16, -62), S(-8, -50), S(-8, -38), S(2, -26), S(2, -14), S(2, -2), S(2, 10), S(6, 22), S(13, 25), S(16, 30), S(16, 35), S(27, 35), S(31, 35), S(34, 35), S(34, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(44, 35), S(47, 35), S(59, 47)},
        {},
    },
    {S(0, 0), S(-40, 25), S(-39, 25), S(-39, 25), S(0, 132), S(77, 350), S(259, 595), S(0, 0)},
    {S(0, 0), S(-7, -2), S(28, 3), S(32, 4), S(42, 35), S(47, 92), S(124, 181), S(0, 0)},
    S(63, 17), // RookOpenFileBonus
    S(33, 16), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(28, 31), // KnightOutpostBonus
    S(46, 26), // BishopOutpostBonus
    S(-36, 0), // TrappedRookByKingPenalty
    S(28, 19), // RookBehindOurPasserBonus
    S(17, 84), // RookBehindTheirPasserBonus
    S(22, 8), // MinorBehindPawnBonus
    S(24, 4), // MinorOnKingRing
    S(1, 0), // RookOnKingRing
    S(-6, -4), // KingProtector
    S(23, 18), // BishopPair
    {
        {S(0, 0), S(75, 0), S(75, 0), S(48, 0), S(40, 0), S(46, 0), S(108, 0)},
        {S(-33, 0), S(123, 0), S(72, 0), S(34, 0), S(0, 0), S(33, 0), S(0, 0)},
        {S(0, 0), S(113, 0), S(37, 0), S(37, 0), S(25, 0), S(50, 0), S(71, 0)},
        {S(0, 0), S(0, 0), S(7, 0), S(9, 0), S(52, 0), S(22, 0), S(60, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(56, 0), S(5, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(7, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(59, 0), S(34, 0), S(2, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(51, 0), S(25, 0), S(2, 0), S(0, 0), S(20, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(109, 0), S(4, 0), S(11, 0), S(14, 0), S(10, 0)}, // BlockedStorm
    S(-25, -1), // UndefendedKingZoneSq
    S(15, 5), // KingMobilityFactor
    S(21, 31), // KingAttackByKnight
    S(2, 19), // KingAttackByBishop
    S(21, 31), // KingAttackByRook
    S(21, 31), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(28, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(2, 73), // KingRingWeakWeight
    S(0, 93), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-44, -21), // DoubledPawnPenalty
    S(-15, -2), // BackwardPawnPenalty
    S(-22, -14), // WeakUnopposedPenalty
    S(-15, -55), // DoubledIsolatedPenalty
    {S(0, -22), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-32, -22), // PawnIslandPenalty
    {S(19, 0), S(0, 0)}, // CentralPawnBonus
    S(24, 12), // BishopLongDiagonalBonus
    S(-13, 0), // BishopXrayPawns
    S(0, 19), // InitiativePasser
    S(0, 7), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 47), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(33, 60), // SliderOnQueenBishop
    S(56, 60), // SliderOnQueenRook
    S(6, 1), // RestrictedPiece
    S(94, 1), // ThreatByPawnPush
    S(-20, 0), // WeakQueenDefender
    S(27, 2), // KnightOnQueen
    S(-113, -104), // PawnlessFlank
    S(0, 52), // QueenInfiltration
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
