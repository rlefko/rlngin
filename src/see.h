#ifndef SEE_H
#define SEE_H

#include "board.h"

int see(const Board &board, const Move &move);
bool seeGE(const Board &board, const Move &move, int threshold);

#endif
