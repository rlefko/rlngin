#include "bitboard.h"
#include "board.h"
#include "catch_amalgamated.hpp"
#include "move_picker.h"
#include "movegen.h"
#include "search.h"
#include <algorithm>
#include <set>
#include <vector>

static void ensureInit() {
    static bool done = false;
    if (!done) {
        initBitboards();
        done = true;
    }
}

static uint64_t moveKey(const Move &m) {
    return (static_cast<uint64_t>(m.from) << 16) | (static_cast<uint64_t>(m.to) << 8) |
           static_cast<uint64_t>(m.promotion);
}

static std::vector<PickedMove> drainPicker(MovePicker &picker, Move skip = {0, 0, None}) {
    std::vector<PickedMove> all;
    PickedMove pm;
    while (picker.next(pm, skip)) {
        all.push_back(pm);
    }
    return all;
}

TEST_CASE("MovePicker: TT move is yielded first when legal", "[move_picker]") {
    ensureInit();
    Board board;
    board.setFen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

    // Pick a legal move from movegen to use as TT move.
    auto all = generateLegalMoves(board);
    REQUIRE(!all.empty());
    Move tt = all.front();

    SearchState state;
    MovePicker picker(board, state, 0, tt, false);
    PickedMove first;
    REQUIRE(picker.next(first));
    CHECK(first.phase == PickPhase::TTMove);
    CHECK(first.move.from == tt.from);
    CHECK(first.move.to == tt.to);
    CHECK(first.move.promotion == tt.promotion);
}

TEST_CASE("MovePicker: illegal TT move is skipped cleanly", "[move_picker]") {
    ensureInit();
    Board board;
    board.setFen("4k3/8/8/8/8/8/8/4K3 w - - 0 1");

    // A1-to-A8 with no piece on A1 is plainly illegal in this position.
    Move bogus = {0, 56, None};
    SearchState state;
    MovePicker picker(board, state, 0, bogus, false);
    PickedMove first;
    REQUIRE(picker.next(first));
    CHECK(first.phase != PickPhase::TTMove);
}

TEST_CASE("MovePicker: good captures precede quiets and bad captures trail", "[move_picker]") {
    ensureInit();
    Board board;
    // White to move with several captures and many quiet options.
    board.setFen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

    SearchState state;
    Move noTT = {0, 0, None};
    MovePicker picker(board, state, 0, noTT, false);
    auto picks = drainPicker(picker);

    int firstQuietIdx = -1;
    int lastGoodCaptureIdx = -1;
    int firstBadCaptureIdx = -1;
    for (size_t i = 0; i < picks.size(); i++) {
        const auto &p = picks[i];
        if (p.phase == PickPhase::GoodCaptures) lastGoodCaptureIdx = static_cast<int>(i);
        if (p.phase == PickPhase::Quiets && firstQuietIdx == -1)
            firstQuietIdx = static_cast<int>(i);
        if (p.phase == PickPhase::BadCaptures && firstBadCaptureIdx == -1)
            firstBadCaptureIdx = static_cast<int>(i);
    }

    if (lastGoodCaptureIdx != -1 && firstQuietIdx != -1) {
        CHECK(lastGoodCaptureIdx < firstQuietIdx);
    }
    if (firstQuietIdx != -1 && firstBadCaptureIdx != -1) {
        CHECK(firstQuietIdx < firstBadCaptureIdx);
    }
}

TEST_CASE("MovePicker: yields the full legal move set exactly once", "[move_picker]") {
    ensureInit();
    const char *fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    };
    for (const char *f : fens) {
        Board board;
        board.setFen(f);
        auto expected = generateLegalMoves(board);
        std::set<uint64_t> expectedKeys;
        for (const Move &m : expected) expectedKeys.insert(moveKey(m));

        SearchState state;
        Move noTT = {0, 0, None};
        MovePicker picker(board, state, 0, noTT, false);
        auto picks = drainPicker(picker);

        std::set<uint64_t> pickedKeys;
        for (const auto &p : picks) pickedKeys.insert(moveKey(p.move));

        CHECK(picks.size() == expected.size());
        CHECK(pickedKeys == expectedKeys);
    }
}

TEST_CASE("MovePicker: skipMove is never yielded", "[move_picker]") {
    ensureInit();
    Board board;
    board.setFen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

    auto all = generateLegalMoves(board);
    REQUIRE(all.size() >= 2);
    Move skip = all[1];

    SearchState state;
    Move tt = all[0];
    MovePicker picker(board, state, 0, tt, false);
    auto picks = drainPicker(picker, skip);

    for (const auto &p : picks) {
        CHECK_FALSE((p.move.from == skip.from && p.move.to == skip.to &&
                     p.move.promotion == skip.promotion));
    }
}

TEST_CASE("MovePicker: qsearch path emits only captures when not in check", "[move_picker]") {
    ensureInit();
    Board board;
    board.setFen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

    auto allCaps = generateLegalCaptures(board);
    std::set<uint64_t> capKeys;
    for (const Move &m : allCaps) capKeys.insert(moveKey(m));

    SearchState state;
    Move noTT = {0, 0, None};
    MovePicker picker(board, state, 0, noTT, false, true);
    auto picks = drainPicker(picker);

    std::set<uint64_t> pickedKeys;
    for (const auto &p : picks) {
        pickedKeys.insert(moveKey(p.move));
        CHECK((p.phase == PickPhase::QsTTMove || p.phase == PickPhase::QsCaptures));
    }
    CHECK(pickedKeys == capKeys);
}

TEST_CASE("MovePicker: qsearch in check yields every legal evasion", "[move_picker]") {
    ensureInit();
    Board board;
    // Black bishop on e6 gives check to the white king on e1 via the diagonal;
    // engine must find at least one legal evasion.
    board.setFen("4k3/8/4b3/8/8/8/8/4K3 w - - 0 1");
    auto allLegal = generateLegalMoves(board);
    std::set<uint64_t> legalKeys;
    for (const Move &m : allLegal) legalKeys.insert(moveKey(m));

    SearchState state;
    Move noTT = {0, 0, None};
    MovePicker picker(board, state, 0, noTT, true, true);
    auto picks = drainPicker(picker);

    std::set<uint64_t> pickedKeys;
    for (const auto &p : picks) pickedKeys.insert(moveKey(p.move));
    CHECK(pickedKeys == legalKeys);
}
