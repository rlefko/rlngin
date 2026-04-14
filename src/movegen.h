#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"
#include <vector>

bool isSquareAttacked(const Board &board, int sq, Color byColor);
std::vector<Move> generateLegalMoves(Board &board);
std::vector<Move> generateLegalCaptures(Board &board);

#endif
