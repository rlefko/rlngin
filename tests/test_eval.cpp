#include "board.h"
#include "catch_amalgamated.hpp"
#include "eval.h"

#include <cstdlib>

TEST_CASE("Eval: starting position equals the tempo bonus", "[eval]") {
    Board board;
    // The positional half of startpos is zero by symmetry, so the score
    // reduces to the middlegame tempo bonus for the side to move.
    CHECK(evaluate(board) == 28);
}

TEST_CASE("Eval: kings only is 0", "[eval]") {
    Board board;
    board.setFen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 0);
}

TEST_CASE("Eval: extra white queen scores positive for white", "[eval]") {
    Board board;
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 3195);
}

TEST_CASE("Eval: positional half of evaluation flips with side to move", "[eval]") {
    Board board;
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    int whiteToMove = evaluate(board);

    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 b - - 0 1");
    int blackToMove = evaluate(board);

    // With a tempo bonus the two scores are no longer pure negations: each
    // side gets the same middlegame tempo boost, so (wtm + btm) measures
    // twice the tempo contribution while (wtm - btm) preserves the
    // positional asymmetry in favor of White.
    CHECK((whiteToMove + blackToMove) > 0);
    CHECK((whiteToMove - blackToMove) > 0);
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
    CHECK(evaluate(board) == 675);

    // Bishop on a1: material, PSQT, square control, and bishop mobility
    // along the long diagonal
    board.setFen("4k3/8/8/8/8/8/8/B3K3 w - - 0 1");
    CHECK(evaluate(board) == 952);

    // Rook on a1: material, PSQT, rook mobility, and the open-file bonus
    // since file a has no pawns of either color
    board.setFen("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    CHECK(evaluate(board) == 1695);

    // Queen on d5: material, PSQT, the undefended-zone term, and mobility
    // over 27 squares on an open board
    board.setFen("4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1");
    CHECK(evaluate(board) == 3195);
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

TEST_CASE("Eval: symmetric positions equal the tempo bonus", "[eval]") {
    Board board;

    // Mirror position: the positional half cancels cleanly, so only the
    // middlegame tempo contribution (scaled by the full startpos phase of
    // 24) is left for the side to move.
    board.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1");
    CHECK(evaluate(board) == 28);
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

    // Fully symmetric position with pawns: positional half cancels and the
    // score reduces to the tempo contribution for whoever is to move.
    board.setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(evaluate(board) == 28);

    // Symmetric with castled kings. Phase is reduced (no queens: 24 - 8 = 16)
    // but the tempo contribution scales with the middlegame weight.
    board.setFen("r1bq1rk1/pppppppp/2n2n2/8/8/2N2N2/PPPPPPPP/R1BQ1RK1 w - - 0 1");
    CHECK(evaluate(board) == 25);
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

TEST_CASE("Eval: symmetric pawn structure leaves only the tempo bonus", "[eval][pawn]") {
    Board board;

    // Kings and pawns: phase is 0 so the middlegame tempo contribution
    // tapers all the way to zero and the score is exactly 0.
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
    // imbalance should produce an equal-and-opposite positional half. The
    // tempo bonus goes to whoever is to move in both positions (White
    // here), so (whiteSide + blackSide) equals twice the tempo contribution.
    board.setFen("4k3/8/8/8/8/8/3RN3/3QK3 w - - 0 1");
    int whiteSide = evaluate(board);

    board.setFen("3qk3/3rn3/8/8/8/8/8/4K3 w - - 0 1");
    int blackSide = evaluate(board);

    CHECK((whiteSide + blackSide) > 0);
    CHECK((whiteSide - blackSide) > 0);
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

    // Both FENs have White to move, so the shared tempo bonus survives the
    // sign flip. The positional halves still mirror (whiteKnight - blackKnight
    // stays positive), while (whiteKnight + blackKnight) captures 2 * tempo.
    CHECK((whiteKnight + blackKnight) > 0);
    CHECK((whiteKnight - blackKnight) > 0);
}

// --- Threats tests ---

TEST_CASE("Eval: pawn attacking enemy minor is a threat", "[eval][threats]") {
    Board board;

    // White pawn on d5 with a black knight on c6 right in its attack mask.
    // The threat-by-pawn bonus should favor White noticeably beyond the
    // bare material and PST deltas.
    board.setFen("4k3/8/2n5/3P4/8/8/8/4K3 w - - 0 1");
    int withPawnThreat = evaluate(board);

    // Same material, same side to move, but the pawn sits on h5 so it no
    // longer attacks the knight. Any remaining delta between the two
    // positions comes from PST and mobility, not from a pawn threat.
    board.setFen("4k3/8/2n5/7P/8/8/8/4K3 w - - 0 1");
    int withoutPawnThreat = evaluate(board);

    CHECK(withPawnThreat > withoutPawnThreat);
}

TEST_CASE("Eval: hanging piece penalizes the side whose piece hangs", "[eval][threats]") {
    Board board;

    // Black knight on e5, undefended, attacked by a White bishop on b2.
    // White should be happy about the hanging knight.
    board.setFen("4k3/8/8/4n3/8/8/1B6/4K3 w - - 0 1");
    int knightHangs = evaluate(board);

    // Same knight, now defended by a black pawn on d6 so it no longer
    // hangs. White's score should be lower.
    board.setFen("4k3/8/3p4/4n3/8/8/1B6/4K3 w - - 0 1");
    int knightDefended = evaluate(board);

    CHECK(knightHangs > knightDefended);
}

// --- Passed pawn refinements ---

TEST_CASE("Eval: blockaded passer scores worse than free passer", "[eval][passed]") {
    Board board;

    // White passed pawn on e6 with a black knight blockading on e7. The
    // blockade penalty should make this worse for White than the same
    // passer with the stop square clear.
    board.setFen("4k3/4n3/4P3/8/8/8/8/4K3 w - - 0 1");
    int blocked = evaluate(board);

    board.setFen("3nk3/8/4P3/8/8/8/8/4K3 w - - 0 1");
    int free = evaluate(board);

    CHECK(free > blocked);
}

TEST_CASE("Eval: bishop blocked by same-color pawns is penalized", "[eval][bishop]") {
    Board board;

    // Two light-square bishops (on b1 and d1) plus four pawns on light
    // squares. Every own pawn blocks both bishops' diagonals, so the
    // bad-bishop penalty fires eight times.
    board.setFen("3k4/8/8/8/8/8/P1P1P1P1/1B1B3K w - - 0 1");
    int badBishops = evaluate(board);

    // Same material (two bishops + four pawns + kings) but the bishops
    // live on dark squares, so none of the light-square pawns block them
    // and the bad-bishop penalty is zero.
    board.setFen("3k4/8/8/8/8/8/P1P1P1P1/B1B4K w - - 0 1");
    int goodBishops = evaluate(board);

    CHECK(goodBishops > badBishops);
}

TEST_CASE("Eval: rook on the seventh with pawns to chew earns a bonus", "[eval][rook]") {
    Board board;

    // White rook already raided to a7 with seven black pawns still on the
    // seventh rank to chew on.
    board.setFen("4k3/Rppppppp/8/8/8/8/8/4K3 w - - 0 1");
    int rookOn7th = evaluate(board);

    // Same pawn chain but the rook sits quietly on a1 and no longer
    // qualifies for the seventh-rank bonus.
    board.setFen("4k3/1ppppppp/8/8/8/8/8/R3K3 w - - 0 1");
    int rookOnBack = evaluate(board);

    CHECK(rookOn7th > rookOnBack);
}

TEST_CASE("Eval: our king close to advanced passer is preferred", "[eval][passed]") {
    Board board;

    // White passer on e6 with our king nearby at e5 vs the same passer
    // with our king stranded at a1. The endgame king-proximity term
    // should favor the close king variant.
    board.setFen("4k3/8/4P3/4K3/8/8/8/8 w - - 0 1");
    int kingClose = evaluate(board);

    board.setFen("4k3/8/4P3/8/8/8/8/K7 w - - 0 1");
    int kingFar = evaluate(board);

    CHECK(kingClose > kingFar);
}

TEST_CASE("Eval: rook attacking enemy queen earns a threat bonus", "[eval][threats]") {
    Board board;

    // White rook on d1 and black queen on d5 with a clear file between
    // them: the rook attacks the queen and should score positively for
    // the attacking side.
    board.setFen("4k3/8/8/3q4/8/8/8/3RK3 w - - 0 1");
    int rookThreatensQueen = evaluate(board);

    // Same material but the rook sits on a1 and no longer attacks the
    // queen. Threat-by-rook on the queen is gone and the eval relative
    // delta drops.
    board.setFen("4k3/8/8/3q4/8/8/8/R3K3 w - - 0 1");
    int rookIdle = evaluate(board);

    CHECK(rookThreatensQueen > rookIdle);
}
