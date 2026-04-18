#include "eval_params.h"

// Current compiled-in defaults for the tunable parameters. The actual
// in-memory `evalParams` instance is initialized from these values, and
// `resetEvalParams()` snaps it back to them after a tune run if needed.
//
// Values below are from a second Texel pass on a qsearch-filtered corpus
// (698k positions from 7000 self-play games at 25k nodes). Loss improved
// from 0.0905 to 0.0897 across 10 coordinate-descent passes. The qsearch
// target collapsed the tactical overfitting signal that inflated the
// first-pass magnitudes: threat, hanging, tempo, and rook-on-7th values
// stayed at their pre-tune defaults, and only the passer tables and a
// couple of scalars moved. Coordinate descent is bounded to +/-80 units
// of total drift per scalar so no one parameter can run away.

static const EvalParams kDefaultEvalParams = {
    // Threats
    S(65, 100), // ThreatByPawn
    {
        // ThreatByMinor (indexed by victim type)
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(33, 44), // Rook victim
        S(70, 88), // Queen victim
        S(0, 0),
    },
    {
        // ThreatByRook (indexed by victim type)
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(70, 66), // Queen victim
        S(0, 0),
    },
    S(44, 48), // ThreatByKing
    S(36, 22), // Hanging
    S(32, 11), // WeakQueen
    S(59, 18), // SafePawnPush

    // Passed pawn refinements (by relative rank). King-proximity tables
    // are multiplied by a closeness factor of up to 7, so values here
    // become per-step weights; the tuner was bounded so no single rank
    // can swing more than a pawn per closeness step on the final table.
    {S(0, 0), S(0, 0), S(0, 0), S(0, 33), S(0, 47), S(0, 59), S(0, 12), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 9), S(0, 33), S(0, 64), S(0, 96), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-5, -11), S(-11, -24), S(-22, -48), S(-47, -97), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(8, 16), S(16, 32), S(36, 70), S(72, 140), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(5, 8), S(11, 22), S(24, 48), S(48, 97), S(0, 0)},

    // Other new terms
    S(29, 55),   // RookOn7thBonus
    S(-16, -10), // BadBishopPenalty
    S(28, 0),    // Tempo
};

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
