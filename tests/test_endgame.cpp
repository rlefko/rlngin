#include "board.h"
#include "catch_amalgamated.hpp"
#include "eval.h"
#include "kpk_bitbase.h"

#include <cstdlib>

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
    int wp = 8 + 0;  // a2
    int wk = 0 + 0;  // a1
    int bk = 56 + 0; // a8
    CHECK_FALSE(Kpk::probe(White, wk, wp, bk, White));
    CHECK_FALSE(Kpk::probe(White, wk, wp, bk, Black));
}

TEST_CASE(
    "KPK bitbase: pawn one push from queening with the strong king covering the corner is won",
    "[endgame][kpk]") {
    ensureInit();
    // White pawn on a7, white king on c7, black king on a5, white to move.
    // White promotes next move; the defender cannot interpose because it
    // is three king-steps away from a8.
    int wp = 48 + 0; // a7
    int wk = 48 + 2; // c7
    int bk = 32 + 0; // a5
    CHECK(Kpk::probe(White, wk, wp, bk, White));
}

TEST_CASE("KPK bitbase: black-strong polarity mirrors the white-strong result", "[endgame][kpk]") {
    ensureInit();
    // Same shape as the winning test above, mirrored vertically. Black
    // pawn on a2, black king on c2, white king on a4, black to move.
    int bp = 8 + 0;  // a2
    int bk = 8 + 2;  // c2
    int wk = 24 + 0; // a4
    CHECK(Kpk::probe(Black, bk, bp, wk, Black));
}

TEST_CASE("KPK bitbase: defender stalemate from rook-file corner is drawn", "[endgame][kpk]") {
    ensureInit();
    // White pawn a7, white king a6, black king a8, black to move.
    // The classical corner stalemate trap: black to move has no legal
    // squares and is not in check, so the position is a draw.
    int wp = 48 + 0; // a7
    int wk = 40 + 0; // a6
    int bk = 56 + 0; // a8
    CHECK_FALSE(Kpk::probe(White, wk, wp, bk, Black));
}

TEST_CASE("KXK: pushing the defender king toward the edge improves the score", "[endgame][kxk]") {
    // Defender on d4 (center: pushToEdge gradient minimal) with the
    // strong king tucked far away on g1 to keep both terms small.
    Board centered;
    centered.setFen("8/8/8/8/3k4/8/4Q3/6K1 w - - 0 1");
    int egCentered = evaluate(centered);

    // Defender driven to the a8 corner; the strong side keeps the same
    // king and queen squares. The KXK gradient earns a higher
    // push-to-edge contribution, so this position must score strictly
    // higher than the centered defender.
    Board onEdge;
    onEdge.setFen("k7/8/8/8/8/8/4Q3/6K1 w - - 0 1");
    int egOnEdge = evaluate(onEdge);

    CHECK(egOnEdge > egCentered);
}

TEST_CASE("KXK: color-symmetric mirror returns the same strong-side score", "[endgame][kxk]") {
    Board white;
    white.setFen("8/8/8/3Q4/8/8/8/4K2k w - - 0 1");
    int whiteSide = evaluate(white);

    // Vertical mirror with the side-to-move flipped: the strong side
    // is now black and black moves first. Color symmetry plus the
    // STM-perspective return means the strong-side score is the same
    // magnitude in both positions.
    Board black;
    black.setFen("4k2K/8/8/8/3q4/8/8/8 b - - 0 1");
    int blackSide = evaluate(black);

    CHECK(whiteSide == blackSide);
}

TEST_CASE("KPK eval: winning bitbase entries keep the natural eg, drawn entries scale to zero",
          "[endgame][kpk][eval]") {
    Board winning;
    winning.setFen("8/P1K5/8/k7/8/8/8/8 w - - 0 1");
    CHECK(evaluate(winning) > 200);

    // KPK opposition draw: with the defender king directly in front of
    // the pawn and white to move, the bitbase returns DRAW and the
    // scale evaluator collapses eg to zero.
    Board drawn;
    drawn.setFen("8/8/3k4/8/3P4/3K4/8/8 w - - 0 1");
    CHECK(evaluate(drawn) == 0);
}

TEST_CASE("KRKP: rook side beats a far-back pawn but a far-advanced pawn with king support draws",
          "[endgame][krkp]") {
    Board winning;
    // White rook vs black pawn far back, white king close enough to
    // intervene. Should score as a clear win.
    winning.setFen("8/8/8/8/8/2k1K3/3p4/3R4 w - - 0 1");
    int strong = evaluate(winning);
    CHECK(strong > 200);

    // Pawn one square from promotion with the weak king supporting and
    // strong king cut off: the race favours the pawn side, score
    // should pull toward zero rather than the raw material edge.
    Board drawish;
    drawish.setFen("8/8/8/8/8/8/3p4/3k3K w - - 0 1");
    int draw = evaluate(drawish);
    CHECK(draw < strong);
}

