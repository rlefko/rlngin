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

    // Tarrasch's rule: rooks belong behind passed pawns. Credit a small
    // bonus when a friendly rook shares a file with a passed pawn and
    // sits behind it relative to the pawn's advancing direction. The
    // "our" case rewards escorting a friendly passer toward promotion;
    // the "their" case rewards chasing an enemy passer from behind,
    // which is the same idea applied to the defender.
    Score RookBehindOurPasserBonus;
    Score RookBehindTheirPasserBonus;

    // Stockfish-lineage "minor behind pawn": a knight or bishop sitting
    // one rank behind a friendly pawn is shielded from direct frontal
    // attack and hard for an enemy pawn on the same file to challenge.
    // The bonus fires per minor-pawn pair so a well-developed kingside
    // of N f3 + P g4 + B g2 (or similar) gets credited cleanly.
    Score MinorBehindPawnBonus;

    // Piece pair synergy.
    Score BishopPair;

    // King-safety scalar tables (structural divisors for the king-danger
    // quadratic remain static const inside eval.cpp).
    Score PawnShieldBonus[2];
    Score PawnStormPenalty[5];
    Score SemiOpenFileNearKing;
    Score OpenFileNearKing;
    Score UndefendedKingZoneSq;
    Score KingSafeSqPenalty[9];

    // Per-attacker weights contributed to the king-danger accumulator for
    // every enemy piece of the given type whose attack set intersects our
    // king zone.
    Score KingAttackByKnight;
    Score KingAttackByBishop;
    Score KingAttackByRook;
    Score KingAttackByQueen;

    // King-danger weight per safe check an enemy piece of the given type
    // can deliver from a square we do not defend. Indexed by victim piece
    // type so the Pawn/King/None slots stay zero and the inner loop can
    // read the table directly without a remap.
    Score KingSafeCheck[7];

    // Per-weak-ring-square weight folded into the king-danger accumulator.
    // Orthogonal to the existing UndefendedKingZoneSq linear term.
    Score KingRingWeakWeight;

    // Flat discount subtracted from the king-danger accumulator when the
    // attacking side has no queen on the board.
    Score KingNoQueenDiscount;

    // Pawn-structure penalties.
    Score IsolatedPawnPenalty;
    Score DoubledPawnPenalty;
    Score BackwardPawnPenalty;

    // Extra penalty on top of IsolatedPawnPenalty or BackwardPawnPenalty
    // when the pawn is "unopposed", meaning no enemy pawn sits on the
    // same file ahead of it. An open file behind a weak pawn makes it an
    // easy target for a rook lift, so unopposed weakness is strictly
    // worse than opposed weakness.
    Score WeakUnopposedPenalty;

    // Extra penalty when a pawn is both doubled and isolated. The joint
    // case is strictly worse than either alone: the doubled pair cannot
    // be defended by a friendly pawn from any file, and losing the lead
    // pawn leaves the trailing pawn equally defenseless.
    Score DoubledIsolatedPenalty;

    // Penalty for a non-passer pawn whose stop square is occupied by an
    // enemy piece (including an enemy pawn), indexed by relative rank
    // minus 5, so entry [0] is rank 5 and entry [1] is rank 6. Captures
    // the over-extended-but-stuck pattern that passers already get via
    // PassedBlockedPenalty.
    Score BlockedPawnPenalty[2];

    // Penalty per extra pawn island beyond the first. A side's pawns
    // split into islands at every empty file: one contiguous group is
    // ideal, and each additional island loses the ability to use the
    // in-between file as a support highway while doubling the number
    // of chain endpoints the opponent can attack.
    Score PawnIslandPenalty;

    // Extra bonus layered on top of ConnectedPawnBonus when the connected
    // pawn sits in a phalanx (same rank, adjacent file) rather than only
    // being defended from behind. Phalanx pawns can advance in lockstep,
    // which is strictly more dynamic than a merely supported pawn.
    // Disabled until a joint Texel re-tune confirms it beats the already-
    // tuned ConnectedPawnBonus, which currently absorbs both phalanx and
    // supported cases with a single rank-indexed value. Re-enable the
    // field, the default, the use in evaluatePawns, and the tuner entry
    // together.
    // Score PhalanxBonus;

    // Classical central pawn occupancy. [0] rewards own pawns on the
    // primary center squares (d4/e4 for White, d5/e5 for Black); [1]
    // rewards pawns on the extended center (c4/f4 and mirror). Kept
    // middlegame-only because the endgame has no structural opinion on
    // which file an extra pawn sits on. The PST carries a related signal
    // but landed after the strict tune with nearly equal MG weights for
    // an e2 pawn and an e4 pawn, so an explicit term is needed to bias
    // classical center occupation at low search depths.
    Score CentralPawnBonus[2];

    // Bonus for a bishop whose long diagonal (a1-h8 or a8-h1) sweeps
    // both central squares on that diagonal unobstructed. Applied at
    // most once per bishop.
    Score BishopLongDiagonalBonus;
};

extern EvalParams evalParams;

// Reset evalParams to the compiled-in defaults.
void resetEvalParams();

#endif
