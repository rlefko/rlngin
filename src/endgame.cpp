#include "endgame.h"

#include "bitboard.h"
#include "eval_params.h"
#include "kpk_bitbase.h"
#include "zobrist.h"

#include <cstdlib>
#include <unordered_map>

namespace Endgame {

namespace {

// Material-key registries populated once in init(). Lookups are read-only
// after init() returns, so no synchronization is required on probe.
std::unordered_map<uint64_t, ValueEntry> g_valueMap;
std::unordered_map<uint64_t, ScaleEntry> g_scaleMap;

// Compute the same material zobrist key Board::computeMaterialKey
// produces for a position with the given piece counts. The argument
// ordering follows the natural piece progression (pawn, knight, bishop,
// rook, queen) for each color; the king count is always 1 and is folded
// in implicitly via material_keys[c][King][1].
uint64_t makeKey(int wp, int wn, int wb, int wr, int wq, int bp, int bn, int bb, int br, int bq) {
    uint64_t k = 0;
    k ^= zobrist::material_keys[White][Pawn][wp];
    k ^= zobrist::material_keys[White][Knight][wn];
    k ^= zobrist::material_keys[White][Bishop][wb];
    k ^= zobrist::material_keys[White][Rook][wr];
    k ^= zobrist::material_keys[White][Queen][wq];
    k ^= zobrist::material_keys[White][King][1];
    k ^= zobrist::material_keys[Black][Pawn][bp];
    k ^= zobrist::material_keys[Black][Knight][bn];
    k ^= zobrist::material_keys[Black][Bishop][bb];
    k ^= zobrist::material_keys[Black][Rook][br];
    k ^= zobrist::material_keys[Black][Queen][bq];
    k ^= zobrist::material_keys[Black][King][1];
    return k;
}

// Geometric helpers shared by every evaluator in this module. Each one
// returns a non-negative integer in 0..7 so the caller can multiply by a
// tunable per-square weight to produce a continuous gradient.
inline int pushToEdge(int sq) {
    int file = squareFile(sq);
    int rank = squareRank(sq);
    int fileDist = std::min(file, 7 - file);
    int rankDist = std::min(rank, 7 - rank);
    return 7 - std::min(fileDist, rankDist);
}

// Distance metric toward the two corners that match the given square
// color (light or dark). Used by KBNK and any future endgame that
// drives the lone king toward bishop-controllable corners.
inline int pushToColoredCorner(int sq, bool light) {
    int c1 = light ? 7 : 0;
    int c2 = light ? 56 : 63;
    int d = std::min(chebyshev(sq, c1), chebyshev(sq, c2));
    return 7 - d;
}

inline int pushClose(int a, int b) {
    return 7 - chebyshev(a, b);
}

// Sum the eg-side material value of every non-king, non-pawn piece on
// the given color. The lone-king side has none of these, so this is the
// material excess that drives the absolute eval of a KXK position.
int strongMaterialEg(const Board &board, Color strongSide) {
    int v = 0;
    for (int pt = Knight; pt <= Queen; pt++) {
        v += board.pieceCount[strongSide][pt] * eg_value(evalParams.PieceScore[pt]);
    }
    return v;
}

int evaluateKXK(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);

