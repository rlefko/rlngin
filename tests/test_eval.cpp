#include "bitboard.h"
#include "board.h"
#include "catch_amalgamated.hpp"
#include "eval.h"

static void ensureInit() {
    static bool done = false;
    if (!done) {
        initBitboards();
        done = true;
    }
}

TEST_CASE("Eval: starting position is 0", "[eval]") {
    ensureInit();
    Board board;
    CHECK(evaluate(board) == 0);
}

TEST_CASE("Eval: kings only is 0", "[eval]") {
    ensureInit();
    Board board;
    board.setFen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 0);
}

TEST_CASE("Eval: extra white queen scores positive for white", "[eval]") {
    ensureInit();
    Board board;
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 989);
}

TEST_CASE("Eval: score flips with side to move", "[eval]") {
    ensureInit();
    Board board;
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    int whiteToMove = evaluate(board);

    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 b - - 0 1");
    int blackToMove = evaluate(board);

    CHECK(whiteToMove == -blackToMove);
}

TEST_CASE("Eval: material values include PST bonuses", "[eval]") {
    ensureInit();
    Board board;

    // Pawn on a2 (sq 8): phase 0, pure endgame: 94 + EgPawnTable[8] = 94 + 13 = 107
    board.setFen("4k3/8/8/8/8/8/P7/4K3 w - - 0 1");
    CHECK(evaluate(board) == 107);

    // Knight on a1 (sq 0): phase 1, tapered: (232*1 + 252*23) / 24 = 251
    board.setFen("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    CHECK(evaluate(board) == 251);

    // Bishop on a1 (sq 0): phase 1, tapered plus square control terms
    board.setFen("4k3/8/8/8/8/8/8/B3K3 w - - 0 1");
    CHECK(evaluate(board) == 277);

    // Rook on a1 (sq 0): phase 2, tapered: (458*2 + 503*22) / 24 = 499
    board.setFen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    CHECK(evaluate(board) == 499);

    // Queen on d5 (sq 35): phase 4, tapered plus the undefended-zone term
    // that fires when a single enemy piece attacks the opposing king zone
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 989);
}

