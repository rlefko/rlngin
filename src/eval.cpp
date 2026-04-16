#include "eval.h"

#include "bitboard.h"

#include <algorithm>

// Material values used by SEE and move ordering (MG values, king kept large for SEE)
const int PieceValue[] = {0, 82, 337, 365, 477, 1025, 20000};

// PeSTO middlegame and endgame material values (king = 0, always present on both sides)
static const int MGPieceValue[] = {0, 82, 337, 365, 477, 1025, 0};
static const int EGPieceValue[] = {0, 94, 281, 297, 512, 936, 0};

// Game phase increments per piece type (max total = 24)
static const int GamePhaseInc[] = {0, 0, 1, 1, 2, 4, 0};

// clang-format off

// PeSTO piece-square tables stored in a1=0 order (rank 1 first, rank 8 last)
// Values are from White's perspective; Black mirrors vertically via sq ^ 56

static const int MgPawnTable[64] = {
      0,   0,   0,   0,   0,   0,   0,   0,
    -35,  -1, -20, -23, -15,  24,  38, -22,
    -26,  -4,  -4, -10,   3,   3,  33, -12,
    -27,  -2,  -5,  12,  17,   6,  10, -25,
    -14,  13,   6,  21,  23,  12,  17, -23,
     -6,   7,  26,  31,  65,  56,  25, -20,
     98, 134,  61,  95,  68, 126,  34, -11,
      0,   0,   0,   0,   0,   0,   0,   0
};

static const int EgPawnTable[64] = {
      0,   0,   0,   0,   0,   0,   0,   0,
     13,   8,   8,  10,  13,   0,   2,  -7,
      4,   7,  -6,   1,   0,  -5,  -1,  -8,
     13,   9,  -3,  -7,  -7,  -8,   3,  -1,
     32,  24,  13,   5,  -2,   4,  17,  17,
     94, 100,  85,  67,  56,  53,  82,  84,
    178, 173, 158, 134, 147, 132, 165, 187,
      0,   0,   0,   0,   0,   0,   0,   0
};

static const int MgKnightTable[64] = {
   -105, -21, -58, -33, -17, -28, -19, -23,
    -29, -53, -12,  -3,  -1,  18, -14, -19,
    -23,  -9,  12,  10,  19,  17,  25, -16,
    -13,   4,  16,  13,  28,  19,  21,  -8,
     -9,  17,  19,  53,  37,  69,  18,  22,
    -47,  60,  37,  65,  84, 129,  73,  44,
    -73, -41,  72,  36,  23,  62,   7, -17,
   -167, -89, -34, -49,  61, -97, -15,-107
};

static const int EgKnightTable[64] = {
    -29, -51, -23, -15, -22, -18, -50, -64,
    -42, -20, -10,  -5,  -2, -20, -23, -44,
    -23,  -3,  -1,  15,  10,  -3, -20, -22,
    -18,  -6,  16,  25,  16,  17,   4, -18,
    -17,   3,  22,  22,  22,  11,   8, -18,
    -24, -20,  10,   9,  -1,  -9, -19, -41,
    -25,  -8, -25,  -2,  -9, -25, -24, -52,
    -58, -38, -13, -28, -31, -27, -63, -99
};

static const int MgBishopTable[64] = {
    -33,  -3, -14, -21, -13, -12, -39, -21,
      4,  15,  16,   0,   7,  21,  33,   1,
      0,  15,  15,  15,  14,  27,  18,  10,
     -6,  13,  13,  26,  34,  12,  10,   4,
     -4,   5,  19,  50,  37,  37,   7,  -2,
    -16,  37,  43,  40,  35,  50,  37,  -2,
    -26,  16, -18, -13,  30,  59,  18, -47,
    -29,   4, -82, -37, -25, -42,   7,  -8
};

static const int EgBishopTable[64] = {
    -23,  -9, -23,  -5,  -9, -16,  -5, -17,
    -14, -18,  -7,  -1,   4,  -9, -15, -27,
    -12,  -3,   8,  10,  13,   3,  -7, -15,
     -6,   3,  13,  19,   7,  10,  -3,  -9,
     -3,   9,  12,   9,  14,  10,   3,   2,
      2,  -8,   0,  -1,  -2,   6,   0,   4,
     -8,  -4,   7, -12,  -3, -13,  -4, -14,
    -14, -21, -11,  -8,  -7,  -9, -17, -24
};

