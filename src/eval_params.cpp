#include "eval_params.h"

// Current compiled-in defaults for the tunable parameters. The actual
// in-memory `evalParams` instance is initialized from these values, and
// `resetEvalParams()` snaps it back to them after a tune run if needed.

static const EvalParams kDefaultEvalParams = {
    // Threats
    S(65, 100), // ThreatByPawn
    {
        // ThreatByMinor (indexed by victim type)
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(33, 44),
        S(70, 88),
        S(0, 0),
    },
    {
        // ThreatByRook (indexed by victim type)
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(70, 66),
        S(0, 0),
    },
    S(44, 48), // ThreatByKing
    S(36, 22), // Hanging
    S(32, 11), // WeakQueen
    S(26, 18), // SafePawnPush

    // Passed pawn refinements (by relative rank)
    {S(0, 0), S(0, 0), S(0, 0), S(0, 3), S(0, 5), S(0, 9), S(0, 14), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 4), S(0, 8), S(0, 13), S(0, 21), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-5, -11), S(-11, -24), S(-22, -48), S(-47, -97), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(8, 16), S(16, 32), S(36, 70), S(72, 140), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(5, 8), S(11, 22), S(24, 48), S(48, 97), S(0, 0)},

    // Other new terms
    S(29, 55), // RookOn7thBonus
    S(-5, -7), // BadBishopPenalty
    S(28, 0),  // Tempo
};

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
