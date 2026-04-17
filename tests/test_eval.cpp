#include "board.h"
#include "catch_amalgamated.hpp"
#include "eval.h"

#include <cstdlib>
#include <sstream>

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
    CHECK(evaluate(board) == 3191);
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

    // Pawn on a2: pure-endgame material with PST plus pawn-structure terms
    // (isolated penalty, passed bonus) collapse into this expected score.
    board.setFen("4k3/8/8/8/8/8/P7/4K3 w - - 0 1");
    CHECK(evaluate(board) == 268);

    // Knight on a1: material and PSQT plus mobility bonus for its two legal
    // moves from the corner
    board.setFen("4k3/8/8/8/8/8/8/N3K3 w - - 0 1");
    CHECK(evaluate(board) == 674);

    // Bishop on a1: material, PSQT, square control, and bishop mobility
    // along the long diagonal
    board.setFen("4k3/8/8/8/8/8/8/B3K3 w - - 0 1");
    CHECK(evaluate(board) == 951);

    // Rook on a1: material, PSQT, rook mobility, and the open-file bonus
    // since file a has no pawns of either color
    board.setFen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    CHECK(evaluate(board) == 1693);

    // Queen on d5: material, PSQT, the undefended-zone term, and mobility
    // over 27 squares on an open board
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 3191);
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

TEST_CASE("Eval: material hash probe is deterministic", "[eval][material]") {
    Board board;

    // Two positions with identical piece counts (same material key) but
    // different PST contributions. The overall eval must differ (PSTs are
    // not cached), but repeating either evaluation must always return the
    // same value regardless of probe order -- a stored entry cannot poison
    // a later query of a different position.
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    int a1 = evaluate(board);
    board.setFen("4k3/3Q4/8/8/8/8/8/4K3 w - - 0 1");
    int b1 = evaluate(board);
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    int a2 = evaluate(board);
    board.setFen("4k3/3Q4/8/8/8/8/8/4K3 w - - 0 1");
    int b2 = evaluate(board);

    CHECK(a1 == a2);
    CHECK(b1 == b2);
    CHECK(a1 != b1);
}

TEST_CASE("Eval: material imbalance is color-symmetric", "[eval][material]") {
    Board board;

    // Q vs R+N, White has the queen. Color-mirror of the same material
    // imbalance should produce an equal-and-opposite eval.
    board.setFen("4k3/8/8/8/8/8/3RN3/3QK3 w - - 0 1");
    int whiteSide = evaluate(board);

    board.setFen("3qk3/3rn3/8/8/8/8/8/4K3 w - - 0 1");
    int blackSide = evaluate(board);

    CHECK(whiteSide == -blackSide);
    CHECK(whiteSide != 0);
}

