#include "board.h"
#include "zobrist.h"
#include <sstream>

static int castlingMask(bool wk, bool wq, bool bk, bool bq) {
    return wk | (wq << 1) | (bk << 2) | (bq << 3);
}

Board::Board() {
    setStartPos();
}

void Board::computeKey() {
    key = 0;
    for (int sq = 0; sq < 64; sq++) {
        if (squares[sq].type != None) {
            key ^= zobrist::piece_keys[squares[sq].color][squares[sq].type][sq];
        }
    }
    if (sideToMove == Black) {
        key ^= zobrist::side_to_move_key;
    }
    key ^= zobrist::castling_keys[castlingMask(castleWK, castleWQ, castleBK, castleBQ)];
    if (enPassantSquare != -1) {
        key ^= zobrist::en_passant_keys[squareFile(enPassantSquare)];
    }
}

void Board::computePawnKey() {
    pawnKey = 0;
    for (int sq = 0; sq < 64; sq++) {
        if (squares[sq].type == Pawn) {
            pawnKey ^= zobrist::piece_keys[squares[sq].color][Pawn][sq];
        }
    }
}

void Board::computeMaterialKey() {
    materialKey = 0;
    for (int c = 0; c < 2; c++) {
        for (int pt = 1; pt < 7; pt++) {
            materialKey ^= zobrist::material_keys[c][pt][pieceCount[c][pt]];
        }
    }
}

void Board::computeNonPawnKeys() {
    nonPawnKey[White] = 0;
    nonPawnKey[Black] = 0;
    for (int sq = 0; sq < 64; sq++) {
        PieceType pt = squares[sq].type;
        if (pt != None && pt != Pawn) {
            nonPawnKey[squares[sq].color] ^= zobrist::piece_keys[squares[sq].color][pt][sq];
        }
    }
}

void Board::computeMinorKey() {
    minorKey = 0;
    for (int sq = 0; sq < 64; sq++) {
        PieceType pt = squares[sq].type;
        if (pt == Knight || pt == Bishop) {
            minorKey ^= zobrist::piece_keys[squares[sq].color][pt][sq];
        }
    }
}

Piece Board::pieceAt(int sq) const {
    return squares[sq];
}