static const int MgRookTable[64] = {
    -19, -13,   1,  17,  16,   7, -37, -26,
    -44, -16, -20,  -9,  -1,  11,  -6, -71,
    -45, -25, -16, -17,   3,   0,  -5, -33,
    -36, -26, -12,  -1,   9,  -7,   6, -23,
    -24, -11,   7,  26,  24,  35,  -8, -20,
     -5,  19,  26,  36,  17,  45,  61,  16,
     27,  32,  58,  62,  80,  67,  26,  44,
     32,  42,  32,  51,  63,   9,  31,  43
};

static const int EgRookTable[64] = {
     -9,   2,   3,  -1,  -5, -13,   4, -20,
     -6,  -6,   0,   2,  -9,  -9, -11,  -3,
     -4,   0,  -5,  -1,  -7, -12,  -8, -16,
      3,   5,   8,   4,  -5,  -6,  -8, -11,
      4,   3,  13,   1,   2,   1,  -1,   2,
      7,   7,   7,   5,   4,  -3,  -5,  -3,
     11,  13,  13,  11,  -3,   3,   8,   3,
     13,  10,  18,  15,  12,  12,   8,   5
};

static const int MgQueenTable[64] = {
     -1, -18,  -9,  10, -15, -25, -31, -50,
    -35,  -8,  11,   2,   8,  15,  -3,   1,
    -14,   2, -11,  -2,  -5,   2,  14,   5,
     -9, -26,  -9, -10,  -2,  -4,   3,  -3,
    -27, -27, -16, -16,  -1,  17,  -2,   1,
    -13, -17,   7,   8,  29,  56,  47,  57,
    -24, -39,  -5,   1, -16,  57,  28,  54,
    -28,   0,  29,  12,  59,  44,  43,  45
};

static const int EgQueenTable[64] = {
    -33, -28, -22, -43,  -5, -32, -20, -41,
    -22, -23, -30, -16, -16, -23, -36, -32,
    -16, -27,  15,   6,   9,  17,  10,   5,
    -18,  28,  19,  47,  31,  34,  39,  23,
      3,  22,  24,  45,  57,  40,  57,  36,
    -20,   6,   9,  49,  47,  35,  19,   9,
    -17,  20,  32,  41,  58,  25,  30,   0,
     -9,  22,  22,  27,  27,  19,  10,  20
};

static const int MgKingTable[64] = {
    -15,  36,  12, -54,   8, -28,  24,  14,
      1,   7,  -8, -64, -43, -16,   9,   8,
    -14, -14, -22, -46, -44, -30, -15, -27,
    -49,  -1, -27, -39, -46, -44, -33, -51,
    -17, -20, -12, -27, -30, -25, -14, -36,
     -9,  24,   2, -16, -20,   6,  22, -22,
     29,  -1, -20,  -7,  -8,  -4, -38, -29,
    -65,  23,  16, -15, -56, -34,   2,  13
};

static const int EgKingTable[64] = {
    -53, -34, -21, -11, -28, -14, -24, -43,
    -27, -11,   4,  13,  14,   4,  -5, -17,
    -19,  -3,  11,  21,  23,  16,   7,  -9,
    -18,  -4,  21,  24,  27,  23,   9, -11,
     -8,  22,  24,  27,  26,  33,  26,   3,
     10,  17,  23,  15,  20,  45,  44,  13,
    -12,  17,  14,  17,  17,  38,  23,  11,
    -74, -35, -18, -18, -11,  15,   4, -17
};

// clang-format on

static const int *MgPST[] = {
    nullptr,       // None
    MgPawnTable,   // Pawn
    MgKnightTable, // Knight
    MgBishopTable, // Bishop
    MgRookTable,   // Rook
    MgQueenTable,  // Queen
    MgKingTable    // King
};

static const int *EgPST[] = {
    nullptr,       // None
    EgPawnTable,   // Pawn
    EgKnightTable, // Knight
    EgBishopTable, // Bishop
    EgRookTable,   // Rook
    EgQueenTable,  // Queen
    EgKingTable    // King
};

// clang-format off

// Passed pawn bonus by rank index (0 = rank 1, 7 = rank 8)
// Indices 0 and 7 are impossible for pawns; included for indexing simplicity
static const int PassedPawnBonus[8][2] = {
    //  MG,  EG
    {   0,   0},  // rank 1
    {  10,  18},  // rank 2
    {  15,  23},  // rank 3
    {  20,  42},  // rank 4
    {  42,  80},  // rank 5
    {  92, 170},  // rank 6
    { 165, 260},  // rank 7
    {   0,   0},  // rank 8
};