TEST_CASE("Eval: bishop pair bonus favors the pair", "[eval][material]") {
    Board board;

    // White has two bishops; Black has bishop and knight. Material on minor
    // pieces is otherwise identical, so the positive delta comes from the
    // bishop-pair bonus fired for White.
    board.setFen("4k3/8/8/8/8/2n5/1B6/B3K3 w - - 0 1");
    int whitePairVsMixed = evaluate(board);
    CHECK(whitePairVsMixed > 0);

    // Mirror: Black has the pair, White has mixed. Score should flip sign.
    board.setFen("b3k3/1b6/2N5/8/8/8/8/4K3 w - - 0 1");
    int blackPairVsMixed = evaluate(board);
    CHECK(blackPairVsMixed < 0);
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

TEST_CASE("Eval: rook on open file beats rook on closed file", "[eval][rook]") {
    Board board;

    // Rook on file e with both an own pawn on e2 and an enemy pawn on e6
    // sitting on the same file -- closed, no bonus
    board.setFen("4k3/8/4p3/8/8/8/4P3/4K2R w K - 0 1");
    int closed = evaluate(board);

    // Same material shifted so the rook sits on file d with no pawns on its
    // file of either color -- fully open
    board.setFen("4k3/8/4p3/8/8/8/4P3/3RK3 w - - 0 1");
    int open = evaluate(board);

    CHECK(open > closed);
}

TEST_CASE("Eval: rook on semi-open file beats rook on closed file", "[eval][rook]") {
    Board board;

    // Closed: rook on a1 with own pawn a2 blocking, enemy pawn a7 capping.
    board.setFen("4k3/p7/8/8/8/8/P7/R3K3 w - - 0 1");
    int closed = evaluate(board);

    // Semi-open: rook on a1 with only the enemy pawn on a7 on its file;
    // the own pawn now sits on b2 so material matches.
    board.setFen("4k3/p7/8/8/8/8/1P6/R3K3 w - - 0 1");
    int semiOpen = evaluate(board);

    CHECK(semiOpen > closed);
}

TEST_CASE("Eval: trapped rook with no mobility is penalized", "[eval][rook]") {
    Board board;

    // White king on g1 plus pawns on g2 and h2 shut the h1 rook in with
    // zero safe squares. All castling rights gone, so the rook cannot be
    // relocated via O-O / O-O-O and the doubled penalty should fire.
    board.setFen("4k3/8/8/8/8/8/6PP/6KR w - - 0 1");
    int trapped = evaluate(board);

    // Same material with the king on b1 -- the rook sits on the opposite
    // side of the board from its king, so the same-side gate fails and
    // no trap penalty applies.
    board.setFen("4k3/8/8/8/8/8/6PP/1K5R w - - 0 1");
    int free = evaluate(board);

    CHECK(trapped < free);
}

TEST_CASE("Eval: knight outpost beats unsupported knight on same square", "[eval][outpost]") {
    Board board;

    // Knight on d5 defended by own c4 pawn, enemy pawns on b6 and d6 leave
    // no way to challenge it. The outpost condition should fire.
    board.setFen("4k3/2p5/3pp3/3N4/2P5/8/8/4K3 w - - 0 1");
    int outpost = evaluate(board);

    // Same layout but the own c4 pawn has been removed, so no friendly
    // pawn defends d5 and the bonus should not apply.
    board.setFen("4k3/2p5/3pp3/3N4/8/8/8/4K3 w - - 0 1");
    int unsupported = evaluate(board);

    CHECK(outpost > unsupported);
}

TEST_CASE("Eval: knight outpost requires no enemy pawn attackers", "[eval][outpost]") {
    Board board;

    // Defended knight on d5 but enemy c7 pawn can push to c6 and challenge
    // it. Not a true outpost.
    board.setFen("4k3/2p5/4p3/3N4/2P5/8/8/4K3 w - - 0 1");
    int contested = evaluate(board);

    // Same layout with the enemy c7 pawn removed -- the outpost is secure
    // and the bonus should apply.
    board.setFen("4k3/8/4p3/3N4/2P5/8/8/4K3 w - - 0 1");
    int secure = evaluate(board);

    CHECK(secure > contested);
}

TEST_CASE("Eval: bishop outpost smaller than knight outpost", "[eval][outpost]") {
    Board board;

    // Knight on outpost d5: bonus fires
    board.setFen("4k3/8/3pp3/3N4/2P5/8/8/4K3 w - - 0 1");
    int knightOutpost = evaluate(board);

    // Knight on d4 (same pawn cover, not on an outpost rank)
    board.setFen("4k3/8/3pp3/8/2PN4/8/8/4K3 w - - 0 1");
    int knightOff = evaluate(board);

    // Bishop on outpost d5: smaller bonus fires
    board.setFen("4k3/8/3pp3/3B4/2P5/8/8/4K3 w - - 0 1");
    int bishopOutpost = evaluate(board);

    // Bishop on d4 (same pawn cover, not on an outpost rank)
    board.setFen("4k3/8/3pp3/8/2PB4/8/8/4K3 w - - 0 1");
    int bishopOff = evaluate(board);

    int knightDelta = knightOutpost - knightOff;
    int bishopDelta = bishopOutpost - bishopOff;
    CHECK(knightDelta > bishopDelta);
}

TEST_CASE("Eval: space bonus favors side with advanced center pawns", "[eval][space]") {
    Board board;

    // White has central pawns on c4/d4/e4 controlling squares on the White
    // side of the board. Heavy minor/major material means the space weight
    // fires and scales quadratically.
    board.setFen("rnbqkbnr/pppppppp/8/8/2PPP3/8/PP3PPP/RNBQKBNR w KQkq - 0 1");
    int advanced = evaluate(board);

    // Same material with the center pawns held back on their starting
    // squares -- fewer safe central squares, so a smaller space bonus.
    board.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    int starting = evaluate(board);

    CHECK(advanced > starting);
}

TEST_CASE("Eval: space bonus vanishes in thin endgames", "[eval][space]") {
    Board board;

    // Two kings plus White's central pawn on e4. No minor or major pieces
    // remain, so the SpaceMinPieceCount gate should suppress the bonus.
    board.setFen("4k3/8/8/8/4P3/8/8/4K3 w - - 0 1");
    int withEndgamePawn = evaluate(board);

    // Compute expected material+PST+pawn-structure contribution without the
    // space term by moving the pawn outside the SpaceMask (file a is not
    // central, so no space credit possible) -- same position shape.
    board.setFen("4k3/8/8/8/P7/8/8/4K3 w - - 0 1");
    int withEdgePawn = evaluate(board);

    // Neither case should produce a space bonus, so the two scores differ
    // only by PST and pawn-structure deltas, not by the space term.
    CHECK(std::abs(withEndgamePawn - withEdgePawn) < 100);
}

TEST_CASE("Eval: mobility term is color-symmetric", "[eval][mobility]") {
    Board board;

    board.setFen("4k3/8/8/8/3N4/8/8/4K3 w - - 0 1");
    int whiteKnight = evaluate(board);

    board.setFen("4k3/8/8/3n4/8/8/8/4K3 w - - 0 1");
    int blackKnight = evaluate(board);

    CHECK(whiteKnight == -blackKnight);
}

// Reassemble the tapered final from an EvalTrace using the same arithmetic
// that evaluateImpl runs internally. If any term is captured at the wrong
// moment the delta here will not round-trip to the flat score.
static int reassembleTrace(const EvalTrace &t) {
    Score combined = (t.material[White] + t.pst[White] + t.pieces[White] + t.space[White] +
                      t.kingSafety[White]) -
                     (t.material[Black] + t.pst[Black] + t.pieces[Black] + t.space[Black] +
                      t.kingSafety[Black]) +
                     t.pawns;

    int mgPhase = std::min(t.gamePhase, 24);
    int egPhase = 24 - mgPhase;
    int tapered = (mg_value(combined) * mgPhase + eg_value(combined) * egPhase) / 24;
    return tapered;
}

TEST_CASE("Eval print: emits every section header the eval command promises",
          "[eval][trace][print]") {
    Board board;
    std::ostringstream out;
    printEvalTrace(out, board);

    const std::string s = out.str();
    const char *sections[] = {
        "Side to move", "Material", "PST",    "Pawns",         "Pieces",   "Space",
        "King safety",  "Total",    "Phase:", "50-move clock", "Tapered:", "Final:",
    };
    for (const char *section : sections) {
        CAPTURE(section);
        CHECK(s.find(section) != std::string::npos);
    }

    // Startpos stays symmetric, so the printed final must land on zero even
    // though the table above may show non-zero PST cells for each side.
    CHECK(s.find("Final:         0 internal") != std::string::npos);
}

TEST_CASE("Eval trace: per-term sums reproduce the flat score", "[eval][trace]") {
    const char *positions[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "4k3/8/8/8/3N4/8/8/4K3 w - - 0 1",
    };

    for (const char *fen : positions) {
        Board board;
        board.setFen(fen);

        EvalTrace trace;
        int traced = evaluateTraced(board, trace);
        int flat = evaluate(board);

        CAPTURE(fen);
        CHECK(traced == flat);
        CHECK(trace.finalFromStm == flat);
        CHECK(trace.sideToMove == board.sideToMove);
        CHECK(trace.halfmoveClock == board.halfmoveClock);
        CHECK(trace.gamePhase >= 0);
        CHECK(trace.gamePhase <= 24);
        CHECK(trace.rawTapered == reassembleTrace(trace));
    }
}
