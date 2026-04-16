#include "board.h"
#include "catch_amalgamated.hpp"
#include "eval.h"

TEST_CASE("Eval: starting position is 0", "[eval]") {
    Board board;
    CHECK(evaluate(board) == 0);
}

TEST_CASE("Eval: kings only is 0", "[eval]") {
    Board board;
    board.setFen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 0);
}

TEST_CASE("Eval: extra white queen scores positive for white", "[eval]") {
    Board board;
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 1185);
}

TEST_CASE("Eval: score flips with side to move", "[eval]") {
    Board board;
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    int whiteToMove = evaluate(board);

    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 b - - 0 1");
    int blackToMove = evaluate(board);

    CHECK(whiteToMove == -blackToMove);
}

TEST_CASE("Eval: material values include PST bonuses", "[eval]") {
    Board board;

    // Pawn on a2 (sq 8): phase 0, pure endgame: 94 + EgPawnTable[8] = 94 + 13 = 107
    // Pawn structure: isolated (-20 EG) + passed rank 1 (+10 EG) = -10 -> 97
    board.setFen("4k3/8/8/8/8/8/P7/4K3 w - - 0 1");
    CHECK(evaluate(board) == 97);

    // Knight on a1: material and PSQT plus mobility bonus for its two legal
    // moves from the corner
    board.setFen("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    CHECK(evaluate(board) == 221);

    // Bishop on a1: material, PSQT, square control, and bishop mobility
    // along the long diagonal
    board.setFen("4k3/8/8/8/8/8/8/B3K3 w - - 0 1");
    CHECK(evaluate(board) == 334);

    // Rook on a1: material, PSQT, and rook mobility across file a and
    // rank 1
    board.setFen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    CHECK(evaluate(board) == 644);

    // Queen on d5: material, PSQT, the undefended-zone term, and mobility
    // over 27 squares on an open board
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 1185);
}

