#include "see.h"
#include "bitboard.h"
#include "eval.h"
#include <algorithm>

static Bitboard getAttackers(const Board &board, int sq, Bitboard occ) {
    return (PawnAttacks[Black][sq] & board.byPiece[Pawn] & board.byColor[White]) |
           (PawnAttacks[White][sq] & board.byPiece[Pawn] & board.byColor[Black]) |
           (KnightAttacks[sq] & board.byPiece[Knight]) |
           (bishopAttacks(sq, occ) & (board.byPiece[Bishop] | board.byPiece[Queen])) |
           (rookAttacks(sq, occ) & (board.byPiece[Rook] | board.byPiece[Queen])) |
           (KingAttacks[sq] & board.byPiece[King]);
}

static PieceType getLeastValuableAttacker(const Board &board, Bitboard attackers, Color side,
                                          int &outSq) {
    for (int pt = Pawn; pt <= King; pt++) {
        Bitboard candidates = attackers & board.byPiece[pt] & board.byColor[side];
        if (candidates) {
            outSq = lsb(candidates);
            return static_cast<PieceType>(pt);
        }
    }
    return None;
}

int see(const Board &board, const Move &move) {
    int gain[32];
    int depth = 0;

    int toSq = move.to;
    int fromSq = move.from;

    PieceType attacker = board.squares[fromSq].type;
    Color side = board.squares[fromSq].color;

    Bitboard occ = board.occupied;

    // Determine the initial captured piece value
    Piece captured = board.squares[toSq];
    if (captured.type != None) {
        gain[0] = PieceValue[captured.type];
    } else if (attacker == Pawn && toSq == board.enPassantSquare && board.enPassantSquare != -1) {
        gain[0] = PieceValue[Pawn];
        // Remove the en passant captured pawn from occupancy
        int epPawnSq = (side == White) ? toSq - 8 : toSq + 8;
        occ ^= squareBB(epPawnSq);
    } else {
        gain[0] = 0;
    }

    // Handle promotions: gain includes promotion bonus, attacker becomes promoted piece
    if (move.promotion != None) {
        gain[0] += PieceValue[move.promotion] - PieceValue[Pawn];
        attacker = move.promotion;
    }

    // Remove the initial attacker from occupancy
    occ ^= squareBB(fromSq);

    // Get all attackers of the target square with updated occupancy
    Bitboard attackers = getAttackers(board, toSq, occ);

    // Remove the initial attacker from the attackers set
    attackers &= occ;

    // Alternate sides
    side = (side == White) ? Black : White;

    while (true) {
        depth++;

        int sq;
        PieceType pt = getLeastValuableAttacker(board, attackers, side, sq);
        if (pt == None) {
            depth--;
            break;
        }

        gain[depth] = PieceValue[attacker] - gain[depth - 1];

        // Pruning: if the current side can't improve even with the best outcome, stop
        if (std::max(-gain[depth - 1], gain[depth]) < 0) break;

        attacker = pt;
        occ ^= squareBB(sq);

        // Recompute attackers to handle x-ray discoveries
        attackers = getAttackers(board, toSq, occ) & occ;

        side = (side == White) ? Black : White;
    }

    // Propagate backwards with negamax
    while (depth > 0) {
        depth--;
        gain[depth] = -std::max(-gain[depth], gain[depth + 1]);
    }

    return gain[0];
}
