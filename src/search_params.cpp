#include "search_params.h"

// Compiled-in defaults for the tunable search parameters. The in-memory
// `searchParams` instance is initialized from this struct and
// `resetSearchParams()` snaps it back if ever needed.
//
// Values below come from the first 104 iterations of an SPSA self-play
// run at 10+0.1, concurrency 6, seed 1 (300-iteration target, still in
// progress). The Spall gain sequence (alpha=0.602, gamma=0.101,
// A=0.1 * iterations) has decayed a_k by roughly 2.5x from the start, so
// early over-shoots have largely pulled back; a follow-up commit on this
// branch will refresh these values once the run completes.

static const SearchParams kDefaultSearchParams = {
    281, // RazorBase         (300 -> 281)
    285, // RazorDepth        (250 -> 285)
    299, // RfpBase           (290 -> 299)
    157, // RfpImproving      (145 -> 157)
    3,   // NmpBase           (unchanged; integer range [2,5] is too tight for SPSA)
    487, // NmpEvalDiv        (483 -> 487)
    232, // FpBase            (241 -> 232)
    222, // FpDepth           (193 -> 222)
    35,  // SeeCaptureCoef    (48  -> 35)
    119, // SeeQuietCoef      (121 -> 119)
    62,  // LmrBase           (75  -> 62; scaled x100, table uses 0.62)
    217, // LmrDivisor        (225 -> 217; scaled x100, table uses 2.17)
};

SearchParams searchParams = kDefaultSearchParams;

void resetSearchParams() {
    searchParams = kDefaultSearchParams;
}