    int materialEg = strongMaterialEg(board, strongSide);
    int gradient = pushToEdge(weakKing) * eg_value(evalParams.KXKPushToEdge) +
                   pushClose(strongKing, weakKing) * eg_value(evalParams.KXKPushClose);
    int whitePov = materialEg + gradient;
    return (strongSide == White) ? whitePov : -whitePov;
}

// K + B + pawns vs K. The textbook wrong-rook-pawn fortress applies
// when every strong-side pawn sits on a single rook file and the
// bishop cannot control the corresponding promotion corner. The
// defender king holds the draw by reaching the corner before the lead
// pawn queens; any other configuration evaluates normally.
ScaleResult scaleKBPsK(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[strongSide];
    Bitboard ourBishop = board.byPiece[Bishop] & board.byColor[strongSide];
    if (!ourPawns || !ourBishop) return {64, 0};

    Bitboard rookFilePawns = ourPawns & (FileABB | FileHBB);
    if (rookFilePawns != ourPawns) return {64, 0};
    bool onA = (ourPawns & FileABB) != 0;
    bool onH = (ourPawns & FileHBB) != 0;
    if (onA && onH) return {64, 0};

    int promoSq = onA ? (strongSide == White ? 56 : 0) : (strongSide == White ? 63 : 7);
    bool promoLight = (squareBB(promoSq) & LightSquaresBB) != 0;
    bool bishopLight = (ourBishop & LightSquaresBB) != 0;
    if (promoLight == bishopLight) return {64, 0};

    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int leadPawn = (strongSide == White) ? msb(ourPawns) : lsb(ourPawns);
    int pushes = (strongSide == White) ? (7 - squareRank(leadPawn)) : squareRank(leadPawn);
    if (chebyshev(weakKing, promoSq) <= pushes) return {0, 0};
    return {64, 0};
}

// K + B + P vs K + B. Same-colored bishops give the pawn side the
// usual winning chances; opposite-colored bishops drop the scale far
// toward a draw because the defender can erect a blockade the pawn
// cannot break without bishop support on the right diagonal.
ScaleResult scaleKBPKB(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    Bitboard strongBishop = board.byPiece[Bishop] & board.byColor[strongSide];
    Bitboard weakBishop = board.byPiece[Bishop] & board.byColor[weakSide];
    bool strongLight = (strongBishop & LightSquaresBB) != 0;
    bool weakLight = (weakBishop & LightSquaresBB) != 0;
    if (strongLight == weakLight) return {64, 0};
    return {16, 0};
}

// K + B + 2 P vs K + B. Two-pawn opposite-bishop endings are drawish
// when the pawns are connected or close together; widely separated
// pawns leave the bishop unable to stop both, lifting the scale.
ScaleResult scaleKBPPKB(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    Bitboard strongBishop = board.byPiece[Bishop] & board.byColor[strongSide];
    Bitboard weakBishop = board.byPiece[Bishop] & board.byColor[weakSide];
    bool strongLight = (strongBishop & LightSquaresBB) != 0;
    bool weakLight = (weakBishop & LightSquaresBB) != 0;
    if (strongLight == weakLight) return {64, 0};

    Bitboard strongPawns = board.byPiece[Pawn] & board.byColor[strongSide];
    int p1 = lsb(strongPawns);
    int p2 = msb(strongPawns);
    int fileDiff = std::abs(squareFile(p1) - squareFile(p2));
    return {fileDiff <= 1 ? 16 : 38, 0};
}

// K + Q vs K + R + pawns. Drawish when the defender keeps the rook on
// its third rank backed by a pawn wall and the king tucked away on the
// first two ranks. The queen cannot pry the rook free of the defender
// king without the defender pawns abandoning the fortress.
ScaleResult scaleKQKRPs(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int weakRook = lsb(board.byPiece[Rook] & board.byColor[weakSide]);
    int weakRookRelRank = relativeRank(weakSide, weakRook);
    int weakKingRelRank = relativeRank(weakSide, weakKing);
    if (weakRookRelRank == 2 && weakKingRelRank <= 1) {
        return {0, 0};
    }
    return {64, 0};
}

// K + R + P vs K + R. Recognizes the three named patterns:
//
//   * Philidor third-rank defense: defender rook on the fifth rank
//     (attacker POV) with the attacker pawn no further than rank 4.
//     The textbook draw - return scale zero.
//   * Lucena bridge: attacker king on rank 7 or 8 in front of the
//     pawn on rank 6, defender king cut off two files away, pawn
//     off the rook file. Winning by bridge-building: scale stays at
//     64 and the LucenaEg additive nudges the search toward the
//     conversion plan.
//   * Otherwise: full scale; the natural eg gradient and the
//     attacker's material edge drive the search.
ScaleResult scaleKRPKR(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int strongPawn = lsb(board.byPiece[Pawn] & board.byColor[strongSide]);
    int weakRook = lsb(board.byPiece[Rook] & board.byColor[weakSide]);

    int pawnRelRank = relativeRank(strongSide, strongPawn);
    int weakRookRelRank = relativeRank(strongSide, weakRook);
    int weakKingRelRank = relativeRank(strongSide, weakKing);

    // Philidor third-rank defense: defender's rook sits on its own
    // third rank (= attacker's sixth, relative rank 5) while the
    // attacker pawn is still no further than rank 4 and the defender
    // king hangs back on the seventh or eighth rank.
    if (pawnRelRank >= 1 && pawnRelRank <= 4 && weakRookRelRank == 5 && weakKingRelRank >= 6) {
        return {0, 0};
    }

    // Lucena bridge configuration.
    if (pawnRelRank == 5) {
        int strongKingRelRank = relativeRank(strongSide, strongKing);
        int pawnFile = squareFile(strongPawn);
        int strongKingFile = squareFile(strongKing);
        int weakKingFile = squareFile(weakKing);
        bool kingInFront = strongKingRelRank >= 6 && std::abs(strongKingFile - pawnFile) <= 1;
        bool cutOff = std::abs(weakKingFile - pawnFile) >= 2;
        bool centerFile = pawnFile != 0 && pawnFile != 7;
        if (kingInFront && cutOff && centerFile) {
            int adjust = eg_value(evalParams.LucenaEg);
            if (strongSide == Black) adjust = -adjust;
            return {64, adjust};
        }
    }

    return {64, 0};
}

// K + R + P vs K + B. The defender holds if the bishop controls the
// promotion square and the defender king is close enough to block the
// pawn's advance.
ScaleResult scaleKRPKB(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongPawn = lsb(board.byPiece[Pawn] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    Bitboard weakBishop = board.byPiece[Bishop] & board.byColor[weakSide];

    int pawnFile = squareFile(strongPawn);
    int promoSq = (strongSide == White) ? (56 + pawnFile) : pawnFile;
    bool promoLight = (squareBB(promoSq) & LightSquaresBB) != 0;
    bool bishopLight = (weakBishop & LightSquaresBB) != 0;
    if (bishopLight == promoLight && chebyshev(weakKing, promoSq) <= 3) {
        return {8, 0};
    }
    return {64, 0};
}

// K + R + 2 P vs K + R + P. Drawish when neither attacker pawn is
// passed and all three pawns sit on the same flank: the defender rook
// keeps the position from progressing despite the material edge.
ScaleResult scaleKRPPKRP(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    Bitboard strongPawns = board.byPiece[Pawn] & board.byColor[strongSide];
    Bitboard weakPawns = board.byPiece[Pawn] & board.byColor[weakSide];

    // Files in 0..7. Track flanks: kingside (4..7) and queenside (0..3).
    bool anyKingside = false;
    bool anyQueenside = false;
    Bitboard allPawns = strongPawns | weakPawns;
    while (allPawns) {
        int sq = popLsb(allPawns);
        if (squareFile(sq) >= 4) anyKingside = true; else anyQueenside = true;
    }
    if (anyKingside && anyQueenside) return {64, 0};

    // Both attacker pawns must be unpassed for the drawish slope.
    Bitboard sp = strongPawns;
    bool hasPasser = false;
    while (sp) {
        int p = popLsb(sp);
        int pFile = squareFile(p);
        int pRank = squareRank(p);
        // A passer has no enemy pawn ahead on the same or adjacent file.
        bool passer = true;
        Bitboard wp = weakPawns;
        while (wp) {
            int q = popLsb(wp);
            if (std::abs(squareFile(q) - pFile) > 1) continue;
            int qRank = squareRank(q);
            bool ahead =
                (strongSide == White) ? (qRank > pRank) : (qRank < pRank);
            if (ahead) {
                passer = false;
                break;
            }
        }
        if (passer) {
            hasPasser = true;
            break;
        }
    }
    if (!hasPasser) return {32, 0};
    return {64, 0};
}

// K + B + P vs K + N. The bishop side usually wins but the knight can
// blockade the pawn from a defended square, especially when the
// defender king also sits in front of the pawn. The scale drops in
// that recognized blockade shape and otherwise keeps the full eg.
ScaleResult scaleKBPKN(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int pawn = lsb(board.byPiece[Pawn] & board.byColor[strongSide]);
    int knight = lsb(board.byPiece[Knight] & board.byColor[weakSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int pushSq = pawn + (strongSide == White ? 8 : -8);
    if (pushSq >= 0 && pushSq < 64 &&
        (knight == pushSq || weakKing == pushSq) &&
        chebyshev(weakKing, pawn) <= 2) {
        return {16, 0};
    }
    return {64, 0};
}

// K + pawns vs K. Multi-pawn extension of the rook-file fortress: if
// every strong-side pawn sits on a single rook file and the defender
// king reaches the promotion corner before the lead pawn queens, the
// position is drawn even without a bishop on the wrong color.
ScaleResult scaleKPsK(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[strongSide];
    if (!ourPawns) return {64, 0};

    Bitboard rookFilePawns = ourPawns & (FileABB | FileHBB);
    if (rookFilePawns != ourPawns) return {64, 0};
    bool onA = (ourPawns & FileABB) != 0;
    bool onH = (ourPawns & FileHBB) != 0;
    if (onA && onH) return {64, 0};

    int promoSq = onA ? (strongSide == White ? 56 : 0) : (strongSide == White ? 63 : 7);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int leadPawn = (strongSide == White) ? msb(ourPawns) : lsb(ourPawns);
    int pushes = (strongSide == White) ? (7 - squareRank(leadPawn)) : squareRank(leadPawn);
    if (chebyshev(weakKing, promoSq) <= pushes) return {0, 0};
    return {64, 0};
}

// K + P vs K + P. Each side wants to promote first; the KPK bitbase
// answers each side's race independently. If neither race wins, the
// position is drawn and the eg collapses; otherwise the natural
// gradient decides.
ScaleResult scaleKPKP(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int strongPawn = lsb(board.byPiece[Pawn] & board.byColor[strongSide]);
    int weakPawn = lsb(board.byPiece[Pawn] & board.byColor[weakSide]);

    bool strongWinsRace = Kpk::probe(strongSide, strongKing, strongPawn, weakKing,
                                     board.sideToMove);
    bool weakWinsRace = Kpk::probe(weakSide, weakKing, weakPawn, strongKing, board.sideToMove);
    if (strongWinsRace || weakWinsRace) return {64, 0};
    return {0, 0};
}

// KPK is wired as a scale evaluator: a bitbase WIN keeps the full
// endgame gradient (material plus KingPawnDistEg plus the natural pawn
// structure terms drive the conversion plan) while a bitbase DRAW
// collapses eg to zero so the engine stops chasing illusory wins.
ScaleResult scaleKPK(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int pawn = lsb(board.byPiece[Pawn] & board.byColor[strongSide]);
    bool win = Kpk::probe(strongSide, strongKing, pawn, weakKing, board.sideToMove);
    return {win ? 64 : 0, 0};
}

// K + R vs K + P. The result hinges on whether the strong king (or the
// rook) can intervene before the lone pawn promotes. The classical
// decomposition into four cases handles each of "strong king in front
// of the pawn", "weak king too far to defend", "advanced supported
// pawn", and a graduated middle ground that combines king-distance and
// pawn-rank terms into a continuous score.
int evaluateKRKP(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int rook = lsb(board.byPiece[Rook] & board.byColor[strongSide]);
    int pawn = lsb(board.byPiece[Pawn] & board.byColor[weakSide]);

    int pawnFile = squareFile(pawn);
    int queeningSq = (weakSide == White) ? (56 + pawnFile) : pawnFile;
    int pushDir = (weakSide == White) ? 8 : -8;
    int pushSq = pawn + pushDir;
    int rookEg = eg_value(evalParams.PieceScore[Rook]);

    int weakStm = (board.sideToMove == weakSide) ? 1 : 0;
    int strongStm = (board.sideToMove == strongSide) ? 1 : 0;

    bool inFrontOfPawn;
    if (weakSide == White) {
        inFrontOfPawn = squareFile(strongKing) == pawnFile &&
                        squareRank(strongKing) > squareRank(pawn);
    } else {
        inFrontOfPawn = squareFile(strongKing) == pawnFile &&
                        squareRank(strongKing) < squareRank(pawn);
    }

    int result;
    if (inFrontOfPawn) {
        result = rookEg - chebyshev(strongKing, pawn);
    } else if (chebyshev(weakKing, pawn) >= 3 + weakStm && chebyshev(weakKing, rook) >= 3) {
        result = rookEg - chebyshev(strongKing, pawn);
    } else if (relativeRank(weakSide, weakKing) >= 5 && chebyshev(weakKing, pawn) == 1 &&
               relativeRank(weakSide, strongKing) <= 4 &&
               chebyshev(strongKing, pawn) > 2 + strongStm) {
        result = 80 - 8 * chebyshev(strongKing, pawn);
    } else {
        result = 200 - 8 * (chebyshev(strongKing, pushSq) - chebyshev(weakKing, pushSq) -
                            chebyshev(pawn, queeningSq));
    }

    return (strongSide == White) ? result : -result;
}

// K + R vs K + B. The defender holds with reasonable play; the score is
// near zero with a token slope toward the edge to keep the search
// pointed at the strongest defensive setup rather than wandering.
int evaluateKRKB(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int gradient = pushToEdge(weakKing) * eg_value(evalParams.KXKPushToEdge) / 4;
    int whitePov = gradient;
    return (strongSide == White) ? whitePov : -whitePov;
}

// K + R vs K + N. Less drawish than KRKB because the knight is much
// less mobile than a bishop. The evaluator rewards separating the weak
// king from its knight so the rook can pick off the knight after a
// king sequence drives the defender into a corner.
int evaluateKRKN(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int knight = lsb(board.byPiece[Knight] & board.byColor[weakSide]);
    int edgePush = pushToEdge(weakKing) * eg_value(evalParams.KXKPushToEdge) / 3;
    int separation = chebyshev(weakKing, knight) * eg_value(evalParams.KXKPushClose) / 2;
    int whitePov = edgePush + separation;
    return (strongSide == White) ? whitePov : -whitePov;
}

// K + Q vs K + R. Queen wins with technique: drive the weak king to the
// edge so the queen can pry the defender's rook from king support, then
// pick it off. The eval encodes the material excess together with the
// per-square edge push and kings-together gradients tuned specifically
// for this material configuration.
int evaluateKQKR(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);

    int matEg = eg_value(evalParams.PieceScore[Queen]) - eg_value(evalParams.PieceScore[Rook]);
    int edgePush = pushToEdge(weakKing) * eg_value(evalParams.KQKRPushToEdge);
    int closePush = pushClose(strongKing, weakKing) * eg_value(evalParams.KQKRPushClose);
    int whitePov = matEg + edgePush + closePush;
    return (strongSide == White) ? whitePov : -whitePov;
}

// K + Q vs K + P. The queen wins almost everywhere; the exception is a
// rook-file pawn one push from promotion with the defender king
// blockading the corner and the attacker king too far to dislodge it.
// The fortress check here subsumes the inline rule in scaleFactor.
int evaluateKQKP(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int pawn = lsb(board.byPiece[Pawn] & board.byColor[weakSide]);

    int pawnFile = squareFile(pawn);
    int pawnRelRank = relativeRank(weakSide, pawn);

    // Rook-file pawn fortress: defender king on or next to the
    // promotion square and the attacker king at least four king-steps
    // away. Returning zero in this slim band of positions stops the
    // search from converting a textbook draw into an imagined win.
    if (pawnRelRank == 6 && (pawnFile == 0 || pawnFile == 7)) {
        int promoSq = (weakSide == White) ? (56 + pawnFile) : pawnFile;
        if (chebyshev(weakKing, promoSq) <= 1 && chebyshev(strongKing, promoSq) > 3) {
            return 0;
        }
    }

    int matEg = eg_value(evalParams.PieceScore[Queen]) - eg_value(evalParams.PieceScore[Pawn]);
    int gradient = pushClose(strongKing, weakKing) * eg_value(evalParams.KXKPushClose);
    int whitePov = matEg + gradient;
    return (strongSide == White) ? whitePov : -whitePov;
}

// K + N + N vs K is a draw because two knights without a defender pawn
// cannot force mate against optimal defense. Return zero so the search
// stops chasing the imagined material advantage.
int evaluateKNNK(const Board &board, Color strongSide) {
    (void)board;
    (void)strongSide;
    return 0;
}

// K + N + N vs K + P. The Troitsky-line theory gives winning chances
// only when the defender pawn is suitably restrained, but the band of
// winning positions is narrow and search-only conversion is unreliable.
// Apply a token edge-push gradient toward driving the defender king to
// a corner without overcommitting to a material-based win signal.
int evaluateKNNKP(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int gradient = pushToEdge(weakKing) * eg_value(evalParams.KXKPushToEdge) / 4;
    int whitePov = gradient;
    return (strongSide == White) ? whitePov : -whitePov;
}

int evaluateKBNK(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    Bitboard strongBishop = board.byPiece[Bishop] & board.byColor[strongSide];
    bool bishopLight = (strongBishop & LightSquaresBB) != 0;

    int materialEg = strongMaterialEg(board, strongSide);
    int gradient = pushToColoredCorner(weakKing, bishopLight) * eg_value(evalParams.KBNKCornerEg) +
                   pushClose(strongKing, weakKing) * eg_value(evalParams.KBNKPushClose);
    int whitePov = materialEg + gradient;
    return (strongSide == White) ? whitePov : -whitePov;
}

void registerValue(int wp, int wn, int wb, int wr, int wq, int bp, int bn, int bb, int br, int bq,
                   ValueFn fn) {
    uint64_t kw = makeKey(wp, wn, wb, wr, wq, bp, bn, bb, br, bq);
    uint64_t kb = makeKey(bp, bn, bb, br, bq, wp, wn, wb, wr, wq);
    g_valueMap[kw] = {fn, White};
    g_valueMap[kb] = {fn, Black};
}

void registerValueVsLoneKing(int wp, int wn, int wb, int wr, int wq, ValueFn fn) {
    registerValue(wp, wn, wb, wr, wq, 0, 0, 0, 0, 0, fn);
}

void registerScale(int wp, int wn, int wb, int wr, int wq, int bp, int bn, int bb, int br, int bq,
                   ScaleFn fn) {
    uint64_t kw = makeKey(wp, wn, wb, wr, wq, bp, bn, bb, br, bq);
    uint64_t kb = makeKey(bp, bn, bb, br, bq, wp, wn, wb, wr, wq);
    g_scaleMap[kw] = {fn, White};
    g_scaleMap[kb] = {fn, Black};
}

bool g_initialized = false;

} // namespace

void init() {
    if (g_initialized) return;
    g_initialized = true;
    Kpk::init();

    // Generic mating material against a lone king. Each registration
    // covers both color polarities so dispatch is symmetric. The
    // configurations enumerated here are the material excesses that
    // suffice to mate without help from any pawn structure; rarer
    // pile-ons like K + 3Q vs K stay on the natural gradient because
    // the material alone already drives the search.
    registerValueVsLoneKing(0, 0, 0, 0, 1, evaluateKXK); // KQK
    registerValueVsLoneKing(0, 0, 0, 1, 0, evaluateKXK); // KRK
    registerValueVsLoneKing(0, 0, 0, 0, 2, evaluateKXK); // KQQK
    registerValueVsLoneKing(0, 0, 0, 1, 1, evaluateKXK); // KQRK
    registerValueVsLoneKing(0, 0, 0, 2, 0, evaluateKXK); // KRRK
    registerValueVsLoneKing(0, 0, 1, 0, 1, evaluateKXK); // KQBK
    registerValueVsLoneKing(0, 1, 0, 0, 1, evaluateKXK); // KQNK
    registerValueVsLoneKing(0, 0, 1, 1, 0, evaluateKXK); // KRBK
    registerValueVsLoneKing(0, 1, 0, 1, 0, evaluateKXK); // KRNK

    // KBNK uses the colored-corner gradient instead of the generic
    // edge push because the bishop only controls one corner color.
    registerValueVsLoneKing(0, 1, 1, 0, 0, evaluateKBNK);

    // K + R vs K + P: rook-vs-pawn race. The dispatched evaluator
    // weighs strong-king-vs-promotion-square distance against
    // weak-king support to discriminate wins from drawing races.
    registerValue(0, 0, 0, 1, 0, 1, 0, 0, 0, 0, evaluateKRKP);

    // K + R vs K + B: drawish. The rook side keeps a token edge but
    // the evaluator collapses the score so the search does not chase
    // illusory wins against a coordinated bishop and king.
    registerValue(0, 0, 0, 1, 0, 0, 0, 1, 0, 0, evaluateKRKB);

    // K + R vs K + N: drawish with a slight edge for the rook side
    // when the defending king and knight are separated. The evaluator
    // rewards driving the weak king to the edge and prying knight
    // support away.
    registerValue(0, 0, 0, 1, 0, 0, 1, 0, 0, 0, evaluateKRKN);

    // K + Q vs K + R: queen wins by technique. The dedicated evaluator
    // tracks edge push and king proximity rather than the generic KXK
    // gradient because the rook can defend more squares than a lone
    // king and the conversion plan is shaped by the rook's mobility.
    registerValue(0, 0, 0, 0, 1, 0, 0, 0, 1, 0, evaluateKQKR);

    // K + Q vs K + P: queen wins everywhere except the textbook
    // rook-file fortress with the defender king on the promotion
    // square and the attacker king too far away to drive it out.
    registerValue(0, 0, 0, 0, 1, 1, 0, 0, 0, 0, evaluateKQKP);

    // K + N + N vs K: pure draw (two knights cannot force mate against
    // the lone king without help from a defender pawn).
    registerValueVsLoneKing(0, 2, 0, 0, 0, evaluateKNNK);

    // K + N + N vs K + P: drawish with narrow winning chances; the
    // evaluator scores edge-push only.
    registerValue(0, 2, 0, 0, 0, 1, 0, 0, 0, 0, evaluateKNNKP);

    // K + P vs K: bitbase scaling. The strong pawn side keeps the full
    // eg gradient for winning bitbase entries and collapses to zero for
    // drawn entries. The previous rule-based rook-file fortress
    // becomes a strict subset of the bitbase coverage and is retired
    // from the inline scaleFactor path.
    registerScale(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, scaleKPK);

    // K + B + N pawns vs K with the wrong-colored bishop: register one
    // entry per pawn count so the dispatch covers single-pawn and
    // multi-pawn rook-file fortresses without falling back to the
    // inline rule.
    for (int n = 1; n <= 8; n++) {
        registerScale(n, 0, 1, 0, 0, 0, 0, 0, 0, 0, scaleKBPsK);
    }

    // K + B + P vs K + B (opposite or same-colored bishops).
    registerScale(1, 0, 1, 0, 0, 0, 0, 1, 0, 0, scaleKBPKB);

    // K + B + 2 P vs K + B.
    registerScale(2, 0, 1, 0, 0, 0, 0, 1, 0, 0, scaleKBPPKB);

    // K + B + P vs K + N.
    registerScale(1, 0, 1, 0, 0, 0, 1, 0, 0, 0, scaleKBPKN);

    // K + pawns vs K with multiple pawns: register two-through-eight
    // pawn variants. Single-pawn KPK is already handled by the bitbase
    // path above.
    for (int n = 2; n <= 8; n++) {
        registerScale(n, 0, 0, 0, 0, 0, 0, 0, 0, 0, scaleKPsK);
    }

    // K + P vs K + P race resolution via two bitbase probes.
    registerScale(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, scaleKPKP);

    // K + R + P vs K + R: Philidor third-rank defense and the
    // Lucena bridge configuration. The eg additive folded in by the
    // Lucena branch absorbs the inline LucenaEg path from
    // endgameEgAdjust.
    registerScale(1, 0, 0, 1, 0, 0, 0, 0, 1, 0, scaleKRPKR);

    // K + R + P vs K + B: rook-pawn-vs-bishop fortress when the
    // bishop controls the promotion square.
    registerScale(1, 0, 0, 1, 0, 0, 0, 1, 0, 0, scaleKRPKB);

    // K + R + 2 P vs K + R + P: drawish slope when the attacker has
    // no passer and all pawns sit on the same flank.
    registerScale(2, 0, 0, 1, 0, 1, 0, 0, 1, 0, scaleKRPPKRP);

    // K + Q vs K + R + pawns: register one entry per defender pawn
    // count so the dispatch covers single-pawn and multi-pawn
    // fortresses. The dispatch fires only for the drawish setup where
    // the defender's rook sits on its third rank with the king behind.
    for (int n = 1; n <= 8; n++) {
        registerScale(0, 0, 0, 0, 1, n, 0, 0, 1, 0, scaleKQKRPs);
    }
}

const ValueEntry *probeValue(uint64_t materialKey) {
    auto it = g_valueMap.find(materialKey);
    return it == g_valueMap.end() ? nullptr : &it->second;
}

const ScaleEntry *probeScale(uint64_t materialKey) {
    auto it = g_scaleMap.find(materialKey);
    return it == g_scaleMap.end() ? nullptr : &it->second;
}

} // namespace Endgame
