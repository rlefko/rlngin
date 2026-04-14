#include "movegen.h"
#include "bitboard.h"
#include <cstdlib>

static bool inBounds(int rank, int file) {
    return rank >= 0 && rank < 8 && file >= 0 && file < 8;
}

bool isSquareAttacked(const Board &board, int sq, Color byColor) {
    Bitboard occ = boardOccupancy(board);

    // Knight attacks
    Bitboard knights = KnightAttacks[sq];
    for (Bitboard b = knights; b;) {
        int s = popLsb(b);
        if (board.squares[s].type == Knight && board.squares[s].color == byColor) return true;
    }

    // King attacks
    Bitboard kings = KingAttacks[sq];
    for (Bitboard b = kings; b;) {
        int s = popLsb(b);
        if (board.squares[s].type == King && board.squares[s].color == byColor) return true;
    }

    // Pawn attacks: squares from which a byColor pawn attacks sq
    // are given by the opposite color's pawn attack pattern from sq
    Color them = (byColor == White) ? Black : White;
    Bitboard pawns = PawnAttacks[them][sq];
    for (Bitboard b = pawns; b;) {
        int s = popLsb(b);
        if (board.squares[s].type == Pawn && board.squares[s].color == byColor) return true;
    }

    // Rook/queen attacks (straight lines)
    Bitboard rooks = rookAttacks(sq, occ);
    for (Bitboard b = rooks; b;) {
        int s = popLsb(b);
        if (board.squares[s].color == byColor &&
            (board.squares[s].type == Rook || board.squares[s].type == Queen))
            return true;
    }

    // Bishop/queen attacks (diagonals)
    Bitboard bishops = bishopAttacks(sq, occ);
    for (Bitboard b = bishops; b;) {
        int s = popLsb(b);
        if (board.squares[s].color == byColor &&
            (board.squares[s].type == Bishop || board.squares[s].type == Queen))
            return true;
    }

    return false;
}

static int findKing(const Board &board, Color color) {
    for (int i = 0; i < 64; i++) {
        if (board.squares[i].type == King && board.squares[i].color == color) return i;
    }
    return -1;
}

static bool isLegalMove(const Board &board, const Move &m) {
    Board copy = board;
    copy.makeMove(m);
    Color us = board.sideToMove;
    int kingSq = findKing(copy, us);
    if (kingSq == -1) return false;
    return !isSquareAttacked(copy, kingSq, (us == White) ? Black : White);
}

static void addPawnMoves(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    int dir = (us == White) ? 1 : -1;
    int startRank = (us == White) ? 1 : 6;
    int promoRank = (us == White) ? 7 : 0;

    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type != Pawn || p.color != us) continue;

        int rank = squareRank(sq);
        int file = squareFile(sq);

        // Single push
        int pushSq = makeSquare(rank + dir, file);
        if (inBounds(rank + dir, file) && board.squares[pushSq].type == None) {
            if (squareRank(pushSq) == promoRank) {
                for (PieceType pt : {Queen, Rook, Bishop, Knight}) {
                    moves.push_back({sq, pushSq, pt});
                }
            } else {
                moves.push_back({sq, pushSq, None});
            }

            // Double push
            if (rank == startRank) {
                int dblSq = makeSquare(rank + 2 * dir, file);
                if (board.squares[dblSq].type == None) {
                    moves.push_back({sq, dblSq, None});
                }
            }
        }

        // Captures
        for (int df : {-1, 1}) {
            int cr = rank + dir, cf = file + df;
            if (!inBounds(cr, cf)) continue;
            int capSq = makeSquare(cr, cf);
            bool isCapture =
                (board.squares[capSq].type != None && board.squares[capSq].color != us);
            bool isEp = (capSq == board.enPassantSquare);
            if (isCapture || isEp) {
                if (squareRank(capSq) == promoRank) {
                    for (PieceType pt : {Queen, Rook, Bishop, Knight}) {
                        moves.push_back({sq, capSq, pt});
                    }
                } else {
                    moves.push_back({sq, capSq, None});
                }
            }
        }
    }
}

