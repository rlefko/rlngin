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

// Returns true when the strong side has a bishop plus one or more
// pawns on a single rook file, the bishop is on the wrong color to
// control the promotion corner, and the defending king can reach the
// promotion square before the lead pawn queens. The textbook fortress
// holds regardless of any defender bishop, knight, or other minor that
// the position may carry, so the check is factored out and reused by
// every bishop-and-rook-file-pawns scale evaluator.
bool isWrongRookPawnFortress(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    Bitboard ourPawns = board.byPiece[Pawn] & board.byColor[strongSide];
    Bitboard ourBishop = board.byPiece[Bishop] & board.byColor[strongSide];
    if (!ourPawns || !ourBishop) return false;

    Bitboard rookFilePawns = ourPawns & (FileABB | FileHBB);
    if (rookFilePawns != ourPawns) return false;
    bool onA = (ourPawns & FileABB) != 0;
    bool onH = (ourPawns & FileHBB) != 0;
    if (onA && onH) return false;

    int promoSq = onA ? (strongSide == White ? 56 : 0) : (strongSide == White ? 63 : 7);
    bool promoLight = (squareBB(promoSq) & LightSquaresBB) != 0;
    bool bishopLight = (ourBishop & LightSquaresBB) != 0;
    if (promoLight == bishopLight) return false;

    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    // Race deadline is set by the furthest-back pawn: the defender king
    // needs to be in the corner by the time the trailing pawn finally
    // queens, not the leading pawn, because the strong side cannot push
    // the rear pawn through until every pawn ahead has cleared. lsb for
    // white gives the lowest-rank pawn; msb for black gives the highest-
    // rank pawn (lowest from black's perspective). Matches the legacy
    // inline rule's choice.
    int rearPawn = (strongSide == White) ? lsb(ourPawns) : msb(ourPawns);
    int pushes = (strongSide == White) ? (7 - squareRank(rearPawn)) : squareRank(rearPawn);
    return chebyshev(weakKing, promoSq) <= pushes;
}

