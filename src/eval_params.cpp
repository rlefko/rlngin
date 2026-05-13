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
    S(296, 400), // ThreatByPawn (eg clamped to SPSA 0..400 bound)
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(47, 0), // ThreatByKing
    S(34, 24), // Hanging
    S(50, 50), // WeakQueen
    S(14, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 18), S(0, 29), S(0, 36), S(0, 36), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 14), S(0, 32), S(0, 56), S(0, 72), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-21, -27), S(-21, -27), S(-21, -51), S(-162, -84), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 22), S(0, 50), S(36, 90), S(126, 373), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(23, 28), S(23, 28), S(86, 28), S(86, 28), S(0, 0)},
    S(0, 46), // RookOn7thBonus
    S(-28, 0), // BadBishop
    S(-5, -6), // BishopPawns
    S(22, 0), // Tempo
    {S(0, 0), S(179, 203), S(663, 566), S(812, 624), S(1156, 1052), S(2129, 2013), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-27, -12), S(-15, -8), S(-13, -6), S(-26, -28), S(-11, -20), S(1, -3), S(-13, -8), S(-28, -19),
        S(-46, -25), S(-39, -14), S(-24, -15), S(-40, -32), S(-23, -23), S(-20, -13), S(-32, -20), S(-56, -23),
        S(-25, -9), S(-32, -11), S(-12, -21), S(5, -29), S(8, -28), S(-11, -18), S(-29, -15), S(-25, -15),
        S(-7, 7), S(-15, 4), S(-7, -5), S(52, -4), S(67, 0), S(3, -3), S(-18, 4), S(-3, 5),
        S(8, 21), S(11, 25), S(27, 17), S(68, 23), S(71, 26), S(30, 19), S(11, 21), S(6, 24),
        S(6, 19), S(13, 23), S(33, 25), S(59, 33), S(60, 34), S(33, 25), S(13, 23), S(7, 20),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-102, -36), S(-115, -21), S(-74, -28), S(-42, -21),
        S(-65, -28), S(-44, -16), S(-39, -18), S(-26, -7),
        S(-64, -17), S(-16, -7), S(7, -4), S(32, 9),
        S(-5, -6), S(12, 6), S(30, 19), S(36, 23),
        S(14, 3), S(37, 15), S(46, 27), S(59, 31),
        S(10, -10), S(35, 9), S(56, 23), S(69, 33),
        S(4, -11), S(15, -3), S(35, 6), S(51, 29),
        S(-11, -21), S(7, -5), S(22, 9), S(33, 16)
    },
    // BishopPST (half-board)
    {
        S(-11, -15), S(14, -5), S(-56, -7), S(-44, -14),
        S(9, -9), S(0, -24), S(9, -3), S(-24, -5),
        S(-12, -8), S(14, 5), S(2, -7), S(23, 9),
        S(-6, -8), S(8, 5), S(31, 10), S(23, 5),
        S(-28, -9), S(16, 6), S(26, 15), S(36, 11),
        S(3, 1), S(13, 16), S(21, 1), S(39, 19),
        S(-21, -7), S(-28, -4), S(3, 16), S(11, 16),
        S(-30, -8), S(-23, 10), S(-13, 7), S(-11, 7)
    },
    // RookPST (half-board)
    {
        S(-56, -20), S(-17, -24), S(-10, -21), S(10, -25),
        S(-43, -28), S(-4, -31), S(-9, -27), S(-2, -24),
        S(-24, -23), S(8, -18), S(-3, -13), S(9, -16),
        S(-14, -8), S(7, 1), S(0, 7), S(6, -2),
        S(-19, 16), S(8, 24), S(15, 22), S(17, 16),
        S(-6, 20), S(17, 26), S(21, 29), S(34, 23),
        S(-1, -2), S(0, 3), S(14, 9), S(24, 11),
        S(-1, 17), S(3, 22), S(9, 22), S(5, 20)
    },
    // QueenPST (half-board)
    {
        S(-7, -51), S(20, -46), S(3, -38), S(20, -24),
        S(2, -43), S(8, -37), S(24, -25), S(21, -13),
        S(4, -24), S(20, -6), S(23, 8), S(15, 12),
        S(19, -6), S(25, 12), S(22, 30), S(10, 40),
        S(-10, -12), S(6, 8), S(1, 28), S(3, 50),
        S(6, -9), S(-5, 8), S(-10, 32), S(4, 49),
        S(-29, -11), S(-56, 7), S(-26, 24), S(-10, 36),
        S(-36, -13), S(-36, 10), S(-25, 16), S(-16, 17)
    },
    // KingPST (half-board)
    {
        S(74, -109), S(66, -75), S(40, -57), S(-47, -42),
        S(75, -74), S(58, -48), S(19, -38), S(13, -30),
        S(13, -48), S(19, -30), S(-1, -19), S(-3, -12),
        S(-17, -18), S(-11, 1), S(-14, 6), S(-19, 8),
        S(-16, 18), S(-14, 30), S(-15, 33), S(-20, 30),
        S(-16, 34), S(-11, 49), S(-13, 49), S(-19, 44),
        S(-16, 31), S(-14, 43), S(-16, 45), S(-18, 46),
        S(-17, 30), S(-16, 37), S(-16, 42), S(-17, 45)
    },
    {
        {},
        {},
        {S(-50, -60), S(-30, -40), S(-24, -20), S(-4, -2), S(12, 10), S(25, 19), S(36, 20), S(37, 20), S(37, 20)},
        {S(-40, -33), S(-22, -23), S(-4, -5), S(12, 8), S(29, 26), S(37, 33), S(38, 40), S(40, 41), S(40, 43), S(41, 43), S(44, 43), S(51, 43), S(51, 43), S(51, 43)},
        {S(-30, -27), S(-15, -15), S(-8, 0), S(7, 13), S(7, 20), S(15, 27), S(20, 34), S(28, 39), S(34, 45), S(40, 49), S(46, 55), S(49, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-18, -39), S(-18, -27), S(-16, -15), S(-13, -7), S(-11, 5), S(0, 13), S(4, 17), S(14, 19), S(18, 27), S(27, 27), S(30, 30), S(31, 34), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(47, 47)},
        {},
    },
    {S(0, 0), S(-39, 5), S(-39, 5), S(-39, 17), S(0, 109), S(117, 282), S(311, 489), S(0, 0)},
    {S(0, 0), S(-16, 1), S(4, 1), S(24, 1), S(73, 15), S(73, 64), S(73, 170), S(0, 0)},
    S(82, 25), // RookOpenFileBonus
    S(38, 25), // RookSemiOpenFileBonus
    S(16, 0), // RookOnQueenFile
    S(52, 16), // KnightOutpostBonus
    S(52, 15), // BishopOutpostBonus
    S(-53, 0), // TrappedRookByKingPenalty
    S(19, 34), // RookBehindOurPasserBonus
    S(12, 76), // RookBehindTheirPasserBonus
    S(24, 7), // MinorBehindPawnBonus
    S(37, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-5, -4), // KingProtector
    S(42, 10), // BishopPair
    {
        {S(0, 0), S(75, 0), S(90, 0), S(56, 0), S(35, 0), S(43, 0), S(145, 0)},
        {S(-33, 0), S(146, 0), S(92, 0), S(44, 0), S(0, 0), S(37, 0), S(0, 0)},
        {S(-8, 0), S(101, 0), S(30, 0), S(29, 0), S(0, 0), S(17, 0), S(71, 0)},
        {S(0, 0), S(57, 0), S(14, 0), S(5, 0), S(38, 0), S(0, 0), S(54, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(73, 0), S(6, 0), S(0, 0), S(4, 0)},
        {S(0, 0), S(0, 0), S(5, 0), S(11, 0), S(0, 0), S(0, 0), S(6, 0)},
        {S(0, 0), S(0, 0), S(99, 0), S(52, 0), S(16, 0), S(3, 0), S(4, 0)},
        {S(0, 0), S(0, 0), S(10, 0), S(38, 0), S(19, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(137, 0), S(2, 0), S(13, 0), S(8, 0), S(0, 0)}, // BlockedStorm
    S(-30, 0), // UndefendedKingZoneSq
    S(13, 3), // KingMobilityFactor
    S(23, 7), // KingAttackByKnight
    S(4, 36), // KingAttackByBishop
    S(23, 7), // KingAttackByRook
    S(23, 7), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(33, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(3, 83), // KingRingWeakWeight
    S(6, 42), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-3, -33), // DoubledPawnPenalty
    S(-28, -3), // BackwardPawnPenalty
    S(-30, -18), // WeakUnopposedPenalty
    S(-48, -40), // DoubledIsolatedPenalty
    {S(0, -5), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-34, -18), // PawnIslandPenalty
    {S(27, 0), S(0, 0)}, // CentralPawnBonus
    S(25, 17), // BishopLongDiagonalBonus
    S(-15, 0), // BishopXrayPawns
    S(0, 15), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 11), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(60, 60), // SliderOnQueenBishop
    S(60, 49), // SliderOnQueenRook
    S(6, 1), // RestrictedPiece
    S(71, 38), // ThreatByPawnPush
    S(-12, -8), // WeakQueenDefender
    S(41, 0), // KnightOnQueen
    S(-181, -67), // PawnlessFlank
    S(0, 55), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 40), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 25), // KBNKPushClose
    S(0, 30), // KQKRPushToEdge
    S(0, 15), // KQKRPushClose
    S(0, 1), // KPsKFortressScale
    S(0, 0), // KBPKNDrawishScale
    S(0, 16), // KRKPDrawishScale
    S(0, 16), // KRKMinorScale
    S(0, 0), // KNNKDrawScale
    S(0, 14), // EscapableThreatScale
    S(456, 0), // CompensationCap (2 pawns mg; eg unused)
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
