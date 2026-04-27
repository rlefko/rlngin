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

    // Bonus when a knight could reach an outpost square in one move.
    // Smaller than the on-outpost bonus because the move still has to
    // be played and the destination square has to remain unchallenged.
    Score ReachableOutpost;

    // Penalty for a knight occupying a "bad" outpost: a flank outpost
    // (a-file / h-file / b-file / g-file) where the enemy has limited
    // exploitable counterplay. The outpost still earns the regular
    // bonus elsewhere; this layer fires when the wing-outpost shape
    // shows there is nothing concrete to attack.
    Score BadOutpost;

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

    // Piece pressure on the enemy king ring, scored per piece whose
    // pseudo-attack set intersects the enemy king zone. Lives outside
    // the multi-attacker king-danger accumulator so single-attacker
    // pressure (a lone outposted knight on f5, say) still gets credit.
    Score MinorOnKingRing;
    Score RookOnKingRing;

    // Bishop x-rays through enemy pawns: per enemy pawn that sits on
    // the bishop's empty-board diagonal. A bishop with multiple enemy
    // pawns on its long diagonal pressures those pawns even through
    // intermediate pieces.
    Score BishopXRayPawns;

    // Bishop x-rays into the enemy king ring through pawns: a bishop
    // that does not directly attack the ring but whose empty-board
    // diagonals would intersect it once the obstruction clears earns a
    // smaller bonus than direct attack.
    Score BishopOnKingRingXRay;

    // Rook on a file shared with any queen of either side.
    Score RookOnQueenFile;

    // Penalty when our queen sits on rank 5 or higher (relative to us)
    // on a square outside the enemy pawn's attack span. The queen is
    // exposed to gain-of-tempo harassment.
    Score QueenInfiltration;

    // Penalty per square of Chebyshev distance from our own king to each
    // of our knights and bishops. Linear in distance because distances to
    // the king are bounded at 7 and nearly all meaningful credit lands in
    // the 2 to 4 range where a table adds no resolution over a slope.
    // Signed negative so a minor drifting away from the defense costs a
    // small amount of eval per step.
    Score KingProtector;

    // Piece pair synergy.
    Score BishopPair;

    // King-safety scalar tables (structural divisors for the king-danger
    // quadratic remain static const inside eval.cpp).
    Score PawnShieldBonus[2];
    // Pawn storm penalty indexed by distance bucket (0..4 where bucket 4
    // is closest). Split into blocked and unblocked variants because a
    // storm pawn frontally blocked by a friendly shield pawn cannot open
    // lines without a trade, so its effective penalty is much smaller
    // than an unblocked ram on the same file.
    Score BlockedPawnStorm[5];
    Score UnblockedPawnStorm[5];
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
    // can deliver from a square we do not defend. The trailing dimension
    // selects between the "single safe check" magnitude (index 0) and the
    // "two or more safe checks" magnitude (index 1): a piece able to give
    // check from multiple squares is qualitatively more dangerous than a
    // piece able to give check from one square, and the multi entry
    // captures that. Pawn/King/None slots stay zero on both halves.
    Score KingSafeCheck[7][2];

    // Per-weak-ring-square weight folded into the king-danger accumulator.
    // Orthogonal to the existing UndefendedKingZoneSq linear term.
    Score KingRingWeakWeight;

    // Per square the enemy reaches that would give check but is not
    // "safe" by the strict KingSafeCheck definition. Captures the milder
    // pressure of a check that would lose material on the recapture.
    Score KingUnsafeCheckWeight;

    // Per square directly adjacent to the king the enemy attacks (with
    // multiplicity: a square hit by two enemy pieces counts twice). The
    // existing UndefendedKingZoneSq term measures area, this measures
    // attack pressure.
    Score KingAttacksWeight;

    // Per friendly piece pinned to the king (the "blockers for king"
    // count). A pinned piece cannot escape the line, which makes the
    // king attack stronger.
    Score KingBlockerWeight;

    // Discount subtracted from the king-danger accumulator when our
    // knight defends a square our own king also attacks. A close-in
    // knight is the strongest defender against rook and queen sacrifices.
    Score KingKnightDefenderDiscount;

    // Negative penalty added to the position score when our king has no
    // friendly pawn on the same flank. A king without flank pawns is
    // structurally exposed regardless of attack-count.
    Score PawnlessFlank;

    // King flank attack: per-square weight folded into the king-danger
    // accumulator for every square on the 3-file band centred on our
    // king and on our half of the board (relative ranks 0-3) that the
    // enemy attacks. KingFlankAttack credits squares attacked at least
    // once; KingFlankAttack2 adds an extra weight for squares the enemy
    // attacks twice or more. KingFlankDefense subtracts a small weight
    // per flank square our own pieces also defend.
    Score KingFlankAttack;
    Score KingFlankAttack2;
    Score KingFlankDefense;

    // Constant term added to the king-danger accumulator. Combined with
    // the multi-attacker gate this gives the quadratic a non-zero
    // baseline so the curve is anchored above zero from the first
    // attacker.
    Score KingDangerConstant;

    // Flat discount subtracted from the king-danger accumulator when the
    // attacking side has no queen on the board.
    Score KingNoQueenDiscount;

    // Pawn-structure penalties.
    Score IsolatedPawnPenalty;
    Score DoubledPawnPenalty;
    Score BackwardPawnPenalty;

    // WeakLever: an unsupported pawn that is attacked diagonally by two
    // or more enemy pawns. The pawn cannot be defended by another pawn
    // and is sure to be lost in subsequent exchanges.
    Score WeakLever;

    // Extra penalty on top of IsolatedPawnPenalty or BackwardPawnPenalty
    // when the pawn is "unopposed", meaning no enemy pawn sits on the
    // same file ahead of it. An open file behind a weak pawn makes it an
    // easy target for a rook lift, so unopposed weakness is strictly
    // worse than opposed weakness.
    Score WeakUnopposedPenalty;

    // Penalty for a non-passer pawn whose stop square is occupied by an
    // enemy piece (including an enemy pawn), indexed by relative rank
    // minus 5, so entry [0] is rank 5 and entry [1] is rank 6. Captures
    // the over-extended-but-stuck pattern that passers already get via
    // PassedBlockedPenalty.
    Score BlockedPawnPenalty[2];

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

    // Bonus for a bishop whose long diagonal (a1-h8 or a8-h1) sweeps
    // both central squares on that diagonal unobstructed. Applied at
    // most once per bishop.
    Score BishopLongDiagonalBonus;

    // Position-wide "initiative" scalar. Features are accumulated into a
    // non-negative magnitude, then signed by the side with the current
    // positional advantage, and folded into the total so the score bends
    // in favor of the better-placed side. The term is clamped to never
    // flip the sign of an already small endgame score.
    //
    // Index meanings:
    //   Passer      -- per passed pawn on either side
    //   PawnCount   -- per pawn on the board (tiny weight, grows with pawns)
    //   Outflank    -- popcount(kingside pawns) * popcount(queenside pawns)
    //   Infiltrate  -- per king that has crossed into enemy territory
    //   PureBase    -- flat when no non-pawn non-king material remains
    //   Constant    -- baseline shift (typically negative)
    //
    // All seven carry MG=0 so the term is primarily an endgame effect,
    // applied at half strength to MG inside evaluateInitiative so sharp
    // middlegame positions still get a nudge.
    Score InitiativePasser;
    Score InitiativePawnCount;
    Score InitiativeOutflank;
    Score InitiativeInfiltrate;
    Score InitiativePureBase;
    Score InitiativeConstant;

    // Bonus when pawns sit on both kingside and queenside flanks. The
    // signal that the position has play on both wings is a complexity
    // multiplier the outflank product alone underweights.
    Score InitiativeBothFlanks;

    // Negative input fired when outflank is negative (sides on opposite
    // wings) AND pawns are not on both flanks. This is the
    // "almost-unwinnable" structural signature.
    Score InitiativeAlmostUnwinnable;

    // Per-step penalty in the eg-only "king to nearest friendly pawn"
    // distance. A king far from its own pawns is structurally exposed
    // and slow to respond to threats.
    Score KingPawnDistance;

    // Per-passer file edge-distance penalty: passers on central files
    // are harder to escort than passers on rook files where the
    // promotion square is in a corner. Negative-signed.
    Score PassedFile;

    // Bonus per friendly slider that "x-rays" the enemy queen through
    // exactly one intermediate blocker: our bishop or rook shares a ray
    // with the queen and there is one piece (of either color) between
    // them. Direct attackers are already credited by the threat-by-minor
    // and threat-by-rook terms plus the weak-queen flag, so this term
    // fires only on the indirect-pressure case. Both bishop and rook
    // variants are scored per counted slider.
    Score SliderOnQueenBishop;
    Score SliderOnQueenRook;

    // Bonus per weak (undefended or non-pawn double-attacked) enemy
    // piece whose only defender is the enemy queen. Tying the queen to
    // a defensive task is itself a positional gain.
    Score WeakQueenProtection;

    // Bonus per safe square our knight could move to that attacks the
    // enemy queen. Counts only safe destinations for the knight, so
    // walk-in tactics don't false-fire.
    Score KnightOnQueen;

    // Bonus per square that both sides' non-pawn pieces attack, minus
    // squares the opponent's pawns defend. A square we attack that the
    // opponent's knight, bishop, rook, or queen also attacks is a square
    // the opponent cannot comfortably use for retreat or rotation: every
    // such square restricts an opposing piece's freedom of movement.
    // Pawn-defended squares are excluded because our attacker is already
    // outvalued there and the recapture is immediate.
    Score RestrictedPiece;
};

extern EvalParams evalParams;

// Reset evalParams to the compiled-in defaults.
void resetEvalParams();

#endif
