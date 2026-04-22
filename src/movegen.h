#ifndef MOVEGEN_H
#define MOVEGEN_H

#include "board.h"
#include <vector>

bool isSquareAttacked(const Board &board, int sq, Color byColor);
// Fully validate that `m` is legal in `board`. Runs makeMove + unmakeMove
// under the hood so callers must pass a mutable board; no state leaks. The
// staged move picker uses this to screen TT, killer, and counter moves
// before handing them to the main search, since a TT collision can carry a
// move that is syntactically well-formed but illegal in the current
// position.
bool isLegalMove(Board &board, const Move &m);
std::vector<Move> generateLegalMoves(Board &board);
std::vector<Move> generateLegalCaptures(Board &board);
// Non-capturing, non-promotion moves. Mirrors `generateLegalCaptures` so the
// staged move picker can request captures and quiets independently. A quiet
// promotion push would change material and is handled by the capture path
// alongside other promotions, so it is intentionally excluded here.
std::vector<Move> generateLegalQuiets(Board &board);

#endif