// Connected pawn bonus by rank index
static const int ConnectedPawnBonus[8][2] = {
    //  MG,  EG
    {   0,   0},  // rank 1
    {  10,   3},  // rank 2
    {  12,   5},  // rank 3
    {  17,  10},  // rank 4
    {  35,  22},  // rank 5
    {  62,  40},  // rank 6
    {  95,  55},  // rank 7
    {   0,   0},  // rank 8
};

// clang-format on

static const int IsolatedPawnPenalty[2] = {-18, -25}; // MG, EG
static const int DoubledPawnPenalty[2] = {-12, -25};  // MG, EG
static const int BackwardPawnPenalty[2] = {-12, -18}; // MG, EG

static void evaluatePawns(const Board &board, int &mg, int &eg) {
    Bitboard whitePawns = board.byPiece[Pawn] & board.byColor[White];
    Bitboard blackPawns = board.byPiece[Pawn] & board.byColor[Black];

    for (int c = 0; c < 2; c++) {
        Bitboard ourPawns = (c == White) ? whitePawns : blackPawns;
        Bitboard theirPawns = (c == White) ? blackPawns : whitePawns;
        int sign = (c == White) ? 1 : -1;

        Bitboard pawns = ourPawns;
        while (pawns) {
            int sq = popLsb(pawns);
            int r = squareRank(sq);
            int f = squareFile(sq);
            int relativeRank = (c == White) ? r : (7 - r);

            // Passed pawn: no enemy pawns ahead on same or adjacent files
            if (!(PassedPawnMask[c][sq] & theirPawns)) {
                mg += sign * PassedPawnBonus[relativeRank][0];
                eg += sign * PassedPawnBonus[relativeRank][1];
            }

            // Isolated pawn: no friendly pawns on adjacent files
            bool isolated = !(AdjacentFilesBB[f] & ourPawns);
            if (isolated) {
                mg += sign * IsolatedPawnPenalty[0];
                eg += sign * IsolatedPawnPenalty[1];
            }

            // Doubled pawn: another friendly pawn ahead on the same file
            if (ForwardFileBB[c][sq] & ourPawns) {
                mg += sign * DoubledPawnPenalty[0];
                eg += sign * DoubledPawnPenalty[1];
            }

            // Connected pawn: phalanx (same rank, adjacent file) or defended by friendly pawn
            bool phalanx = (ourPawns & AdjacentFilesBB[f] & RankBB[r]) != 0;
            bool defended = (PawnAttacks[c ^ 1][sq] & ourPawns) != 0;
            if (phalanx || defended) {
                mg += sign * ConnectedPawnBonus[relativeRank][0];
                eg += sign * ConnectedPawnBonus[relativeRank][1];
            }

            // Backward pawn: not connected, not isolated, all adjacent friendly pawns
            // are ahead, and the stop square is controlled by an enemy pawn
            if (!phalanx && !defended && !isolated) {
                bool noneBelow = !(PawnSpanMask[c ^ 1][sq] & ourPawns);
                if (noneBelow) {
                    int stopSq = (c == White) ? sq + 8 : sq - 8;
                    if (PawnAttacks[c][stopSq] & theirPawns) {
                        mg += sign * BackwardPawnPenalty[0];
                        eg += sign * BackwardPawnPenalty[1];
                    }
                }
            }
        }
    }
}

int evaluate(const Board &board) {
    int mg[2] = {0, 0};
    int eg[2] = {0, 0};
    int gamePhase = 0;

    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type == None) continue;

        int idx = (p.color == White) ? sq : (sq ^ 56);

        mg[p.color] += MGPieceValue[p.type] + MgPST[p.type][idx];
        eg[p.color] += EGPieceValue[p.type] + EgPST[p.type][idx];
        gamePhase += GamePhaseInc[p.type];
    }

    int pawnMg = 0, pawnEg = 0;
    evaluatePawns(board, pawnMg, pawnEg);

    int mgResult = mg[White] - mg[Black] + pawnMg;
    int egResult = eg[White] - eg[Black] + pawnEg;

    int mgPhase = std::min(gamePhase, 24);
    int egPhase = 24 - mgPhase;
    int result = (mgResult * mgPhase + egResult * egPhase) / 24;

    // Scale evaluation toward 0 as the halfmove clock approaches 100 so the
    // engine prefers moves that make progress (captures, pawn pushes) and
    // avoids blundering into 50-move rule draws beyond the search horizon.
    // Skip when no pawns remain: in pawnless endgames like KQ vs K the winning
    // side has no way to reset the clock other than capturing, so the search
    // tree itself must handle the 50-move horizon without penalizing the eval.
    if (board.byPiece[Pawn]) {
        result = result * (200 - board.halfmoveClock) / 200;
    }

    return (board.sideToMove == White) ? result : -result;
}