TEST_CASE("Eval: central knight scores higher than corner knight", "[eval]") {
    Board board;

    board.setFen("4k3/8/8/8/4N3/8/8/4K3 w - - 0 1");
    int centralKnight = evaluate(board);

    board.setFen("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    int cornerKnight = evaluate(board);

    CHECK(centralKnight > cornerKnight);
}

TEST_CASE("Eval: endgame king prefers center", "[eval]") {
    Board board;

    // No queens, no pieces -> phase 0, pure endgame: king prefers center
    board.setFen("4k3/8/8/8/3K4/8/8/8 w - - 0 1");
    int centralKing = evaluate(board);

    board.setFen("4k3/8/8/8/8/8/8/K7 w - - 0 1");
    int cornerKing = evaluate(board);

    CHECK(centralKing > cornerKing);
}

TEST_CASE("Eval: middlegame king prefers castled position", "[eval]") {
    Board board;

    // Heavy material (Q+R+B+N per side, phase 16) -> MG-dominated, king prefers safety
    board.setFen("rnbqk3/8/8/8/8/8/8/RNBQ2K1 w - - 0 1");
    int castledKing = evaluate(board);

    board.setFen("rnbqk3/8/8/8/4K3/8/8/RNBQ4 w - - 0 1");
    int exposedKing = evaluate(board);

    CHECK(castledKing > exposedKing);
}

TEST_CASE("Eval: tapered eval blends middlegame and endgame", "[eval]") {
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
    Board board;

    // Mirror position: identical pieces on mirrored squares
    board.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1");
    CHECK(evaluate(board) == 0);
}

// --- King safety tests ---

TEST_CASE("Eval: pawn shield improves king safety", "[eval][kingsafety]") {
    Board board;

    // White castled kingside with full f/g/h shield vs missing g-pawn
    board.setFen("r1bqk2r/pppppppp/2n2n2/8/8/2N2N2/PPPPPPPP/R1BQ1RK1 w kq - 0 1");
    int fullShield = evaluate(board);

    board.setFen("r1bqk2r/pppppppp/2n2n2/8/6P1/2N2N2/PPPPPP1P/R1BQ1RK1 w kq - 0 1");
    int pushedShield = evaluate(board);

    CHECK(fullShield > pushedShield);
}

TEST_CASE("Eval: open file near king is penalized", "[eval][kingsafety]") {
    Board board;

    // Symmetric piece setup, White missing g-pawn vs full pawns
    board.setFen("r1bqk2r/pppppppp/2n2n2/8/8/2N2N2/PPPPPPPP/R1BQK2R w KQkq - 0 1");
    int fullPawns = evaluate(board);

    board.setFen("r1bqk2r/pppppppp/2n2n2/8/8/2N2N2/PPPPPP1P/R1BQK2R w KQkq - 0 1");
    int missingGPawn = evaluate(board);

    CHECK(fullPawns > missingGPawn);
}

TEST_CASE("Eval: king zone attacks reduce eval for defending side", "[eval][kingsafety]") {
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
    Board board;

    // Black f-pawn on f3 storming White's castled kingside (king g1, shield f/g/h)
    board.setFen("r1bqk2r/ppppp1pp/2n2n2/8/8/2N2p2/PPPPPPPP/R1BQ1RK1 w kq - 0 1");
    int withStorm = evaluate(board);

    board.setFen("r1bqk2r/pppppppp/2n2n2/8/8/2N2N2/PPPPPPPP/R1BQ1RK1 w kq - 0 1");
    int noStorm = evaluate(board);

    CHECK(noStorm > withStorm);
}

TEST_CASE("Eval: king safety is symmetric", "[eval][kingsafety]") {
    Board board;

    // Fully symmetric position with pawns -- should still be 0
    board.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(evaluate(board) == 0);

    // Symmetric with castled kings
    board.setFen("r1bq1rk1/pppppppp/2n2n2/8/8/2N2N2/PPPPPPPP/R1BQ1RK1 w - - 0 1");
    CHECK(evaluate(board) == 0);
}

TEST_CASE("Eval: king with fewer safe squares scores worse", "[eval][kingsafety]") {
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
    Board board;

    // Same material (just kings plus white queen). Queen on h4 attacks
    // f6 and d8 in Black's king zone -- both squares are undefended by
    // the lone king, so Black is penalized for the exposed zone.
    board.setFen("4k3/8/8/8/7Q/8/8/4K3 w - - 0 1");
    int weakZone = evaluate(board);

    // Same material with queen on a3, which covers fewer undefended
    // kzone squares from farther away.
    board.setFen("4k3/8/8/8/8/Q7/8/4K3 w - - 0 1");
    int strongZone = evaluate(board);

    CHECK(weakZone > strongZone);
}

// --- Pawn structure tests ---

TEST_CASE("Eval: passed pawn scores higher than blocked pawn", "[eval][pawn]") {
    Board board;

    // White pawn on e5, no black pawns on d/e/f files ahead = passed
    board.setFen("4k3/8/8/4P3/8/8/8/4K3 w - - 0 1");
    int passedScore = evaluate(board);

    // White pawn on e5, black pawn on e6 blocks = not passed
    board.setFen("4k3/8/4p3/4P3/8/8/8/4K3 w - - 0 1");
    int blockedScore = evaluate(board);

    CHECK(passedScore > blockedScore);
}

TEST_CASE("Eval: advanced passed pawn worth more than rear passed pawn", "[eval][pawn]") {
    Board board;

    // White passed pawn on e6 (rank index 5)
    board.setFen("4k3/8/4P3/8/8/8/8/4K3 w - - 0 1");
    int advanced = evaluate(board);

    // White passed pawn on e3 (rank index 2)
    board.setFen("4k3/8/8/8/8/4P3/8/4K3 w - - 0 1");
    int rear = evaluate(board);

    CHECK(advanced > rear);
}

TEST_CASE("Eval: isolated pawn scores lower than supported pawn", "[eval][pawn]") {
    Board board;

    // White pawn on e4, alone (isolated)
    board.setFen("4k3/8/8/8/4P3/8/8/4K3 w - - 0 1");
    int isolated = evaluate(board);

    // White pawn on e4 with friendly pawn on d3 (not isolated, d3 supports)
    board.setFen("4k3/8/8/8/4P3/3P4/8/4K3 w - - 0 1");
    int supported = evaluate(board);

    CHECK(supported > isolated);
}

TEST_CASE("Eval: doubled pawns score lower than separated pawns", "[eval][pawn]") {
    Board board;

    // Two white pawns doubled on e-file
    board.setFen("4k3/8/8/8/4P3/4P3/8/4K3 w - - 0 1");
    int doubled = evaluate(board);

    // Two white pawns on adjacent files (not doubled)
    board.setFen("4k3/8/8/8/4P3/3P4/8/4K3 w - - 0 1");
    int separated = evaluate(board);

    CHECK(separated > doubled);
}

TEST_CASE("Eval: rear doubled pawn does not get passed bonus", "[eval][pawn]") {
    Board board;

    // Two white pawns doubled on e-file (e3, e5), no enemy pawns.
    // Only e5 is truly passed; e3 is the rear doubled pawn and must NOT get a
    // passed bonus.
    board.setFen("4k3/8/8/4P3/8/4P3/8/4K3 w - - 0 1");
    int doubled = evaluate(board);

    // Two white pawns on separate files (a5, e5), both isolated and passed.
    // Same pawn count, but both pawns get the passed bonus.
    board.setFen("4k3/8/8/P3P3/8/8/8/4K3 w - - 0 1");
    int bothPassed = evaluate(board);

    // The separated position where both pawns are truly passed should score
    // higher than the doubled position where the rear pawn is not passed.
    CHECK(bothPassed > doubled);
}

TEST_CASE("Eval: connected pawns score higher than disconnected pawns", "[eval][pawn]") {
    Board board;

    // Two white pawns side by side (phalanx) on d4, e4
    board.setFen("4k3/8/8/8/3PP3/8/8/4K3 w - - 0 1");
    int connected = evaluate(board);

    // Two white pawns far apart (a4, h4), both isolated
    board.setFen("4k3/8/8/8/P6P/8/8/4K3 w - - 0 1");
    int disconnected = evaluate(board);

    CHECK(connected > disconnected);
}

TEST_CASE("Eval: backward pawn scores lower than non-backward pawn", "[eval][pawn]") {
    Board board;

    // White pawn on e3, friendly pawn on d4 (ahead on adjacent file)
    // Black pawns on d5, f5 control e4 (stop square) -> e3 is backward
    board.setFen("4k3/8/8/3p1p2/3P4/4P3/8/4K3 w - - 0 1");
    int backward = evaluate(board);

    // Same but remove f5 so stop square is not fully controlled
    board.setFen("4k3/8/8/3p4/3P4/4P3/8/4K3 w - - 0 1");
    int notBackward = evaluate(board);

    CHECK(notBackward > backward);
}

TEST_CASE("Eval: pawn chain gives connected bonus", "[eval][pawn]") {
    Board board;

    // White pawn chain: e4 defended by d3
    board.setFen("4k3/8/8/8/4P3/3P4/8/4K3 w - - 0 1");
    int chain = evaluate(board);

    // Two isolated white pawns on a4 and h3
    board.setFen("4k3/8/8/8/P7/7P/8/4K3 w - - 0 1");
    int noChain = evaluate(board);

    CHECK(chain > noChain);
}

TEST_CASE("Eval: symmetric pawn structure scores 0", "[eval][pawn]") {
    Board board;

    board.setFen("4k3/pppppppp/8/8/8/8/PPPPPPPP/4K3 w - - 0 1");
    CHECK(evaluate(board) == 0);
}

TEST_CASE("Eval: black pawn structure mirrors white", "[eval][pawn]") {
    Board board;

    // White passed pawn on e5
    board.setFen("4k3/8/8/4P3/8/8/8/4K3 w - - 0 1");
    int whitePassed = evaluate(board);

    // Black passed pawn on e4 (mirror of e5)
    board.setFen("4k3/8/8/8/4p3/8/8/4K3 w - - 0 1");
    int blackPassed = evaluate(board);

    // Scores should be equal and opposite
    CHECK(whitePassed == -blackPassed);
}

TEST_CASE("Eval: central rook has more mobility than cornered rook", "[eval][mobility]") {
    Board board;

    board.setFen("4k3/8/8/8/3R4/8/8/4K3 w - - 0 1");
    int centralRook = evaluate(board);

    board.setFen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    int cornerRook = evaluate(board);

    CHECK(centralRook > cornerRook);
}

TEST_CASE("Eval: bishop blocked by own pawn scores lower than open bishop", "[eval][mobility]") {
    Board board;

    // Bishop on a1 with an own pawn on b2 shuts down the long diagonal
    board.setFen("4k3/8/8/8/8/8/1P6/B3K3 w - - 0 1");
    int blocked = evaluate(board);

    // Same material but the pawn sits on a3 and leaves the diagonal open
    board.setFen("4k3/8/8/8/8/P7/8/B3K3 w - - 0 1");
    int open = evaluate(board);

    CHECK(blocked < open);
}

TEST_CASE("Eval: queen mobility excludes squares attacked by enemy pawns", "[eval][mobility]") {
    Board board;

    // White queen on e4 with enemy pawns on d5 and f5 attacking c4, d4, e4,
    // f4, g4. Mobility area excludes squares covered by enemy pawn attacks.
    board.setFen("4k3/8/8/3p1p2/4Q3/8/8/4K3 w - - 0 1");
    int queenVsPawns = evaluate(board);

    board.setFen("4k3/8/8/8/4Q3/8/8/4K3 w - - 0 1");
    int queenOpen = evaluate(board);

    CHECK(queenVsPawns < queenOpen);
}

TEST_CASE("Eval: mobility term is color-symmetric", "[eval][mobility]") {
    Board board;

    board.setFen("4k3/8/8/8/3N4/8/8/4K3 w - - 0 1");
    int whiteKnight = evaluate(board);

    board.setFen("4k3/8/8/3n4/8/8/8/4K3 w - - 0 1");
    int blackKnight = evaluate(board);

    CHECK(whiteKnight == -blackKnight);
}
