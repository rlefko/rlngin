#ifndef EVAL_PARAMS_H
#define EVAL_PARAMS_H

#include "types.h"

// Tunable evaluation parameters. The struct is mutated at runtime by the
// Texel tuner so every scalar the static evaluator reads can be adjusted
// without rebuilding the engine. Non-tuned constants (phase increments,
// quadratic imbalance tables, king-attack-units / curve, space gating)
// remain file-scope `static const` inside eval.cpp.
struct EvalParams {
    // --- Existing first-pass tuned terms (layout preserved from prior
    // commits so the aggregate initializer does not reshuffle). ---
    Score ThreatByPawn;
    Score ThreatByMinor[7]; // indexed by victim piece type
    Score ThreatByRook[7];  // indexed by victim piece type
    Score ThreatByKing;
    Score Hanging;
    Score WeakQueen;
    Score SafePawnPush;
    Score PassedKingProxBonus[8];
    Score PassedEnemyKingProxPenalty[8];
    Score PassedBlockedPenalty[8];
    Score PassedSupportedBonus[8];
    Score ConnectedPassersBonus[8];
    Score RookOn7thBonus;
    Score BadBishopPenalty;
    Score Tempo;

    // --- Newly exposed for the broad-scope Texel tune. ---

    // Packed material values by piece type. Index 0 (None) and 6 (King)
    // carry zero because material for those slots is implicit.
    Score PieceScore[7];

    // Piece-square tables stored in a1=0 order (rank 1 first, rank 8 last).
    // Values are from White's perspective; Black mirrors via `sq ^ 56`.
    Score PawnPST[64];
    Score KnightPST[64];
    Score BishopPST[64];
    Score RookPST[64];
    Score QueenPST[64];
    Score KingPST[64];

    // Mobility bonus by piece type and count of mobility-area squares
    // attacked. Pawn and King rows remain zero.
    Score MobilityBonus[7][28];

    // Passed and connected pawn bonus by relative rank.
    Score PassedPawnBonus[8];
    Score ConnectedPawnBonus[8];

    // Rook file bonuses.
    Score RookOpenFileBonus;
    Score RookSemiOpenFileBonus;

    // Minor-piece outposts and trapped-rook-by-own-king.
    Score KnightOutpostBonus;
    Score BishopOutpostBonus;
    Score TrappedRookByKingPenalty;

    // Piece pair synergy.
    Score BishopPair;

    // King-safety scalar tables (non-tuned attack-unit curve remains
    // static const inside eval.cpp).
    Score PawnShieldBonus[2];
    Score PawnStormPenalty[5];
    Score SemiOpenFileNearKing;
    Score OpenFileNearKing;
    Score UndefendedKingZoneSq;
    Score KingSafeSqPenalty[9];

    // Pawn-structure penalties.
    Score IsolatedPawnPenalty;
    Score DoubledPawnPenalty;
    Score BackwardPawnPenalty;
};

extern EvalParams evalParams;

// Reset evalParams to the compiled-in defaults.
void resetEvalParams();

#endif
