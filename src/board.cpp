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

    computeKey();
}

void Board::makeMove(const Move &m) {
    Piece moving = squares[m.from];
    Piece captured = squares[m.to];

    // XOR out old castling and en passant state
    int oldCastleMask = castlingMask(castleWK, castleWQ, castleBK, castleBQ);
    key ^= zobrist::castling_keys[oldCastleMask];
    if (enPassantSquare != -1) {
        key ^= zobrist::en_passant_keys[squareFile(enPassantSquare)];
    }

    // XOR out moving piece from origin
    key ^= zobrist::piece_keys[moving.color][moving.type][m.from];

    // En passant capture
    if (moving.type == Pawn && m.to == enPassantSquare) {
        int capturedPawnSq = (moving.color == White) ? m.to - 8 : m.to + 8;
        key ^= zobrist::piece_keys[1 - moving.color][Pawn][capturedPawnSq];
        squares[capturedPawnSq] = {None, White};
    } else if (captured.type != None) {
        // Regular capture
        key ^= zobrist::piece_keys[captured.color][captured.type][m.to];
    }

    // Move the piece
    squares[m.to] = moving;
    squares[m.from] = {None, White};

    // Promotion
    if (m.promotion != None) {
        squares[m.to].type = m.promotion;
    }

    // XOR in piece at destination (with promoted type if applicable)
    PieceType destType = (m.promotion != None) ? m.promotion : moving.type;
    key ^= zobrist::piece_keys[moving.color][destType][m.to];

    // Castling: move the rook
    if (moving.type == King) {
        int diff = m.to - m.from;
        if (diff == 2) {
            // Kingside
            key ^= zobrist::piece_keys[moving.color][Rook][m.from + 3];
            key ^= zobrist::piece_keys[moving.color][Rook][m.from + 1];
            squares[m.from + 1] = squares[m.from + 3];
            squares[m.from + 3] = {None, White};
        } else if (diff == -2) {
            // Queenside
            key ^= zobrist::piece_keys[moving.color][Rook][m.from - 4];
            key ^= zobrist::piece_keys[moving.color][Rook][m.from - 1];
            squares[m.from - 1] = squares[m.from - 4];
            squares[m.from - 4] = {None, White};
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
}
