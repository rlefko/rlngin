#include "search.h"
#include "eval.h"
#include "movegen.h"
#include <limits>

Move findBestMove(const Board &board) {
    std::vector<Move> moves = generateLegalMoves(board);
    if (moves.empty()) return {0, 0, None};

    Move bestMove = moves[0];
    int bestScore = std::numeric_limits<int>::min();

    for (const Move &m : moves) {
        Board copy = board;
        copy.makeMove(m);
        int score = -evaluate(copy);
        if (score > bestScore) {
            bestScore = score;
            bestMove = m;
        }
    }

    return bestMove;
}
