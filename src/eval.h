#ifndef EVAL_H
#define EVAL_H

#include "board.h"

#include <cstddef>

extern const int PieceValue[7];

// Internal eval grain per pawn. The material rescale landed with PR #28 put
// one pawn at ~228 internal units; UCI centipawns divide by this to recover
// the conventional 100 cp / pawn scale. Consumed by search's info output and
// by the eval command's tapered/cp columns.
constexpr int NormalizePawn = 228;

int evaluate(const Board &board);

void clearPawnHash();
void setPawnHashSize(size_t mb);

void clearMaterialHash();
void setMaterialHashSize(size_t mb);

#endif
