#ifndef BOARD_H
#define BOARD_H

#include "types.h"
#include <string>

class Board {
public:
    Piece squares[64];
    Color sideToMove = White;
    bool castleWK = true;
    bool castleWQ = true;
    bool castleBK = true;
    bool castleBQ = true;
    int enPassantSquare = -1;
    int halfmoveClock = 0;
    int fullmoveNumber = 1;

    Board();
    void setStartPos();
    void setFen(const std::string& fen);
    void makeMove(const Move& m);
    Piece pieceAt(int sq) const;
};

#endif
