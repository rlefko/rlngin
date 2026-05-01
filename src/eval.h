#ifndef EVAL_H
#define EVAL_H

#include "board.h"

#include <cstddef>
#include <ostream>

extern const int PieceValue[7];

int evaluate(const Board &board);

// Print a per-term breakdown of the static evaluation to os. The trailing
// total matches evaluate(board) (from the side-to-move perspective) so
// the verbose path never silently diverges from the real evaluator.
void evaluateVerbose(const Board &board, std::ostream &os);

void clearPawnHash();
void setPawnHashSize(size_t mb);

void clearMaterialHash();
void setMaterialHashSize(size_t mb);

#endif
