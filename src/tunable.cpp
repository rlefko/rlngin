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
TunableSpec makeIntSpec(std::string name, int *target, int minValue, int maxValue, int defaultValue,
                        double cEnd, double rEnd, std::function<void()> afterWrite = nullptr) {
    TunableSpec s;
    s.name = std::move(name);
    s.minValue = minValue;
    s.maxValue = maxValue;
    s.defaultValue = defaultValue;
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
TunableSpec makeScoreHalfSpec(std::string name, Score *target, bool isMg, int minValue,
                              int maxValue, int defaultValue, double cEnd, double rEnd) {
    TunableSpec s;
    s.name = std::move(name);
    s.minValue = minValue;
    s.maxValue = maxValue;
    s.defaultValue = defaultValue;
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

std::vector<TunableSpec> buildRegistry() {
    std::vector<TunableSpec> out;
    out.reserve(20);

    // --- Search pruning and reduction scalars ---
    out.push_back(makeIntSpec("RazorBase", &searchParams.RazorBase, 150, 500, 300, 15.0, 5.0));
    out.push_back(makeIntSpec("RazorDepth", &searchParams.RazorDepth, 100, 400, 250, 15.0, 5.0));
    out.push_back(makeIntSpec("RfpBase", &searchParams.RfpBase, 150, 450, 290, 15.0, 5.0));
    out.push_back(makeIntSpec("RfpImproving", &searchParams.RfpImproving, 50, 300, 145, 10.0, 4.0));
    out.push_back(makeIntSpec("NmpBase", &searchParams.NmpBase, 2, 5, 3, 0.5, 0.25));
    out.push_back(makeIntSpec("NmpEvalDiv", &searchParams.NmpEvalDiv, 200, 800, 483, 25.0, 8.0));
    out.push_back(makeIntSpec("FpBase", &searchParams.FpBase, 100, 400, 241, 15.0, 5.0));
    out.push_back(makeIntSpec("FpDepth", &searchParams.FpDepth, 75, 350, 193, 15.0, 5.0));
    out.push_back(
        makeIntSpec("SeeCaptureCoef", &searchParams.SeeCaptureCoef, 15, 120, 48, 5.0, 2.0));
    out.push_back(makeIntSpec("SeeQuietCoef", &searchParams.SeeQuietCoef, 40, 250, 121, 10.0, 4.0));

    // --- LMR table coefficients (scaled x100, table is rebuilt on write) ---
    out.push_back(
        makeIntSpec("LmrBase", &searchParams.LmrBase, 40, 110, 75, 5.0, 2.0, rebuildLmrTable));
    out.push_back(makeIntSpec("LmrDivisor", &searchParams.LmrDivisor, 150, 350, 225, 10.0, 4.0,
                              rebuildLmrTable));

    // --- Eval Score halves ---
    out.push_back(makeScoreHalfSpec("TempoMg", &evalParams.Tempo, true, 0, 200, 96, 8.0, 3.0));
    out.push_back(makeScoreHalfSpec("ThreatByPawnMg", &evalParams.ThreatByPawn, true, 50, 400, 204,
                                    15.0, 5.0));
    out.push_back(makeScoreHalfSpec("ThreatByPawnEg", &evalParams.ThreatByPawn, false, 50, 400, 245,
                                    15.0, 5.0));
    out.push_back(
        makeScoreHalfSpec("HangingMg", &evalParams.Hanging, true, 0, 300, 158, 12.0, 4.0));
    out.push_back(
        makeScoreHalfSpec("HangingEg", &evalParams.Hanging, false, 0, 300, 148, 12.0, 4.0));
    out.push_back(makeScoreHalfSpec("SafePawnPushMg", &evalParams.SafePawnPush, true, 0, 250, 107,
                                    10.0, 4.0));
    out.push_back(makeScoreHalfSpec("SafePawnPushEg", &evalParams.SafePawnPush, false, 0, 250, 37,
                                    10.0, 4.0));
    out.push_back(makeScoreHalfSpec("RookOn7thBonusEg", &evalParams.RookOn7thBonus, false, 0, 200,
                                    69, 10.0, 4.0));

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