void Board::setStartPos() {
    setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

void Board::setFen(const std::string &fen) {
    for (int i = 0; i < 64; i++) {
        squares[i] = {None, White};
    }

    std::istringstream ss(fen);
    std::string board, side, castling, ep, halfmove, fullmove;
    ss >> board >> side >> castling >> ep;

    if (ss >> halfmove)
        halfmoveClock = std::stoi(halfmove);
    else
        halfmoveClock = 0;

    if (ss >> fullmove)
        fullmoveNumber = std::stoi(fullmove);
    else
        fullmoveNumber = 1;

    int rank = 7, file = 0;
    for (char c : board) {
        if (c == '/') {
            rank--;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
        } else {
            int sq = makeSquare(rank, file);
            Color color = (c >= 'A' && c <= 'Z') ? White : Black;
            char lower = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
            PieceType type = None;
            switch (lower) {
            case 'p':
                type = Pawn;
                break;
            case 'n':
                type = Knight;
                break;
            case 'b':
                type = Bishop;
                break;
            case 'r':
                type = Rook;
                break;
            case 'q':
                type = Queen;
                break;
            case 'k':
                type = King;
                break;
            default:
                break;
            }
            squares[sq] = {type, color};
            file++;
        }
    }

    sideToMove = (side == "b") ? Black : White;

    castleWK = castleWQ = castleBK = castleBQ = false;
    for (char c : castling) {
        switch (c) {
        case 'K':
            castleWK = true;
            break;
        case 'Q':
            castleWQ = true;
            break;
        case 'k':
            castleBK = true;
            break;
        case 'q':
            castleBQ = true;
            break;
        default:
            break;
        }
    }

    if (ep != "-" && ep.size() >= 2) {
        enPassantSquare = stringToSquare(ep);
    } else {
        enPassantSquare = -1;
    }

    occupied = 0;
    byColor[White] = 0;
    byColor[Black] = 0;
    for (int i = 0; i < 7; i++)
        byPiece[i] = 0;
    for (int c = 0; c < 2; c++)
        for (int pt = 0; pt < 7; pt++)
            pieceCount[c][pt] = 0;
    for (int sq = 0; sq < 64; sq++) {
        if (squares[sq].type != None) {
            occupied |= 1ULL << sq;
            byColor[squares[sq].color] |= 1ULL << sq;
            byPiece[squares[sq].type] |= 1ULL << sq;
            pieceCount[squares[sq].color][squares[sq].type]++;
        }
    }

    computeKey();
    computePawnKey();
    computeMaterialKey();
    computeNonPawnKeys();
    computeMinorKey();
}

UndoInfo Board::makeMove(const Move &m) {
    UndoInfo undo;
    undo.enPassantSquare = enPassantSquare;
    undo.castleWK = castleWK;
    undo.castleWQ = castleWQ;
    undo.castleBK = castleBK;
    undo.castleBQ = castleBQ;
    undo.halfmoveClock = halfmoveClock;
    undo.key = key;
    undo.pawnKey = pawnKey;
    undo.materialKey = materialKey;
    undo.nonPawnKey[White] = nonPawnKey[White];
    undo.nonPawnKey[Black] = nonPawnKey[Black];
    undo.minorKey = minorKey;

    Piece moving = squares[m.from];
    Piece captured = squares[m.to];
    undo.captured = captured;

    // XOR out old castling and en passant state
    int oldCastleMask = castlingMask(castleWK, castleWQ, castleBK, castleBQ);
    key ^= zobrist::castling_keys[oldCastleMask];
    if (enPassantSquare != -1) {
        key ^= zobrist::en_passant_keys[squareFile(enPassantSquare)];
    }

    // XOR out moving piece from origin
    key ^= zobrist::piece_keys[moving.color][moving.type][m.from];
    if (moving.type == Pawn) {
        pawnKey ^= zobrist::piece_keys[moving.color][Pawn][m.from];
    } else {
        nonPawnKey[moving.color] ^= zobrist::piece_keys[moving.color][moving.type][m.from];
    }
    if (moving.type == Knight || moving.type == Bishop) {
        minorKey ^= zobrist::piece_keys[moving.color][moving.type][m.from];
    }

    // En passant capture
    if (moving.type == Pawn && m.to == enPassantSquare) {
        int capturedPawnSq = (moving.color == White) ? m.to - 8 : m.to + 8;
        key ^= zobrist::piece_keys[1 - moving.color][Pawn][capturedPawnSq];
        pawnKey ^= zobrist::piece_keys[1 - moving.color][Pawn][capturedPawnSq];
        squares[capturedPawnSq] = {None, White};
        occupied ^= 1ULL << capturedPawnSq;
        byColor[1 - moving.color] ^= 1ULL << capturedPawnSq;
        byPiece[Pawn] ^= 1ULL << capturedPawnSq;
        int &cnt = pieceCount[1 - moving.color][Pawn];
        materialKey ^= zobrist::material_keys[1 - moving.color][Pawn][cnt];
        cnt--;
        materialKey ^= zobrist::material_keys[1 - moving.color][Pawn][cnt];
    } else if (captured.type != None) {
        // Regular capture: remove captured piece from opponent's bitboard
        key ^= zobrist::piece_keys[captured.color][captured.type][m.to];
        if (captured.type == Pawn) {
            pawnKey ^= zobrist::piece_keys[captured.color][Pawn][m.to];
        } else {
            nonPawnKey[captured.color] ^= zobrist::piece_keys[captured.color][captured.type][m.to];
        }
        if (captured.type == Knight || captured.type == Bishop) {
            minorKey ^= zobrist::piece_keys[captured.color][captured.type][m.to];
        }
        occupied ^= 1ULL << m.to;
        byColor[captured.color] ^= 1ULL << m.to;
        byPiece[captured.type] ^= 1ULL << m.to;
        int &cnt = pieceCount[captured.color][captured.type];
        materialKey ^= zobrist::material_keys[captured.color][captured.type][cnt];
        cnt--;
        materialKey ^= zobrist::material_keys[captured.color][captured.type][cnt];
    }

    // Move the piece
    squares[m.to] = moving;
    squares[m.from] = {None, White};
    occupied ^= (1ULL << m.from) | (1ULL << m.to);
    byColor[moving.color] ^= (1ULL << m.from) | (1ULL << m.to);
    byPiece[moving.type] ^= (1ULL << m.from) | (1ULL << m.to);

    // Promotion
    if (m.promotion != None) {
        squares[m.to].type = m.promotion;
        byPiece[moving.type] ^= 1ULL << m.to;
        byPiece[m.promotion] |= 1ULL << m.to;
        int &pawnCnt = pieceCount[moving.color][Pawn];
        materialKey ^= zobrist::material_keys[moving.color][Pawn][pawnCnt];
        pawnCnt--;
        materialKey ^= zobrist::material_keys[moving.color][Pawn][pawnCnt];
        int &promoCnt = pieceCount[moving.color][m.promotion];
        materialKey ^= zobrist::material_keys[moving.color][m.promotion][promoCnt];
        promoCnt++;
        materialKey ^= zobrist::material_keys[moving.color][m.promotion][promoCnt];
    }

    // XOR in piece at destination (with promoted type if applicable)
    PieceType destType = (m.promotion != None) ? m.promotion : moving.type;
    key ^= zobrist::piece_keys[moving.color][destType][m.to];
    if (destType == Pawn) {
        pawnKey ^= zobrist::piece_keys[moving.color][Pawn][m.to];
    } else {
        nonPawnKey[moving.color] ^= zobrist::piece_keys[moving.color][destType][m.to];
    }
    if (destType == Knight || destType == Bishop) {
        minorKey ^= zobrist::piece_keys[moving.color][destType][m.to];
    }

    // Castling: move the rook
    if (moving.type == King) {
        int diff = m.to - m.from;
        if (diff == 2) {
            // Kingside
            key ^= zobrist::piece_keys[moving.color][Rook][m.from + 3];
            key ^= zobrist::piece_keys[moving.color][Rook][m.from + 1];
            nonPawnKey[moving.color] ^= zobrist::piece_keys[moving.color][Rook][m.from + 3];
            nonPawnKey[moving.color] ^= zobrist::piece_keys[moving.color][Rook][m.from + 1];
            squares[m.from + 1] = squares[m.from + 3];
            squares[m.from + 3] = {None, White};
            occupied ^= (1ULL << (m.from + 3)) | (1ULL << (m.from + 1));
            byColor[moving.color] ^= (1ULL << (m.from + 3)) | (1ULL << (m.from + 1));
            byPiece[Rook] ^= (1ULL << (m.from + 3)) | (1ULL << (m.from + 1));
        } else if (diff == -2) {
            // Queenside
            key ^= zobrist::piece_keys[moving.color][Rook][m.from - 4];
            key ^= zobrist::piece_keys[moving.color][Rook][m.from - 1];
            nonPawnKey[moving.color] ^= zobrist::piece_keys[moving.color][Rook][m.from - 4];
            nonPawnKey[moving.color] ^= zobrist::piece_keys[moving.color][Rook][m.from - 1];
            squares[m.from - 1] = squares[m.from - 4];
            squares[m.from - 4] = {None, White};
            occupied ^= (1ULL << (m.from - 4)) | (1ULL << (m.from - 1));
            byColor[moving.color] ^= (1ULL << (m.from - 4)) | (1ULL << (m.from - 1));
            byPiece[Rook] ^= (1ULL << (m.from - 4)) | (1ULL << (m.from - 1));
        }
    }

    // Update castling rights
    if (moving.type == King) {
        if (moving.color == White) {
            castleWK = false;
            castleWQ = false;
        } else {
            castleBK = false;
            castleBQ = false;
        }
    }
    if (moving.type == Rook) {
        if (m.from == 0) castleWQ = false;
        if (m.from == 7) castleWK = false;
        if (m.from == 56) castleBQ = false;
        if (m.from == 63) castleBK = false;
    }
    if (captured.type == Rook) {
        if (m.to == 0) castleWQ = false;
        if (m.to == 7) castleWK = false;
        if (m.to == 56) castleBQ = false;
        if (m.to == 63) castleBK = false;
    }

    // Update en passant square
    if (moving.type == Pawn && std::abs(m.to - m.from) == 16) {
        enPassantSquare = (m.from + m.to) / 2;
    } else {
        enPassantSquare = -1;
    }

    // XOR in new castling and en passant state
    int newCastleMask = castlingMask(castleWK, castleWQ, castleBK, castleBQ);
    key ^= zobrist::castling_keys[newCastleMask];
    if (enPassantSquare != -1) {
        key ^= zobrist::en_passant_keys[squareFile(enPassantSquare)];
    }

    // Update clocks
    if (moving.type == Pawn || captured.type != None) {
        halfmoveClock = 0;
    } else {
        halfmoveClock++;
    }
    if (sideToMove == Black) {
        fullmoveNumber++;
    }

    // Switch side and flip side-to-move key
    sideToMove = (sideToMove == White) ? Black : White;
    key ^= zobrist::side_to_move_key;

    return undo;
}

UndoInfo Board::makeNullMove() {
    UndoInfo undo;
    undo.captured = {None, White};
    undo.enPassantSquare = enPassantSquare;
    undo.castleWK = castleWK;
    undo.castleWQ = castleWQ;
    undo.castleBK = castleBK;
    undo.castleBQ = castleBQ;
    undo.halfmoveClock = halfmoveClock;
    undo.key = key;
    undo.pawnKey = pawnKey;
    undo.materialKey = materialKey;
    undo.nonPawnKey[White] = nonPawnKey[White];
    undo.nonPawnKey[Black] = nonPawnKey[Black];
    undo.minorKey = minorKey;

    // Clear en passant
    if (enPassantSquare != -1) {
        key ^= zobrist::en_passant_keys[squareFile(enPassantSquare)];
        enPassantSquare = -1;
    }

    // Flip side to move
    sideToMove = (sideToMove == White) ? Black : White;
    key ^= zobrist::side_to_move_key;

    halfmoveClock++;

    return undo;
}

void Board::unmakeNullMove(const UndoInfo &undo) {
    sideToMove = (sideToMove == White) ? Black : White;
    enPassantSquare = undo.enPassantSquare;
    castleWK = undo.castleWK;
    castleWQ = undo.castleWQ;
    castleBK = undo.castleBK;
    castleBQ = undo.castleBQ;
    halfmoveClock = undo.halfmoveClock;
    key = undo.key;
    pawnKey = undo.pawnKey;
    materialKey = undo.materialKey;
    nonPawnKey[White] = undo.nonPawnKey[White];
    nonPawnKey[Black] = undo.nonPawnKey[Black];
    minorKey = undo.minorKey;
}

void Board::unmakeMove(const Move &m, const UndoInfo &undo) {
    // Flip side back (makeMove flipped it at the end)
    sideToMove = (sideToMove == White) ? Black : White;
    Color us = sideToMove;

    // Determine piece types
    PieceType movedType = squares[m.to].type;
    PieceType originalType = (m.promotion != None) ? Pawn : movedType;

    // Undo castling rook movement
    if (originalType == King) {
        int diff = m.to - m.from;
        if (diff == 2) {
            // Kingside: rook moved from m.from+3 to m.from+1
            squares[m.from + 3] = squares[m.from + 1];
            squares[m.from + 1] = {None, White};
            occupied ^= (1ULL << (m.from + 3)) | (1ULL << (m.from + 1));
            byColor[us] ^= (1ULL << (m.from + 3)) | (1ULL << (m.from + 1));
            byPiece[Rook] ^= (1ULL << (m.from + 3)) | (1ULL << (m.from + 1));
        } else if (diff == -2) {
            // Queenside: rook moved from m.from-4 to m.from-1
            squares[m.from - 4] = squares[m.from - 1];
            squares[m.from - 1] = {None, White};
            occupied ^= (1ULL << (m.from - 4)) | (1ULL << (m.from - 1));
            byColor[us] ^= (1ULL << (m.from - 4)) | (1ULL << (m.from - 1));
            byPiece[Rook] ^= (1ULL << (m.from - 4)) | (1ULL << (m.from - 1));
        }
    }

    // Move the piece back
    squares[m.from] = {originalType, us};
    squares[m.to] = {None, White};
    if (m.promotion != None) {
        byPiece[movedType] ^= 1ULL << m.to;
        byPiece[Pawn] ^= 1ULL << m.from;
        pieceCount[us][movedType]--;
        pieceCount[us][Pawn]++;
    } else {
        byPiece[originalType] ^= (1ULL << m.to) | (1ULL << m.from);
    }
    occupied ^= (1ULL << m.from) | (1ULL << m.to);
    byColor[us] ^= (1ULL << m.from) | (1ULL << m.to);

    // Restore captured piece
    bool wasEnPassant =
        (originalType == Pawn && m.to == undo.enPassantSquare && undo.enPassantSquare != -1);
    if (wasEnPassant) {
        int capturedPawnSq = (us == White) ? m.to - 8 : m.to + 8;
        Color them = (us == White) ? Black : White;
        squares[capturedPawnSq] = {Pawn, them};
        occupied |= 1ULL << capturedPawnSq;
        byColor[them] |= 1ULL << capturedPawnSq;
        byPiece[Pawn] |= 1ULL << capturedPawnSq;
        pieceCount[them][Pawn]++;
    } else if (undo.captured.type != None) {
        squares[m.to] = undo.captured;
        occupied |= 1ULL << m.to;
        byColor[undo.captured.color] |= 1ULL << m.to;
        byPiece[undo.captured.type] |= 1ULL << m.to;
        pieceCount[undo.captured.color][undo.captured.type]++;
    }

    // Restore saved state
    enPassantSquare = undo.enPassantSquare;
    castleWK = undo.castleWK;
    castleWQ = undo.castleWQ;
    castleBK = undo.castleBK;
    castleBQ = undo.castleBQ;
    halfmoveClock = undo.halfmoveClock;
    key = undo.key;
    pawnKey = undo.pawnKey;
    materialKey = undo.materialKey;
    nonPawnKey[White] = undo.nonPawnKey[White];
    nonPawnKey[Black] = undo.nonPawnKey[Black];
    minorKey = undo.minorKey;
    if (us == Black) fullmoveNumber--;
}