// Unified bishop-and-pawns vs king-with-optional-bishop scale
// evaluator. Checks the textbook wrong-rook-pawn fortress first; if
// that fails and the defender holds a bishop, applies the same
// opposite-colored-bishop pawn-count damping the legacy inline rule
// used (scale 10 for one pawn, 26 for two or three, 38 for four or
// more). Otherwise the natural eg passes through unchanged.
ScaleResult scaleKBPsK(const Board &board, Color strongSide) {
    if (isWrongRookPawnFortress(board, strongSide)) return {0, 0};

    Color weakSide = (strongSide == White) ? Black : White;
    Bitboard weakBishop = board.byPiece[Bishop] & board.byColor[weakSide];
    if (!weakBishop) return {64, 0};

    Bitboard strongBishop = board.byPiece[Bishop] & board.byColor[strongSide];
    bool strongLight = (strongBishop & LightSquaresBB) != 0;
    bool weakLight = (weakBishop & LightSquaresBB) != 0;
    if (strongLight == weakLight) return {64, 0};

    int strongPawns = popcount(board.byPiece[Pawn] & board.byColor[strongSide]);
    if (strongPawns <= 1) return {10, 0};
    if (strongPawns <= 3) return {26, 0};
    return {38, 0};
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

    // Lucena bridge configuration: attacker pawn on relative rank 6
    // (chess rank 7 for white), attacker king on relative rank 7 (chess
    // rank 8) within one file of the pawn, defender king cut off two
    // files away, and the pawn off the a/h rook files. Matches the
    // legacy inline Lucena rule exactly.
    if (pawnRelRank == 6) {
        int strongKingRelRank = relativeRank(strongSide, strongKing);
        int pawnFile = squareFile(strongPawn);
        int strongKingFile = squareFile(strongKing);
        int weakKingFile = squareFile(weakKing);
        bool kingInFront = strongKingRelRank == 7 && std::abs(strongKingFile - pawnFile) <= 1;
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

// K + B + P vs K + N. The bishop side usually wins; the scale only
// drops when the defender king itself blockades the pawn's push square
// because a knight on the push square is normally captured by the
// bishop on the next move rather than holding a fortress.
ScaleResult scaleKBPKN(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int pawn = lsb(board.byPiece[Pawn] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int pushSq = pawn + (strongSide == White ? 8 : -8);
    if (pushSq >= 0 && pushSq < 64 && weakKing == pushSq) {
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

    bool strongWinsRace =
        Kpk::probe(strongSide, strongKing, strongPawn, weakKing, board.sideToMove);
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

// K + Q vs K + P. The queen wins almost everywhere; the exception is a
// rook-file pawn one push from promotion with the defender king
// blockading the corner and the attacker king too far to dislodge it.
// Matches the legacy inline KQKP fortress rule exactly: fortress yields
// scale zero, every other configuration leaves the natural eval intact.
ScaleResult scaleKQKP(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int pawn = lsb(board.byPiece[Pawn] & board.byColor[weakSide]);

    int pawnFile = squareFile(pawn);
    int pawnRelRank = relativeRank(weakSide, pawn);
    if (pawnRelRank == 6 && (pawnFile == 0 || pawnFile == 7)) {
        int promoSq = (weakSide == White) ? (56 + pawnFile) : pawnFile;
        if (chebyshev(weakKing, promoSq) <= 1 && chebyshev(strongKing, promoSq) > 3) {
            return {0, 0};
        }
    }
    return {64, 0};
}

// K + minor vs K + minor (pawnless). Any of KNKN, KBKB, KBKN, KNKB
// without pawns is a draw with proper defense. The shared scale
// evaluator returns zero to subsume the inline pawnless-minor-only
// recognizer in scaleFactor.
ScaleResult scaleMinorVsMinorDraw(const Board &board, Color strongSide) {
    (void)board;
    (void)strongSide;
    return {0, 0};
}

// K + B + N vs K. Drives the lone king toward the corner the bishop
// controls. Matches the legacy inline KBNK adjustment exactly: only
// KBNKCornerEg drives the gradient. Scale stays at 64 so the rest of
// the eg (material plus PST plus king-safety pressure on the lone
// king) passes through unchanged.
ScaleResult scaleKBNK(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    Bitboard strongBishop = board.byPiece[Bishop] & board.byColor[strongSide];
    bool bishopLight = (strongBishop & LightSquaresBB) != 0;
    int gradient = pushToColoredCorner(weakKing, bishopLight) * eg_value(evalParams.KBNKCornerEg);
    if (strongSide == Black) gradient = -gradient;
    return {64, gradient};
}

void registerScale(int wp, int wn, int wb, int wr, int wq, int bp, int bn, int bb, int br, int bq,
                   ScaleFn fn) {
    uint64_t kw = makeKey(wp, wn, wb, wr, wq, bp, bn, bb, br, bq);
    uint64_t kb = makeKey(bp, bn, bb, br, bq, wp, wn, wb, wr, wq);
    g_scaleMap[kw] = {fn, White};
    g_scaleMap[kb] = {fn, Black};
}

void registerScaleVsLoneKing(int wp, int wn, int wb, int wr, int wq, ScaleFn fn) {
    registerScale(wp, wn, wb, wr, wq, 0, 0, 0, 0, 0, fn);
}

bool g_initialized = false;

} // namespace

void init() {
    if (g_initialized) return;
    g_initialized = true;
    Kpk::init();

    // K + B + N vs K: route through the dedicated colored-corner
    // gradient. Mating material configurations without colored-corner
    // constraints (KQK, KRK, KQQK, KQRK, KRRK, KQBK, KQNK, KRBK, KRNK)
    // are left to the natural eval because the material excess alone
    // already drives the search; pre-bitbase did not apply any
    // dedicated lone-king push gradient for those material configs.
    registerScaleVsLoneKing(0, 1, 1, 0, 0, scaleKBNK);

    // K + Q vs K + P: queen wins everywhere except the textbook
    // rook-file fortress with the defender king on the promotion
    // square and the attacker king too far away to drive it out.
    registerScale(0, 0, 0, 0, 1, 1, 0, 0, 0, 0, scaleKQKP);

    // K + P vs K: bitbase scaling. The strong pawn side keeps the full
    // eg gradient for winning bitbase entries and collapses to zero for
    // drawn entries. The previous rule-based rook-file fortress
    // becomes a strict subset of the bitbase coverage and is retired
    // from the inline scaleFactor path.
    registerScale(1, 0, 0, 0, 0, 0, 0, 0, 0, 0, scaleKPK);

    // K + B + N pawns vs K (defender bare king): unified scaleKBPsK
    // handles the wrong-rook-pawn fortress check.
    for (int n = 1; n <= 8; n++) {
        registerScale(n, 0, 1, 0, 0, 0, 0, 0, 0, 0, scaleKBPsK);
    }

    // K + B + N pawns vs K + B (defender holds a bishop): same
    // unified evaluator, with OCB pawn-count damping folded in.
    for (int n = 1; n <= 8; n++) {
        registerScale(n, 0, 1, 0, 0, 0, 0, 1, 0, 0, scaleKBPsK);
    }

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

    // K + minor vs K: drawn with insufficient material. Scale zero
    // kills eg so the natural eg drops to zero while the mg residual
    // (mostly PSTs at low phase) survives, matching the legacy
    // pawnless-minor-only inline behavior.
    registerScaleVsLoneKing(0, 1, 0, 0, 0, scaleMinorVsMinorDraw); // KNK
    registerScaleVsLoneKing(0, 0, 1, 0, 0, scaleMinorVsMinorDraw); // KBK

    // K + minor vs K + minor (pawnless): drawn with proper defense.
    registerScale(0, 1, 0, 0, 0, 0, 1, 0, 0, 0, scaleMinorVsMinorDraw); // KNKN
    registerScale(0, 0, 1, 0, 0, 0, 0, 1, 0, 0, scaleMinorVsMinorDraw); // KBKB
    registerScale(0, 1, 0, 0, 0, 0, 0, 1, 0, 0, scaleMinorVsMinorDraw); // KNKB
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
