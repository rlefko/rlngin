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
    // Bad bishop split into a flat baseline that fires when at least
    // one own pawn sits on the bishop's color, and a per-pawn weight
    // scaled by the number of own d/e file pawns whose stop square is
    // occupied. Closed center positions are far worse for the bad
    // bishop than the same pawn count on an open board, so the per
    // pawn term needs to scale with the closure of the position.
    Score BadBishop;
    Score BishopPawns;
    Score Tempo;

    // --- Newly exposed for the broad-scope Texel tune. ---

    // Packed material values by piece type. Index 0 (None) and 6 (King)
    // carry zero because material for those slots is implicit.
    Score PieceScore[7];

    // Piece-square tables. PawnPST keeps the full 64-square layout
    // because pawn structure is asymmetric (e.g., the f and h pawns
    // play very differently). Non-pawn PSTs are stored half-board
    // with the file mirrored to the queenside (4 file buckets x 8
    // ranks = 32 entries). Black mirrors via the rank flip plus the
    // file fold, so a knight on c3 and a knight on f3 share the same
    // tunable. PawnPST values are White-perspective; Black mirrors
    // via `sq ^ 56`. Non-pawn PSTs index by `(rel_rank << 2) | min(file, 7 - file)`.
    Score PawnPST[64];
    Score KnightPST[32];
    Score BishopPST[32];
    Score RookPST[32];
    Score QueenPST[32];
    Score KingPST[32];

    // Mobility bonus by piece type and count of mobility-area squares
    // attacked. Pawn and King rows remain zero.
    Score MobilityBonus[7][28];

    // Passed and connected pawn bonus by relative rank.
    Score PassedPawnBonus[8];
    Score ConnectedPawnBonus[8];

    // Rook file bonuses.
    Score RookOpenFileBonus;
    Score RookSemiOpenFileBonus;
    // Bonus per friendly rook sharing a file with an enemy queen.
    // Pure pressure pattern that is otherwise only credited via the
    // generic mobility term.
    Score RookOnQueenFile;

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

    // Piece pressure on the enemy king ring, scored per piece whose
    // pseudo-attack set intersects the enemy king zone. Lives outside
    // the multi-attacker king-danger accumulator so single-attacker
    // pressure (a lone outposted knight on f5, say) still gets credit.
    Score MinorOnKingRing;
    Score RookOnKingRing;

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
    //
    // Classical shelter and storm grids indexed by [edge_distance][rank]
    // where edge_distance is min(file, 7 - file) of the shield file
    // (0 = a/h files, 3 = d/e files) and rank is the relative rank of
    // the most advanced pawn on that file from our side's perspective.
    // Rank 0 stands for "no pawn on file": the Shelter[d][0] entry is
    // the semi-open file penalty, the storm entries on rank 0 are
    // structurally zero because there is no enemy pawn to advance.
    Score Shelter[4][7];
    Score UnblockedStorm[4][7];
    // Blocked storm: enemy pawn frontally blocked by our pawn one rank
    // ahead. The threat is greatly reduced because the rammer cannot
    // open the file without a trade, so the table is one dimensional
    // in pawn rank only (the file distance signal is dominated by the
    // blocker geometry).
    Score BlockedStorm[7];
    Score UndefendedKingZoneSq;
    // Linear king-mobility factor folded into the king-danger
    // accumulator. Each square the king can step to that is not
    // attacked by the enemy reduces the danger score by this much.
    // Replaces the saturating 9-bucket KingSafeSqPenalty curve: the
    // classical signal is a count, not a discrete bucket table.
    Score KingMobilityFactor;

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

    // Penalty per enemy pawn the bishop x-rays through its own pieces.
    // The bishop's diagonal is mobility-dead in the direction of every
    // such pawn even when the immediate squares are open, and our
    // pieces between the bishop and the pawn are not blockers in the
    // x-ray sense.
    Score BishopXrayPawns;

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
    //   Tension     -- enemy pawns under our pawn attacks plus mirror; only
    //                  the MG half is consumed because the conventional
    //                  initiative system has no EG analogue for tension.
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
    Score InitiativeTension;
    Score InitiativeInfiltrate;
    Score InitiativePureBase;
    Score InitiativeConstant;

    // Bonus per friendly slider that "x-rays" the enemy queen through
    // exactly one intermediate blocker: our bishop or rook shares a ray
    // with the queen and there is one piece (of either color) between
    // them. Direct attackers are already credited by the threat-by-minor
    // and threat-by-rook terms plus the weak-queen flag, so this term
    // fires only on the indirect-pressure case. Both bishop and rook
    // variants are scored per counted slider.
    Score SliderOnQueenBishop;
    Score SliderOnQueenRook;

    // Bonus per square that both sides' non-pawn pieces attack, minus
    // squares the opponent's pawns defend. A square we attack that the
    // opponent's knight, bishop, rook, or queen also attacks is a square
    // the opponent cannot comfortably use for retreat or rotation: every
    // such square restricts an opposing piece's freedom of movement.
    // Pawn-defended squares are excluded because our attacker is already
    // outvalued there and the recapture is immediate.
    Score RestrictedPiece;

    // Per-square pawn-push threat. For every legal single- or double-push
    // target, count enemy non-pawn / non-king pieces that the pawn would
    // attack from that landing square, including pushes that are not safe
    // today. Distinct from SafePawnPush: the safe variant only fires when
    // the push itself is supported, while this term credits the latent
    // double-threat shape (push the pawn or move the piece) regardless of
    // whether the push lands cleanly. Pieces already attacked by our pawns
    // in place are excluded so the bonus does not double count with
    // ThreatByPawn.
    Score ThreatByPawnPush;

    // Penalty per friendly minor or rook that is under enemy attack
    // and whose only friendly defender is the queen. Such pieces are
    // vulnerable to overload and discovery patterns because the queen
    // has to spend a tempo to recapture, which is strictly worse than
    // a piece defended by a less-valuable defender. Pawns and the
    // queen herself are excluded so the term only fires on the minor
    // / rook material patterns where the discovery loss is
    // significant. Applied as a negative score so the magnitude lives
    // in the table itself.
    Score WeakQueenDefender;

    // Bonus per friendly knight that has at least two safe candidate
    // squares from which it attacks the enemy queen. Even a single
    // such square is a real threat in branch-on-recapture, but two or
    // more candidates make the fork unrecoverable for the defender:
    // they cannot guard every landing square at once and the knight
    // wins the race for material on the next move.
    Score KnightOnQueen;

    // Penalty when our king sits on a flank that has no pawns of
    // either color. The four-file flank (a..d or e..h, by king file)
    // captures the surface area an enemy king-side or queen-side
    // attack would sweep through; with no pawns there the king has
    // no shelter and no pawn-attack web to slow incoming pieces.
    // The shelter / storm grids do not capture this case because
    // they only walk the three shield files immediately around the
    // king and silently zero out when every shield file is empty.
    Score PawnlessFlank;

    // Bonus per friendly queen sitting on the enemy half of the board
    // and not attacked by an enemy pawn or minor piece. Queens parked
    // safely in the opponent's territory dominate the position because
    // they cannot be cheaply evicted, so they sustain pressure across
    // multiple files and ranks at no defender cost.
    Score QueenInfiltration;

    // Endgame penalty per square of Chebyshev distance from our king
    // to our nearest pawn. King-and-pawn endings reward the king that
    // walks toward its pawns to support them; the existing
    // PassedKingProxBonus only fires for passers, so this term
    // captures the same signal for non-passer pawns. The mg half is
    // structurally zero; only the eg half carries a tunable value.
    Score KingPawnDistEg;

    // KBNK corner-push gradient: in K + B + N vs K endings, the eg
    // adjustment rewards driving the weak king toward the corner whose
    // colour matches the strong bishop. The bonus fires per square of
    // Chebyshev closeness (0..7) so the weak king sees a continuous
    // pull toward the right corner across the search horizon. Mg half
    // is structurally zero (KBNK only matters in the eg phase); only
    // the eg half is tunable.
    Score KBNKCornerEg;

    // Lucena bridge-building win: in K + R + P vs K + R with the pawn
    // on rank 6 (attacker's POV), the strong king on rank 7 or 8 in
    // front of the pawn, and the defender king cut off at least two
    // files away from the pawn file, the position is a textbook win
    // by the bridge-building rook technique. The default scale stays
    // 64 (the material edge already says winning), but a flat eg
    // bonus drives the search toward this configuration earlier than
    // the raw material gradient alone would. Mg structurally zero;
    // only the eg half is tunable.
    Score LucenaEg;

    // KXK mating-conversion gradients: in K + mating-material vs lone
    // K endings (KQK, KRK, KQQK, KQRK, KRRK, KQBK, KQNK, KRBK, KRNK),
    // the strong side needs to drive the lone king toward an edge and
    // bring its own king close enough to deliver mate inside the
    // 50-move horizon. The natural eval has no such gradient; without
    // these two terms the search can wander indefinitely with a clearly
    // winning material advantage. Mg structurally zero (these only
    // matter when the eg phase weight dominates); only the eg half is
    // tunable. Per-square weighting: pushToEdge returns 0..7, pushClose
    // returns 0..7, so each term contributes up to 7 * weight to eg.
    Score KXKPushToEdge;
    Score KXKPushClose;

    // KBNK kings-together gradient: paired with KBNKCornerEg, this term
    // pulls the strong king toward the weak king so the bishop and
    // knight can actually deliver mate after driving the lone king to
    // the colored corner. Pre-bitbase only had the colored-corner term;
    // adding kings-together accelerates conversion. Mg zero, eg tunable.
    Score KBNKPushClose;

    // KQKR mating-conversion gradients: queen versus rook is a known
    // win but technical. Push-to-edge drives the rook-side king toward
    // the edge; push-close brings the queen-side king in support.
    // Without these, the search rarely converts KQKR inside the
    // 50-move horizon. Mg zero, eg tunable.
    Score KQKRPushToEdge;
    Score KQKRPushClose;

    // KPsK rook-file fortress scale: K + pawns vs lone K with every
    // pawn on a single rook file. The textbook draw fires when the
    // defender king reaches the promotion corner before the most-
    // advanced pawn queens (defender-to-move gains one extra tempo).
    // Default is zero (full fortress draw); the tuner can widen the
    // recognized drawishness if it pays off. Mg zero, eg in 0..32.
    Score KPsKFortressScale;

    // KBPKN drawishness: K + B + P vs K + N. The bishop side usually
    // wins, but if the defender king blockades the pawn's push square
    // and the knight stays within two squares of the king to support,
    // the strong side has only a residual edge. The tuner picks the
    // damping magnitude. Mg zero, eg in 0..32.
    Score KBPKNDrawishScale;

    // KRKP drawishness: K + R vs K + P. Rook usually wins, but if the
    // pawn is on relative rank 5 or higher (rank 6 or 7 from the
    // defender's perspective), the defender king is adjacent to the
    // pawn, and the rook-side king is more than three squares from
    // the promotion square, the race tilts toward a draw. Mg zero,
    // eg in 16..48.
    Score KRKPDrawishScale;

    // KRKB / KRKN drawishness: K + R vs K + minor without pawns. The
    // rook holds the material edge but the lone minor with active king
    // play often draws. The scale is applied uniformly to every KRKB
    // and KRKN position. Mg zero, eg in 16..48.
    Score KRKMinorScale;

    // KNNK draw: two knights versus a lone king cannot force mate
    // against best defense. Default is zero (drawn); the tuner can
    // widen if it finds the natural material edge pays off. Mg zero,
    // eg in 0..32.
    Score KNNKDrawScale;

    // SEE-aware threat discount: scales the credit for a threat
    // whose target has a quiet escape from our lower-value attackers.
    // Each affected threat term in evaluateThreats splits its victims
    // into "stuck" (no safe quiet escape) and "escapable" sets; stuck
    // victims earn the full threat magnitude, escapable victims earn
    // (full * EscapableThreatScale.eg / 64). Without this, a queen
    // attacked by a knight prices as a permanent loss even when the
    // queen has obvious retreats, and the engine prefers gambits that
    // are capture-resolved at the horizon over principled recaptures
    // that leave the queen attacked. Mg structurally zero; the eg
    // half holds the integer scale in 0..64.
    Score EscapableThreatScale;

    // Sacrificed-material compensation cap: when one side is down
    // material and the other side is up positionally, scale the
    // positionally-up side's compensation by (1 - matDeficit / cap)
    // so a linear-feature eval cannot credit "one developed knight"
    // as full compensation for a pawn. Without this, game-result
    // Texel converges to PSTs that make one developed piece worth
    // ~0.8 pawn-equivalents and the engine starts gambitting for
    // development in openings where the gambit is dubious (the
    // Scandinavian Blackburne-Kloosterboer 2...c6 was the
    // motivating case). Acts on mg only - the eg phase is dominated
    // by material fundamentals and does not need the cap. Cap is
    // in internal mg units (228 = one pawn); the eg slot stays
    // zero and is not consumed.
    Score CompensationCap;
};

extern EvalParams evalParams;

// Reset evalParams to the compiled-in defaults.
void resetEvalParams();

#endif
