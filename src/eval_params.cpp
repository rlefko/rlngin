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
    S(228, 36), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(179, 17), S(160, 34), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(226, 8), S(0, 0)},
    S(63, 1), // ThreatByKing
    S(98, 31), // Hanging
    S(26, 4), // WeakQueen
    S(61, 16), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 30), S(0, 48), S(0, 48), S(0, 48), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 26), S(0, 47), S(0, 80), S(0, 92), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, -46), S(-28, -66), S(-80, -66), S(-324, -103), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(26, 21), S(51, 63), S(222, 112), S(222, 389), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-26, 28), S(125, 28), S(188, 28), S(188, 28), S(0, 0)},
    S(0, 27), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-2, -9), // BishopPawns
    S(42, 0), // Tempo
    {S(0, 0), S(242, 256), S(986, 629), S(1040, 569), S(1578, 902), S(2706, 2053), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-50, -68), S(-42, -58), S(-63, -61), S(-33, -68), S(-21, -70), S(-79, -55), S(-59, -67), S(-40, -78),
        S(-101, -64), S(-88, -62), S(-77, -80), S(-79, -85), S(-30, -65), S(-111, -64), S(-82, -81), S(-112, -71),
        S(-72, -51), S(-105, -50), S(-105, -83), S(-58, -89), S(-48, -90), S(-73, -76), S(-86, -66), S(-77, -71),
        S(-33, -6), S(8, -50), S(-11, -67), S(41, -95), S(49, -82), S(-22, -88), S(-31, -51), S(-31, -25),
        S(116, 68), S(45, 79), S(93, 16), S(76, 10), S(164, 22), S(207, 8), S(130, 59), S(-7, 81),
        S(159, 184), S(43, 229), S(232, 211), S(132, 217), S(236, 198), S(147, 228), S(-106, 300), S(23, 203),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-292, 30), S(-88, -5), S(-90, -8), S(-48, -13),
        S(-56, -18), S(-77, 7), S(-23, -6), S(9, -4),
        S(-41, -7), S(19, 1), S(28, -15), S(62, 14),
        S(28, 1), S(102, 14), S(88, 30), S(97, 28),
        S(103, 0), S(94, 23), S(157, 21), S(120, 50),
        S(-4, -6), S(136, -4), S(84, 17), S(186, 23),
        S(12, -73), S(-89, 20), S(76, -20), S(28, 40),
        S(-299, -152), S(-134, -26), S(-237, 24), S(48, 6)
    },
    // BishopPST (half-board)
    {
        S(-59, -7), S(105, -33), S(-60, -3), S(-8, 0),
        S(9, -28), S(36, -23), S(40, -12), S(-10, 10),
        S(-1, -8), S(50, -4), S(7, 1), S(5, 18),
        S(-11, -32), S(34, 12), S(46, 15), S(57, 7),
        S(-14, -9), S(77, 12), S(93, 10), S(45, 21),
        S(69, -12), S(37, -8), S(73, 5), S(117, -9),
        S(-53, -42), S(-136, 11), S(19, 2), S(-25, 15),
        S(-219, 27), S(-17, -6), S(-135, 27), S(-193, 41)
    },
    // RookPST (half-board)
    {
        S(-25, -31), S(-38, -10), S(-2, -17), S(7, -25),
        S(-74, -35), S(-36, -28), S(17, -33), S(-22, -38),
        S(-81, -26), S(-74, -20), S(-31, -25), S(-73, -19),
        S(-105, 9), S(-72, 21), S(-40, 11), S(8, -8),
        S(-40, 23), S(5, 26), S(42, 20), S(20, 2),
        S(6, 26), S(58, 18), S(52, 20), S(70, 17),
        S(-1, 18), S(-22, 20), S(115, 6), S(90, 2),
        S(21, 20), S(31, 19), S(60, 13), S(132, 18)
    },
    // QueenPST (half-board)
    {
        S(-30, -40), S(-25, -60), S(4, -67), S(13, -40),
        S(58, -100), S(43, -96), S(29, -45), S(35, -39),
        S(-101, 0), S(24, -28), S(31, -4), S(3, -19),
        S(-26, -24), S(18, 20), S(17, 18), S(7, 39),
        S(53, -51), S(12, 27), S(43, 32), S(-17, 44),
        S(49, -13), S(30, -1), S(-15, 64), S(-55, 119),
        S(-96, 13), S(-173, 66), S(-145, 112), S(-70, 100),
        S(70, -18), S(50, -22), S(63, -2), S(118, -8)
    },
    // KingPST (half-board)
    {
        S(233, -141), S(221, -87), S(43, -52), S(-38, -61),
        S(257, -83), S(199, -55), S(39, -34), S(5, -21),
        S(-41, -48), S(62, -28), S(20, -7), S(-99, 8),
        S(-103, -6), S(-13, 3), S(-109, 36), S(-229, 31),
        S(23, 9), S(37, 35), S(-186, 53), S(-300, 65),
        S(131, 29), S(-119, 51), S(-111, 85), S(-299, 87),
        S(247, -65), S(157, 72), S(99, 57), S(-95, 77),
        S(-283, -203), S(293, 28), S(22, 109), S(-51, 77)
    },
    {
        {},
        {},
        {S(-43, -208), S(-34, -34), S(-5, -4), S(15, 12), S(27, 22), S(39, 39), S(56, 43), S(56, 43), S(56, 43)},
        {S(-16, 80), S(48, 104), S(58, 131), S(81, 138), S(98, 146), S(107, 154), S(122, 158), S(122, 159), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(128, 165), S(128, 165)},
        {S(-34, 258), S(7, 288), S(20, 290), S(20, 301), S(20, 303), S(22, 317), S(22, 328), S(22, 328), S(34, 336), S(34, 348), S(43, 348), S(43, 357), S(43, 357), S(43, 357), S(43, 357)},
        {S(85, 35), S(101, 249), S(123, 250), S(123, 318), S(124, 334), S(135, 348), S(144, 348), S(145, 373), S(154, 373), S(163, 373), S(168, 373), S(174, 377), S(174, 379), S(174, 390), S(174, 395), S(182, 402), S(182, 402), S(188, 403), S(210, 403), S(210, 403), S(218, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 404), S(230, 404)},
        {},
    },
    {S(0, 0), S(-32, 42), S(-22, 43), S(-22, 82), S(0, 171), S(5, 351), S(448, 380), S(0, 0)},
    {S(0, 0), S(-4, -9), S(45, 5), S(51, 13), S(52, 29), S(175, 64), S(212, 123), S(0, 0)},
    S(132, 17), // RookOpenFileBonus
    S(62, 17), // RookSemiOpenFileBonus
    S(8, 2), // RookOnQueenFile
    S(78, 12), // KnightOutpostBonus
    S(104, 25), // BishopOutpostBonus
    S(-70, 0), // TrappedRookByKingPenalty
    S(46, 19), // RookBehindOurPasserBonus
    S(-113, 104), // RookBehindTheirPasserBonus
    S(37, 3), // MinorBehindPawnBonus
    S(15, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-14, -4), // KingProtector
    S(11, 18), // BishopPair
    {
        {S(-23, 0), S(60, 0), S(78, 0), S(48, 0), S(52, 0), S(48, 0), S(152, 0)},
        {S(-111, 0), S(184, 0), S(79, 0), S(0, 0), S(2, 0), S(64, 0), S(32, 0)},
        {S(-53, 0), S(182, 0), S(74, 0), S(0, 0), S(72, 0), S(104, 0), S(56, 0)},
        {S(-68, 0), S(140, 0), S(56, 0), S(15, 0), S(34, 0), S(56, 0), S(16, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(38, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(30, 0), S(19, 0), S(8, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(174, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(110, 0), S(67, 0), S(0, 0), S(0, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(51, 0), S(11, 0), S(1, 0), S(0, 0), S(18, 0)}, // BlockedStorm
    S(-45, 0), // UndefendedKingZoneSq
    S(5, 18), // KingMobilityFactor
    S(24, 15), // KingAttackByKnight
    S(9, 29), // KingAttackByBishop
    S(24, 17), // KingAttackByRook
    S(24, 18), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 8), S(32, 152), S(29, 8), S(29, 8), S(0, 0)}, // KingSafeCheck
    S(22, 0), // KingRingWeakWeight
    S(34, 3), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -14), // DoubledPawnPenalty
    S(-3, -9), // BackwardPawnPenalty
    S(-37, -22), // WeakUnopposedPenalty
    S(-10, -52), // DoubledIsolatedPenalty
    {S(-87, -28), S(-152, -80)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-10, -17), // PawnIslandPenalty
    {S(39, 0), S(27, 0)}, // CentralPawnBonus
    S(53, 10), // BishopLongDiagonalBonus
    S(-12, 0), // BishopXrayPawns
    S(0, 47), // InitiativePasser
    S(0, 56), // InitiativePawnCount
    S(0, 0), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 22), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(93, 0), // SliderOnQueenBishop
    S(35, 1), // SliderOnQueenRook
    S(12, 1), // RestrictedPiece
    S(35, 20), // ThreatByPawnPush
    S(-50, -10), // WeakQueenDefender
    S(16, 12), // KnightOnQueen
    S(-17, -78), // PawnlessFlank
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
