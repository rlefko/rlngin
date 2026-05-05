#include "board.h"
#include "catch_amalgamated.hpp"
#include "search.h"
#include "tt.h"
#include "types.h"
#include <thread>

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

    // Two keys with distinct upper-16 bits so the packed key check can tell
    // them apart, but small enough lower bits that multiplicative hashing
    // routes both to cluster 0. The cluster has room for both, and probes
    // come back cleanly without one evicting the other.
    uint64_t k1 = 1ULL << 48;
    uint64_t k2 = 2ULL << 48;
    table.store(k1, 100, 10, 5, TT_EXACT, move1, 0);
    table.store(k2, 200, 20, 6, TT_LOWER_BOUND, move2, 0);

    TTEntry e1;
    REQUIRE(table.probe(k1, e1, 0));
    CHECK(e1.score == 100);
    CHECK(e1.depth == 5);
    CHECK(e1.flag == TT_EXACT);
    CHECK(e1.best_move.from == 1);

    TTEntry e2;
    REQUIRE(table.probe(k2, e2, 0));
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

TEST_CASE("TT: new_search bumps generation", "[tt]") {
    TranspositionTable table(1);
    uint8_t start = table.generation();
    table.new_search();
    CHECK(table.generation() == static_cast<uint8_t>(start + 1));
    table.new_search();
    CHECK(table.generation() == static_cast<uint8_t>(start + 2));
}

TEST_CASE("TT: clear resets the generation counter", "[tt]") {
    TranspositionTable table(1);
    table.new_search();
    table.new_search();
    REQUIRE(table.generation() != 0);
    table.clear();
    CHECK(table.generation() == 0);
}

TEST_CASE("TT: same-key shallow non-exact store preserves deeper bound", "[tt]") {
    TranspositionTable table(1);
    Move deep = {1, 2, None};
    Move shallow = {3, 4, None};

    table.new_search();
    table.store(0xABC, 100, 0, 10, TT_EXACT, deep, 0);
    // A shallower non-exact write for the same key must not clobber the
    // deeper exact bound: the deeper entry carries more information and
    // erasing it here would bleed depth out of the table on every PV
    // node re-search that hits the same transposition at a smaller
    // remaining depth than its prior visit.
    table.store(0xABC, 250, 0, 3, TT_LOWER_BOUND, shallow, 0);

    TTEntry entry;
    REQUIRE(table.probe(0xABC, entry, 0));
    CHECK(entry.score == 100);
    CHECK(entry.depth == 10);
    CHECK(entry.flag == TT_EXACT);
    CHECK(entry.best_move.from == 1);
}

TEST_CASE("TT: exact store overwrites a deeper same-key non-exact bound", "[tt]") {
    TranspositionTable table(1);
    Move deep = {1, 2, None};
    Move shallow = {3, 4, None};

    table.new_search();
    table.store(0xABC, 100, 0, 10, TT_LOWER_BOUND, deep, 0);
    // EXACT carries maximal information about a position, so it wins
    // over a deeper non-exact bound. This prevents a stale lowerbound
    // from pinning future probes to an incorrect cutoff when we have
    // newer, more definitive information.
    table.store(0xABC, 250, 0, 3, TT_EXACT, shallow, 0);

    TTEntry entry;
    REQUIRE(table.probe(0xABC, entry, 0));
    CHECK(entry.score == 250);
    CHECK(entry.depth == 3);
    CHECK(entry.flag == TT_EXACT);
    CHECK(entry.best_move.from == 3);
}

TEST_CASE("TT: same-key store at comparable depth takes over", "[tt]") {
    TranspositionTable table(1);
    Move before = {1, 2, None};
    Move after = {3, 4, None};

    table.new_search();
    table.store(0xABC, 100, 0, 10, TT_LOWER_BOUND, before, 0);
    // Within the tolerance band the newer search wins even if its depth is
    // one ply shallower, because that information reflects the current
    // search's move ordering and bound tightness.
    table.store(0xABC, 250, 0, 9, TT_LOWER_BOUND, after, 0);

    TTEntry entry;
    REQUIRE(table.probe(0xABC, entry, 0));
    CHECK(entry.score == 250);
    CHECK(entry.depth == 9);
    CHECK(entry.best_move.from == 3);
}

