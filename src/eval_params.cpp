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
//
// ISOLATION TEST: this struct uses main's tuned values verbatim for every
// field shared with main (everything through LucenaEg). New HEAD-only fields
// (the endgame conversion gradients and damping scales, plus
// EscapableThreatScale) get neutral defaults so the new structural code
// either does nothing or behaves as close to main's pre-refactor behavior
// as possible. Goal: if this binary plays near main's strength, the
// regression is in the tune outputs (corpus-self-circularity), not in the
// structural code changes; if it plays at HEAD's strength, then the
// structural code changes (endgame module, SEE-aware threats) themselves
// cost Elo and need re-examination.
//
// EscapableThreatScale is set to 24 (the SPSA upper bound; main has no
// damping at all, but 24/64 = 37.5% credit for escapable threats is the
// closest we can get without breaking the test_tunable bound check).
static const EvalParams kDefaultEvalParams = {
    S(222, 507), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(113, 388), S(227, 186), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(609, 294), S(0, 0)},
    S(50, 9), // ThreatByKing
    S(31, 31), // Hanging
    S(178, 334), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 27), S(0, 32), S(0, 32), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 13), S(0, 34), S(0, 59), S(0, 79), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-8, -19), S(-8, -48), S(-8, -71), S(-125, -112), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 45), S(30, 104), S(266, 335), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(28, 28), S(28, 28), S(69, 28), S(69, 28), S(0, 0)},
    S(0, 36), // RookOn7thBonus
    S(-13, -4), // BadBishop
    S(-3, -8), // BishopPawns
    S(40, 0), // Tempo
    {S(0, 0), S(155, 214), S(504, 700), S(594, 775), S(866, 1298), S(1645, 2440), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-43, -11), S(-24, -7), S(-36, 0), S(-13, -23), S(0, -15), S(-13, 6), S(-26, -1), S(-49, -15),
        S(-74, -23), S(-73, -13), S(-41, -24), S(-57, -45), S(-30, -34), S(-42, -14), S(-63, -22), S(-86, -23),
        S(-37, -6), S(-43, -10), S(-22, -33), S(10, -40), S(3, -41), S(-17, -28), S(-41, -17), S(-38, -13),
        S(3, 18), S(-6, 14), S(1, -3), S(68, -7), S(88, 2), S(8, 1), S(-12, 13), S(0, 22),
        S(15, 28), S(22, 30), S(47, 18), S(95, 17), S(97, 20), S(48, 19), S(24, 34), S(15, 31),
        S(15, 21), S(24, 22), S(51, 25), S(83, 31), S(84, 33), S(51, 26), S(23, 21), S(16, 24),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-117, -37), S(-125, -38), S(-112, -17), S(-85, -3),
        S(-95, -27), S(-75, -11), S(-80, -18), S(-57, -16),
        S(-95, -18), S(-39, -12), S(-27, -12), S(1, 7),
        S(-13, -7), S(17, -1), S(19, 26), S(-1, 34),
        S(4, 11), S(29, 17), S(34, 38), S(44, 39),
        S(26, -13), S(58, -2), S(78, 21), S(98, 28),
        S(40, -17), S(47, -10), S(78, 1), S(101, 29),
        S(54, -25), S(55, -10), S(72, 6), S(86, 15)
    },
    // BishopPST (half-board)
    {
        S(12, -26), S(21, -16), S(-80, -8), S(-75, -12),
        S(27, -23), S(5, -54), S(9, -8), S(-43, -7),
        S(-13, -16), S(10, 5), S(-14, -1), S(10, 9),
        S(-21, -7), S(0, 11), S(30, 12), S(26, 8),
        S(-33, -15), S(7, 11), S(29, 20), S(41, 22),
        S(2, 1), S(15, 13), S(15, 10), S(52, 27),
        S(-12, -10), S(-20, -7), S(4, 20), S(31, 24),
        S(-22, -9), S(-14, 9), S(3, 13), S(22, 18)
    },
    // RookPST (half-board)
    {
        S(-83, -31), S(-47, -35), S(-43, -20), S(-7, -31),
        S(-81, -24), S(-21, -34), S(-28, -28), S(-8, -31),
        S(-54, -5), S(-7, -22), S(-14, -19), S(14, -35),
        S(-38, 11), S(8, 2), S(11, 5), S(0, -2),
        S(-43, 31), S(2, 29), S(30, 24), S(24, 22),
        S(-6, 21), S(29, 27), S(46, 28), S(77, 30),
        S(-7, -3), S(0, -1), S(45, 7), S(78, 24),
        S(5, 21), S(23, 18), S(47, 21), S(53, 16)
    },
    // QueenPST (half-board)
    {
        S(-16, -94), S(15, -78), S(5, -75), S(18, -52),
        S(4, -67), S(20, -70), S(28, -61), S(33, -48),
        S(-1, -41), S(34, -18), S(23, -18), S(18, -15),
        S(47, -34), S(44, 0), S(28, 34), S(1, 42),
        S(15, -10), S(29, 12), S(7, 40), S(-7, 80),
        S(6, 31), S(-17, 27), S(8, 40), S(8, 71),
        S(-33, -2), S(-95, 33), S(-20, 52), S(-14, 59),
        S(-112, 40), S(-71, 51), S(-34, 48), S(10, 40)
    },
    // KingPST (half-board)
    {
        S(57, -132), S(28, -86), S(-4, -69), S(-95, -58),
        S(67, -95), S(25, -63), S(-19, -44), S(-14, -37),
        S(-16, -55), S(-13, -34), S(-21, -28), S(-13, -19),
        S(-66, -13), S(-18, -8), S(-1, -1), S(6, -8),
        S(-26, 20), S(-3, 32), S(7, 31), S(5, 38),
        S(-7, 38), S(7, 64), S(19, 61), S(12, 50),
        S(-11, 37), S(8, 57), S(15, 58), S(1, 48),
        S(-1, 43), S(11, 53), S(27, 62), S(15, 57)
    },
    {
        {},
        {},
        {S(-108, -106), S(-68, -66), S(-41, -47), S(-27, -20), S(-14, 1), S(-5, 17), S(9, 19), S(18, 20), S(18, 20)},
        {S(-41, -43), S(-25, -31), S(1, -27), S(14, 5), S(22, 24), S(29, 39), S(31, 40), S(37, 43), S(37, 43), S(37, 43), S(37, 43), S(37, 43), S(37, 43), S(37, 43)},
        {S(-23, -9), S(-14, -7), S(-5, 4), S(7, 4), S(8, 5), S(20, 20), S(20, 26), S(22, 35), S(34, 42), S(41, 44), S(50, 50), S(50, 53), S(50, 55), S(50, 55), S(50, 55)},
        {S(2, -116), S(2, -94), S(2, -69), S(2, -44), S(10, -28), S(10, -28), S(10, -11), S(10, -3), S(12, 22), S(20, 22), S(24, 22), S(27, 31), S(33, 31), S(33, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(37, 35), S(51, 35), S(51, 35), S(52, 39), S(76, 64)},
        {},
    },
    {S(0, 0), S(-43, 7), S(-43, 26), S(-43, 29), S(0, 130), S(64, 338), S(215, 578), S(0, 0)},
    {S(0, 0), S(-9, 1), S(30, 5), S(32, 7), S(41, 39), S(41, 92), S(41, 220), S(0, 0)},
    S(69, 19), // RookOpenFileBonus
    S(36, 18), // RookSemiOpenFileBonus
    S(6, 0), // RookOnQueenFile
    S(44, 25), // KnightOutpostBonus
    S(56, 17), // BishopOutpostBonus
    S(-45, 0), // TrappedRookByKingPenalty
    S(17, 31), // RookBehindOurPasserBonus
    S(41, 76), // RookBehindTheirPasserBonus
    S(14, 19), // MinorBehindPawnBonus
    S(22, 1), // MinorOnKingRing
    S(1, 0), // RookOnKingRing
    S(-8, -3), // KingProtector
    S(19, 18), // BishopPair
    {
        {S(0, 0), S(80, 0), S(85, 0), S(56, 0), S(34, 0), S(70, 0), S(222, 0)},
        {S(-34, 0), S(123, 0), S(82, 0), S(39, 0), S(0, 0), S(43, 0), S(0, 0)},
        {S(-6, 0), S(102, 0), S(17, 0), S(31, 0), S(11, 0), S(28, 0), S(26, 0)},
        {S(0, 0), S(0, 0), S(0, 0), S(9, 0), S(52, 0), S(15, 0), S(39, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(59, 0), S(5, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(16, 0), S(24, 0), S(0, 0), S(0, 0), S(1, 0)},
        {S(0, 0), S(0, 0), S(96, 0), S(41, 0), S(18, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(35, 0), S(33, 0), S(19, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(126, 0), S(0, 0), S(9, 0), S(14, 0), S(8, 0)}, // BlockedStorm
    S(-26, 0), // UndefendedKingZoneSq
    S(11, 0), // KingMobilityFactor
    S(21, 36), // KingAttackByKnight
    S(6, 19), // KingAttackByBishop
    S(21, 36), // KingAttackByRook
    S(21, 36), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 0), S(25, 619), S(29, 0), S(29, 0), S(0, 0)}, // KingSafeCheck
    S(7, 31), // KingRingWeakWeight
    S(4, 91), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-15, -12), // DoubledPawnPenalty
    S(-18, 0), // BackwardPawnPenalty
    S(-27, -10), // WeakUnopposedPenalty
    S(-20, -59), // DoubledIsolatedPenalty
    {S(0, -23), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-31, -17), // PawnIslandPenalty
    {S(9, 0), S(8, 0)}, // CentralPawnBonus
    S(22, 18), // BishopLongDiagonalBonus
    S(-15, 0), // BishopXrayPawns
    S(0, 11), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 0), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(60, 60), // SliderOnQueenBishop
    S(47, 55), // SliderOnQueenRook
    S(6, 4), // RestrictedPiece
    S(112, 20), // ThreatByPawnPush
    S(-26, -7), // WeakQueenDefender
    S(25, 8), // KnightOnQueen
    S(-68, -98), // PawnlessFlank
    S(0, 19), // QueenInfiltration
    S(0, -2), // KingPawnDistEg
    S(0, 37), // KBNKCornerEg
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
