#include "search_params.h"

// Current compiled-in defaults for the tunable search parameters. The
// in-memory `searchParams` instance is initialized from these values so
// the engine behavior at startup matches the previously hardcoded
// literals byte for byte.

static const SearchParams kDefaultSearchParams = {
    300, // RazorBase
    250, // RazorDepth
    290, // RfpBase
    145, // RfpImproving
    3,   // NmpBase
    483, // NmpEvalDiv
    241, // FpBase
    193, // FpDepth
    48,  // SeeCaptureCoef
    121, // SeeQuietCoef
};

SearchParams searchParams = kDefaultSearchParams;

void resetSearchParams() {
    searchParams = kDefaultSearchParams;
}
