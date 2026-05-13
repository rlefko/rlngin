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
    S(275, 411), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 86), S(80, 80), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(100, 100), S(0, 0)},
    S(47, 5), // ThreatByKing
    S(39, 19), // Hanging
    S(50, 50), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 22), S(0, 31), S(0, 40), S(0, 40), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 11), S(0, 30), S(0, 56), S(0, 73), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-14, -29), S(-14, -45), S(-14, -74), S(-155, -117), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 24), S(0, 58), S(22, 112), S(131, 374), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(22, 28), S(22, 28), S(68, 28), S(68, 28), S(0, 0)},
    S(0, 41), // RookOn7thBonus
    S(-18, 0), // BadBishop
    S(-4, -8), // BishopPawns
    S(40, 0), // Tempo
    {S(0, 0), S(135, 225), S(585, 644), S(700, 708), S(936, 1239), S(1944, 2264), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-25, -15), S(-8, -3), S(-18, -2), S(-8, -24), S(0, -20), S(-6, 1), S(-13, -1), S(-29, -22),
        S(-53, -25), S(-44, -17), S(-34, -21), S(-48, -42), S(-26, -33), S(-31, -15), S(-39, -19), S(-59, -25),
        S(-25, -8), S(-30, -8), S(-12, -27), S(6, -46), S(9, -42), S(-10, -22), S(-27, -11), S(-27, -13),
        S(-2, 21), S(-6, 14), S(7, -4), S(65, -10), S(74, -6), S(13, -2), S(-6, 14), S(-1, 20),
        S(10, 30), S(11, 29), S(29, 19), S(61, 16), S(62, 16), S(31, 19), S(11, 27), S(10, 32),
        S(11, 26), S(14, 27), S(31, 25), S(51, 25), S(51, 26), S(31, 25), S(14, 27), S(11, 27),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-108, -34), S(-116, -23), S(-90, -23), S(-63, -17),
        S(-89, -27), S(-67, -14), S(-65, -15), S(-50, -6),
        S(-74, -21), S(-36, -8), S(-15, -5), S(3, 15),
        S(-4, -3), S(8, 8), S(19, 23), S(12, 29),
        S(23, 4), S(28, 15), S(42, 29), S(48, 36),
        S(29, -8), S(45, 5), S(65, 24), S(75, 34),
        S(28, -17), S(38, -7), S(56, 5), S(72, 27),
        S(18, -26), S(34, -10), S(49, 4), S(60, 14)
    },
    // BishopPST (half-board)
    {
        S(-8, -19), S(6, -12), S(-77, -12), S(-69, -16),
        S(7, -19), S(-11, -37), S(-3, -8), S(-47, -6),
        S(-15, -16), S(7, 3), S(-12, -3), S(4, 11),
        S(-5, -16), S(6, 4), S(24, 19), S(15, 14),
        S(-19, -16), S(11, 9), S(27, 19), S(33, 20),
        S(12, -5), S(16, 15), S(24, 12), S(38, 24),
        S(-8, -13), S(-12, -1), S(10, 18), S(22, 22),
        S(-13, -8), S(-7, 5), S(3, 9), S(10, 12)
    },
    // RookPST (half-board)
    {
        S(-72, -30), S(-35, -32), S(-26, -29), S(1, -37),
        S(-64, -37), S(-26, -38), S(-20, -31), S(-9, -31),
        S(-41, -28), S(-5, -18), S(-10, -16), S(3, -21),
        S(-30, -4), S(3, 8), S(2, 11), S(3, 3),
        S(-14, 19), S(15, 31), S(24, 29), S(24, 22),
        S(2, 24), S(24, 32), S(36, 35), S(49, 34),
        S(-4, -1), S(0, 3), S(26, 11), S(42, 15),
        S(13, 14), S(21, 22), S(34, 26), S(36, 25)
    },
    // QueenPST (half-board)
    {
        S(-1, -66), S(10, -61), S(0, -55), S(10, -47),
        S(11, -57), S(11, -56), S(23, -41), S(23, -28),
        S(20, -34), S(26, -10), S(23, 11), S(14, 14),
        S(34, -7), S(39, 18), S(27, 44), S(15, 59),
        S(4, -15), S(12, 12), S(5, 37), S(1, 62),
        S(5, -6), S(-7, 11), S(-7, 41), S(2, 66),
        S(-24, -10), S(-56, 0), S(-33, 29), S(-17, 50),
        S(-42, -10), S(-44, 6), S(-39, 24), S(-30, 35)
    },
    // KingPST (half-board)
    {
        S(69, -127), S(46, -96), S(8, -76), S(-80, -64),
        S(69, -86), S(40, -64), S(-9, -55), S(-19, -48),
        S(4, -58), S(1, -40), S(-14, -31), S(-18, -27),
        S(-18, -19), S(-10, -2), S(-11, 3), S(-15, 3),
        S(-13, 20), S(-6, 32), S(-6, 35), S(-8, 31),
        S(-5, 43), S(1, 56), S(2, 58), S(-1, 53),
        S(-5, 47), S(1, 57), S(1, 59), S(1, 59),
        S(-5, 48), S(-1, 54), S(1, 58), S(2, 59)
    },
    {
        {},
        {},
        {S(-62, -64), S(-42, -44), S(-23, -25), S(-9, -6), S(5, 9), S(17, 20), S(27, 20), S(35, 20), S(35, 20)},
        {S(-40, -38), S(-22, -24), S(-4, -6), S(14, 7), S(26, 25), S(27, 37), S(30, 41), S(30, 43), S(33, 43), S(34, 43), S(38, 43), S(43, 43), S(45, 43), S(45, 43)},
        {S(-30, -8), S(-15, -6), S(-5, 0), S(6, 12), S(6, 16), S(15, 25), S(19, 31), S(26, 37), S(32, 43), S(39, 49), S(42, 55), S(50, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(-27, -57), S(-15, -45), S(-15, -33), S(-15, -21), S(-8, -18), S(0, -6), S(6, -1), S(11, 11), S(17, 17), S(23, 20), S(25, 22), S(26, 31), S(29, 34), S(32, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(47, 35), S(59, 47)},
        {},
    },
    {S(0, 0), S(-39, 17), S(-39, 17), S(-39, 17), S(0, 117), S(96, 318), S(280, 559), S(0, 0)},
    {S(0, 0), S(-7, 0), S(29, 4), S(34, 4), S(56, 28), S(59, 83), S(134, 172), S(0, 0)},
    S(66, 15), // RookOpenFileBonus
    S(30, 15), // RookSemiOpenFileBonus
    S(7, 0), // RookOnQueenFile
    S(37, 27), // KnightOutpostBonus
    S(50, 24), // BishopOutpostBonus
    S(-37, 0), // TrappedRookByKingPenalty
    S(30, 19), // RookBehindOurPasserBonus
    S(5, 82), // RookBehindTheirPasserBonus
    S(19, 12), // MinorBehindPawnBonus
    S(27, 1), // MinorOnKingRing
    S(2, 0), // RookOnKingRing
    S(-9, -2), // KingProtector
    S(16, 19), // BishopPair
    {
        {S(0, 0), S(78, 0), S(80, 0), S(53, 0), S(41, 0), S(43, 0), S(123, 0)},
        {S(-35, 0), S(133, 0), S(81, 0), S(42, 0), S(0, 0), S(40, 0), S(0, 0)},
        {S(-5, 0), S(110, 0), S(34, 0), S(37, 0), S(18, 0), S(37, 0), S(89, 0)},
        {S(0, 0), S(43, 0), S(12, 0), S(10, 0), S(44, 0), S(19, 0), S(66, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(57, 0), S(4, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(57, 0), S(32, 0), S(4, 0), S(1, 0), S(2, 0)},
        {S(0, 0), S(0, 0), S(45, 0), S(31, 0), S(9, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(113, 0), S(0, 0), S(11, 0), S(6, 0), S(2, 0)}, // BlockedStorm
    S(-24, -2), // UndefendedKingZoneSq
    S(14, 0), // KingMobilityFactor
    S(21, 11), // KingAttackByKnight
    S(1, 26), // KingAttackByBishop
    S(21, 11), // KingAttackByRook
    S(21, 13), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(28, 619), S(32, 0), S(32, 0), S(0, 0)}, // KingSafeCheck
    S(3, 78), // KingRingWeakWeight
    S(0, 63), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-18, -33), // DoubledPawnPenalty
    S(-14, -3), // BackwardPawnPenalty
    S(-23, -13), // WeakUnopposedPenalty
    S(-27, -45), // DoubledIsolatedPenalty
    {S(0, -21), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-30, -19), // PawnIslandPenalty
    {S(22, 0), S(1, 0)}, // CentralPawnBonus
    S(26, 12), // BishopLongDiagonalBonus
    S(-16, 0), // BishopXrayPawns
    S(0, 24), // InitiativePasser
    S(0, 10), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 48), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(47, 60), // SliderOnQueenBishop
    S(56, 51), // SliderOnQueenRook
    S(8, 1), // RestrictedPiece
    S(78, 27), // ThreatByPawnPush
    S(-13, -4), // WeakQueenDefender
    S(36, 0), // KnightOnQueen
    S(-144, -95), // PawnlessFlank
    S(0, 63), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 0), // KBNKCornerEg
    S(0, 250), // LucenaEg
    S(0, 30), // KXKPushToEdge
    S(0, 25), // KXKPushClose
    S(0, 0), // KBNKPushClose
    S(0, 4), // KQKRPushToEdge
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
