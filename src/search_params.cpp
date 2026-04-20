#include "search_params.h"

// Compiled-in defaults for the tunable search parameters. The in-memory
// `searchParams` instance is initialized from this struct and
// `resetSearchParams()` snaps it back if ever needed.
//
// Values below combine the pre-SPSA defaults with the subset of the
// 300-iteration SPSA self-play run (10+0.1, concurrency 6, seed 1, 3,600
// games, Spall alpha=0.602 / gamma=0.101 / A=0.1*iterations) that we
// chose to apply. Eight of the twelve scalars carry the SPSA-converged
// values; `RazorBase` and `RazorDepth` were reverted because razoring
// drops straight into qsearch at shallow depth and any mistuning shows
// up as a tactical blunder, and `NmpBase` / `SeeQuietCoef` never moved
// during the run (NmpBase's [2,5] integer range is too tight for
// c_end=0.5 to produce a perturbation, and SeeQuietCoef showed no
// gradient signal). The full SPSA output sits alongside this file in
// `tuning/spsa/theta.json` for reference.

static const SearchParams kDefaultSearchParams = {
    300, // RazorBase         (SPSA proposed 282; reverted -- tactical impact)
    250, // RazorDepth        (SPSA proposed 288; reverted -- tactical impact)
    286, // RfpBase           (290 -> 286)
    161, // RfpImproving      (145 -> 161)
    3,   // NmpBase           (integer-range trap; no SPSA movement)
    484, // NmpEvalDiv        (483 -> 484)
    227, // FpBase            (241 -> 227)
    223, // FpDepth           (193 -> 223)
    34,  // SeeCaptureCoef    (48  -> 34)
    121, // SeeQuietCoef      (no SPSA gradient signal)
    73,  // LmrBase           (75  -> 73; scaled x100, table uses 0.73)
    199, // LmrDivisor        (225 -> 199; scaled x100, table uses 1.99)
};

SearchParams searchParams = kDefaultSearchParams;

void resetSearchParams() {
    searchParams = kDefaultSearchParams;
}