TEST_CASE("TT: same-key store preserves an existing best move on empty write", "[tt]") {
    TranspositionTable table(1);
    Move real = {5, 21, None};
    Move empty = {0, 0, None};

    table.new_search();
    table.store(0xABC, 100, 0, 6, TT_EXACT, real, 0);
    // Leaf-level stand-pat writes and fail-high paths that never tried a
    // move can store with an empty best move. Those writes should not wipe
    // the previously cached move hint, because the hint is useful for
    // ordering the next visit even when the score itself is refreshed.
    table.store(0xABC, 120, 0, 6, TT_LOWER_BOUND, empty, 0);

    TTEntry entry;
    REQUIRE(table.probe(0xABC, entry, 0));
    CHECK(entry.score == 120);
    CHECK(entry.flag == TT_LOWER_BOUND);
    CHECK(entry.best_move.from == 5);
    CHECK(entry.best_move.to == 21);
}

TEST_CASE("TT: aging evicts stale deep entries before fresh shallow ones", "[tt]") {
    // Size the table down to a single cluster so every key, regardless of
    // its upper bits, lands in the same six-slot cluster. The first store
    // gets a deep entry that we then age by bumping generations; the next
    // four keys fill the rest of the cluster with current-generation
    // shallow entries; the final seventh key forces a replacement decision
    // against a full cluster.
    TranspositionTable table(0);

    uint64_t kStale = 1ULL << 48;
    uint64_t kFresh[4] = {2ULL << 48, 3ULL << 48, 4ULL << 48, 5ULL << 48};
    uint64_t kCurrent = 6ULL << 48;
    uint64_t kIncoming = 7ULL << 48;
    Move stale = {1, 2, None};
    Move fresh = {3, 4, None};
    Move current = {7, 8, None};
    Move incoming = {5, 6, None};

    table.new_search();
    table.store(kStale, 100, 0, 20, TT_EXACT, stale, 0);

    // Bump four generations so the stale entry trails the live search by
    // four; the aging penalty (4 generations * 8 = 32 effective ply) easily
    // beats the 19-ply raw depth advantage of the stale slot.
    for (int i = 0; i < 4; i++)
        table.new_search();

    for (int i = 0; i < 4; i++) {
        table.store(kFresh[i], 200 + i, 0, 5, TT_EXACT, fresh, 0);
    }
    table.store(kCurrent, 250, 0, 6, TT_EXACT, current, 0);

    // Cluster is now full (six slots). The new current-generation entry must
    // evict the stale deep slot, not any of the fresh shallow ones.
    table.store(kIncoming, 300, 0, 1, TT_EXACT, incoming, 0);

    TTEntry entry;
    for (int i = 0; i < 4; i++) {
        REQUIRE(table.probe(kFresh[i], entry, 0));
        CHECK(entry.score == 200 + i);
    }
    REQUIRE(table.probe(kCurrent, entry, 0));
    CHECK(entry.score == 250);
    REQUIRE(table.probe(kIncoming, entry, 0));
    CHECK(entry.score == 300);
    CHECK_FALSE(table.probe(kStale, entry, 0));
}

TEST_CASE("TT: generation counter wraps cleanly", "[tt]") {
    TranspositionTable table(1);
    Move move = {0, 1, None};

    table.new_search();
    uint8_t first = table.generation();
    table.store(0xBEEF, 42, 0, 5, TT_EXACT, move, 0);

    // Drive the uint8_t counter fully around and confirm we land back on the
    // same value. Probes remain valid because the key match is the primary
    // validity signal, independent of the age bookkeeping.
    for (int i = 0; i < 256; i++)
        table.new_search();
    CHECK(table.generation() == first);

    TTEntry entry;
    REQUIRE(table.probe(0xBEEF, entry, 0));
    CHECK(entry.score == 42);
    CHECK(entry.depth == 5);
}

