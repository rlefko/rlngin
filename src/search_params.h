#ifndef SEARCH_PARAMS_H
#define SEARCH_PARAMS_H

// Tunable search pruning and reduction scalars. The values live in a
// mutable struct so UCI setoption and the SPSA tuner can adjust them
// without rebuilding the engine. Defaults match the previously hardcoded
// literals exactly, so the initial runtime behavior is bit-identical.
struct SearchParams {
    // Razoring: fold shallow losing nodes straight into qsearch once
    //   staticEval + RazorBase + RazorDepth * depth <= alpha
    int RazorBase;
    int RazorDepth;

    // Reverse futility pruning: fail high on shallow nodes when
    //   staticEval - (RfpBase - RfpImproving * improving) * depth >= beta
    int RfpBase;
    int RfpImproving;

    // Null-move pruning reduction: R = NmpBase + depth / 3 + clamp(
    //   (staticEval - beta) / NmpEvalDiv, 0, 3).
    int NmpBase;
    int NmpEvalDiv;

    // Futility pruning for quiet moves:
    //   staticEval + FpBase + FpDepth * depth <= alpha implies skip.
    int FpBase;
    int FpDepth;

    // SEE pruning thresholds applied as negative margins at the call site.
    // Captures: -SeeCaptureCoef * depth * depth. Quiets: -SeeQuietCoef * depth.
    int SeeCaptureCoef;
    int SeeQuietCoef;

    // Qsearch delta-pruning margin: a capture (or the node as a whole)
    // is pruned when standPat + maxPossibleGain + QsDeltaMargin <= alpha.
    // Captures the positional slack between a pure material swap and the
    // realistic best-case eval after the exchange resolves.
    int QsDeltaMargin;

    // Late move reductions table: scaled integers that are divided by 100
    // at table-fill time. Formula:
    //   lmrReductions[d][m] = int(LmrBase/100 + log(d) * log(m) / (LmrDivisor/100))
    int LmrBase;
    int LmrDivisor;

    // Correction-history weights. The corrected eval is:
    //   staticEval + sum(weight_i * table_i_entry) / CorrHistGrain
    // Non-pawn correction is weighted once per color term. CorrHistGrain is
    // the shared denominator; raising it shrinks every correction uniformly.
    int PawnCorrWeight;
    int NonPawnCorrWeight;
    int MinorCorrWeight;
    int ContCorrWeight;
    int CorrHistGrain;

    // Late-history pruning: once the MovePicker enters the Quiets phase at
    // a shallow non-PV node, skip any quiet whose butterfly + continuation
    // history score is below `-HistoryPruningCoef * depth`. Gating on the
    // picker phase keeps TT moves, good captures, killers, and the counter
    // move unaffected, while the depth-scaled threshold softens at depth.
    int HistoryPruningCoef;

    // Attack-aware quiet move ordering. Per-node enemy attack maps are
    // sampled by attacker class (pawn, minor, rook); when a quiet move
    // leaves a square attacked by a less-valuable enemy piece for a safe
    // square, `ThreatEscapeBonus` lifts its score above mid-history
    // quiets. The symmetric `ThreatWalkInPenalty` pushes a quiet that
    // walks a safe piece into the same kind of attack behind mid-history
    // quiets. Scaled to the same magnitude space as the butterfly +
    // continuation history sum so a strong historical signal can still
    // outrank a weak threat cue and vice versa.
    int ThreatEscapeBonus;
    int ThreatWalkInPenalty;

    // Threat-aware LMR deltas. When a quiet move evacuates a piece from a
    // square attacked by a less-valuable enemy piece onto a safe square,
    // LMR reduces the search depth by `LmrThreatEscape` fewer plies. When
    // it walks a safe piece onto such a square, LMR reduces by
    // `LmrThreatWalkIn` more plies. Stored as integer ply counts, not
    // magnitudes, because the LMR table itself is integer-valued and
    // sub-unit perturbations would never cross a reduction boundary.
    int LmrThreatEscape;
    int LmrThreatWalkIn;

    // Singular extension scalars. `singularBeta = ttScore - SingularBetaMul *
    // depth` controls how far below the TT score a non-TT move has to come up
    // to "tie" the TT move; lowering it widens the singular window so more
    // moves get extended. `singularDepth = (depth - 1) / SingularDepthDiv`
    // controls how shallow the verification search runs; raising it cheapens
    // the verification but loses signal. Defaults reproduce the prior
    // hardcoded literals (2, 2).
    int SingularBetaMul;
    int SingularDepthDiv;
    // Double-extension trigger: when the singular search fails low by more
    // than this margin against `singularBeta`, the TT move is treated as
    // sharply better than every alternative and gets extended by two plies
    // instead of one. Gated to non-PV nodes so the double extension never
    // stacks with PV check or recapture extensions on the same move.
    int SingularDoubleMargin;

    // Cut-node Internal Iterative Reduction depth gate. When the search is
    // at a cut-node and the TT either has no best move or a much shallower
    // record than the current iteration, drop one extra ply so the next
    // visit gets a better TT move at full depth. Only fires once the
    // remaining depth is at least this large.
    int IirCutNodeDepth;

    // Aspiration window seeding. The first iteration with a reliable previous
    // score opens a window around it of half-width
    //   delta = max(AspWindowBase, |prevScore| / AspWindowDiv)
    // matching the Stockfish-lineage formula. A narrow base catches boundary
    // cases where the second-best move's null-window probe would otherwise
    // be clipped against an over-generous alpha; the |prevScore| term keeps
    // the window wide enough in heavily one-sided positions where the score
    // can swing several pawns between iterations.
    int AspWindowBase;
    int AspWindowDiv;
};

extern SearchParams searchParams;

// Reset searchParams to the compiled-in defaults.
void resetSearchParams();

#endif
