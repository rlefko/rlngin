#include "search_params.h"

// Compiled-in defaults for the search parameters. The in-memory
// `searchParams` instance is initialized from this struct and
// `resetSearchParams()` snaps it back if ever needed.
//
// Final values from the 1000-iteration focused SPSA retune at 10+0.1,
// concurrency 6, 9-scalar scope. Razor margins and NmpBase were excluded
// from the scope (tactical hard-prune; integer-range trap, respectively).

static const SearchParams kDefaultSearchParams = {
    300,   // RazorBase
    250,   // RazorDepth
    307,   // RfpBase
    185,   // RfpImproving
    3,     // NmpBase
    429,   // NmpEvalDiv
    158,   // FpBase
    246,   // FpDepth
    38,    // SeeCaptureCoef
    97,    // SeeQuietCoef
    724,   // QsDeltaMargin
    74,    // LmrBase           (scaled x100; 0.74 in LMR formula)
    181,   // LmrDivisor        (scaled x100; 1.81 in LMR formula)
    128,   // PawnCorrWeight    (preserves the prior pawn-only correction at full strength)
    16,    // NonPawnCorrWeight (modest additive signal per color, summed across both sides)
    8,     // MinorCorrWeight   (small refinement on top of the non-pawn term)
    16,    // ContCorrWeight    (two-ply keyed table; modest weight keeps magnitude in check)
    16384, // CorrHistGrain     (shared denominator; also the per-table clamp max)
    4000,  // HistoryPruningCoef (threshold per depth ply for late-history pruning)
    32768, // ThreatEscapeBonus (attack-aware quiet escape lift; see SearchParams.h)
    32768, // ThreatWalkInPenalty (symmetric walk-into-threat quiet penalty)
    1,     // LmrThreatEscape (ply discount for evacuating a threatened piece)
    1,     // LmrThreatWalkIn (ply surcharge for walking into a lesser-piece attack)
    2,     // SingularBetaMul (matches the prior hardcoded depth * 2 in singularBeta)
    2,     // SingularDepthDiv (matches the prior hardcoded (depth - 1) / 2 in singularDepth)
    17,    // SingularDoubleMargin (Stockfish-lineage default; extends by two when alts fall below)
    7,     // IirCutNodeDepth (extra IIR at cut-nodes once remaining depth >= this)
    12,    // AspWindowBase (Stockfish-lineage minimum half-width)
    8,     // AspWindowDiv (divisor for the |prevScore|-scaled term)
};

SearchParams searchParams = kDefaultSearchParams;

void resetSearchParams() {
    searchParams = kDefaultSearchParams;
}