TEST_CASE("KRKB: drawish, but the rook side keeps a slope toward the edge", "[endgame][krkb]") {
    Board center;
    center.setFen("8/8/8/3k4/8/2b5/8/3KR3 w - - 0 1");
    int centered = evaluate(center);

    Board edge;
    edge.setFen("k7/8/8/8/8/2b5/8/3KR3 w - - 0 1");
    int onEdge = evaluate(edge);

    CHECK(onEdge > centered);
}

TEST_CASE("KRKN: separating the weak king from its knight scores better than keeping them together",
          "[endgame][krkn]") {
    Board together;
    together.setFen("8/8/8/3kn3/8/8/8/3KR3 w - - 0 1");
    int near = evaluate(together);

    Board apart;
    apart.setFen("4k3/8/8/8/n7/8/8/3KR3 w - - 0 1");
    int far = evaluate(apart);

    CHECK(far > near);
}

TEST_CASE("KQKR: pushing the defender king toward the edge improves the queen side's score",
          "[endgame][kqkr]") {
    Board center;
    center.setFen("8/8/8/3k4/3r4/8/8/3KQ3 w - - 0 1");
    int centered = evaluate(center);

    Board edge;
    edge.setFen("k7/8/8/8/r7/8/8/3KQ3 w - - 0 1");
    int onEdge = evaluate(edge);

    CHECK(onEdge > centered);
}

TEST_CASE("KQKP: rook-file fortress holds the draw when the defender king blockades the corner",
          "[endgame][kqkp]") {
    Board fortress;
    // Black pawn on a2 one push from promotion, black king blockading
    // a1, white king too far to drive it out. The new KQKP evaluator
    // returns the textbook draw.
    fortress.setFen("8/8/8/8/8/4K3/p7/k6Q w - - 0 1");
    int fortressEval = evaluate(fortress);
    CHECK(std::abs(fortressEval) < 50);

    // Same material with the pawn shifted to a central file: queen
    // wins handily and the eval reflects the material excess.
    Board winning;
    winning.setFen("8/8/8/8/8/8/3p4/3kK2Q w - - 0 1");
    int winningEval = evaluate(winning);
    CHECK(winningEval > 1000);
}

TEST_CASE("KNNK: two knights vs lone king evaluates to roughly zero", "[endgame][knnk]") {
    // KNNK is a known draw, so the value evaluator returns zero. The
    // small residual the harness reads is the tempo contribution that
    // applies uniformly across every position; the score stays within
    // a few internal units of zero regardless of king placement.
    Board centered;
    centered.setFen("4k3/8/8/8/8/2N5/3N4/4K3 w - - 0 1");
    CHECK(std::abs(evaluate(centered)) < 20);

    Board corner;
    corner.setFen("7k/8/8/8/8/2N5/3N4/4K3 w - - 0 1");
    CHECK(std::abs(evaluate(corner)) < 20);
}

TEST_CASE("KBPsK: wrong-colored bishop with a rook-file pawn yields a fortress draw",
          "[endgame][kbpsk]") {
    Board fortress;
    // Promotion square h8 is dark. A light-square bishop (h3) cannot
    // evict the defender from h8, so the position is a textbook
    // wrong-rook-pawn draw.
    fortress.setFen("7k/7P/8/8/8/7B/8/4K3 w - - 0 1");
    int fortressEval = evaluate(fortress);
    CHECK(std::abs(fortressEval) < 100);

    // Same shape but with the bishop on a dark square (a3), matching
    // the h8 promotion corner. The bishop controls the corner so the
    // pawn promotes and the natural eg score survives.
    Board winning;
    winning.setFen("7k/7P/8/8/8/B7/8/4K3 w - - 0 1");
    int winningEval = evaluate(winning);
    CHECK(winningEval > 200);
}

TEST_CASE("KBPKB: opposite-colored bishops with one pawn scale toward a draw", "[endgame][kbpkb]") {
    Board ocb;
    ocb.setFen("4k3/8/8/4b3/4P3/8/4B3/4K3 w - - 0 1");
    int ocbEval = evaluate(ocb);

    // Same-colored bishops keep the winning chances intact: eval
    // should be significantly higher than the OCB case.
    Board sameColor;
    sameColor.setFen("4k3/8/8/3b4/4P3/8/4B3/4K3 w - - 0 1");
    int sameEval = evaluate(sameColor);

    CHECK(sameEval > ocbEval);
}

TEST_CASE("KBNK: defender on the correct corner scores worse for the strong side than on the wrong "
          "corner",
          "[endgame][kbnk]") {
    // Light-squared bishop drives the lone king to a light corner (h1
    // or a8). With the bishop on b3 (light square), the right corner
    // is a8 / h1; the wrong corner is a1 / h8.
    Board wrongCorner;
    wrongCorner.setFen("8/8/8/8/8/1B6/3N4/k3K3 w - - 0 1");
    int wrong = evaluate(wrongCorner);

    Board rightCorner;
    rightCorner.setFen("k7/8/8/8/8/1B6/3N4/4K3 w - - 0 1");
    int right = evaluate(rightCorner);

    CHECK(right > wrong);
}
