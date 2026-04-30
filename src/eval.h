#ifndef EVAL_H
#define EVAL_H

#include "board.h"
#include "types.h"

#include <cstddef>
#include <ostream>

extern const int PieceValue[7];

// Two-stage evaluator. The cheap material + PST + tempo prelude is
// computed first; if the side-to-move score with a `LazyMargin` envelope
// already lies fully outside `[alpha, beta]`, the function returns that
// bounded prelude and skips the residual half (mobility, threats, king
// safety, pawn structure beyond the cached half, initiative). Defaults
// pass `[-INF_SCORE, INF_SCORE]` so any caller that does not have an
// alpha-beta window pays for the full eval.
int evaluate(const Board &board, int alpha = -INF_SCORE, int beta = INF_SCORE);

// Print a per-term breakdown of the static evaluation to os. The trailing
// total matches evaluate(board) (from the side-to-move perspective) so
// the verbose path never silently diverges from the real evaluator.
void evaluateVerbose(const Board &board, std::ostream &os);

void clearPawnHash();
void setPawnHashSize(size_t mb);

void clearMaterialHash();
void setMaterialHashSize(size_t mb);

#endif
