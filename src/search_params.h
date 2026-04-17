#ifndef SEARCH_PARAMS_H
#define SEARCH_PARAMS_H

// Tunable search pruning and reduction scalars. The values live in a
// mutable struct so UCI setoption and the SPSA tuner can adjust them
// without rebuilding the engine. Defaults match the previously hardcoded
// literals exactly, so the initial runtime behavior is bit-identical.
struct SearchParams {
    // Razoring: fold shallow losing nodes straight into qsearch once
    //   staticEval + RazorBase + RazorDepth * depth <= alpha
    int RazorBase;
    int RazorDepth;

    // Reverse futility pruning: fail high on shallow nodes when
    //   staticEval - (RfpBase - RfpImproving * improving) * depth >= beta
    int RfpBase;
    int RfpImproving;

    // Null-move pruning reduction: R = NmpBase + depth / 3 + clamp(
    //   (staticEval - beta) / NmpEvalDiv, 0, 3).
    int NmpBase;
    int NmpEvalDiv;

    // Futility pruning for quiet moves:
    //   staticEval + FpBase + FpDepth * depth <= alpha implies skip.
    int FpBase;
    int FpDepth;

    // SEE pruning thresholds applied as negative margins at the call site.
    // Captures: -SeeCaptureCoef * depth * depth. Quiets: -SeeQuietCoef * depth.
    int SeeCaptureCoef;
    int SeeQuietCoef;
};

extern SearchParams searchParams;

// Reset searchParams to the compiled-in defaults.
void resetSearchParams();

#endif
