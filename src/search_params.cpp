#include "search_params.h"

// Compiled-in defaults for the tunable search parameters. The in-memory
// `searchParams` instance is initialized from this struct and
// `resetSearchParams()` snaps it back if ever needed.
//
// Values below come from an iter-183 mid-run snapshot of an SPSA self-play
// run at 10+0.1, concurrency 6, seed 1 (300-iteration target). At iter
// 183 the Spall gain sequence (alpha=0.602, gamma=0.101) has decayed
// `a_k` to roughly 40 percent of its starting value; early over-shoots
// have largely been retraced. A follow-up commit on this branch will
// refresh these values once the run completes.

static const SearchParams kDefaultSearchParams = {
    284, // RazorBase         (300 -> 284)
    294, // RazorDepth        (250 -> 294)
    292, // RfpBase           (290 -> 292)
    159, // RfpImproving      (145 -> 159)
    3,   // NmpBase           (unchanged; integer range [2,5] is too tight for SPSA)
    483, // NmpEvalDiv        (unchanged at default after drifting both ways)
    247, // FpBase            (241 -> 247)
    249, // FpDepth           (193 -> 249)
    33,  // SeeCaptureCoef    (48  -> 33)
    121, // SeeQuietCoef      (121 -> 121)
    66,  // LmrBase           (75  -> 66; scaled x100, table uses 0.66)
    213, // LmrDivisor        (225 -> 213; scaled x100, table uses 2.13)
};

SearchParams searchParams = kDefaultSearchParams;

void resetSearchParams() {
    searchParams = kDefaultSearchParams;
}
