#include "board.h"
#include "catch_amalgamated.hpp"
#include "eval.h"
#include "kpk_bitbase.h"

namespace {

// Helper that initializes the engine globals so the bitbase build runs
// before any probe. evaluate() drives ensureEvalInit() which in turn
// triggers Kpk::init via the endgame module scaffold.
int ensureInit() {
    Board b;
    return evaluate(b);
}

} // namespace

TEST_CASE("KPK bitbase: lone pawn on rook file is drawn against a king on the promotion corner",
          "[endgame][kpk]") {
    ensureInit();
    // White pawn on a2, white king on a1, black king on a8, white to move.
    // The defender already sits on the promotion square so no race
    // outcome favors white.
    int wp = 8 + 0;      // a2
    int wk = 0 + 0;      // a1
    int bk = 56 + 0;     // a8
    CHECK_FALSE(Kpk::probe(White, wk, wp, bk, White));
    CHECK_FALSE(Kpk::probe(White, wk, wp, bk, Black));
}

TEST_CASE("KPK bitbase: pawn one push from queening with the strong king covering the corner is won",
          "[endgame][kpk]") {
    ensureInit();
    // White pawn on a7, white king on c7, black king on a5, white to move.
    // White promotes next move; the defender cannot interpose because it
    // is three king-steps away from a8.
    int wp = 48 + 0;     // a7
    int wk = 48 + 2;     // c7
    int bk = 32 + 0;     // a5
    CHECK(Kpk::probe(White, wk, wp, bk, White));
}

TEST_CASE("KPK bitbase: black-strong polarity mirrors the white-strong result",
          "[endgame][kpk]") {
    ensureInit();
    // Same shape as the winning test above, mirrored vertically. Black
    // pawn on a2, black king on c2, white king on a4, black to move.
    int bp = 8 + 0;      // a2
    int bk = 8 + 2;      // c2
    int wk = 24 + 0;     // a4
    CHECK(Kpk::probe(Black, bk, bp, wk, Black));
}

TEST_CASE("KPK bitbase: defender stalemate from rook-file corner is drawn", "[endgame][kpk]") {
    ensureInit();
    // White pawn a7, white king a6, black king a8, black to move.
    // The classical corner stalemate trap: black to move has no legal
    // squares and is not in check, so the position is a draw.
    int wp = 48 + 0;     // a7
    int wk = 40 + 0;     // a6
    int bk = 56 + 0;     // a8
    CHECK_FALSE(Kpk::probe(White, wk, wp, bk, Black));
}
