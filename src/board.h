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
    uint64_t materialKey;
    uint64_t nonPawnKey[2];
    uint64_t minorKey;
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
    uint64_t materialKey = 0;
    // Non-pawn placement hash per color (includes the king). Minor hash covers
    // knights and bishops of both colors. Both are maintained incrementally
    // alongside key/pawnKey/materialKey in make/unmake.
    uint64_t nonPawnKey[2] = {0, 0};
    uint64_t minorKey = 0;
    uint64_t occupied = 0;
    uint64_t byColor[2] = {0, 0};
    uint64_t byPiece[7] = {0, 0, 0, 0, 0, 0, 0};
    int pieceCount[2][7] = {{0}};

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
    void computeMaterialKey();
    void computeNonPawnKeys();
    void computeMinorKey();
};

#endif
