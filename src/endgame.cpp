#include "endgame.h"

#include "bitboard.h"
#include "eval_params.h"
#include "kpk_bitbase.h"
#include "zobrist.h"

#include <cstdlib>
#include <unordered_map>

namespace Endgame {

namespace {

// Material-key registry populated once in init(). Lookups are read-only
// after init() returns, so no synchronization is required on probe.
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

// Closeness to the nearest board edge, returned as 0..7 where 7 is on
// the edge and 0 is on a central square. Used by the mating-conversion
// gradients (KXK, KQKR) to drive the lone king toward an edge so the
// search converges to mate inside the 50-move horizon.
inline int pushToEdge(int sq) {
    int file = squareFile(sq);
    int rank = squareRank(sq);
    int fileDist = std::min(file, 7 - file);
    int rankDist = std::min(rank, 7 - rank);
    return 7 - std::min(fileDist, rankDist);
}

// Closeness of two squares, returned as 0..7 where 7 is adjacent and 0
// is maximally separated. The mating gradients use this to bring the
// strong king close enough to deliver mate after driving the lone king
// to the edge.
inline int pushClose(int a, int b) {
    return 7 - chebyshev(a, b);
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
// evaluator. Mirrors the legacy inline rule's ordering: opposite-
// colored-bishop pawn-count damping fires first when the defender
// holds a bishop on the other color (scale 10 for one pawn, 26 for
// two or three, 38 for four or more), then the wrong-rook-pawn
// fortress check runs for the no-defender-bishop and same-colored-
// bishop branches. Anything else leaves the natural eg untouched.
ScaleResult scaleKBPsK(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    Bitboard weakBishop = board.byPiece[Bishop] & board.byColor[weakSide];

    if (weakBishop) {
        Bitboard strongBishop = board.byPiece[Bishop] & board.byColor[strongSide];
        bool strongLight = (strongBishop & LightSquaresBB) != 0;
        bool weakLight = (weakBishop & LightSquaresBB) != 0;
        if (strongLight != weakLight) {
            int strongPawns = popcount(board.byPiece[Pawn] & board.byColor[strongSide]);
            if (strongPawns <= 1) return {10, 0};
            if (strongPawns <= 3) return {26, 0};
            return {38, 0};
        }
    }

    if (isWrongRookPawnFortress(board, strongSide)) return {0, 0};
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

// K + pawns vs lone K, all pawns on a single rook file. The defender
// king holds the textbook draw if it reaches the promotion corner
// before the most-advanced pawn queens; defender-to-move gains one
// extra tempo. The strong king cannot break this fortress without
// support (the bishop's wrong color or, here, no bishop at all).
// Single-pawn KPK already routes through the bitbase, so this fires
// only for the multi-pawn extension.
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
    if (board.sideToMove == weakSide) pushes++;

    if (chebyshev(weakKing, promoSq) <= pushes) {
        return {eg_value(evalParams.KPsKFortressScale), 0};
    }
    return {64, 0};
}

// K + B + P vs K + N. The bishop side normally wins, but if the
// defender king sits on the pawn's push square AND the knight is
// close enough to support the king (within two squares), the
// blockade is hard to break without sacrificing the bishop. The
// tunable scale damps the eg in that pattern; everywhere else the
// natural eval flows through.
ScaleResult scaleKBPKN(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int pawn = lsb(board.byPiece[Pawn] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int weakKnight = lsb(board.byPiece[Knight] & board.byColor[weakSide]);
    int pushSq = pawn + (strongSide == White ? 8 : -8);
    if (pushSq >= 0 && pushSq < 64 && weakKing == pushSq && chebyshev(weakKnight, weakKing) <= 2) {
        return {eg_value(evalParams.KBPKNDrawishScale), 0};
    }
    return {64, 0};
}

// K + R vs K + P. The rook side normally wins, but if the pawn is
// already on its 6th or 7th rank (relative to the defender) with the
// defender king sitting next to it and the rook-side king more than
// three squares from the promotion square, the race tilts toward a
// draw because the rook alone cannot both attack the pawn and cover
// the promotion square while the defender king and pawn coordinate.
ScaleResult scaleKRKP(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int pawn = lsb(board.byPiece[Pawn] & board.byColor[weakSide]);
    int pawnRelRank = relativeRank(weakSide, pawn);
    int promoSq = (weakSide == White) ? (56 + squareFile(pawn)) : squareFile(pawn);

    if (pawnRelRank >= 5 && chebyshev(weakKing, pawn) <= 1 && chebyshev(strongKing, promoSq) > 3) {
        return {eg_value(evalParams.KRKPDrawishScale), 0};
    }
    return {64, 0};
}

// K + R vs K + B or K + R vs K + N. The rook side holds a material
// edge but lone-minor defenses with active king play frequently draw.
// A uniform drawish scale damps the eg so the engine does not chase
// the win through a sterile rook gradient.
ScaleResult scaleKRKMinor(const Board &board, Color strongSide) {
    (void)board;
    (void)strongSide;
    return {eg_value(evalParams.KRKMinorScale), 0};
}

// K + N + N vs lone K. Two knights cannot force mate against best
// defense, so the natural material edge of two minors is illusory.
// The tunable scale damps the eg toward zero.
ScaleResult scaleKNNK(const Board &board, Color strongSide) {
    (void)board;
    (void)strongSide;
    return {eg_value(evalParams.KNNKDrawScale), 0};
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
// without pawns is a draw with proper defense. KBKB with opposite-
// colored bishops keeps the legacy OCB damping magnitude of ten so
// the eg matches the inline rule it replaces; every other pawnless-
// minor configuration collapses fully to zero.
ScaleResult scaleMinorVsMinorDraw(const Board &board, Color strongSide) {
    (void)strongSide;
    if (board.pieceCount[White][Bishop] == 1 && board.pieceCount[Black][Bishop] == 1) {
        Bitboard whiteBishop = board.byPiece[Bishop] & board.byColor[White];
        Bitboard blackBishop = board.byPiece[Bishop] & board.byColor[Black];
        bool whiteLight = (whiteBishop & LightSquaresBB) != 0;
        bool blackLight = (blackBishop & LightSquaresBB) != 0;
        if (whiteLight != blackLight) return {10, 0};
    }
    return {0, 0};
}

// K + mating-material vs lone K (KQK, KRK, KQQK, KQRK, KRRK, KQBK,
// KQNK, KRBK, KRNK). Drives the lone king toward an edge and brings
// the strong king close enough to deliver mate. Without these
// gradients the search has no eg signal beyond the material delta,
// which is too flat for the 50-move horizon. Scale stays at 64 so the
// natural eg (material plus PSTs plus king-safety pressure on the
// lone king) passes through unchanged.
ScaleResult scaleKXK(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int gradient = pushToEdge(weakKing) * eg_value(evalParams.KXKPushToEdge) +
                   pushClose(strongKing, weakKing) * eg_value(evalParams.KXKPushClose);
    if (strongSide == Black) gradient = -gradient;
    return {64, gradient};
}

// K + B + N vs K. Drives the lone king toward the corner the bishop
// controls (KBNKCornerEg) and brings the strong king close enough to
// deliver mate (KBNKPushClose). The colored-corner term is the
// dominant signal; the kings-together term is added on top so the
// strong king is not stranded once the lone king reaches the corner.
// Scale stays at 64.
ScaleResult scaleKBNK(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    Bitboard strongBishop = board.byPiece[Bishop] & board.byColor[strongSide];
    bool bishopLight = (strongBishop & LightSquaresBB) != 0;
    int gradient = pushToColoredCorner(weakKing, bishopLight) * eg_value(evalParams.KBNKCornerEg) +
                   pushClose(strongKing, weakKing) * eg_value(evalParams.KBNKPushClose);
    if (strongSide == Black) gradient = -gradient;
    return {64, gradient};
}

// K + Q vs K + R. The queen wins but conversion is technical: drive
// the rook-side king toward the edge and bring the queen-side king
// close enough to deliver mate. Without these gradients the search
// often shuffles inside the 50-move horizon. Scale stays at 64.
ScaleResult scaleKQKR(const Board &board, Color strongSide) {
    Color weakSide = (strongSide == White) ? Black : White;
    int strongKing = lsb(board.byPiece[King] & board.byColor[strongSide]);
    int weakKing = lsb(board.byPiece[King] & board.byColor[weakSide]);
    int gradient = pushToEdge(weakKing) * eg_value(evalParams.KQKRPushToEdge) +
                   pushClose(strongKing, weakKing) * eg_value(evalParams.KQKRPushClose);
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

    // K + B + N vs K: route through the dedicated colored-corner plus
    // kings-together gradient.
    registerScaleVsLoneKing(0, 1, 1, 0, 0, scaleKBNK);

    // K + mating-material vs lone K: push-to-edge plus kings-together
    // gradients. Covers the common mating configurations so the
    // search has an eg pull toward checkmate beyond the raw material
    // delta. The natural eval has no such gradient; without it the
    // engine often shuffles inside the 50-move horizon when a clearly
    // winning material edge could have converted.
    registerScaleVsLoneKing(0, 0, 0, 0, 1, scaleKXK); // KQK
    registerScaleVsLoneKing(0, 0, 0, 1, 0, scaleKXK); // KRK
    registerScaleVsLoneKing(0, 0, 0, 0, 2, scaleKXK); // KQQK
    registerScaleVsLoneKing(0, 0, 0, 2, 0, scaleKXK); // KRRK
    registerScaleVsLoneKing(0, 0, 0, 1, 1, scaleKXK); // KQRK
    registerScaleVsLoneKing(0, 0, 1, 0, 1, scaleKXK); // KQBK
    registerScaleVsLoneKing(0, 1, 0, 0, 1, scaleKXK); // KQNK
    registerScaleVsLoneKing(0, 0, 1, 1, 0, scaleKXK); // KRBK
    registerScaleVsLoneKing(0, 1, 0, 1, 0, scaleKXK); // KRNK

    // K + Q vs K + R: push-to-edge plus kings-together for the
    // queen-vs-rook mating conversion.
    registerScale(0, 0, 0, 0, 1, 0, 0, 0, 1, 0, scaleKQKR);

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

    // K + R + P vs K + R: Philidor third-rank defense and the
    // Lucena bridge configuration. The eg additive folded in by the
    // Lucena branch absorbs the inline LucenaEg path from
    // endgameEgAdjust.
    registerScale(1, 0, 0, 1, 0, 0, 0, 0, 1, 0, scaleKRPKR);

    // K + B + P vs K + N: damps eg when the defender king blockades
    // the pawn's push square and the knight is close enough to support.
    registerScale(1, 0, 1, 0, 0, 0, 1, 0, 0, 0, scaleKBPKN);

    // K + R vs K + P: damps eg in the race-too-close-to-draw shape.
    registerScale(0, 0, 0, 1, 0, 1, 0, 0, 0, 0, scaleKRKP);

    // K + R vs K + minor: structurally drawish across the board.
    registerScale(0, 0, 0, 1, 0, 0, 0, 1, 0, 0, scaleKRKMinor); // KRKB
    registerScale(0, 0, 0, 1, 0, 0, 1, 0, 0, 0, scaleKRKMinor); // KRKN

    // K + N + N vs lone K: two knights cannot force mate.
    registerScaleVsLoneKing(0, 2, 0, 0, 0, scaleKNNK);

    // K + pawns vs lone K, multi-pawn rook-file fortress. Single-pawn
    // KPK already routes through the bitbase; register two-through-
    // eight pawn variants so the fortress recognizer fires for the
    // multi-pawn extension.
    for (int n = 2; n <= 8; n++) {
        registerScale(n, 0, 0, 0, 0, 0, 0, 0, 0, 0, scaleKPsK);
    }

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

const ScaleEntry *probeScale(uint64_t materialKey) {
    auto it = g_scaleMap.find(materialKey);
    return it == g_scaleMap.end() ? nullptr : &it->second;
}

} // namespace Endgame
