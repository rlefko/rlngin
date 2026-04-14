#ifndef BOARD_H
#define BOARD_H

#include "types.h"
#include <cstdint>
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
    uint64_t key = 0;

    Board();
    void setStartPos();
    void setFen(const std::string &fen);
    void makeMove(const Move &m);
    Piece pieceAt(int sq) const;

  private:
    void computeKey();
};

#endif