TEST_CASE("Eval: central knight scores higher than corner knight", "[eval]") {
    ensureInit();
    Board board;

    board.setFen("4k3/8/8/8/4N3/8/8/4K3 w - - 0 1");
    int centralKnight = evaluate(board);

    board.setFen("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    int cornerKnight = evaluate(board);

    CHECK(centralKnight > cornerKnight);
}

TEST_CASE("Eval: endgame king prefers center", "[eval]") {
    ensureInit();
    Board board;

    // No queens, no pieces -> phase 0, pure endgame: king prefers center
    board.setFen("4k3/8/8/8/3K4/8/8/8 w - - 0 1");
    int centralKing = evaluate(board);

    board.setFen("4k3/8/8/8/8/8/8/K7 w - - 0 1");
    int cornerKing = evaluate(board);

    CHECK(centralKing > cornerKing);
}

TEST_CASE("Eval: middlegame king prefers castled position", "[eval]") {
    ensureInit();
    Board board;

    // Heavy material (Q+R+B+N per side, phase 16) -> MG-dominated, king prefers safety
    board.setFen("rnbqk3/8/8/8/8/8/8/RNBQ2K1 w - - 0 1");
    int castledKing = evaluate(board);

    board.setFen("rnbqk3/8/8/8/4K3/8/8/RNBQ4 w - - 0 1");
    int exposedKing = evaluate(board);

    CHECK(castledKing > exposedKing);
}

TEST_CASE("Eval: tapered eval blends middlegame and endgame", "[eval]") {
    ensureInit();
    Board board;

    // Pure endgame (phase 0): king prefers center over edge
    board.setFen("4k3/8/8/8/3K4/8/8/8 w - - 0 1");
    int egCenter = evaluate(board);
    board.setFen("4k3/8/8/8/8/8/8/3K4 w - - 0 1");
    int egEdge = evaluate(board);
    CHECK(egCenter > egEdge);

    // Light material (Q+minor per side, phase ~10): center king still viable
    board.setFen("3qk3/8/8/8/3K4/8/8/3QN3 w - - 0 1");
    int lightCenter = evaluate(board);
    board.setFen("3qk3/8/8/8/8/8/8/3QNK2 w - - 0 1");
    int lightEdge = evaluate(board);
    CHECK(lightCenter > lightEdge);

    // Heavy material (Q+R+B+N per side, phase 16): castled king preferred
    board.setFen("rnbqk3/8/8/8/8/8/8/RNBQ2K1 w - - 0 1");
    int heavyCastled = evaluate(board);
    board.setFen("rnbqk3/8/8/8/4K3/8/8/RNBQ4 w - - 0 1");
    int heavyExposed = evaluate(board);
    CHECK(heavyCastled > heavyExposed);
}

TEST_CASE("Eval: symmetric positions score 0", "[eval]") {
    ensureInit();
    Board board;

    // Mirror position: identical pieces on mirrored squares
    board.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1");
    CHECK(evaluate(board) == 0);
}

// --- King safety tests ---

TEST_CASE("Eval: pawn shield improves king safety", "[eval][kingsafety]") {
    ensureInit();
    Board board;

    // White castled kingside with full f/g/h shield vs missing g-pawn
    board.setFen("r1bqk2r/pppppppp/2n2n2/8/8/2N2N2/PPPPPPPP/R1BQ1RK1 w kq - 0 1");
    int fullShield = evaluate(board);

    board.setFen("r1bqk2r/pppppppp/2n2n2/8/6P1/2N2N2/PPPPPP1P/R1BQ1RK1 w kq - 0 1");
    int pushedShield = evaluate(board);

    CHECK(fullShield > pushedShield);
}

TEST_CASE("Eval: open file near king is penalized", "[eval][kingsafety]") {
    ensureInit();
    Board board;

    // Symmetric piece setup, White missing g-pawn vs full pawns
    board.setFen("r1bqk2r/pppppppp/2n2n2/8/8/2N2N2/PPPPPPPP/R1BQK2R w KQkq - 0 1");
    int fullPawns = evaluate(board);

    board.setFen("r1bqk2r/pppppppp/2n2n2/8/8/2N2N2/PPPPPP1P/R1BQK2R w KQkq - 0 1");
    int missingGPawn = evaluate(board);

    CHECK(fullPawns > missingGPawn);
}

TEST_CASE("Eval: king zone attacks reduce eval for defending side", "[eval][kingsafety]") {
    ensureInit();
    Board board;

    // Same material, but Black's queen shifts into White's king zone.
    board.setFen("6k1/5ppp/8/2b5/7q/8/6PP/2BQ2K1 w - - 0 1");
    int attacking = evaluate(board);

    board.setFen("6k1/5ppp/8/2b5/q7/8/6PP/2BQ2K1 w - - 0 1");
    int passive = evaluate(board);

    CHECK(attacking < passive);
    CHECK(passive - attacking < 200);
}

TEST_CASE("Eval: pawn storm penalizes defending side", "[eval][kingsafety]") {
    ensureInit();
    Board board;

    // Black f-pawn on f3 storming White's castled kingside (king g1, shield f/g/h)
    board.setFen("r1bqk2r/ppppp1pp/2n2n2/8/8/2N2p2/PPPPPPPP/R1BQ1RK1 w kq - 0 1");
    int withStorm = evaluate(board);

    board.setFen("r1bqk2r/pppppppp/2n2n2/8/8/2N2N2/PPPPPPPP/R1BQ1RK1 w kq - 0 1");
    int noStorm = evaluate(board);

    CHECK(noStorm > withStorm);
}

TEST_CASE("Eval: king safety is symmetric", "[eval][kingsafety]") {
    ensureInit();
    Board board;

    // Fully symmetric position with pawns -- should still be 0
    board.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(evaluate(board) == 0);

    // Symmetric with castled kings
    board.setFen("r1bq1rk1/pppppppp/2n2n2/8/8/2N2N2/PPPPPPPP/R1BQ1RK1 w - - 0 1");
    CHECK(evaluate(board) == 0);
}

TEST_CASE("Eval: king with fewer safe squares scores worse", "[eval][kingsafety]") {
    ensureInit();
    Board board;

    // Black queen on f3 covers f1 and f2, restricting White king escape
    // on g1 (only h1 is safe). Queen PST also penalizes Black less when
    // the queen is well-placed, so both effects align.
    board.setFen("6k1/5ppp/8/8/8/5q2/6PP/6K1 w - - 0 1");
    int restricted = evaluate(board);

    // Same material, queen on a3 far from White king -- f1, f2, h1 all safe
    board.setFen("6k1/5ppp/8/8/8/q7/6PP/6K1 w - - 0 1");
    int unrestricted = evaluate(board);

    CHECK(unrestricted > restricted);
}

TEST_CASE("Eval: undefended king zone squares penalize defender", "[eval][kingsafety]") {
    ensureInit();
    Board board;

    // Black g-pawn advanced to g5 -- no longer defends f6/h6 in king
    // zone, weaker shield, and queen h4 attacks undefended zone squares
    board.setFen("6k1/5p1p/8/6p1/7Q/8/8/6K1 w - - 0 1");
    int weakZone = evaluate(board);

    // Same material but g-pawn on g7 -- defends f6 and h6, strong shield
    board.setFen("6k1/5ppp/8/8/7Q/8/8/6K1 w - - 0 1");
    int strongZone = evaluate(board);

    // White scores better when Black's king zone has undefended squares
    CHECK(weakZone > strongZone);
}
