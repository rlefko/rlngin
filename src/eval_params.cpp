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
    S(262, 52), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(193, 0), S(148, 0), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(207, 44), S(0, 0)},
    S(131, 25), // ThreatByKing
    S(96, 21), // Hanging
    S(0, 17), // WeakQueen
    S(19, 33), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 25), S(0, 43), S(0, 43), S(0, 43), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 21), S(0, 39), S(0, 70), S(0, 77), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-41, -40), S(-41, -57), S(-108, -63), S(-319, -125), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(53, 21), S(53, 65), S(165, 143), S(165, 428), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-45, 13), S(46, 28), S(87, 28), S(87, 28), S(0, 0)},
    S(0, 22), // RookOn7thBonus
    S(0, 0), // BadBishop
    S(-1, -7), // BishopPawns
    S(85, 0), // Tempo
    {S(0, 0), S(196, 219), S(904, 552), S(933, 480), S(1473, 754), S(2628, 1710), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-46, -55), S(-46, -55), S(-36, -57), S(-15, -59), S(-11, -53), S(-37, -52), S(-49, -55), S(-53, -67),
        S(-88, -52), S(-82, -60), S(-44, -66), S(-37, -64), S(-27, -55), S(-48, -60), S(-70, -69), S(-85, -62),
        S(-63, -41), S(-63, -52), S(-39, -71), S(-20, -71), S(-15, -70), S(-31, -66), S(-57, -51), S(-65, -49),
        S(-3, 1), S(-20, -23), S(2, -43), S(35, -56), S(39, -53), S(7, -43), S(-25, -17), S(-6, -6),
        S(59, 80), S(28, 88), S(65, 46), S(149, 25), S(152, 27), S(59, 44), S(9, 90), S(48, 87),
        S(141, 136), S(-89, 185), S(51, 142), S(233, 98), S(209, 96), S(43, 147), S(-208, 201), S(104, 136),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board)
    {
        S(-66, -35), S(-66, -10), S(-46, -14), S(-52, 19),
        S(-58, -24), S(-39, -18), S(-16, -14), S(-9, 5),
        S(-4, 4), S(18, 6), S(39, -12), S(43, 17),
        S(-5, 3), S(42, 5), S(59, 27), S(76, 27),
        S(31, 19), S(68, 9), S(90, 24), S(79, 29),
        S(-16, -5), S(28, 8), S(88, 19), S(88, 26),
        S(-97, -31), S(-33, -8), S(33, -15), S(43, 13),
        S(-233, -59), S(-107, -31), S(-19, -14), S(14, 0)
    },
    // BishopPST (half-board)
    {
        S(19, -23), S(19, -25), S(-33, -6), S(-26, 1),
        S(10, -23), S(15, -24), S(37, -4), S(0, 6),
        S(18, -13), S(44, 8), S(21, -18), S(9, 18),
        S(34, -11), S(53, -4), S(49, 20), S(36, -4),
        S(17, -12), S(41, 34), S(46, 17), S(48, -3),
        S(7, -8), S(6, 16), S(15, 6), S(20, 8),
        S(-36, -20), S(-51, 1), S(-46, 8), S(-47, 0),
        S(-61, -1), S(-82, 12), S(-124, 16), S(-82, 27)
    },
    // RookPST (half-board)
    {
        S(-47, -11), S(-46, 4), S(-7, -4), S(-8, -9),
        S(-66, -27), S(-50, -20), S(-26, -13), S(-26, -14),
        S(-68, -24), S(-53, -12), S(-51, -18), S(-47, -26),
        S(-37, 4), S(-33, 5), S(-24, 7), S(9, -6),
        S(-33, 21), S(-9, 21), S(18, 14), S(40, 1),
        S(6, 14), S(16, 13), S(49, 17), S(68, 12),
        S(27, 1), S(34, 10), S(61, 1), S(81, 12),
        S(38, 11), S(45, 13), S(60, 3), S(72, -2)
    },
    // QueenPST (half-board)
    {
        S(20, -79), S(20, -76), S(22, -68), S(43, -49),
        S(7, -36), S(19, -39), S(51, -37), S(59, -32),
        S(-8, -42), S(37, -16), S(35, 6), S(27, -18),
        S(13, -16), S(36, 15), S(35, 29), S(15, 22),
        S(-3, 5), S(1, 14), S(0, 59), S(-8, 65),
        S(-26, 12), S(-43, 23), S(-45, 62), S(-41, 86),
        S(-98, 10), S(-107, 29), S(-80, 53), S(-68, 71),
        S(-87, -10), S(-62, -19), S(191, -31), S(52, -15)
    },
    // KingPST (half-board)
    {
        S(201, -86), S(206, -57), S(63, -25), S(-45, -34),
        S(219, -39), S(177, -19), S(47, 7), S(-18, 14),
        S(72, -17), S(96, -1), S(11, 20), S(-53, 32),
        S(-12, 2), S(-2, 28), S(-64, 48), S(-142, 49),
        S(17, 10), S(-40, 43), S(-119, 61), S(-220, 70),
        S(15, -1), S(-52, 34), S(-130, 57), S(-229, 74),
        S(194, -57), S(-56, -7), S(-19, 14), S(-148, 26),
        S(-232, -147), S(223, -56), S(233, -43), S(-193, 0)
    },
    {
        {},
        {},
        {S(-88, -92), S(-48, -52), S(-8, -12), S(13, -8), S(28, 8), S(39, 27), S(56, 34), S(56, 36), S(56, 36)},
        {S(-16, 61), S(19, 88), S(53, 123), S(82, 138), S(110, 138), S(113, 148), S(118, 161), S(121, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163), S(122, 163)},
        {S(-26, 254), S(4, 284), S(19, 294), S(20, 303), S(20, 305), S(20, 323), S(20, 323), S(33, 333), S(33, 337), S(57, 347), S(57, 352), S(67, 353), S(67, 357), S(67, 357), S(89, 357)},
        {S(18, 291), S(41, 316), S(66, 341), S(75, 351), S(100, 351), S(125, 351), S(138, 362), S(149, 363), S(155, 377), S(171, 377), S(171, 380), S(174, 380), S(174, 380), S(176, 395), S(176, 402), S(180, 403), S(186, 403), S(202, 403), S(203, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(236, 403), S(237, 403)},
        {},
    },
    {S(0, 0), S(-39, 42), S(-39, 42), S(-39, 83), S(0, 141), S(40, 286), S(520, 378), S(0, 0)},
    {S(0, 0), S(-8, -7), S(51, 9), S(51, 9), S(65, 13), S(65, 79), S(245, 99), S(0, 0)},
    S(114, 12), // RookOpenFileBonus
    S(29, 10), // RookSemiOpenFileBonus
    S(0, 2), // RookOnQueenFile
    S(107, 10), // KnightOutpostBonus
    S(107, 11), // BishopOutpostBonus
    S(-99, 0), // TrappedRookByKingPenalty
    S(67, 3), // RookBehindOurPasserBonus
    S(-149, 105), // RookBehindTheirPasserBonus
    S(35, 4), // MinorBehindPawnBonus
    S(27, 0), // MinorOnKingRing
    S(5, 0), // RookOnKingRing
    S(-16, -2), // KingProtector
    S(8, 3), // BishopPair
    {
        {S(0, 0), S(102, 0), S(98, 0), S(63, 0), S(55, 0), S(15, 0), S(0, 0)},
        {S(-89, 0), S(197, 0), S(88, 0), S(0, 0), S(0, 0), S(86, 0), S(167, 0)},
        {S(-6, 0), S(202, 0), S(65, 0), S(39, 0), S(64, 0), S(63, 0), S(89, 0)},
        {S(-19, 0), S(156, 0), S(74, 0), S(39, 0), S(19, 0), S(116, 0), S(74, 0)}
    }, // Shelter
    {
        {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(9, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(93, 0), S(0, 0), S(1, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(8, 0), S(274, 0), S(2, 0), S(1, 0), S(2, 0), S(0, 0)},
        {S(0, 0), S(32, 0), S(333, 0), S(30, 0), S(2, 0), S(4, 0), S(0, 0)}
    }, // UnblockedStorm
    {S(0, 0), S(0, 0), S(218, 0), S(7, 0), S(0, 0), S(44, 0), S(0, 0)}, // BlockedStorm
    S(-31, 0), // UndefendedKingZoneSq
    S(10, 49), // KingMobilityFactor
    S(30, 41), // KingAttackByKnight
    S(11, 45), // KingAttackByBishop
    S(32, 49), // KingAttackByRook
    S(32, 49), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(32, 0), S(37, 299), S(32, 1), S(32, 1), S(0, 0)}, // KingSafeCheck
    S(23, 6), // KingRingWeakWeight
    S(28, 0), // KingNoQueenDiscount
    S(0, 0), // IsolatedPawnPenalty
    S(0, -6), // DoubledPawnPenalty
    S(-12, -6), // BackwardPawnPenalty
    S(-27, -16), // WeakUnopposedPenalty
    S(0, -39), // DoubledIsolatedPenalty
    {S(-103, -29), S(-87, -108)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-4, -21), // PawnIslandPenalty
    {S(25, 0), S(3, 0)}, // CentralPawnBonus
    S(64, 24), // BishopLongDiagonalBonus
    S(-20, 0), // BishopXrayPawns
    S(0, 60), // InitiativePasser
    S(0, 53), // InitiativePawnCount
    S(0, 3), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 61), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(45, 16), // SliderOnQueenBishop
    S(56, 2), // SliderOnQueenRook
    S(8, 0), // RestrictedPiece
    S(29, 0), // ThreatByPawnPush
    S(-39, -7), // WeakQueenDefender
    S(99, 0), // KnightOnQueen
    S(-330, -134), // PawnlessFlank
    S(0, 1), // QueenInfiltration
    S(0, 0), // KingPawnDistEg
    S(0, 9), // KBNKCornerEg
    S(0, 308), // LucenaEg
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
