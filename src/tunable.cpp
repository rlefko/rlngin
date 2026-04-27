#include "tunable.h"
#include "eval_params.h"
#include "search.h"
#include "search_params.h"
#include "types.h"

#include <algorithm>

namespace {

int clampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Build a spec that reads and writes a plain int field of `searchParams`.
// The default reported over UCI and via the `tune` command is the live
// value clamped into [minValue, maxValue] so the SPSA driver never starts
// theta in the infeasible region. The live storage is untouched, so any
// startup behaviour that depended on a drifted default stays intact until
// the driver issues its first setoption.
TunableSpec makeIntSpec(std::string name, int *target, int minValue, int maxValue, double cEnd,
                        double rEnd, std::function<void()> afterWrite = nullptr) {
    TunableSpec s;
    s.name = std::move(name);
    s.minValue = minValue;
    s.maxValue = maxValue;
    s.defaultValue = clampInt(*target, minValue, maxValue);
    s.cEnd = cEnd;
    s.rEnd = rEnd;
    s.get = [target]() { return *target; };
    s.set = [target, minValue, maxValue, afterWrite](int v) {
        *target = clampInt(v, minValue, maxValue);
        if (afterWrite) afterWrite();
    };
    return s;
}

// Build a spec that reads and writes a single mg/eg half of an eval Score.
// `isMg` selects which half is exposed; the other half is preserved on write.
// Default reporting uses the live-clamped half (see makeIntSpec rationale).
TunableSpec makeScoreHalfSpec(std::string name, Score *target, bool isMg, int minValue,
                              int maxValue, double cEnd, double rEnd) {
    TunableSpec s;
    s.name = std::move(name);
    s.minValue = minValue;
    s.maxValue = maxValue;
    int liveHalf = isMg ? mg_value(*target) : eg_value(*target);
    s.defaultValue = clampInt(liveHalf, minValue, maxValue);
    s.cEnd = cEnd;
    s.rEnd = rEnd;
    s.get = [target, isMg]() { return isMg ? mg_value(*target) : eg_value(*target); };
    s.set = [target, isMg, minValue, maxValue](int v) {
        v = clampInt(v, minValue, maxValue);
        int mg = mg_value(*target);
        int eg = eg_value(*target);
        if (isMg)
            mg = v;
        else
            eg = v;
        *target = S(mg, eg);
    };
    return s;
}

// Every scalar in this registry carries a chess-prior sign / magnitude
// constraint baked into its [minValue, maxValue] bounds:
//   * Search margins, divisors, and LMR coefficients are structurally
//     positive (a negative razor margin, say, would flip its comparator).
//   * SEE*Coef fields are stored as positive magnitudes; the negation to
//     form the pruning threshold happens at the call site in search.cpp,
//     so the stored value must stay non-negative.
//   * Every eval term in this list named "Bonus" or "ThreatBy" is added
//     with a positive sign. Locking min >= 0 keeps SPSA from driving a
//     bonus across zero and accidentally turning it into a penalty (the
//     "bonuses stay bonuses" prior that the Texel tuner enforces for
//     symmetrically-named penalties).
//   * Scalars named "Penalty" mirror the same invariant in the opposite
//     direction: max <= 0 keeps the sign and "penalties stay penalties"
//     holds through every SPSA perturbation.
std::vector<TunableSpec> buildRegistry() {
    std::vector<TunableSpec> out;
    out.reserve(24);

    // --- Search pruning and reduction scalars ---
    //
    // Per-axis (min, max, c_end, r_end) tightened for the focused retune.
    // c_end is chosen per parameter so the final-iteration perturbation
    // crosses at least one engine decision boundary: for most scalars that
    // is a few-unit shift in the margin at the depths where the heuristic
    // fires; for LMR coefficients it has to cross the integer rounding
    // floor in the reduction table. Razor and the SEE* hard-prune scalars
    // stay exposed at the same specs they had in PR #38 so manual UCI
    // experimentation still works; the SPSA driver selects the retune
    // subset via --params.
    out.push_back(makeIntSpec("RazorBase", &searchParams.RazorBase, 150, 500, 15.0, 5.0));
    out.push_back(makeIntSpec("RazorDepth", &searchParams.RazorDepth, 100, 400, 15.0, 5.0));
    out.push_back(makeIntSpec("RfpBase", &searchParams.RfpBase, 200, 400, 10.0, 4.0));
    out.push_back(makeIntSpec("RfpImproving", &searchParams.RfpImproving, 80, 250, 8.0, 3.0));
    out.push_back(makeIntSpec("NmpBase", &searchParams.NmpBase, 2, 5, 0.5, 0.25));
    out.push_back(makeIntSpec("NmpEvalDiv", &searchParams.NmpEvalDiv, 300, 700, 30.0, 10.0));
    out.push_back(makeIntSpec("FpBase", &searchParams.FpBase, 150, 350, 10.0, 4.0));
    out.push_back(makeIntSpec("FpDepth", &searchParams.FpDepth, 120, 300, 10.0, 4.0));
    out.push_back(makeIntSpec("SeeCaptureCoef", &searchParams.SeeCaptureCoef, 25, 90, 4.0, 1.5));
    out.push_back(makeIntSpec("SeeQuietCoef", &searchParams.SeeQuietCoef, 60, 200, 8.0, 3.0));
    out.push_back(makeIntSpec("QsDeltaMargin", &searchParams.QsDeltaMargin, 300, 1200, 30.0, 10.0));

    // --- LMR table coefficients (scaled x100, table is rebuilt on write) ---
    // c_end values here are deliberately larger than the prior spec: LmrBase
    // contributes LmrBase/100 to the final int-cast of the reduction, so
    // sub-unit perturbations never cross rounding and show up as no-ops
    // in self-play. LmrDivisor bounds are narrowed ([180, 280]) for LTC
    // safety -- the STC-aggressive lower end past 180 meaningfully hurts
    // deep-tc play and no 10+0.1 SPSA signal justifies pushing there.
    out.push_back(
        makeIntSpec("LmrBase", &searchParams.LmrBase, 55, 100, 15.0, 5.0, rebuildLmrTable));
    out.push_back(
        makeIntSpec("LmrDivisor", &searchParams.LmrDivisor, 180, 280, 25.0, 8.0, rebuildLmrTable));

    // --- Correction-history weights ---
    // Each weight is divided by CorrHistGrain at read time. Bounds were picked
    // so a saturated table entry can contribute between ~a handful of units
    // and a bit over a pawn, matching the magnitude range Stockfish-style
    // correction tables are known to work in. Grain bounds keep the overall
    // correction scale within a sensible band even under extreme weights.
    out.push_back(makeIntSpec("PawnCorrWeight", &searchParams.PawnCorrWeight, 0, 256, 8.0, 3.0));
    out.push_back(
        makeIntSpec("NonPawnCorrWeight", &searchParams.NonPawnCorrWeight, 0, 256, 8.0, 3.0));
    out.push_back(makeIntSpec("MinorCorrWeight", &searchParams.MinorCorrWeight, 0, 256, 8.0, 3.0));
    out.push_back(makeIntSpec("ContCorrWeight", &searchParams.ContCorrWeight, 0, 256, 8.0, 3.0));
    out.push_back(
        makeIntSpec("CorrHistGrain", &searchParams.CorrHistGrain, 4096, 65536, 512.0, 256.0));

    // --- Late-history pruning coefficient ---
    // Skips quiets below `-HistoryPruningCoef * depth` once the picker is in
    // the Quiets phase. Magnitude space scales against the ~16K cap on
    // butterfly + continuation history, so bounds were picked to let SPSA
    // search the full range from "prune almost no quiets" up to "prune
    // anything below average".
    out.push_back(makeIntSpec("HistoryPruningCoef", &searchParams.HistoryPruningCoef, 1000, 12000,
                              400.0, 150.0));

    // --- Attack-aware quiet ordering and threat-based LMR ---
    // Bonuses stay in the butterfly + continuation history magnitude space
    // (cap at 65,536 = 2 * MAX_HISTORY) so SPSA can explore the regime where
    // a strong historical signal still outranks a threat cue. LMR deltas
    // stay integer-ply because the reduction table is integer-valued;
    // sub-unit perturbations never cross a reduction boundary so bounding
    // them tight to [0, 2] keeps the search space meaningful.
    out.push_back(
        makeIntSpec("ThreatEscapeBonus", &searchParams.ThreatEscapeBonus, 0, 65536, 2048.0, 512.0));
    out.push_back(makeIntSpec("ThreatWalkInPenalty", &searchParams.ThreatWalkInPenalty, 0, 65536,
                              2048.0, 512.0));
    out.push_back(makeIntSpec("LmrThreatEscape", &searchParams.LmrThreatEscape, 0, 2, 0.5, 0.25));
    out.push_back(makeIntSpec("LmrThreatWalkIn", &searchParams.LmrThreatWalkIn, 0, 2, 0.5, 0.25));

    // --- Eval Score halves. Every min is >= 0 so each bonus stays a bonus. ---
    out.push_back(makeScoreHalfSpec("TempoMg", &evalParams.Tempo, true, 0, 200, 8.0, 3.0));
    out.push_back(
        makeScoreHalfSpec("ThreatByPawnMg", &evalParams.ThreatByPawn, true, 0, 400, 15.0, 5.0));
    out.push_back(
        makeScoreHalfSpec("ThreatByPawnEg", &evalParams.ThreatByPawn, false, 0, 400, 15.0, 5.0));
    out.push_back(makeScoreHalfSpec("HangingMg", &evalParams.Hanging, true, 0, 300, 12.0, 4.0));
    out.push_back(makeScoreHalfSpec("HangingEg", &evalParams.Hanging, false, 0, 300, 12.0, 4.0));
    out.push_back(
        makeScoreHalfSpec("SafePawnPushMg", &evalParams.SafePawnPush, true, 0, 250, 10.0, 4.0));
    out.push_back(
        makeScoreHalfSpec("SafePawnPushEg", &evalParams.SafePawnPush, false, 0, 250, 10.0, 4.0));

    // --- King safety scalars feeding the king-danger accumulator. The mg
    // halves carry the dominant signal because the quadratic mapping
    // amplifies mg; the eg halves stay bonus-signed so the bonus-stays-
    // bonus invariant applies to both. ---
    out.push_back(makeScoreHalfSpec("KingAttackByKnightMg", &evalParams.KingAttackByKnight, true, 0,
                                    120, 8.0, 3.0));
    out.push_back(makeScoreHalfSpec("KingAttackByKnightEg", &evalParams.KingAttackByKnight, false,
                                    0, 40, 4.0, 1.5));
    out.push_back(makeScoreHalfSpec("KingAttackByBishopMg", &evalParams.KingAttackByBishop, true, 0,
                                    120, 8.0, 3.0));
    out.push_back(makeScoreHalfSpec("KingAttackByBishopEg", &evalParams.KingAttackByBishop, false,
                                    0, 40, 4.0, 1.5));
    out.push_back(makeScoreHalfSpec("KingAttackByRookMg", &evalParams.KingAttackByRook, true, 0,
                                    160, 10.0, 4.0));
    out.push_back(makeScoreHalfSpec("KingAttackByRookEg", &evalParams.KingAttackByRook, false, 0,
                                    40, 4.0, 1.5));
    out.push_back(makeScoreHalfSpec("KingAttackByQueenMg", &evalParams.KingAttackByQueen, true, 0,
                                    200, 10.0, 4.0));
    out.push_back(makeScoreHalfSpec("KingAttackByQueenEg", &evalParams.KingAttackByQueen, false, 0,
                                    60, 5.0, 2.0));

    out.push_back(makeScoreHalfSpec("KingSafeCheckKnightMg", &evalParams.KingSafeCheck[Knight][0],
                                    true, 0, 200, 10.0, 4.0));
    out.push_back(makeScoreHalfSpec("KingSafeCheckKnightMultiMg",
                                    &evalParams.KingSafeCheck[Knight][1], true, 0, 400, 15.0, 5.0));
    out.push_back(makeScoreHalfSpec("KingSafeCheckBishopMg", &evalParams.KingSafeCheck[Bishop][0],
                                    true, 0, 200, 10.0, 4.0));
    out.push_back(makeScoreHalfSpec("KingSafeCheckBishopMultiMg",
                                    &evalParams.KingSafeCheck[Bishop][1], true, 0, 400, 15.0, 5.0));
    out.push_back(makeScoreHalfSpec("KingSafeCheckRookMg", &evalParams.KingSafeCheck[Rook][0], true,
                                    0, 200, 10.0, 4.0));
    out.push_back(makeScoreHalfSpec("KingSafeCheckRookMultiMg", &evalParams.KingSafeCheck[Rook][1],
                                    true, 0, 400, 15.0, 5.0));
    out.push_back(makeScoreHalfSpec("KingSafeCheckQueenMg", &evalParams.KingSafeCheck[Queen][0],
                                    true, 0, 240, 12.0, 4.0));
    out.push_back(makeScoreHalfSpec("KingSafeCheckQueenMultiMg",
                                    &evalParams.KingSafeCheck[Queen][1], true, 0, 480, 16.0, 5.0));

    out.push_back(makeScoreHalfSpec("KingRingWeakWeightMg", &evalParams.KingRingWeakWeight, true, 0,
                                    60, 5.0, 2.0));
    out.push_back(makeScoreHalfSpec("KingNoQueenDiscountMg", &evalParams.KingNoQueenDiscount, true,
                                    0, 200, 10.0, 4.0));

    // --- Rook coordination with passed pawns (Tarrasch). Bonus-signed per
    // the usual "bonus stays a bonus" invariant, sized small because the
    // eval applies them per rook-passer pair and a doubled rook lift can
    // credit twice on the same file. ---
    out.push_back(makeScoreHalfSpec("RookBehindOurPasserBonusMg",
                                    &evalParams.RookBehindOurPasserBonus, true, 0, 60, 5.0, 2.0));
    out.push_back(makeScoreHalfSpec("RookBehindOurPasserBonusEg",
                                    &evalParams.RookBehindOurPasserBonus, false, 0, 80, 6.0, 2.0));
    out.push_back(makeScoreHalfSpec("RookBehindTheirPasserBonusMg",
                                    &evalParams.RookBehindTheirPasserBonus, true, 0, 60, 5.0, 2.0));
    out.push_back(makeScoreHalfSpec("RookBehindTheirPasserBonusEg",
                                    &evalParams.RookBehindTheirPasserBonus, false, 0, 80, 6.0,
                                    2.0));

    // --- Minor behind pawn: small bonus applied per shielded minor,
    // sized in line with other per-piece activity scalars. ---
    out.push_back(makeScoreHalfSpec("MinorBehindPawnBonusMg", &evalParams.MinorBehindPawnBonus,
                                    true, 0, 60, 5.0, 2.0));
    out.push_back(makeScoreHalfSpec("MinorBehindPawnBonusEg", &evalParams.MinorBehindPawnBonus,
                                    false, 0, 60, 5.0, 2.0));

    // --- Piece on king ring: per-piece linear bonus whose trigger is
    // shared with the multi-attacker king-danger accumulator, so the
    // SPSA ranges stay small to keep the two buckets independently
    // observable without a joint retune. ---
    out.push_back(
        makeScoreHalfSpec("MinorOnKingRingMg", &evalParams.MinorOnKingRing, true, 0, 40, 3.0, 1.0));
    out.push_back(makeScoreHalfSpec("MinorOnKingRingEg", &evalParams.MinorOnKingRing, false, 0, 20,
                                    2.0, 1.0));
    out.push_back(
        makeScoreHalfSpec("RookOnKingRingMg", &evalParams.RookOnKingRing, true, 0, 60, 4.0, 1.5));
    out.push_back(
        makeScoreHalfSpec("RookOnKingRingEg", &evalParams.RookOnKingRing, false, 0, 30, 2.0, 1.0));

    // --- King protector: per-Chebyshev-step penalty that keeps knights
    // and bishops anchored near our own king. Penalty-signed so SPSA
    // cannot flip it into a bonus; the band stays narrow because the
    // per-step cost already multiplies by up to seven. ---
    out.push_back(
        makeScoreHalfSpec("KingProtectorMg", &evalParams.KingProtector, true, -20, 0, 2.0, 1.0));
    out.push_back(
        makeScoreHalfSpec("KingProtectorEg", &evalParams.KingProtector, false, -20, 0, 2.0, 1.0));

    // --- Slider on queen x-ray: bonus-signed per indirect diagonal or
    // orthogonal pressure line ending at the enemy queen. Bishop and
    // rook variants carry separate bands because rook x-rays tend to
    // ride open files and score a touch higher at parity with heavy
    // pieces on the board. ---
    out.push_back(makeScoreHalfSpec("SliderOnQueenBishopMg", &evalParams.SliderOnQueenBishop, true,
                                    0, 60, 4.0, 1.5));
    out.push_back(makeScoreHalfSpec("SliderOnQueenBishopEg", &evalParams.SliderOnQueenBishop, false,
                                    0, 60, 4.0, 1.5));
    out.push_back(makeScoreHalfSpec("SliderOnQueenRookMg", &evalParams.SliderOnQueenRook, true, 0,
                                    80, 5.0, 2.0));
    out.push_back(makeScoreHalfSpec("SliderOnQueenRookEg", &evalParams.SliderOnQueenRook, false, 0,
                                    80, 5.0, 2.0));

    // --- Restricted piece: bonus per square of shared attack pressure.
    // Counts in the 10 to 40 range are common, so the per-square weight
    // is intentionally tiny. ---
    out.push_back(
        makeScoreHalfSpec("RestrictedPieceMg", &evalParams.RestrictedPiece, true, 0, 20, 1.5, 0.5));
    out.push_back(makeScoreHalfSpec("RestrictedPieceEg", &evalParams.RestrictedPiece, false, 0, 20,
                                    1.5, 0.5));

    // --- New king-safety scalars matching the classical reference. ---
    out.push_back(makeScoreHalfSpec("KingUnsafeCheckWeightMg", &evalParams.KingUnsafeCheckWeight,
                                    true, 0, 60, 4.0, 1.5));
    out.push_back(makeScoreHalfSpec("KingAttacksWeightMg", &evalParams.KingAttacksWeight, true, 0,
                                    30, 2.0, 1.0));
    out.push_back(makeScoreHalfSpec("KingBlockerWeightMg", &evalParams.KingBlockerWeight, true, 0,
                                    40, 3.0, 1.0));
    out.push_back(makeScoreHalfSpec("KingKnightDefenderDiscountMg",
                                    &evalParams.KingKnightDefenderDiscount, true, 0, 200, 10.0,
                                    4.0));
    out.push_back(makeScoreHalfSpec("KingDangerConstantMg", &evalParams.KingDangerConstant, true, 0,
                                    80, 5.0, 2.0));
    out.push_back(
        makeScoreHalfSpec("PawnlessFlankMg", &evalParams.PawnlessFlank, true, -60, 0, 4.0, 1.5));
    out.push_back(
        makeScoreHalfSpec("PawnlessFlankEg", &evalParams.PawnlessFlank, false, -200, 0, 10.0, 4.0));
    out.push_back(
        makeScoreHalfSpec("KingFlankAttackMg", &evalParams.KingFlankAttack, true, 0, 30, 2.0, 1.0));
    out.push_back(makeScoreHalfSpec("KingFlankAttack2Mg", &evalParams.KingFlankAttack2, true, 0, 40,
                                    3.0, 1.0));
    out.push_back(makeScoreHalfSpec("KingFlankDefenseMg", &evalParams.KingFlankDefense, true, -20,
                                    0, 2.0, 1.0));

    // --- Outposts and pieces ---
    out.push_back(makeScoreHalfSpec("ReachableOutpostMg", &evalParams.ReachableOutpost, true, 0, 80,
                                    5.0, 2.0));
    out.push_back(makeScoreHalfSpec("ReachableOutpostEg", &evalParams.ReachableOutpost, false, 0,
                                    60, 4.0, 1.5));
    out.push_back(
        makeScoreHalfSpec("BadOutpostMg", &evalParams.BadOutpost, true, -60, 60, 4.0, 1.5));
    out.push_back(
        makeScoreHalfSpec("BadOutpostEg", &evalParams.BadOutpost, false, -60, 80, 5.0, 2.0));
    out.push_back(
        makeScoreHalfSpec("BishopXRayPawnsMg", &evalParams.BishopXRayPawns, true, 0, 20, 1.5, 0.5));
    out.push_back(makeScoreHalfSpec("BishopXRayPawnsEg", &evalParams.BishopXRayPawns, false, 0, 20,
                                    1.5, 0.5));
    out.push_back(makeScoreHalfSpec("BishopOnKingRingXRayMg", &evalParams.BishopOnKingRingXRay,
                                    true, 0, 60, 4.0, 1.5));
    out.push_back(
        makeScoreHalfSpec("RookOnQueenFileMg", &evalParams.RookOnQueenFile, true, 0, 30, 2.0, 1.0));
    out.push_back(makeScoreHalfSpec("RookOnQueenFileEg", &evalParams.RookOnQueenFile, false, 0, 30,
                                    2.0, 1.0));
    out.push_back(makeScoreHalfSpec("QueenInfiltrationMg", &evalParams.QueenInfiltration, true, -20,
                                    20, 2.0, 1.0));
    out.push_back(makeScoreHalfSpec("QueenInfiltrationEg", &evalParams.QueenInfiltration, false,
                                    -20, 40, 2.0, 1.0));

    // --- Threats: weak lever, weak queen protection, knight on queen ---
    out.push_back(makeScoreHalfSpec("WeakLeverMg", &evalParams.WeakLever, true, -20, 0, 2.0, 1.0));
    out.push_back(
        makeScoreHalfSpec("WeakLeverEg", &evalParams.WeakLever, false, -120, 0, 6.0, 2.0));
    out.push_back(makeScoreHalfSpec("WeakQueenProtectionMg", &evalParams.WeakQueenProtection, true,
                                    0, 40, 3.0, 1.0));
    out.push_back(
        makeScoreHalfSpec("KnightOnQueenMg", &evalParams.KnightOnQueen, true, 0, 60, 4.0, 1.5));
    out.push_back(
        makeScoreHalfSpec("KnightOnQueenEg", &evalParams.KnightOnQueen, false, 0, 60, 4.0, 1.5));

    // --- Threat-table fills (rook/minor on minor) carried over ---
    out.push_back(makeScoreHalfSpec("ThreatByRookKnightMg", &evalParams.ThreatByRook[Knight], true,
                                    0, 120, 6.0, 2.0));
    out.push_back(makeScoreHalfSpec("ThreatByRookKnightEg", &evalParams.ThreatByRook[Knight], false,
                                    0, 120, 6.0, 2.0));
    out.push_back(makeScoreHalfSpec("ThreatByRookBishopMg", &evalParams.ThreatByRook[Bishop], true,
                                    0, 120, 6.0, 2.0));
    out.push_back(makeScoreHalfSpec("ThreatByRookBishopEg", &evalParams.ThreatByRook[Bishop], false,
                                    0, 120, 6.0, 2.0));
    out.push_back(makeScoreHalfSpec("ThreatByMinorKnightMg", &evalParams.ThreatByMinor[Knight],
                                    true, 0, 80, 5.0, 2.0));
    out.push_back(makeScoreHalfSpec("ThreatByMinorKnightEg", &evalParams.ThreatByMinor[Knight],
                                    false, 0, 80, 5.0, 2.0));
    out.push_back(makeScoreHalfSpec("ThreatByMinorBishopMg", &evalParams.ThreatByMinor[Bishop],
                                    true, 0, 80, 5.0, 2.0));
    out.push_back(makeScoreHalfSpec("ThreatByMinorBishopEg", &evalParams.ThreatByMinor[Bishop],
                                    false, 0, 80, 5.0, 2.0));

    // --- Initiative and passed pawn additions ---
    out.push_back(makeScoreHalfSpec("InitiativeBothFlanksEg", &evalParams.InitiativeBothFlanks,
                                    false, 0, 60, 4.0, 1.5));
    out.push_back(makeScoreHalfSpec("InitiativeAlmostUnwinnableEg",
                                    &evalParams.InitiativeAlmostUnwinnable, false, -120, 0, 6.0,
                                    2.0));
    out.push_back(makeScoreHalfSpec("KingPawnDistanceEg", &evalParams.KingPawnDistance, false, -40,
                                    0, 3.0, 1.0));
    out.push_back(
        makeScoreHalfSpec("PassedFileMg", &evalParams.PassedFile, true, -40, 0, 3.0, 1.0));
    out.push_back(
        makeScoreHalfSpec("PassedFileEg", &evalParams.PassedFile, false, -40, 0, 3.0, 1.0));

    return out;
}

} // namespace

const std::vector<TunableSpec> &tunables() {
    static const std::vector<TunableSpec> registry = buildRegistry();
    return registry;
}

const TunableSpec *findTunable(const std::string &name) {
    const auto &specs = tunables();
    auto it = std::find_if(specs.begin(), specs.end(),
                           [&name](const TunableSpec &s) { return s.name == name; });
    if (it == specs.end()) return nullptr;
    return &*it;
}
