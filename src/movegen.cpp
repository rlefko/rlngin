#include "movegen.h"
#include "bitboard.h"

static bool inBounds(int rank, int file) {
    return rank >= 0 && rank < 8 && file >= 0 && file < 8;
}

bool isSquareAttacked(const Board &board, int sq, Color byColor) {
    Bitboard them = board.byColor[byColor];
    if (KnightAttacks[sq] & board.byPiece[Knight] & them) return true;
    if (KingAttacks[sq] & board.byPiece[King] & them) return true;
    Color us = (byColor == White) ? Black : White;
    if (PawnAttacks[us][sq] & board.byPiece[Pawn] & them) return true;
    Bitboard occ = board.occupied;
    if (rookAttacks(sq, occ) & (board.byPiece[Rook] | board.byPiece[Queen]) & them) return true;
    if (bishopAttacks(sq, occ) & (board.byPiece[Bishop] | board.byPiece[Queen]) & them) return true;
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
    Bitboard friendly = boardColorOccupancy(board, us);

    for (int sq = 0; sq < 64; sq++) {
        if (board.squares[sq].type != Knight || board.squares[sq].color != us) continue;
        Bitboard attacks = KnightAttacks[sq] & ~friendly;
        for (Bitboard b = attacks; b;) {
            int to = popLsb(b);
            moves.push_back({sq, to, None});
        }
    }
}

static void addSlidingMoves(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    Bitboard occ = boardOccupancy(board);
    Bitboard friendly = boardColorOccupancy(board, us);

    for (int sq = 0; sq < 64; sq++) {
        Piece p = board.squares[sq];
        if (p.color != us) continue;

        Bitboard attacks = 0;
        if (p.type == Rook) {
            attacks = rookAttacks(sq, occ);
        } else if (p.type == Bishop) {
            attacks = bishopAttacks(sq, occ);
        } else if (p.type == Queen) {
            attacks = queenAttacks(sq, occ);
        } else {
            continue;
        }

        attacks &= ~friendly;
        for (Bitboard b = attacks; b;) {
            int to = popLsb(b);
            moves.push_back({sq, to, None});
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
    addSlidingMoves(board, pseudo);
    addKingMoves(board, pseudo);

    std::vector<Move> legal;
    for (const Move &m : pseudo) {
        if (isLegalMove(board, m)) {
            legal.push_back(m);
        }
    }
    return legal;
}
