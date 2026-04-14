#include "movegen.h"
#include "bitboard.h"

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

static bool isLegalMove(Board &board, const Move &m) {
    Color us = board.sideToMove;
    UndoInfo undo = board.makeMove(m);
    Bitboard kingBB = board.byPiece[King] & board.byColor[us];
    bool legal = kingBB && !isSquareAttacked(board, lsb(kingBB), (us == White) ? Black : White);
    board.unmakeMove(m, undo);
    return legal;
}

static void addPawnPushes(Bitboard bb, int offset, Bitboard promoRank, std::vector<Move> &moves) {
    Bitboard promo = bb & promoRank;
    Bitboard quiet = bb & ~promoRank;
    while (quiet) {
        int to = popLsb(quiet);
        moves.push_back({to + offset, to, None});
    }
    while (promo) {
        int to = popLsb(promo);
        for (PieceType pt : {Queen, Rook, Bishop, Knight}) {
            moves.push_back({to + offset, to, pt});
        }
    }
}

static void addPawnMoves(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    Bitboard pawns = board.byPiece[Pawn] & board.byColor[us];
    Bitboard empty = ~board.occupied;
    Bitboard enemies = board.byColor[(us == White) ? Black : White];
    Bitboard promoRank = (us == White) ? Rank8BB : Rank1BB;

    // Single pushes
    Bitboard singlePush = (us == White) ? (pawns << 8) : (pawns >> 8);
    singlePush &= empty;
    int pushOffset = (us == White) ? -8 : 8;
    addPawnPushes(singlePush, pushOffset, promoRank, moves);

    // Double pushes
    Bitboard dblRank = (us == White) ? Rank3BB : Rank6BB;
    Bitboard doublePush =
        (us == White) ? ((singlePush & dblRank) << 8) : ((singlePush & dblRank) >> 8);
    doublePush &= empty;
    while (doublePush) {
        int to = popLsb(doublePush);
        int from = (us == White) ? to - 16 : to + 16;
        moves.push_back({from, to, None});
    }

    // Captures
    Bitboard captureTargets = enemies;
    if (board.enPassantSquare != -1) {
        captureTargets |= squareBB(board.enPassantSquare);
    }

    Bitboard leftCap, rightCap;
    int leftOffset, rightOffset;
    if (us == White) {
        leftCap = (pawns << 7) & ~FileHBB & captureTargets;
        rightCap = (pawns << 9) & ~FileABB & captureTargets;
        leftOffset = -7;
        rightOffset = -9;
    } else {
        leftCap = (pawns >> 9) & ~FileHBB & captureTargets;
        rightCap = (pawns >> 7) & ~FileABB & captureTargets;
        leftOffset = 9;
        rightOffset = 7;
    }

    addPawnPushes(leftCap, leftOffset, promoRank, moves);
    addPawnPushes(rightCap, rightOffset, promoRank, moves);
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

static void addPawnCaptures(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    Bitboard pawns = board.byPiece[Pawn] & board.byColor[us];
    Bitboard empty = ~board.occupied;
    Bitboard enemies = board.byColor[(us == White) ? Black : White];
    Bitboard promoRank = (us == White) ? Rank8BB : Rank1BB;

    // Promotion pushes (non-capture promotions are material-changing)
    Bitboard singlePush = (us == White) ? (pawns << 8) : (pawns >> 8);
    singlePush &= empty;
    int pushOffset = (us == White) ? -8 : 8;
    Bitboard promoPush = singlePush & promoRank;
    while (promoPush) {
        int to = popLsb(promoPush);
        for (PieceType pt : {Queen, Rook, Bishop, Knight}) {
            moves.push_back({to + pushOffset, to, pt});
        }
    }

    // Captures
    Bitboard captureTargets = enemies;
    if (board.enPassantSquare != -1) {
        captureTargets |= squareBB(board.enPassantSquare);
    }

    Bitboard leftCap, rightCap;
    int leftOffset, rightOffset;
    if (us == White) {
        leftCap = (pawns << 7) & ~FileHBB & captureTargets;
        rightCap = (pawns << 9) & ~FileABB & captureTargets;
        leftOffset = -7;
        rightOffset = -9;
    } else {
        leftCap = (pawns >> 9) & ~FileHBB & captureTargets;
        rightCap = (pawns >> 7) & ~FileABB & captureTargets;
        leftOffset = 9;
        rightOffset = 7;
    }

    addPawnPushes(leftCap, leftOffset, promoRank, moves);
    addPawnPushes(rightCap, rightOffset, promoRank, moves);
}

static void addKnightCaptures(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    Bitboard enemies = board.byColor[(us == White) ? Black : White];
    Bitboard knights = board.byPiece[Knight] & board.byColor[us];

    while (knights) {
        int sq = popLsb(knights);
        Bitboard attacks = KnightAttacks[sq] & enemies;
        while (attacks) {
            moves.push_back({sq, popLsb(attacks), None});
        }
    }
}

static void addSlidingCaptures(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    Bitboard occ = board.occupied;
    Bitboard enemies = board.byColor[(us == White) ? Black : White];

    Bitboard bishops = board.byPiece[Bishop] & board.byColor[us];
    while (bishops) {
        int sq = popLsb(bishops);
        Bitboard attacks = bishopAttacks(sq, occ) & enemies;
        while (attacks) {
            moves.push_back({sq, popLsb(attacks), None});
        }
    }

    Bitboard rooks = board.byPiece[Rook] & board.byColor[us];
    while (rooks) {
        int sq = popLsb(rooks);
        Bitboard attacks = rookAttacks(sq, occ) & enemies;
        while (attacks) {
            moves.push_back({sq, popLsb(attacks), None});
        }
    }

    Bitboard queens = board.byPiece[Queen] & board.byColor[us];
    while (queens) {
        int sq = popLsb(queens);
        Bitboard attacks = queenAttacks(sq, occ) & enemies;
        while (attacks) {
            moves.push_back({sq, popLsb(attacks), None});
        }
    }
}

static void addKingCaptures(const Board &board, std::vector<Move> &moves) {
    Color us = board.sideToMove;
    Bitboard kingBB = board.byPiece[King] & board.byColor[us];
    if (!kingBB) return;
    int kingSq = lsb(kingBB);
    Bitboard enemies = board.byColor[(us == White) ? Black : White];

    Bitboard attacks = KingAttacks[kingSq] & enemies;
    while (attacks) {
        moves.push_back({kingSq, popLsb(attacks), None});
    }
}

std::vector<Move> generateLegalCaptures(Board &board) {
    std::vector<Move> pseudo;

    addPawnCaptures(board, pseudo);
    addKnightCaptures(board, pseudo);
    addSlidingCaptures(board, pseudo);
    addKingCaptures(board, pseudo);

    std::vector<Move> legal;
    for (const Move &m : pseudo) {
        if (isLegalMove(board, m)) {
            legal.push_back(m);
        }
    }
    return legal;
}

std::vector<Move> generateLegalMoves(Board &board) {
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
