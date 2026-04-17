#ifndef TUNABLE_H
#define TUNABLE_H

#include <functional>
#include <string>
#include <vector>

// A tunable scalar exposed to both UCI setoption and the SPSA driver.
// The get/set lambdas know how to route reads and writes through the
// underlying storage, so eval Score halves, scaled-integer LMR coefficients,
// and plain SearchParams ints all share one registry. Setters are expected
// to clamp incoming values into `[minValue, maxValue]` so UCI clients and
// SPSA updates see the same invariants.
struct TunableSpec {
    std::string name;
    std::function<int()> get;
    std::function<void(int)> set;
    int minValue;
    int maxValue;
    int defaultValue;
    // SPSA hyperparameters. cEnd is the target perturbation magnitude at
    // the final iteration; rEnd is the target per-iteration step size at
    // the final iteration. The driver derives the Spall gain sequences
    // (a, c, A, alpha, gamma) from these together with the iteration count.
    double cEnd;
    double rEnd;
};

// All registered tunables. Built lazily on first call so it sees fully
// initialized globals regardless of static initialization order.
const std::vector<TunableSpec> &tunables();

// Lookup by exact UCI option name. Returns nullptr if unknown.
const TunableSpec *findTunable(const std::string &name);

#endif
