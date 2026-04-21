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
    74,    // LmrBase           (scaled x100; 0.74 in LMR formula)
    181,   // LmrDivisor        (scaled x100; 1.81 in LMR formula)
    128,   // PawnCorrWeight    (reproduces the pre-unified entry / 128 term)
    64,    // NonPawnCorrWeight (reproduces entry / 256 per color term)
    43,    // MinorCorrWeight   (reproduces the pre-unified entry / 384 term)
    64,    // ContCorrWeight    (reproduces entry / 256 term)
    16384, // CorrHistGrain     (shared denominator; also the per-table clamp max)
};

SearchParams searchParams = kDefaultSearchParams;

void resetSearchParams() {
    searchParams = kDefaultSearchParams;
}
