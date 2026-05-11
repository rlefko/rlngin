#include "endgame.h"

#include "bitboard.h"
#include "eval_params.h"
#include "kpk_bitbase.h"
#include "zobrist.h"

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

    // K + P vs K: bitbase scaling. The strong pawn side keeps the full
    // eg gradient for winning bitbase entries and collapses to zero for
    // drawn entries. The previous rule-based rook-file fortress
    // becomes a strict subset of the bitbase coverage and is retired
    // from the inline scaleFactor path.
    registerScale(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, scaleKPK);
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
