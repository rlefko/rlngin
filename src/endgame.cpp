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

void registerValueBothColors(int wp, int wn, int wb, int wr, int wq, ValueFn fn) {
    uint64_t kw = makeKey(wp, wn, wb, wr, wq, 0, 0, 0, 0, 0);
    uint64_t kb = makeKey(0, 0, 0, 0, 0, wp, wn, wb, wr, wq);
    g_valueMap[kw] = {fn, White};
    g_valueMap[kb] = {fn, Black};
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
    registerValueBothColors(0, 0, 0, 0, 1, evaluateKXK); // KQK
    registerValueBothColors(0, 0, 0, 1, 0, evaluateKXK); // KRK
    registerValueBothColors(0, 0, 0, 0, 2, evaluateKXK); // KQQK
    registerValueBothColors(0, 0, 0, 1, 1, evaluateKXK); // KQRK
    registerValueBothColors(0, 0, 0, 2, 0, evaluateKXK); // KRRK
    registerValueBothColors(0, 0, 1, 0, 1, evaluateKXK); // KQBK
    registerValueBothColors(0, 1, 0, 0, 1, evaluateKXK); // KQNK
    registerValueBothColors(0, 0, 1, 1, 0, evaluateKXK); // KRBK
    registerValueBothColors(0, 1, 0, 1, 0, evaluateKXK); // KRNK

    // KBNK uses the colored-corner gradient instead of the generic
    // edge push because the bishop only controls one corner color.
    registerValueBothColors(0, 1, 1, 0, 0, evaluateKBNK);
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
