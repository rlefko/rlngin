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
    S(258, 445), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(123, 301), S(201, 175), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(430, 347), S(0, 0)},
    S(46, 11), // ThreatByKing
    S(38, 26), // Hanging
    S(235, 229), // WeakQueen
    S(0, 0), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 19), S(0, 29), S(0, 31), S(0, 33), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 13), S(0, 35), S(0, 60), S(0, 79), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-8, -25), S(-8, -41), S(-8, -69), S(-143, -104), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 20), S(0, 48), S(38, 95), S(274, 321), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(29, 28), S(29, 28), S(72, 28), S(72, 28), S(0, 0)},
    S(0, 40), // RookOn7thBonus
    S(-12, -2), // BadBishop
    S(-3, -8), // BishopPawns
    S(37, 0), // Tempo
    {S(0, 0), S(157, 210), S(498, 700), S(596, 773), S(874, 1291), S(1648, 2436), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-41, -8), S(-23, -4), S(-30, 1), S(-23, -25), S(-9, -18), S(-8, 5), S(-26, 1), S(-45, -16),
        S(-70, -27), S(-69, -14), S(-41, -21), S(-60, -42), S(-34, -30), S(-40, -16), S(-60, -22), S(-84, -25),
        S(-31, -9), S(-42, -12), S(-23, -30), S(0, -42), S(-1, -41), S(-20, -25), S(-42, -16), S(-33, -15),
        S(2, 18), S(-8, 15), S(2, -2), S(71, -8), S(94, -1), S(11, 1), S(-12, 15), S(3, 18),
        S(17, 25), S(23, 29), S(44, 17), S(93, 21), S(96, 23), S(47, 19), S(22, 26), S(16, 29),
        S(17, 19), S(27, 23), S(52, 23), S(82, 31), S(83, 32), S(53, 23), S(27, 23), S(18, 21),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-118, -34), S(-134, -25), S(-105, -25), S(-70, -13),
        S(-89, -24), S(-70, -13), S(-72, -20), S(-54, -13),
        S(-92, -17), S(-35, -12), S(-18, -14), S(12, 7),
        S(-13, -2), S(10, 4), S(23, 25), S(15, 29),
        S(11, 10), S(31, 17), S(43, 33), S(51, 37),
        S(25, -10), S(50, 6), S(76, 22), S(90, 31),
        S(30, -15), S(40, -7), S(66, 1), S(89, 29),
        S(20, -23), S(39, -8), S(60, 8), S(76, 17)
    },
    // BishopPST (half-board)
    {
        S(-6, -20), S(19, -12), S(-78, -11), S(-69, -14),
        S(18, -24), S(1, -52), S(6, -10), S(-39, -11),
        S(-17, -17), S(7, 5), S(-11, -11), S(10, 12),
        S(-11, -12), S(8, 6), S(32, 15), S(22, 8),
        S(-33, -17), S(9, 12), S(32, 18), S(41, 16),
        S(10, 1), S(15, 18), S(24, 4), S(52, 25),
        S(-17, -10), S(-29, -2), S(7, 19), S(29, 24),
        S(-25, -8), S(-16, 13), S(-2, 12), S(8, 14)
    },
    // RookPST (half-board)
    {
        S(-85, -28), S(-41, -32), S(-38, -24), S(-4, -35),
        S(-73, -30), S(-18, -34), S(-25, -29), S(-11, -28),
        S(-44, -24), S(1, -24), S(-16, -16), S(9, -26),
        S(-31, -1), S(8, 3), S(5, 7), S(1, -1),
        S(-39, 26), S(3, 33), S(27, 30), S(25, 20),
        S(-10, 25), S(25, 31), S(41, 34), S(66, 28),
        S(-9, -5), S(-3, 0), S(38, 6), S(61, 11),
        S(14, 19), S(28, 23), S(47, 23), S(45, 20)
    },
    // QueenPST (half-board)
    {
        S(-11, -89), S(26, -87), S(4, -73), S(22, -62),
        S(6, -73), S(17, -77), S(34, -65), S(31, -47),
        S(9, -48), S(30, -26), S(24, -12), S(19, -10),
        S(40, -30), S(46, -5), S(31, 32), S(6, 44),
        S(10, -21), S(16, 15), S(3, 47), S(-4, 82),
        S(13, 8), S(-8, 30), S(-4, 59), S(6, 84),
        S(-43, 4), S(-79, 29), S(-34, 55), S(-19, 66),
        S(-74, 26), S(-64, 45), S(-45, 50), S(-25, 53)
    },
    // KingPST (half-board)
    {
        S(58, -136), S(31, -90), S(-3, -71), S(-101, -57),
        S(63, -91), S(30, -62), S(-17, -49), S(-16, -44),
        S(-14, -57), S(-8, -40), S(-22, -30), S(-13, -24),
        S(-48, -19), S(-16, -4), S(-5, 0), S(-3, -2),
        S(-25, 23), S(-6, 34), S(2, 36), S(4, 32),
        S(-8, 45), S(6, 61), S(13, 60), S(15, 53),
        S(1, 51), S(9, 60), S(13, 61), S(15, 60),
        S(3, 50), S(9, 55), S(14, 60), S(17, 61)
    },
    {
        {},
        {},
        {S(-105, -108), S(-67, -68), S(-39, -44), S(-27, -15), S(-11, 3), S(1, 19), S(12, 20), S(21, 20), S(21, 20)},
        {S(-54, -37), S(-31, -30), S(1, -17), S(12, 5), S(26, 27), S(32, 36), S(37, 40), S(37, 42), S(37, 43), S(37, 43), S(37, 43), S(37, 43), S(37, 43), S(37, 43)},
        {S(-36, -17), S(-14, -10), S(-4, -4), S(8, 6), S(8, 10), S(18, 18), S(21, 28), S(26, 35), S(36, 40), S(43, 44), S(49, 49), S(50, 55), S(50, 55), S(50, 55), S(50, 55)},
        {S(2, -83), S(2, -62), S(2, -62), S(2, -51), S(4, -40), S(10, -31), S(10, -10), S(14, 3), S(14, 22), S(24, 22), S(26, 22), S(27, 33), S(32, 35), S(33, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(35, 35), S(51, 35), S(51, 35), S(51, 39), S(54, 64)},
        {},
    },
    {S(0, 0), S(-43, 15), S(-43, 25), S(-43, 29), S(0, 127), S(69, 341), S(227, 571), S(0, 0)},
    {S(0, 0), S(-9, 1), S(29, 6), S(31, 8), S(41, 37), S(41, 90), S(41, 219), S(0, 0)},
    S(69, 17), // RookOpenFileBonus
    S(31, 17), // RookSemiOpenFileBonus
    S(7, 0), // RookOnQueenFile
    S(42, 22), // KnightOutpostBonus
    S(50, 15), // BishopOutpostBonus
    S(-39, 0), // TrappedRookByKingPenalty
    S(20, 25), // RookBehindOurPasserBonus
    S(44, 76), // RookBehindTheirPasserBonus
    S(21, 14), // MinorBehindPawnBonus
    S(28, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-6, -4), // KingProtector
    S(23, 13), // BishopPair
    {
        {S(0, 0), S(78, 0), S(83, 0), S(52, 0), S(34, 0), S(54, 0), S(165, 0)},
        {S(-32, 0), S(123, 0), S(77, 0), S(38, 0), S(0, 0), S(38, 0), S(0, 0)},
        {S(-6, 0), S(96, 0), S(18, 0), S(24, 0), S(7, 0), S(24, 0), S(74, 0)},
        {S(0, 0), S(27, 0), S(3, 0), S(4, 0), S(44, 0), S(2, 0), S(47, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(66, 0), S(4, 0), S(0, 0), S(2, 0)},
        {S(0, 0), S(0, 0), S(33, 0), S(15, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(94, 0), S(39, 0), S(14, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(20, 0), S(28, 0), S(16, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(122, 0), S(3, 0), S(7, 0), S(14, 0), S(15, 0)}, // BlockedStorm
    S(-22, -1), // UndefendedKingZoneSq
    S(11, 0), // KingMobilityFactor
    S(21, 36), // KingAttackByKnight
    S(4, 20), // KingAttackByBishop
    S(21, 36), // KingAttackByRook
    S(21, 36), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 0), S(28, 619), S(29, 0), S(29, 0), S(0, 0)}, // KingSafeCheck
    S(7, 30), // KingRingWeakWeight
    S(6, 91), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(-6, -23), // DoubledPawnPenalty
    S(-16, -4), // BackwardPawnPenalty
    S(-24, -14), // WeakUnopposedPenalty
    S(-31, -46), // DoubledIsolatedPenalty
    {S(0, -26), S(0, 0)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-36, -16), // PawnIslandPenalty
    {S(26, 0), S(11, 0)}, // CentralPawnBonus
    S(25, 19), // BishopLongDiagonalBonus
    S(-17, 0), // BishopXrayPawns
    S(0, 7), // InitiativePasser
    S(0, 12), // InitiativePawnCount
    S(0, 4), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 0), // InitiativeInfiltrate
    S(0, 0), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(60, 60), // SliderOnQueenBishop
    S(60, 60), // SliderOnQueenRook
    S(7, 3), // RestrictedPiece
    S(109, 18), // ThreatByPawnPush
    S(-17, -12), // WeakQueenDefender
    S(29, 4), // KnightOnQueen
    S(-84, -97), // PawnlessFlank
    S(0, 20), // QueenInfiltration
    S(0, -1), // KingPawnDistEg
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
    S(0, 16), // EscapableThreatScale
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
