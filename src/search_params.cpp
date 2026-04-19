#include "search_params.h"

// Compiled-in defaults for the tunable search parameters. The in-memory
// `searchParams` instance is initialized from this struct and
// `resetSearchParams()` snaps it back if ever needed.
//
// Values below come from the first 52 iterations of an SPSA self-play run
// at 10+0.1, concurrency 6, seed 1 (300-iteration target, still in
// progress). The per-parameter Spall gain sequence (alpha=0.602,
// gamma=0.101, A=0.1 * iterations) has started decaying the step size but
// has not yet converged; a follow-up commit on this branch will refresh
// these values from the completed run.

static const SearchParams kDefaultSearchParams = {
    287, // RazorBase         (300 -> 287)
    273, // RazorDepth        (250 -> 273)
    287, // RfpBase           (290 -> 287)
    135, // RfpImproving      (145 -> 135)
    3,   // NmpBase           (unchanged; integer range [2,5] is too tight for SPSA)
    488, // NmpEvalDiv        (483 -> 488)
    228, // FpBase            (241 -> 228)
    216, // FpDepth           (193 -> 216)
    39,  // SeeCaptureCoef    (48  -> 39)
    123, // SeeQuietCoef      (121 -> 123)
    68,  // LmrBase           (75  -> 68; scaled x100, table uses 0.68)
    217, // LmrDivisor        (225 -> 217; scaled x100, table uses 2.17)
};

SearchParams searchParams = kDefaultSearchParams;

void resetSearchParams() {
    searchParams = kDefaultSearchParams;
}