static void addKnightMoves(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    const int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    const int df[] = {-1, 1, -2, 2, -2, 2, -1, 1};

    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.type != Knight || p.color != us) continue;
        int rank = squareRank(sq);
        int file = squareFile(sq);
        for (int i = 0; i < 8; i++) {
            int r = rank + dr[i], f = file + df[i];
            if (!inBounds(r, f)) continue;
            int to = makeSquare(r, f);
            if (board.squares[to].type == None || board.squares[to].color != us) {
                moves.push_back({sq, to, None});
            }
        }
    }
}

static void addSlidingMoves(const Board &board, std::vector<Move> &moves, PieceType type,
                            const int dirs[][2], int numDirs) {
    Color us = board.sideToMove;
    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.color != us) continue;
        if (p.type != type && !(p.type == Queen && (type == Rook || type == Bishop))) continue;

        int rank = squareRank(sq);
        int file = squareFile(sq);
        for (int d = 0; d < numDirs; d++) {
            for (int dist = 1; dist < 8; dist++) {
                int r = rank + dirs[d][0] * dist;
                int f = file + dirs[d][1] * dist;
                if (!inBounds(r, f)) break;
                int to = makeSquare(r, f);
                Piece target = board.squares[to];
                if (target.type == None) {
                    moves.push_back({sq, to, None});
                } else {
                    if (target.color != us) {
                        moves.push_back({sq, to, None});
                    }
                    break;
                }
            }
        }
    }
}

static void addKingMoves(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    int kingSq = findKing(board, us);
    if (kingSq == -1) return;

    int rank = squareRank(kingSq);
    int file = squareFile(kingSq);

    for (int dr = -1; dr <= 1; dr++) {
        for (int df = -1; df <= 1; df++) {
            if (dr == 0 && df == 0) continue;
            int r = rank + dr, f = file + df;
            if (!inBounds(r, f)) continue;
            int to = makeSquare(r, f);
            if (board.squares[to].type == None || board.squares[to].color != us) {
                moves.push_back({kingSq, to, None});
            }
        }
    }

    // Castling
    Color enemy = (us == White) ? Black : White;
    if (us == White) {
        if (board.castleWK && board.squares[5].type == None && board.squares[6].type == None &&
            !isSquareAttacked(board, 4, enemy) && !isSquareAttacked(board, 5, enemy) &&
            !isSquareAttacked(board, 6, enemy)) {
            moves.push_back({4, 6, None});
        }
        if (board.castleWQ && board.squares[3].type == None && board.squares[2].type == None &&
            board.squares[1].type == None && !isSquareAttacked(board, 4, enemy) &&
            !isSquareAttacked(board, 3, enemy) && !isSquareAttacked(board, 2, enemy)) {
            moves.push_back({4, 2, None});
        }
    } else {
        if (board.castleBK && board.squares[61].type == None && board.squares[62].type == None &&
            !isSquareAttacked(board, 60, enemy) && !isSquareAttacked(board, 61, enemy) &&
            !isSquareAttacked(board, 62, enemy)) {
            moves.push_back({60, 62, None});
        }
        if (board.castleBQ && board.squares[59].type == None && board.squares[58].type == None &&
            board.squares[57].type == None && !isSquareAttacked(board, 60, enemy) &&
            !isSquareAttacked(board, 59, enemy) && !isSquareAttacked(board, 58, enemy)) {
            moves.push_back({60, 58, None});
        }
    }
}

std::vector<Move> generateLegalMoves(const Board &board) {
    std::vector<Move> pseudo;

    addPawnMoves(board, pseudo);
    addKnightMoves(board, pseudo);

    const int straightDirs[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    const int diagDirs[][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}};

    addSlidingMoves(board, pseudo, Rook, straightDirs, 4);
    addSlidingMoves(board, pseudo, Bishop, diagDirs, 4);

    addKingMoves(board, pseudo);

    std::vector<Move> legal;
    for (const Move &m : pseudo) {
        if (isLegalMove(board, m)) {
            legal.push_back(m);
        }
    }
    return legal;
}
