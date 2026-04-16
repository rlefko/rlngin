#ifndef BOARD_H
#define BOARD_H

#include "types.h"
#include <cstdint>
#include <string>

struct UndoInfo {
    Piece captured;
    int enPassantSquare;
    bool castleWK, castleWQ, castleBK, castleBQ;
    int halfmoveClock;
    uint64_t key;
    uint64_t pawnKey;
};

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
    uint64_t pawnKey = 0;
    uint64_t occupied = 0;
    uint64_t byColor[2] = {0, 0};
    uint64_t byPiece[7] = {0, 0, 0, 0, 0, 0, 0};

    Board();
    void setStartPos();
    void setFen(const std::string &fen);
    UndoInfo makeMove(const Move &m);
    void unmakeMove(const Move &m, const UndoInfo &undo);
    UndoInfo makeNullMove();
    void unmakeNullMove(const UndoInfo &undo);
    Piece pieceAt(int sq) const;

  private:
    void computeKey();
    void computePawnKey();
};

#endif
