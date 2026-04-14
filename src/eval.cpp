#include "eval.h"

static const int pieceValue[] = {0, 100, 300, 300, 500, 900, 0};

int evaluate(const Board &board) {
    int white = 0;
    int black = 0;

    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type == None || p.type == King) continue;
        if (p.color == White)
            white += pieceValue[p.type];
        else
            black += pieceValue[p.type];
    }

    int score = white - black;
    return (board.sideToMove == White) ? score : -score;
}