TEST_CASE("TT: hashfull counts only current-generation entries", "[tt]") {
    TranspositionTable table(1);
    Move move = {0, 1, None};

    table.new_search();
    for (uint64_t k = 1; k <= 64; k++) {
        table.store(k, 0, 0, 3, TT_EXACT, move, 0);
    }
    CHECK(table.hashfull() > 0);

    // The next search must not count the previous generation's entries as
    // full, since they no longer reflect work from the live search.
    table.new_search();
    CHECK(table.hashfull() == 0);
}

TEST_CASE("TT: packed move encoding round-trips for boundary cases", "[tt]") {
    TranspositionTable table(1);

    // Walk every from / to combination plus every promotion type to confirm
    // the 6 + 6 + 3 bit packing of `Move` survives the store / probe cycle.
    // Boundary squares (0 and 63) catch any off-by-one in the bit layout,
    // and each promotion piece exercises the high bits of the encoding.
    PieceType promos[] = {None, Knight, Bishop, Rook, Queen};
    int pairs[][2] = {{0, 63}, {63, 0}, {7, 56}, {56, 7}, {31, 32}, {0, 0}};
    uint64_t key = 0;
    for (int p = 0; p < 5; p++) {
        for (auto &pair : pairs) {
            Move m = {pair[0], pair[1], promos[p]};
            // Use distinct upper-16 bits so each store lands in a slot the
            // packed key check can identify uniquely on probe.
            key += 1ULL << 48;
            table.store(key, 0, 0, 1, TT_EXACT, m, 0);

            TTEntry e;
            REQUIRE(table.probe(key, e, 0));
            CHECK(e.best_move.from == m.from);
            CHECK(e.best_move.to == m.to);
            CHECK(e.best_move.promotion == m.promotion);
        }
    }
}

TEST_CASE("TT: prefetch is safe on any key", "[tt]") {
    TranspositionTable table(1);
    // Smoke test: prefetch is a hint and must never crash or mutate state.
    // The probe afterward confirms the table is still empty and functional.
    table.prefetch(0);
    table.prefetch(0xFFFFFFFFFFFFFFFFULL);

    TTEntry entry;
    CHECK_FALSE(table.probe(0, entry, 0));
}

TEST_CASE("TT: UCI Hash setting reaches a spawned search thread", "[tt][global]") {
    // Regression for the bug where every `go` UCI command spawned a fresh
    // search thread that default-constructed its own thread_local TT,
    // silently ignoring the user's Hash setting and dropping all TT state
    // between commands. Sizing the table on the test thread, then running a
    // search on a separate thread, must observe the configured hash on the
    // spawned side and persist entries across consecutive searches.

    initSearch();
    // Pick a hash small enough for a depth-10 search to land entries in the
    // hashfull sample range with high confidence, but large enough that the
    // search is not dominated by TT collisions.
    setHashSize(1);
    clearTT();

    Board board;
    board.setStartPos();

    int firstHashfull = -1;
    std::thread first([&] {
        (void)findBestMove(board, 10);
        firstHashfull = getHashfull();
    });
    first.join();
    // A depth-10 search from startpos must fill a meaningful fraction of a
    // 1 MB TT. If the spawned thread had silently fallen back to a private
    // 16 MB thread_local instance, hashfull on the test thread would read
    // zero because the entries never landed in `g_mainTT`.
    REQUIRE(firstHashfull > 0);

    int secondInitialHashfull = -1;
    std::thread second([&] { secondInitialHashfull = getHashfull(); });
    second.join();
    // A freshly spawned thread must already see the persistent TT carrying
    // entries from the first search, proving the table is truly global and
    // not re-created per spawned search thread.
    CHECK(secondInitialHashfull > 0);

    clearTT();
    int afterClear = -1;
    std::thread third([&] { afterClear = getHashfull(); });
    third.join();
    CHECK(afterClear == 0);
}
