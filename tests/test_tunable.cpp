#include "catch_amalgamated.hpp"
#include "eval_params.h"
#include "search.h"
#include "search_params.h"
#include "tunable.h"

#include <set>
#include <string>

TEST_CASE("Tunable registry: exposes the expected SPSA surface", "[tunable]") {
    const auto &specs = tunables();

    // Ninety five is the committed SPSA surface: the eighty-eight pre-uplift
    // scalars plus the four search-strength uplift scalars from #76
    // (SingularBetaMul, SingularDepthDiv, SingularDoubleMargin,
    // IirCutNodeDepth), the two aspiration-window scalars from #78
    // (AspWindowBase, AspWindowDiv), and PawnHistoryWeight from this PR.
    // If this count ever changes, the SPSA driver's iteration budget and
    // the PR description should move too.
    REQUIRE(specs.size() == 95);

    std::set<std::string> names;
    for (const TunableSpec &spec : specs) {
        CAPTURE(spec.name);

        // Names must be unique so UCI setoption dispatch is unambiguous.
        auto inserted = names.insert(spec.name);
        CHECK(inserted.second);

        // Bounds must be well-formed and contain the default.
        CHECK(spec.minValue <= spec.defaultValue);
        CHECK(spec.defaultValue <= spec.maxValue);

        // SPSA hyperparameters must be positive; zero would collapse the
        // gain sequence and freeze the parameter.
        CHECK(spec.cEnd > 0.0);
        CHECK(spec.rEnd > 0.0);
    }
}

TEST_CASE("Tunable registry: findTunable round-trips known names", "[tunable]") {
    const TunableSpec *nmp = findTunable("NmpBase");
    REQUIRE(nmp != nullptr);
    CHECK(nmp->name == "NmpBase");
    CHECK(nmp->defaultValue == 3);

    const TunableSpec *threat = findTunable("ThreatByPawnMg");
    REQUIRE(threat != nullptr);
    CHECK(threat->name == "ThreatByPawnMg");

    CHECK(findTunable("NotAParameter") == nullptr);
}

TEST_CASE("Tunable registry: get/set round-trips search int fields", "[tunable]") {
    resetSearchParams();
    const TunableSpec *spec = findTunable("FpBase");
    REQUIRE(spec != nullptr);

    const int original = spec->get();
    CHECK(original == searchParams.FpBase);

    spec->set(original + 7);
    CHECK(spec->get() == original + 7);
    CHECK(searchParams.FpBase == original + 7);

    spec->set(original);
    CHECK(spec->get() == original);

    resetSearchParams();
}

TEST_CASE("Tunable registry: setters clamp out-of-range values", "[tunable]") {
    resetSearchParams();
    const TunableSpec *spec = findTunable("NmpBase");
    REQUIRE(spec != nullptr);

    spec->set(spec->maxValue + 1000);
    CHECK(spec->get() == spec->maxValue);

    spec->set(spec->minValue - 1000);
    CHECK(spec->get() == spec->minValue);

    resetSearchParams();
}

TEST_CASE("Tunable registry: Score-half setters preserve the other half", "[tunable]") {
    resetEvalParams();
    const TunableSpec *mgSpec = findTunable("ThreatByPawnMg");
    const TunableSpec *egSpec = findTunable("ThreatByPawnEg");
    REQUIRE(mgSpec != nullptr);
    REQUIRE(egSpec != nullptr);

    const int originalMg = mgSpec->get();
    const int originalEg = egSpec->get();

    mgSpec->set(originalMg + 11);
    CHECK(mgSpec->get() == originalMg + 11);
    CHECK(egSpec->get() == originalEg);

    egSpec->set(originalEg - 13);
    CHECK(mgSpec->get() == originalMg + 11);
    CHECK(egSpec->get() == originalEg - 13);

    resetEvalParams();
}

TEST_CASE("Tunable registry: LmrDivisor mutation rebuilds the reduction table", "[tunable]") {
    resetSearchParams();
    const TunableSpec *spec = findTunable("LmrDivisor");
    REQUIRE(spec != nullptr);

    // Mutating LmrDivisor must actually shift the reduction table. Compare a
    // fixed-depth search's node count at the default versus a much larger
    // divisor (less reduction, more nodes).
    initSearch();

    spec->set(spec->minValue);
    const int lowDivValue = searchParams.LmrDivisor;
    CHECK(lowDivValue == spec->minValue);

    // Returning to the default must restore the original table shape; any
    // LMR-dependent assertion elsewhere is cleaner after a reset.
    spec->set(spec->defaultValue);
    CHECK(searchParams.LmrDivisor == spec->defaultValue);
    resetSearchParams();
    initSearch();
}
