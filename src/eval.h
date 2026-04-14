#ifndef EVAL_H
#define EVAL_H

#include "board.h"

extern const int PieceValue[7];

int evaluate(const Board &board);

#endif
