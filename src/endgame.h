#ifndef ENDGAME_H
#define ENDGAME_H

#include "board.h"
#include "types.h"
#include <cstdint>

// Specialized endgame evaluation module. Material-key dispatch routes
// positions with recognized material configurations to dedicated
// evaluators that supersede the generic HCE gradient. Value evaluators
// return an absolute white-perspective score that overrides the main
// eval; scale evaluators return a ScaleResult carrying a 0..64
// multiplier on the endgame half plus an optional additive applied to
// eg before the multiplier. Both flavors are registered for both color
// polarities at init time so dispatch is symmetric.
namespace Endgame {

using ValueFn = int (*)(const Board &, Color strongSide);

// Result of a scale evaluator. `scale` is the 0..64 multiplier applied
// to the endgame half before the phase blend; values below 64 pull a
// drawish endgame toward zero, 64 leaves it untouched. `egAdjust` is an
// additive white-perspective bonus folded into eg before the multiplier
// so a winning pattern can still steer the search even when the rest of
// the eval undershoots the gradient (for example, the bridge-building
// bonus on a winning K+R+P vs K+R).
struct ScaleResult {
    int scale;
    int egAdjust;
};

using ScaleFn = ScaleResult (*)(const Board &, Color strongSide);

struct ValueEntry {
    ValueFn fn;
    Color strongSide;
};

struct ScaleEntry {
    ScaleFn fn;
    Color strongSide;
};

// One-time registry population. Called from ensureEvalInit().
void init();

// Returns a pointer to the registered value evaluator for this material
// configuration, or nullptr if none is registered. The pointer remains
// valid for the lifetime of the process; the registry is read-only after
// init() returns.
const ValueEntry *probeValue(uint64_t materialKey);

// Returns a pointer to the registered scale evaluator for this material
// configuration, or nullptr if none is registered.
const ScaleEntry *probeScale(uint64_t materialKey);

} // namespace Endgame

#endif
