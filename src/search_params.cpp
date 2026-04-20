#include "search_params.h"

// Compiled-in defaults for the tunable search parameters. The in-memory
// `searchParams` instance is initialized from this struct and
// `resetSearchParams()` snaps it back if ever needed.
//
// Values below are the completed output of a 300-iteration SPSA self-play
// run at 10+0.1, concurrency 6, seed 1 (3,600 games total, ~6.7 hours on
// a 14-core machine). Spall gain sequence: alpha=0.602, gamma=0.101,
// A = 0.1 * iterations. NmpBase did not move because its [2,5] integer
// range is too tight for a c_end of 0.5 to produce meaningful
// perturbations; the rest converged naturally.

static const SearchParams kDefaultSearchParams = {
    282, // RazorBase         (300 -> 282)
    288, // RazorDepth        (250 -> 288)
    286, // RfpBase           (290 -> 286)
    161, // RfpImproving      (145 -> 161)
    3,   // NmpBase           (unchanged; integer-range trap)
    484, // NmpEvalDiv        (483 -> 484)
    227, // FpBase            (241 -> 227)
    223, // FpDepth           (193 -> 223)
    34,  // SeeCaptureCoef    (48  -> 34)
    121, // SeeQuietCoef      (121 -> 121)
    73,  // LmrBase           (75  -> 73; scaled x100, table uses 0.73)
    199, // LmrDivisor        (225 -> 199; scaled x100, table uses 1.99)
};

SearchParams searchParams = kDefaultSearchParams;

void resetSearchParams() {
    searchParams = kDefaultSearchParams;
}
