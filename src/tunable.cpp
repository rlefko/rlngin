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
//   * Every eval term in this list is a "Bonus" or "ThreatBy" scalar,
//     which the engine adds with a positive sign. Locking min >= 0 keeps
//     SPSA from driving a bonus across zero and accidentally turning it
//     into a penalty (the "bonuses stay bonuses" prior that the Texel
//     tuner enforces for symmetrically-named penalties).
// No scalar in the current list is a "Penalty", so no non-positive bounds
// are needed here; that side of the constraint catalog lives in the Texel
// tuner instead.
std::vector<TunableSpec> buildRegistry() {
    std::vector<TunableSpec> out;
    out.reserve(20);

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
    out.push_back(makeScoreHalfSpec("RookOn7thBonusEg", &evalParams.RookOn7thBonus, false, 0, 200,
                                    10.0, 4.0));

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
