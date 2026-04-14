#ifndef TYPES_H
#define TYPES_H

#include <string>

enum Color { White, Black };

enum PieceType { None, Pawn, Knight, Bishop, Rook, Queen, King };

struct Piece {
    PieceType type = None;
    Color color = White;
};

struct Move {
    int from = 0;
    int to = 0;
    PieceType promotion = None;
};

inline int squareRank(int sq) {
    return sq / 8;
}
inline int squareFile(int sq) {
    return sq % 8;
}
inline int makeSquare(int rank, int file) {
    return rank * 8 + file;
}

inline std::string squareToString(int sq) {
    std::string s;
    s += static_cast<char>('a' + squareFile(sq));
    s += static_cast<char>('1' + squareRank(sq));
    return s;
}

inline int stringToSquare(const std::string &s) {
    if (s.size() < 2) return -1;
    int file = s[0] - 'a';
    int rank = s[1] - '1';
    if (file < 0 || file > 7 || rank < 0 || rank > 7) return -1;
    return makeSquare(rank, file);
}

inline std::string moveToString(const Move &m) {
    std::string s = squareToString(m.from) + squareToString(m.to);
    if (m.promotion != None) {
        const char promoChars[] = {' ', ' ', 'n', 'b', 'r', 'q', ' '};
        s += promoChars[m.promotion];
    }
    return s;
}

inline Move stringToMove(const std::string &s) {
    Move m;
    if (s.size() < 4) return m;
    m.from = stringToSquare(s.substr(0, 2));
    m.to = stringToSquare(s.substr(2, 2));
    if (s.size() > 4) {
        switch (s[4]) {
        case 'n':
            m.promotion = Knight;
            break;
        case 'b':
            m.promotion = Bishop;
            break;
        case 'r':
            m.promotion = Rook;
            break;
        case 'q':
            m.promotion = Queen;
            break;
        default:
            break;
        }
    }
    return m;
}

constexpr int MATE_SCORE = 30000;
constexpr int INF_SCORE = MATE_SCORE + 1;

#endif
