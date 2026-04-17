#ifndef TYPES_H
#define TYPES_H

#include <cstdint>
#include <string>

enum Color { White, Black };

enum PieceType { None, Pawn, Knight, Bishop, Rook, Queen, King };

// Packed tapered score: upper 16 bits hold the endgame value, lower 16 hold
// the middlegame value. Storing both halves in a single int32 lets the eval
// accumulate midgame and endgame contributions with one add per term.
using Score = int32_t;

constexpr Score make_score(int mg, int eg) {
    return static_cast<Score>(static_cast<int32_t>(static_cast<uint32_t>(eg) << 16) + mg);
}

#define S(mg, eg) make_score((mg), (eg))

inline int mg_value(Score s) {
    union {
        uint16_t u;
        int16_t v;
    } r = {static_cast<uint16_t>(static_cast<uint32_t>(s))};
    return static_cast<int>(r.v);
}

inline int eg_value(Score s) {
    // Adding 0x8000 before the shift compensates for borrow from a negative
    // mg half so the eg half is extracted with the correct sign.
    union {
        uint16_t u;
        int16_t v;
    } r = {static_cast<uint16_t>(static_cast<uint32_t>(s + 0x8000) >> 16)};
    return static_cast<int>(r.v);
}

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
constexpr int MAX_PLY = 128;

#endif
