#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"
#include <vector>

bool isSquareAttacked(const Board &board, int sq, Color byColor);
std::vector<Move> generateLegalMoves(Board &board);
std::vector<Move> generateLegalCaptures(Board &board);
// Non-capturing, non-promotion moves. Mirrors `generateLegalCaptures` so the
// staged move picker can request captures and quiets independently. A quiet
// promotion push would change material and is handled by the capture path
// alongside other promotions, so it is intentionally excluded here.
std::vector<Move> generateLegalQuiets(Board &board);

#endif
