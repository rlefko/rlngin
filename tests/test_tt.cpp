#include "catch_amalgamated.hpp"
#include "tt.h"
#include "types.h"

TEST_CASE("TT: store and probe round-trip", "[tt]") {
    TranspositionTable table(1);
    Move move = {12, 28, None};

    table.store(0xABCDEF, 150, 42, 5, TT_EXACT, move, 0);

    TTEntry entry;
    bool hit = table.probe(0xABCDEF, entry, 0);

    CHECK(hit);
    CHECK(entry.score == 150);
    CHECK(entry.depth == 5);
    CHECK(entry.eval == 42);
    CHECK(entry.flag == TT_EXACT);
    CHECK(entry.best_move.from == 12);
    CHECK(entry.best_move.to == 28);
}

TEST_CASE("TT: probe miss returns false", "[tt]") {
    TranspositionTable table(1);

    TTEntry entry;
    bool hit = table.probe(0xDEADBEEF, entry, 0);

    CHECK_FALSE(hit);
}

TEST_CASE("TT: overwrite replaces existing entry", "[tt]") {
    TranspositionTable table(1);
    Move move1 = {0, 16, None};
    Move move2 = {4, 36, None};

    table.store(0xABC, 100, 10, 3, TT_EXACT, move1, 0);
    table.store(0xABC, 200, 20, 5, TT_LOWER_BOUND, move2, 0);

    TTEntry entry;
    bool hit = table.probe(0xABC, entry, 0);

    CHECK(hit);
    CHECK(entry.score == 200);
    CHECK(entry.depth == 5);
    CHECK(entry.eval == 20);
    CHECK(entry.flag == TT_LOWER_BOUND);
    CHECK(entry.best_move.from == 4);
}

TEST_CASE("TT: clear removes all entries", "[tt]") {
    TranspositionTable table(1);
    Move move = {0, 8, None};

    table.store(0x123, 50, 0, 2, TT_EXACT, move, 0);
    table.clear();

    TTEntry entry;
    CHECK_FALSE(table.probe(0x123, entry, 0));
}

TEST_CASE("TT: mate score adjusted for ply", "[tt]") {
    TranspositionTable table(1);
    Move move = {0, 0, None};

    // Store a mate score at ply 5
    int mateScore = MATE_SCORE - 3; // Mate in 3 from root perspective at ply 5
    table.store(0xAAAA, mateScore, 0, 10, TT_EXACT, move, 5);

    // Probe at ply 2: should adjust the score
    TTEntry entry;
    bool hit = table.probe(0xAAAA, entry, 2);

    CHECK(hit);
    // Stored as score+ply = (MATE_SCORE-3)+5, retrieved as stored-ply = (MATE_SCORE-3)+5-2
    CHECK(entry.score == MATE_SCORE - 3 + 5 - 2);
}

TEST_CASE("TT: eval round-trips without ply adjustment", "[tt]") {
    TranspositionTable table(1);
    Move move = {0, 0, None};

    // Eval should be stored and returned raw, independent of ply adjustment
    // that the mate-in-N score machinery applies to `score`.
    table.store(0xFEED, 25, 137, 6, TT_EXACT, move, 4);

    TTEntry entry;
    bool hit = table.probe(0xFEED, entry, 1);

    CHECK(hit);
    CHECK(entry.eval == 137);
}

TEST_CASE("TT: missing eval exposes sentinel", "[tt]") {
    TranspositionTable table(1);
    Move move = {0, 0, None};

    // Nodes that were in check store TT_NO_EVAL so readers can tell the static
    // eval was never computed at that position.
    table.store(0xCAFE, -50, TT_NO_EVAL, 3, TT_UPPER_BOUND, move, 2);

    TTEntry entry;
    bool hit = table.probe(0xCAFE, entry, 0);

    CHECK(hit);
    CHECK(entry.eval == TT_NO_EVAL);
}

TEST_CASE("TT: cluster absorbs colliding keys", "[tt]") {
    TranspositionTable table(1);
    Move move1 = {1, 2, None};
    Move move2 = {3, 4, None};

    // Small keys always map to the first cluster under multiplicative hashing,
    // so both stores should land in the same two-way cluster and both should
    // probe back cleanly instead of one evicting the other.
    table.store(1, 100, 10, 5, TT_EXACT, move1, 0);
    table.store(2, 200, 20, 6, TT_LOWER_BOUND, move2, 0);

    TTEntry e1;
    REQUIRE(table.probe(1, e1, 0));
    CHECK(e1.score == 100);
    CHECK(e1.depth == 5);
    CHECK(e1.flag == TT_EXACT);
    CHECK(e1.best_move.from == 1);

    TTEntry e2;
    REQUIRE(table.probe(2, e2, 0));
    CHECK(e2.score == 200);
    CHECK(e2.depth == 6);
    CHECK(e2.flag == TT_LOWER_BOUND);
    CHECK(e2.best_move.from == 3);
}

TEST_CASE("TT: negative mate score adjusted for ply", "[tt]") {
    TranspositionTable table(1);
    Move move = {0, 0, None};

    // Store a negative mate score (being mated) at ply 4
    int matedScore = -(MATE_SCORE - 2);
    table.store(0xDEAD, matedScore, 0, 8, TT_EXACT, move, 4);

    // Probe at ply 1
    TTEntry entry;
    bool hit = table.probe(0xDEAD, entry, 1);

    CHECK(hit);
    // Stored as score-ply = -(MATE_SCORE-2)-4, retrieved as stored+ply = -(MATE_SCORE-2)-4+1
    CHECK(entry.score == -(MATE_SCORE - 2) - 4 + 1);
}
