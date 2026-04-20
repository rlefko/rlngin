#include "search_params.h"

// Compiled-in defaults for the tunable search parameters. The in-memory
// `searchParams` instance is initialized from this struct and
// `resetSearchParams()` snaps it back if ever needed.
//
// Values below come from an iter-222 mid-run snapshot of an SPSA
// self-play run at 10+0.1, concurrency 6, seed 1 (300-iteration target).
// At iter 222 the Spall gain sequence (alpha=0.602, gamma=0.101) has
// decayed `a_k` to roughly 30 percent of its starting value; updates are
// now fine-tuning rather than exploring. A final commit on this branch
// will refresh these values once the run completes.

static const SearchParams kDefaultSearchParams = {
    296, // RazorBase         (300 -> 296)
    296, // RazorDepth        (250 -> 296)
    284, // RfpBase           (290 -> 284)
    157, // RfpImproving      (145 -> 157)
    3,   // NmpBase           (unchanged; integer range [2,5] is too tight for SPSA)
    489, // NmpEvalDiv        (483 -> 489)
    255, // FpBase            (241 -> 255)
    235, // FpDepth           (193 -> 235)
    35,  // SeeCaptureCoef    (48  -> 35)
    127, // SeeQuietCoef      (121 -> 127)
    68,  // LmrBase           (75  -> 68; scaled x100, table uses 0.68)
    209, // LmrDivisor        (225 -> 209; scaled x100, table uses 2.09)
};

SearchParams searchParams = kDefaultSearchParams;

void resetSearchParams() {
    searchParams = kDefaultSearchParams;
}
