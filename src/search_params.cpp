#include "search_params.h"

// Compiled-in defaults for the tunable search parameters. The in-memory
// `searchParams` instance is initialized from this struct and
// `resetSearchParams()` snaps it back if ever needed.
//
// Values below keep only the subset of the 300-iteration SPSA self-play
// run output (10+0.1, concurrency 6, seed 1, 3,600 games, Spall
// alpha=0.602 / gamma=0.101 / A=0.1*iterations) that survived two
// filters: the SPSA move had to be large enough to outrun the noise
// floor (greater than ~5 percent of the original default) AND the
// parameter had to sit behind a research-at-full-depth fallback so a
// mistuned value costs nodes rather than missed tactics. That leaves
// four kept scalars (RfpImproving, FpBase, FpDepth, LmrDivisor) and
// eight reverted back to their pre-SPSA values: RazorBase and RazorDepth
// were rejected because razoring drops straight into qsearch (tactical
// hard prune); SeeCaptureCoef was rejected for the same reason (hard
// skip of low-SEE captures); NmpBase did not move during the run
// because its [2,5] integer range is too tight for c_end=0.5 to produce
// a perturbation; RfpBase, NmpEvalDiv, LmrBase, and SeeQuietCoef moved
// by magnitudes at or below the noise floor. The full SPSA output sits
// alongside this file in `tuning/spsa/theta.json` for reference.

static const SearchParams kDefaultSearchParams = {
    300, // RazorBase         (SPSA proposed 282; reverted -- tactical hard prune)
    250, // RazorDepth        (SPSA proposed 288; reverted -- tactical hard prune)
    290, // RfpBase           (SPSA proposed 286; reverted -- 1% move is noise)
    161, // RfpImproving      (145 -> 161)
    3,   // NmpBase           (integer-range trap; no SPSA movement)
    483, // NmpEvalDiv        (SPSA proposed 484; reverted -- 0.2% move is noise)
    227, // FpBase            (241 -> 227)
    223, // FpDepth           (193 -> 223)
    48,  // SeeCaptureCoef    (SPSA proposed 34; reverted -- tactical hard prune)
    121, // SeeQuietCoef      (no SPSA gradient signal)
    75,  // LmrBase           (SPSA proposed 73; reverted -- sub-rounding change; scaled x100, table
         // uses 0.75)
    199, // LmrDivisor        (225 -> 199; scaled x100, table uses 1.99)
};

SearchParams searchParams = kDefaultSearchParams;

void resetSearchParams() {
    searchParams = kDefaultSearchParams;
}
