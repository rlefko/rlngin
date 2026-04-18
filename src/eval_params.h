#ifndef EVAL_PARAMS_H
#define EVAL_PARAMS_H

#include "types.h"

// Tunable evaluation parameters for the newly added terms. Values live in
// a mutable struct so the Texel tuner can adjust them without rebuilding
// the engine. Non-tuned constants (PSTs, mobility, material, existing
// pawn structure) stay as static const inside eval.cpp.
struct EvalParams {
    // --- Threats ---
    Score ThreatByPawn;
    Score ThreatByMinor[7]; // indexed by victim piece type
    Score ThreatByRook[7];  // indexed by victim piece type
    Score ThreatByKing;
    Score Hanging;
    Score WeakQueen;
    Score SafePawnPush;

    // --- Passed pawn refinements ---
    Score PassedKingProxBonus[8]; // by relative rank of passer
    Score PassedEnemyKingProxPenalty[8];
    Score PassedBlockedPenalty[8];
    Score PassedSupportedBonus[8];
    Score ConnectedPassersBonus[8];

    // --- Other new terms ---
    Score RookOn7thBonus;
    Score BadBishopPenalty;
    Score Tempo;
};

extern EvalParams evalParams;

// Reset evalParams to the compiled-in defaults.
void resetEvalParams();

#endif
