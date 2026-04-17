#ifndef EVAL_H
#define EVAL_H

#include "board.h"

#include <cstddef>

extern const int PieceValue[7];

int evaluate(const Board &board);

void clearPawnHash();
void setPawnHashSize(size_t mb);

void clearMaterialHash();
void setMaterialHashSize(size_t mb);

#endif
