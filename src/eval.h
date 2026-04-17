#ifndef EVAL_H
#define EVAL_H

#include "board.h"
#include "types.h"

#include <cstddef>

extern const int PieceValue[7];

// Internal eval grain per pawn. The material rescale landed with PR #28 put
// one pawn at ~228 internal units; UCI centipawns divide by this to recover
// the conventional 100 cp / pawn scale. Consumed by search's info output and
// by the eval command's tapered/cp columns.
constexpr int NormalizePawn = 228;

int evaluate(const Board &board);

// Per-term breakdown emitted by evaluateTraced. Terms are packed Scores with
// MG in the low half and EG in the high half (see types.h). White- and
// black-perspective entries are stored separately so consumers can render a
// side-by-side table; pawns stays as a single signed total because its hash
// cache stores only the combined white-minus-black figure.
struct EvalTrace {
    Score material[2];
    Score pst[2];
    Score pawns;
    Score pieces[2];
    Score space[2];
    Score kingSafety[2];
    int gamePhase;
    int halfmoveClock;
    Color sideToMove;
    int rawTapered;     // tapered score from White's POV, before 50-move scale
    int finalFromWhite; // post 50-move scale, still from White's POV
    int finalFromStm;   // final eval as returned to the caller
};

int evaluateTraced(const Board &board, EvalTrace &trace);

void clearPawnHash();
void setPawnHashSize(size_t mb);

void clearMaterialHash();
void setMaterialHashSize(size_t mb);

#endif
