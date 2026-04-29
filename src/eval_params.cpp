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
    S(226, 36), // ThreatByPawn
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(211, 14), S(196, 13), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(246, 0), S(0, 0)},
    S(215, 21), // ThreatByKing
    // Hanging: re-armed at the legacy starting value because main's
    // tune drove the term to zero against the old over-strict trigger.
    // The rewritten conventional trigger fires more broadly, so a
    // non-zero weight is needed for it to carry signal. The next tune
    // will refit on the new trigger.
    S(12, 0), // Hanging
    S(34, 28), // WeakQueen
    S(60, 16), // SafePawnPush
    {S(0, 0), S(0, 0), S(0, 0), S(0, 29), S(0, 43), S(0, 46), S(0, 46), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 22), S(0, 49), S(0, 78), S(0, 88), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-33, -41), S(-33, -49), S(-93, -62), S(-312, -137), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(33, 23), S(33, 65), S(230, 124), S(230, 405), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(82, 24), S(89, 28), S(124, 28), S(124, 28), S(0, 0)},
    S(0, 32), // RookOn7thBonus
    // BadBishop / BishopPawns split. Kept dormant in the baseline
    // (BadBishop = 0) so that BishopPawns alone reproduces main's
    // legacy `BadBishopPenalty * count` behavior at no closed center;
    // the closed-center scaling only adds to the legacy line. This
    // keeps eval-on-open-positions identical to main while the tuner
    // refits both terms on the next pass.
    S(0, 0), // BadBishop
    S(-11, -11), // BishopPawns
    S(36, 0), // Tempo
    {S(0, 0), S(261, 256), S(1002, 656), S(1056, 602), S(1610, 927), S(2692, 2153), S(0, 0)}, // PieceScore
    // PawnPST
    {
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0),
        S(-40, -59), S(-62, -61), S(-58, -60), S(-46, -71), S(-53, -39), S(-17, -49), S(20, -61), S(-118, -74),
        S(-82, -69), S(-126, -68), S(-90, -75), S(-94, -75), S(-28, -74), S(-89, -68), S(-42, -81), S(-85, -79),
        S(-70, -51), S(-93, -53), S(-95, -85), S(-37, -96), S(-51, -81), S(-35, -85), S(-89, -73), S(-74, -71),
        S(-6, -27), S(7, -43), S(18, -80), S(9, -83), S(32, -73), S(18, -93), S(-49, -43), S(-75, -28),
        S(99, 56), S(82, 54), S(129, 17), S(137, 6), S(105, 12), S(160, 41), S(25, 79), S(8, 77),
        S(325, 163), S(-20, 238), S(137, 224), S(165, 258), S(205, 159), S(346, 191), S(-201, 287), S(-88, 236),
        S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)
    },
    // KnightPST (half-board: 4 file buckets x 8 ranks)
    {
        S(-300, -4), S(-82, -16), S(-90, -18), S(-12, -22),
        S(-88, -40), S(-17, 6), S(-21, -22), S(2, -9),
        S(-20, -2), S(40, -18), S(41, -16), S(50, 8),
        S(16, 16), S(72, 14), S(97, 23), S(104, 25),
        S(73, 4), S(98, 18), S(187, 6), S(132, 27),
        S(-100, 8), S(120, 8), S(176, -6), S(154, 16),
        S(-44, -10), S(23, -6), S(196, -18), S(52, 4),
        S(-609, -26), S(-94, -4), S(-245, 6), S(72, 28)
    },
    // BishopPST (half-board)
    {
        S(-26, -42), S(10, -14), S(-58, -8), S(-57, 0),
        S(13, -52), S(43, -28), S(35, -7), S(-12, 8),
        S(18, -16), S(42, 16), S(-4, 5), S(10, 24),
        S(29, -9), S(32, 6), S(44, 18), S(46, 14),
        S(13, -2), S(76, 19), S(90, 8), S(64, 2),
        S(92, -4), S(138, 17), S(78, 0), S(74, 4),
        S(-124, -15), S(-139, 6), S(22, -4), S(-12, 4),
        S(-74, -36), S(-128, 23), S(-142, -12), S(-192, 54)
    },
    // RookPST (half-board)
    {
        S(-47, -23), S(-42, -4), S(-19, -10), S(-14, -14),
        S(-172, -4), S(-68, -18), S(-45, -6), S(-22, -19),
        S(-115, -12), S(-60, -21), S(-56, -12), S(-87, -16),
        S(-83, 5), S(24, 6), S(-54, 16), S(-10, -3),
        S(-80, 18), S(85, 11), S(47, 18), S(44, 6),
        S(-26, 30), S(114, 10), S(146, 4), S(144, -6),
        S(37, -12), S(52, 4), S(177, -18), S(138, -9),
        S(-22, 18), S(-101, 34), S(48, 16), S(40, 19)
    },
    // QueenPST (half-board)
    {
        S(-160, -10), S(32, -138), S(-86, -40), S(10, -43),
        S(-96, -52), S(16, -88), S(34, -54), S(11, -32),
        S(-39, -16), S(23, -12), S(16, 14), S(2, -9),
        S(-2, -28), S(-22, 28), S(30, 32), S(28, 37),
        S(40, -41), S(-19, 61), S(55, 44), S(27, 54),
        S(30, -7), S(28, 3), S(16, 84), S(11, 58),
        S(4, -65), S(-174, 70), S(5, 29), S(-32, 92),
        S(-36, 10), S(24, 10), S(69, 26), S(128, -12)
    },
    // KingPST (half-board)
    {
        S(198, -124), S(184, -70), S(0, -20), S(-58, -26),
        S(306, -71), S(192, -36), S(102, -6), S(56, 0),
        S(58, -36), S(128, -10), S(133, 8), S(16, 28),
        S(-16, -12), S(146, 6), S(-10, 46), S(-62, 56),
        S(-114, 28), S(12, 68), S(-59, 60), S(-211, 74),
        S(-6, 20), S(-16, 86), S(-248, 106), S(-360, 112),
        S(110, -50), S(20, 45), S(50, 52), S(-64, 84),
        S(-488, -350), S(276, -21), S(-115, 20), S(-172, -60)
    },
    {
        {},
        {},
        {S(-107, -164), S(-48, -41), S(-2, -14), S(9, 8), S(28, 24), S(43, 43), S(56, 43), S(56, 43), S(56, 43)},
        {S(-15, 82), S(34, 91), S(67, 112), S(85, 128), S(103, 145), S(112, 150), S(112, 159), S(112, 161), S(122, 163), S(122, 163), S(122, 165), S(122, 165), S(128, 165), S(128, 165)},
        {S(-37, 250), S(6, 285), S(12, 294), S(20, 300), S(20, 300), S(26, 315), S(26, 322), S(36, 329), S(36, 336), S(36, 344), S(43, 347), S(43, 356), S(43, 357), S(43, 357), S(43, 357)},
        {S(5, 3), S(101, 225), S(109, 290), S(110, 318), S(130, 346), S(130, 348), S(143, 348), S(148, 370), S(160, 370), S(160, 372), S(160, 373), S(174, 380), S(174, 388), S(174, 388), S(176, 392), S(176, 400), S(176, 403), S(192, 403), S(212, 403), S(212, 403), S(218, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(228, 403), S(230, 404), S(230, 404)},
        {},
    },
    {S(0, 0), S(-16, 38), S(-11, 48), S(6, 69), S(12, 201), S(79, 353), S(437, 396), S(0, 0)},
    {S(0, 0), S(3, -13), S(53, 3), S(64, 6), S(65, 32), S(65, 73), S(68, 155), S(0, 0)},
    S(136, 18), // RookOpenFileBonus
    S(57, 18), // RookSemiOpenFileBonus
    S(8, 0), // RookOnQueenFile
    S(95, 21), // KnightOutpostBonus
    S(95, 24), // BishopOutpostBonus
    S(-82, 0), // TrappedRookByKingPenalty
    S(67, 0), // RookBehindOurPasserBonus
    S(-17, 90), // RookBehindTheirPasserBonus
    S(29, 11), // MinorBehindPawnBonus
    S(27, 0), // MinorOnKingRing
    S(0, 0), // RookOnKingRing
    S(-20, -4), // KingProtector
    S(35, 11), // BishopPair
    // Shelter[edge_distance][pawn_rank], rank 0 = no own pawn (semi-open
    // file penalty); ranks 1-6 are relative own-pawn ranks. Mg only.
    // Defaults reproduce the dominant peaks of the prior PawnShieldBonus
    // and Semi/OpenFileNearKing terms while spreading edge-distance
    // resolution that the legacy single-bucket shape lacked.
    {
        {S(-30, 0), S(75, 0), S(38, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(-50, 0), S(96, 0), S(47, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(-100, 0), S(115, 0), S(57, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(-186, 0), S(128, 0), S(63, 0), S(0, 0), S(0, 0), S(0, 0), S(0, 0)},
    },
    // UnblockedStorm[edge_distance][storm_rank], rank 0 = no enemy pawn
    // (structurally zero); ranks 1-6 are relative enemy-pawn ranks. Mg
    // only; subtracted at the call site so values stay non-negative.
    // Defaults reproduce the prior UnblockedPawnStorm peak at the
    // most-threatening rank (legacy distance bucket 2) and decay across
    // less-threatening ranks and edge distances.
    {
        {S(0, 0), S(0, 0), S(70, 0), S(25, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(100, 0), S(35, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(125, 0), S(45, 0), S(0, 0), S(0, 0), S(0, 0)},
        {S(0, 0), S(0, 0), S(142, 0), S(50, 0), S(0, 0), S(0, 0), S(0, 0)},
    },
    // BlockedStorm[storm_rank]: file distance dimension collapses out
    // because the rammer is frontally blocked. Mg only; subtracted.
    {S(0, 0), S(0, 0), S(87, 0), S(9, 0), S(0, 0), S(0, 0), S(0, 0)},
    S(-46, 0), // UndefendedKingZoneSq
    // KingMobilityFactor: linear weight subtracted from the king
    // danger accumulator per safe king move. Both halves carry signal
    // because the legacy KingSafeSqPenalty at 0 safe squares applied
    // S(-12, -68); folding the same shape into the danger accumulator
    // requires a non-zero eg coefficient so endgame exposed-king
    // positions still penalize.
    S(8, 8), // KingMobilityFactor
    S(24, 13), // KingAttackByKnight
    S(6, 13), // KingAttackByBishop
    S(24, 13), // KingAttackByRook
    S(24, 18), // KingAttackByQueen
    {S(0, 0), S(0, 0), S(29, 4), S(29, 8), S(29, 8), S(29, 8), S(0, 0)}, // KingSafeCheck
    S(16, 0), // KingRingWeakWeight
    S(39, 35), // KingNoQueenDiscount
    S(-8, -1), // IsolatedPawnPenalty
    S(-5, -33), // DoubledPawnPenalty
    S(0, -10), // BackwardPawnPenalty
    S(-26, -19), // WeakUnopposedPenalty
    S(0, -24), // DoubledIsolatedPenalty
    {S(-102, -26), S(-66, -94)}, // BlockedPawnPenalty (rel rank 5, 6)
    S(-16, -24), // PawnIslandPenalty
    {S(27, 0), S(23, 0)}, // CentralPawnBonus
    S(64, 15), // BishopLongDiagonalBonus
    S(0, -4), // BishopXrayPawns
    S(0, 53), // InitiativePasser
    S(0, 48), // InitiativePawnCount
    S(0, 12), // InitiativeOutflank
    S(0, 0), // InitiativeTension
    S(0, 35), // InitiativeInfiltrate
    S(0, 48), // InitiativePureBase
    S(0, -1), // InitiativeConstant
    S(77, 0), // SliderOnQueenBishop
    S(67, 0), // SliderOnQueenRook
    S(13, 3), // RestrictedPiece
};

// clang-format on

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
