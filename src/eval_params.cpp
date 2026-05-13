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
    S(303, 400), // ThreatByPawn (clamped to SPSA 0..400 bound)
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(65, 0), // ThreatByKing
    S(47, 20), // Hanging
    S(50, 49), // WeakQueen
    S(29, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 17), S(0, 27), S(0, 33), S(0, 33), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 13), S(0, 32), S(0, 56), S(0, 71), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-21, -23), S(-21, -37), S(-21, -59), S(-197, -83), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 47), S(72, 82), S(123, 368), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(28, 28), S(28, 28), S(109, 28), S(109, 28), S(0, 0)},
    S(0, 45), // RookOn7thBonus
    S(-13, -1), // BadBishop
    S(-5, -7), // BishopPawns
    S(36, 0), // Tempo
    {S(0, 0), S(178, 200), S(704, 583), S(810, 603), S(1195, 963), S(2165, 1909), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0),
        S(  0,   0), S(  5,   0), S(  5,   0), S( 15,   0), S( 15,   0), S(  5,   0), S(  5,   0), S(  0,   0),
        S( -5,   0), S(  0,   0), S(  5,   0), S( 15,   0), S( 15,   0), S(  5,   0), S(  0,   0), S( -5,   0),
        S(  0,   5), S(  5,   5), S( 10,   5), S( 25,  10), S( 25,  10), S( 10,   5), S(  5,   5), S(  0,   5),
        S( 10,  15), S( 15,  15), S( 20,  20), S( 35,  25), S( 35,  25), S( 20,  20), S( 15,  15), S( 10,  15),
        S( 20,  30), S( 25,  35), S( 35,  40), S( 50,  45), S( 50,  45), S( 35,  40), S( 25,  35), S( 20,  30),
        S( 40,  60), S( 50,  65), S( 60,  70), S( 80,  80), S( 80,  80), S( 60,  70), S( 50,  65), S( 40,  60),
        S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0), S(  0,   0)
    },

    // KnightPST (half-board)
    {
        S(-50, -30), S(-30, -15), S(-15, -10), S(  0,  -5),
        S(-20, -10), S( -5,  -5), S(  5,   0), S( 15,   5),
        S(-15,  -5), S(  5,   5), S( 20,  10), S( 30,  15),
        S(-10,   0), S( 15,  10), S( 30,  15), S( 40,  20),
        S( -5,   5), S( 20,  15), S( 35,  20), S( 45,  25),
        S(-10,   0), S( 15,  10), S( 30,  15), S( 40,  20),
        S(-20,  -5), S(  5,   5), S( 20,  10), S( 30,  10),
        S(-50, -20), S(-20, -10), S( -5,   0), S(  5,   5)
    },

    // BishopPST (half-board)
    {
        S(-20, -10), S(-15,  -5), S(-10,  -5), S( -5,   0),
        S(-10,  -5), S(  5,   0), S(  0,   0), S(  5,   5),
        S(-10,  -5), S( 10,   0), S( 15,   5), S( 15,   5),
        S( -5,   0), S(  5,   5), S( 15,  10), S( 20,  10),
        S( -5,   0), S(  5,   5), S( 15,  10), S( 20,  10),
        S(-10,  -5), S( 10,   0), S( 15,   5), S( 15,   5),
        S(-15, -10), S(  0,  -5), S(  5,   0), S(  5,   0),
        S(-20, -10), S(-10,  -5), S(-10,  -5), S( -5,  -5)
    },

    // RookPST (half-board)
    {
        S(-10,  -5), S( -5,  -3), S(  0,  -3), S(  5,   0),
        S(-10,  -5), S( -5,   0), S(  0,   0), S(  5,   3),
        S(-10,  -5), S(  0,   0), S(  5,   3), S(  5,   5),
        S( -5,   0), S(  0,   3), S(  5,   5), S( 10,   5),
        S( -5,   5), S(  5,   5), S(  5,  10), S( 10,  10),
        S(  0,  10), S(  5,  10), S( 10,  10), S( 10,  10),
        S( 15,  10), S( 20,  15), S( 20,  15), S( 20,  15),
        S(  0,   5), S(  5,   5), S( 10,   5), S( 10,   5)
    },

    // QueenPST (half-board)
    {
        S( -5, -30), S(  0, -20), S(  0, -15), S(  5, -10),
        S(  0, -20), S(  5, -10), S(  5,  -5), S(  5,   0),
        S(  0, -15), S(  5,  -5), S(  5,   0), S(  5,   5),
        S(  5, -10), S(  5,   0), S( 10,   5), S( 10,  10),
        S(  5, -10), S(  5,   0), S( 10,   5), S( 10,  10),
        S(  0, -15), S(  5,  -5), S(  5,   0), S(  5,   5),
        S(  0, -20), S(  5, -10), S(  5,  -5), S(  5,   0),
        S( -5, -30), S(  0, -20), S(  0, -15), S(  5, -10)
    },

    // KingPST (half-board)
    {
        S( 40, -50), S( 50, -30), S( 30, -10), S( 10,   0),
        S( 30, -30), S( 30, -10), S( 10,  10), S(  0,  20),
        S( 10, -10), S( 10,  10), S(-10,  20), S(-20,  25),
        S(-20,  10), S(-20,  20), S(-30,  25), S(-30,  30),
        S(-30,  20), S(-30,  25), S(-30,  30), S(-30,  30),
        S(-30,  10), S(-30,  20), S(-30,  25), S(-30,  25),
        S(-30,   0), S(-30,  10), S(-30,  15), S(-30,  15),
        S(-30, -10), S(-30,   0), S(-30,   5), S(-30,   5)
    },

    {
        {},
        {},
        {S(-60, -50), S(-30, -25), S(-10, -10), S(  0,   0), S( 10,  10), S( 20,  15), S( 25,  20), S( 30,  20), S( 35,  20)},
        {S(-40, -30), S(-15, -10), S(  0,   0), S( 10,  10), S( 20,  20), S( 25,  25), S( 30,  30), S( 35,  35), S( 40,  40), S( 45,  40), S( 50,  45), S( 55,  45), S( 55,  50), S( 60,  50)},
        {S(-30, -40), S(-15, -10), S(  0,   0), S(  5,  10), S( 10,  20), S( 15,  25), S( 20,  30), S( 25,  35), S( 30,  40), S( 35,  45), S( 40,  50), S( 45,  50), S( 50,  55), S( 50,  55), S( 50,  55)},
        {S(-30, -30), S(-15, -15), S( -5,  -5), S(  0,   0), S(  5,   5), S( 10,  10), S( 15,  15), S( 20,  20), S( 25,  25), S( 30,  30), S( 30,  30), S( 30,  30), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 35,  35), S( 40,  40)},
        {},
    },
    {S(0, 0), S(-39, 6), S(-39, 13), S(-39, 20), S(0, 112), S(105, 297), S(336, 497), S(0, 0)},
    {S(0, 0), S(-13, 1), S(19, 1), S(35, 3), S(60, 28), S(68, 71), S(68, 184), S(0, 0)},
    S(85, 18), // RookOpenFileBonus
    S(36, 18), // RookSemiOpenFileBonus
    S(10, 0), // RookOnQueenFile
    S(47, 21), // KnightOutpostBonus
    S(54, 11), // BishopOutpostBonus
    S(-50, 0), // TrappedRookByKingPenalty
    S(22, 25), // RookBehindOurPasserBonus
    S(11, 75), // RookBehindTheirPasserBonus
    S(27, 11), // MinorBehindPawnBonus
    S(35, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-8, -3), // KingProtector
    S(42, 12), // BishopPair
    {
        {S(0, 0), S(85, 0), S(95, 0), S(56, 0), S(34, 0), S(47, 0), S(150, 0)},
        {S(-37, 0), S(153, 0), S(87, 0), S(47, 0), S(0, 0), S(45, 0), S(0, 0)},
        {S(0, 0), S(111, 0), S(36, 0), S(37, 0), S(15, 0), S(32, 0), S(75, 0)},
        {S(0, 0), S(85, 0), S(26, 0), S(16, 0), S(39, 0), S(6, 0), S(56, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(76, 0), S(10, 0), S(0, 0), S(10, 0)},
        {S(0, 0), S(0, 0), S(7, 0), S(17, 0), S(0, 0), S(0, 0), S(4, 0)},
        {S(0, 0), S(0, 0), S(101, 0), S(50, 0), S(12, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(19, 0), S(29, 0), S(10, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(137, 0), S(9, 0), S(14, 0), S(8, 0), S(4, 0)}, // BlockedStorm
    S(-29, -1), // UndefendedKingZoneSq
    S(14, 0), // KingMobilityFactor
    S(26, 7), // KingAttackByKnight
    S(4, 40), // KingAttackByBishop (clamped to 0..40)
    S(26, 7), // KingAttackByRook
    S(26, 7), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(33, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(3, 2), // KingRingWeakWeight
    S(5, 65), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -29), // DoubledPawnPenalty
    S(-16, -6), // BackwardPawnPenalty
    S(-30, -15), // WeakUnopposedPenalty
    S(-39, -43), // DoubledIsolatedPenalty
    {S(0, -20), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-34, -16), // PawnIslandPenalty
    {S(25, 0), S(2, 0)}, // CentralPawnBonus
    S(30, 19), // BishopLongDiagonalBonus
    S(-17, 0), // BishopXrayPawns
    S(0, 19), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 10), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(60, 41), // SliderOnQueenBishop
    S(57, 60), // SliderOnQueenRook
    S(8, 1), // RestrictedPiece
    S(62, 37), // ThreatByPawnPush
    S(-20, -5), // WeakQueenDefender
    S(47, 4), // KnightOnQueen
    S(-193, -71), // PawnlessFlank
    S(0, 51), // QueenInfiltration
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
    S(0, 17), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
