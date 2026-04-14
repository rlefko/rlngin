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

static bool isLegalMove(const Board &board, const Move &m) {
    Board copy = board;
    copy.makeMove(m);
    Color us = board.sideToMove;
    Bitboard kingBB = copy.byPiece[King] & copy.byColor[us];
    if (!kingBB) return false;
    return !isSquareAttacked(copy, lsb(kingBB), (us == White) ? Black : White);
}

static void addPawnMoves(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    int dir = (us == White) ? 1 : -1;
    int startRank = (us == White) ? 1 : 6;
    int promoRank = (us == White) ? 7 : 0;

    Bitboard pawns = board.byPiece[Pawn] & board.byColor[us];
    while (pawns) {
        int sq = popLsb(pawns);
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
    Bitboard friendly = board.byColor[us];
    Bitboard knights = board.byPiece[Knight] & friendly;

    while (knights) {
        int sq = popLsb(knights);
        Bitboard attacks = KnightAttacks[sq] & ~friendly;
        while (attacks) {
            moves.push_back({sq, popLsb(attacks), None});
        }
    }
}

static void addSlidingMoves(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    Bitboard occ = board.occupied;
    Bitboard friendly = board.byColor[us];

    Bitboard bishops = board.byPiece[Bishop] & friendly;
    while (bishops) {
        int sq = popLsb(bishops);
        Bitboard attacks = bishopAttacks(sq, occ) & ~friendly;
        while (attacks) {
            moves.push_back({sq, popLsb(attacks), None});
        }
    }

    Bitboard rooks = board.byPiece[Rook] & friendly;
    while (rooks) {
        int sq = popLsb(rooks);
        Bitboard attacks = rookAttacks(sq, occ) & ~friendly;
        while (attacks) {
            moves.push_back({sq, popLsb(attacks), None});
        }
    }

    Bitboard queens = board.byPiece[Queen] & friendly;
    while (queens) {
        int sq = popLsb(queens);
        Bitboard attacks = queenAttacks(sq, occ) & ~friendly;
        while (attacks) {
            moves.push_back({sq, popLsb(attacks), None});
        }
    }
}

static void addKingMoves(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    Bitboard kingBB = board.byPiece[King] & board.byColor[us];
    if (!kingBB) return;
    int kingSq = lsb(kingBB);
    Bitboard friendly = board.byColor[us];

    Bitboard attacks = KingAttacks[kingSq] & ~friendly;
    while (attacks) {
        moves.push_back({kingSq, popLsb(attacks), None});
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
