#include "eval.h"
#include "bitboard.h"

static const int PieceValue[] = {0, 100, 320, 330, 500, 900, 20000};

// clang-format off

// Piece-square tables stored in a1=0 order (rank 1 first, rank 8 last)
// Values are from White's perspective; Black mirrors vertically via sq ^ 56

static const int PawnPST[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10,-20,-20, 10, 10,  5,
     5, -5,-10,  0,  0,-10, -5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5,  5, 10, 25, 25, 10,  5,  5,
    10, 10, 20, 30, 30, 20, 10, 10,
    50, 50, 50, 50, 50, 50, 50, 50,
     0,  0,  0,  0,  0,  0,  0,  0
};

static const int KnightPST[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50,
    -40,-20,  0,  5,  5,  0,-20,-40,
    -30,  5, 10, 15, 15, 10,  5,-30,
    -30,  0, 15, 20, 20, 15,  0,-30,
    -30,  5, 15, 20, 20, 15,  5,-30,
    -30,  0, 10, 15, 15, 10,  0,-30,
    -40,-20,  0,  0,  0,  0,-20,-40,
    -50,-40,-30,-30,-30,-30,-40,-50
};

static const int BishopPST[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20,
    -10,  5,  0,  0,  0,  0,  5,-10,
    -10, 10, 10, 10, 10, 10, 10,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10,  5,  5, 10, 10,  5,  5,-10,
    -10,  0,  5, 10, 10,  5,  0,-10,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -20,-10,-10,-10,-10,-10,-10,-20
};

static const int RookPST[64] = {
     0,  0,  0,  5,  5,  0,  0,  0,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     5, 10, 10, 10, 10, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0
};

static const int QueenPST[64] = {
    -20,-10,-10, -5, -5,-10,-10,-20,
    -10,  0,  5,  0,  0,  0,  0,-10,
    -10,  5,  5,  5,  5,  5,  0,-10,
      0,  0,  5,  5,  5,  5,  0, -5,
     -5,  0,  5,  5,  5,  5,  0, -5,
    -10,  0,  5,  5,  5,  5,  0,-10,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -20,-10,-10, -5, -5,-10,-10,-20
};

static const int KingMGPST[64] = {
     20, 30, 10,  0,  0, 10, 30, 20,
     20, 20,  0,  0,  0,  0, 20, 20,
    -10,-20,-20,-20,-20,-20,-20,-10,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30
};

static const int KingEGPST[64] = {
    -50,-30,-30,-30,-30,-30,-30,-50,
    -30,-30,  0,  0,  0,  0,-30,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-20,-10,  0,  0,-10,-20,-30,
    -50,-40,-30,-20,-20,-30,-40,-50
};

// clang-format on

static const int *PieceSquareTable[] = {
    nullptr,   // None
    PawnPST,   // Pawn
    KnightPST, // Knight
    BishopPST, // Bishop
    RookPST,   // Rook
    QueenPST,  // Queen
    nullptr    // King (handled separately)
};

static inline int mirror(int sq) {
    return sq ^ 56;
}

static bool isEndgame(const Board &board) {
    if (board.byPiece[Queen] == 0) return true;

    for (int c = 0; c < 2; c++) {
        Bitboard queens = board.byPiece[Queen] & board.byColor[c];
        if (queens == 0) continue;

        Bitboard rooks = board.byPiece[Rook] & board.byColor[c];
        if (rooks != 0) return false;

        Bitboard minors = (board.byPiece[Knight] | board.byPiece[Bishop]) & board.byColor[c];
        if (popcount(minors) > 1) return false;
    }
    return true;
}

int evaluate(const Board &board) {
    int score[2] = {0, 0};
    bool endgame = isEndgame(board);

    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type == None) continue;

        int idx = (p.color == White) ? sq : mirror(sq);

        if (p.type == King) {
            const int *kingTable = endgame ? KingEGPST : KingMGPST;
            score[p.color] += PieceValue[King] + kingTable[idx];
        } else {
            score[p.color] += PieceValue[p.type] + PieceSquareTable[p.type][idx];
        }
    }

    int result = score[White] - score[Black];
    return (board.sideToMove == White) ? result : -result;
}
