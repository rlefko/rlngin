#include "eval_params.h"

// Current compiled-in defaults for the tunable parameters. The actual
// in-memory `evalParams` instance is initialized from these values, and
// `resetEvalParams()` snaps it back to them after a tune run if needed.
//
// Values below are the Texel tuner's output from a 2000-game self-play
// corpus (~197k quiet positions). Loss improved from 0.0993 to 0.0975.

static const EvalParams kDefaultEvalParams = {
    // Threats
    S(204, 245), // ThreatByPawn
    {
        // ThreatByMinor (indexed by victim type)
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(150, 152), // Rook victim
        S(145, 95),  // Queen victim
        S(0, 0),
    },
    {
        // ThreatByRook (indexed by victim type)
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(0, 0),
        S(107, 68), // Queen victim
        S(0, 0),
    },
    S(78, 108),  // ThreatByKing
    S(158, 148), // Hanging
    S(33, 11),   // WeakQueen
    S(107, 37),  // SafePawnPush

    // Passed pawn refinements (by relative rank). King-proximity tables
    // are multiplied by a closeness factor of up to 7, so the tuner's
    // raw output was capped to keep the effective per-passer swing in a
    // plausible range.
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 14), S(0, 20), S(0, 14), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(0, 12), S(0, 16), S(0, 24), S(0, 28), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(-6, -60), S(0, -27), S(-44, -102), S(-102, -203), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(9, 57), S(45, 102), S(36, 136), S(72, 235), S(0, 0)},
    {S(0, 0), S(0, 0), S(0, 0), S(5, 9), S(10, 8), S(24, 48), S(48, 74), S(0, 0)},

    // Other new terms
    S(-12, 69), // RookOn7thBonus
    S(-24, 2),  // BadBishopPenalty
    S(96, 0),   // Tempo
};

EvalParams evalParams = kDefaultEvalParams;

void resetEvalParams() {
    evalParams = kDefaultEvalParams;
}
