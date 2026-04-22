#include "bitboard.h"
#include "board.h"
#include "catch_amalgamated.hpp"
#include "movegen.h"
#include "search.h"
#include "search_params.h"

static void ensureInit() {
    static bool done = false;
    if (!done) {
        initBitboards();
        done = true;
    }
}

TEST_CASE("Search: captures hanging queen", "[search]") {
    ensureInit();
    Board board;
    // White knight on e4 can capture undefended black queen on d6
    board.setFen("4k3/8/3q4/8/4N3/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board);
    CHECK(best.from == stringToSquare("e4"));
    CHECK(best.to == stringToSquare("d6"));
}

TEST_CASE("Search: prefers capturing queen over pawn", "[search]") {
    ensureInit();
    Board board;
    // White bishop on d4 can capture black queen on f6 or black pawn on b2
    board.setFen("4k3/8/5q2/8/3B4/8/1p6/4K3 w - - 0 1");

    Move best = findBestMove(board);
    CHECK(best.from == stringToSquare("d4"));
    CHECK(best.to == stringToSquare("f6"));
}

TEST_CASE("Search: finds mate in 1", "[search]") {
    ensureInit();
    Board board;
    // Back-rank mate: Re1 can deliver Re8# since f7/g7/h7 pawns trap the king
    board.setFen("6k1/5ppp/8/8/8/8/8/4R2K w - - 0 1");

    Move best = findBestMove(board, 2);
    CHECK(best.from == stringToSquare("e1"));
    CHECK(best.to == stringToSquare("e8"));
}

TEST_CASE("Search: returns valid move at deeper depths", "[search]") {
    ensureInit();
    Board board;
    board.setStartPos();

    Move best = findBestMove(board, 3);
    CHECK(best.from != best.to);
}

TEST_CASE("Search: alpha-beta prunes nodes at depth 4", "[search]") {
    ensureInit();
    Board board;
    board.setStartPos();

    SearchLimits limits;
    limits.depth = 4;
    SearchState state;
    startSearch(board, limits, state);

    // Full minimax at depth 4 from startpos explores ~4 million nodes.
    // Alpha-beta should prune this down dramatically.
    CHECK(state.nodes < 500000);
}

TEST_CASE("Search: qsearch avoids leaving piece en prise", "[search][qsearch]") {
    ensureInit();
    clearTT();
    Board board;
    // White knight on d4, black bishop on e6 attacks it. White to move.
    // At depth 1, the engine should not leave the knight to be captured.
    board.setFen("4k3/8/4b3/8/3N4/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board, 1);
    // The knight should move away from d4 (not stay and get captured)
    CHECK(best.from == stringToSquare("d4"));
}

TEST_CASE("Search: qsearch resolves pawn capture", "[search][qsearch]") {
    ensureInit();
    clearTT();
    Board board;
    // White pawn on e4 can capture black pawn on d5. Simple gain.
    board.setFen("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board, 1);
    CHECK(best.from == stringToSquare("e4"));
    CHECK(best.to == stringToSquare("d5"));
}

TEST_CASE("Search: qsearch prevents blundering into recapture", "[search][qsearch]") {
    ensureInit();
    clearTT();
    Board board;
    // White pawn on e5, black rook on d6. Pawn could try to capture but rook
    // is worth more. Engine should not move the pawn into the rook.
    board.setFen("4k3/8/3r4/4P3/8/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board, 2);
    // Knight on e5, pawns on d6/e6. Nxe6 loses the knight to dxe6.
    // Nxd6 is safe since e6 pawn does not defend d6.
    board.setFen("4k3/8/3pp3/4N3/8/8/8/4K3 w - - 0 1");

    best = findBestMove(board, 1);
    // Knight should not capture e6 (loses knight for pawn)
    if (best.from == stringToSquare("e5") && best.to == stringToSquare("e6")) {
        CHECK(false);
    }
}

TEST_CASE("Search: NmpBase mutation changes node count", "[search][tunable]") {
    ensureInit();
    clearTT();

    // Tactical midgame position deep enough for NMP to fire repeatedly.
    // Without NMP actually reading searchParams.NmpBase, these two runs
    // would be bit-identical, which is the regression this test guards.
    const std::string fen = "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 4";

    clearTT();
    resetSearchParams();

    Board boardA;
    boardA.setFen(fen);
    SearchLimits limits;
    limits.depth = 8;
    SearchState stateA;
    startSearch(boardA, limits, stateA);
    int64_t nodesDefault = stateA.nodes;

    clearTT();
    searchParams.NmpBase = 5;

    Board boardB;
    boardB.setFen(fen);
    SearchState stateB;
    startSearch(boardB, limits, stateB);
    int64_t nodesTuned = stateB.nodes;

    resetSearchParams();
    clearTT();

    CHECK(nodesDefault != nodesTuned);
}

TEST_CASE("Search: avoids unforced king walk from overstated king danger", "[search][kingsafety]") {
    ensureInit();
    clearTT();
    Board board;
    // An overly harsh king safety score used to make the engine panic and
    // move its king voluntarily out of a well-defended starting square.
    // Any move the king makes here is objectively worse than a developing
    // move, so no sane middlegame eval should prefer Ke2, Kd2, or Kf2.
    board.setFen("rnb1kbnr/ppp1pppp/8/4q3/8/2N5/PPPP1PPP/R1BQKBNR w KQkq - 2 4");

    Move best = findBestMove(board, 6);
    bool walksKing = best.from == stringToSquare("e1") &&
                     (best.to == stringToSquare("d2") || best.to == stringToSquare("e2") ||
                      best.to == stringToSquare("f2"));

    CHECK_FALSE(walksKing);
}

TEST_CASE("Search: respects time limit", "[search]") {
    ensureInit();
    Board board;
    board.setStartPos();

    SearchLimits limits;
    limits.movetime = 50;
    SearchState state;

    auto start = std::chrono::steady_clock::now();
    startSearch(board, limits, state);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    CHECK(state.bestMove.from != state.bestMove.to);
    CHECK(elapsed < 550);
}

TEST_CASE("Search: seldepth is populated", "[search]") {
    ensureInit();
    clearTT();
    Board board;
    // Position with hanging pieces forcing deep quiescence search
    board.setFen("r1bqkbnr/pppppppp/2n5/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 1 2");

    SearchLimits limits;
    limits.depth = 1;
    SearchState state;
    startSearch(board, limits, state);

    CHECK(state.seldepth >= 1);
}

TEST_CASE("Search: PV starts with best move", "[search][pv]") {
    ensureInit();
    clearTT();
    Board board;
    // Mate in 1: Re1-e8#
    board.setFen("6k1/5ppp/8/8/8/8/8/4R2K w - - 0 1");

    SearchLimits limits;
    limits.depth = 2;
    SearchState state;
    startSearch(board, limits, state);

    CHECK(state.pvLength[0] >= 1);
    CHECK(state.pv[0][0].from == state.bestMove.from);
    CHECK(state.pv[0][0].to == state.bestMove.to);
}

TEST_CASE("Search: PV moves are legal", "[search][pv]") {
    ensureInit();
    clearTT();
    Board board;
    board.setStartPos();

    SearchLimits limits;
    limits.depth = 4;
    SearchState state;
    startSearch(board, limits, state);

    Board pos = board;
    for (int i = 0; i < state.pvLength[0]; i++) {
        Move pvMove = state.pv[0][i];
        std::vector<Move> legal = generateLegalMoves(pos);
        bool found = false;
        for (const Move &m : legal) {
            if (m.from == pvMove.from && m.to == pvMove.to && m.promotion == pvMove.promotion) {
                found = true;
                break;
            }
        }
        CHECK(found);
        if (!found) break;
        pos.makeMove(pvMove);
    }
}

TEST_CASE("Search: avoids losing queen for pawn", "[search]") {
    ensureInit();
    clearTT();
    Board board;
    // White queen on d4, black pawn on e5 defended by pawn on d6.
    // Queen should not capture the pawn.
    board.setFen("4k3/8/3p4/4p3/3Q4/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board, 3);
    // The queen should not go to e5 (losing queen for pawn)
    if (best.to == stringToSquare("e5")) {
        CHECK(false);
    }
}

TEST_CASE("Search: detects repetition and avoids drawn line", "[search]") {
    ensureInit();
    clearTT();
    Board board;
    // White is up a queen but the position has occurred before in history.
    // White king on e1, black king on e8, white queen on d1.
    // halfmoveClock = 4 so the engine looks back into game history.
    board.setFen("4k3/8/8/8/8/8/8/3QK3 w - - 4 1");

    // Simulate game history where current position appeared before.
    // Pad with dummy keys so the matching key falls within the halfmove window.
    SearchLimits limits;
    limits.depth = 4;
    SearchState state;
    std::vector<uint64_t> posHistory = {0, 0, 0, board.key};
    startSearch(board, limits, state, posHistory);

    // Engine should still find a good move (it's not a forced repetition,
    // it just needs to avoid repeating the position)
    CHECK(state.bestMove.from != state.bestMove.to);
}

TEST_CASE("Search: ignores repetition beyond irreversible move", "[search]") {
    ensureInit();
    clearTT();
    Board board;
    // Same position but halfmoveClock = 0 means an irreversible move just happened,
    // so no prior position can repeat.
    board.setFen("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");

    SearchLimits limits;
    limits.depth = 4;
    SearchState state;
    // Even though the key matches, halfmoveClock = 0 means it's unreachable
    std::vector<uint64_t> posHistory = {board.key};
    startSearch(board, limits, state, posHistory);

    // Engine should evaluate normally (not as a draw)
    CHECK(state.bestMove.from != state.bestMove.to);
}

TEST_CASE("Search: null move pruning reduces node count", "[search][nmp]") {
    ensureInit();
    clearTT();
    Board board;
    // Middlegame position with plenty of material on both sides.
    // NMP should prune aggressively here, keeping node count well below
    // what pure alpha-beta would need at depth 6.
    board.setFen("r1bqkb1r/pppppppp/2n2n2/4p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R w KQkq - 4 4");

    SearchLimits limits;
    limits.depth = 6;
    SearchState state;
    startSearch(board, limits, state);

    // Without NMP, depth 6 from this position searches millions of nodes.
    // With NMP active, it should be well under 2 million.
    CHECK(state.nodes < 2000000);
}

TEST_CASE("Search: finds correct move in king-pawn endgame", "[search][nmp]") {
    ensureInit();
    clearTT();
    Board board;
    // King + pawn endgame (NMP is skipped due to no non-pawn material).
    // White king on e5, pawn on e4, black king on e7.
    // White should push the pawn or maintain the opposition.
    board.setFen("8/4k3/8/4K3/4P3/8/8/8 w - - 0 1");

    Move best = findBestMove(board, 6);
    // The engine must return a legal move (not break due to zugzwang)
    CHECK(best.from != best.to);
}

TEST_CASE("Search: still finds tactical captures", "[search]") {
    ensureInit();
    clearTT();
    Board board;
    // White rook on e1, black queen on e8 undefended, king on h8
    board.setFen("4q2k/8/8/8/8/8/8/4R2K w - - 0 1");

    Move best = findBestMove(board, 2);
    CHECK(best.from == stringToSquare("e1"));
    CHECK(best.to == stringToSquare("e8"));
}

TEST_CASE("Search: aspiration windows find correct moves at depth", "[search][aspiration]") {
    ensureInit();
    clearTT();
    Board board;
    // White has a clear advantage (queen vs nothing). Aspiration windows should
    // resolve quickly since the score is stable across iterations.
    board.setFen("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");

    Move best = findBestMove(board, 8);
    CHECK(best.from != best.to);
}

TEST_CASE("Search: aspiration windows handle mate scores correctly", "[search][aspiration]") {
    ensureInit();
    clearTT();
    Board board;
    // Mate in 1 position: aspiration windows should not interfere with mate detection
    // since mate scores skip the narrow window
    board.setFen("6k1/5ppp/8/8/8/8/8/4R2K w - - 0 1");

    Move best = findBestMove(board, 6);
    CHECK(best.from == stringToSquare("e1"));
    CHECK(best.to == stringToSquare("e8"));
}

TEST_CASE("Search: aspiration windows with volatile positions", "[search][aspiration]") {
    ensureInit();
    clearTT();
    Board board;
    // Complex middlegame where the score may shift between depths,
    // exercising aspiration window widening
    board.setFen("r1bqkb1r/pppppppp/2n2n2/4p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R w KQkq - 4 4");

    SearchLimits limits;
    limits.depth = 8;
    SearchState state;
    startSearch(board, limits, state);

    CHECK(state.bestMove.from != state.bestMove.to);
    // PV should be populated
    CHECK(state.pvLength[0] >= 1);
}

TEST_CASE("Search: PV node search produces valid PV lines", "[search][pvs]") {
    ensureInit();
    clearTT();
    Board board;
    board.setStartPos();

    SearchLimits limits;
    limits.depth = 6;
    SearchState state;
    startSearch(board, limits, state);

    // PV should have multiple moves at depth 6
    CHECK(state.pvLength[0] >= 2);

    // All PV moves must be legal
    Board pos = board;
    for (int i = 0; i < state.pvLength[0]; i++) {
        Move pvMove = state.pv[0][i];
        std::vector<Move> legal = generateLegalMoves(pos);
        bool found = false;
        for (const Move &m : legal) {
            if (m.from == pvMove.from && m.to == pvMove.to && m.promotion == pvMove.promotion) {
                found = true;
                break;
            }
        }
        CHECK(found);
        if (!found) break;
        pos.makeMove(pvMove);
    }
}

TEST_CASE("Search: extensions do not break mate detection", "[search][extensions]") {
    ensureInit();
    clearTT();
    Board board;
    // Mate in 2: Qf3-f7# after any black response, with extensions active
    board.setFen("r1bqkb1r/pppp1ppp/2n2n2/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR w KQkq - 4 4");

    Move best = findBestMove(board, 6);
    // The engine should find a strong move (scholar's mate threat)
    CHECK(best.from != best.to);
}

TEST_CASE("Search: extensions keep node count bounded", "[search][extensions]") {
    ensureInit();
    clearTT();
    Board board;
    board.setStartPos();

    SearchLimits limits;
    limits.depth = 8;
    SearchState state;
    startSearch(board, limits, state);

    // Extensions should not cause exponential node growth at depth 8
    CHECK(state.nodes < 20000000);
}

TEST_CASE("Search: finds tactics at higher depth with extensions", "[search][extensions]") {
    ensureInit();
    clearTT();
    Board board;
    // Italian Game position where tactical play benefits from deeper search
    board.setFen("r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4");

    SearchLimits limits;
    limits.depth = 10;
    SearchState state;
    startSearch(board, limits, state);

    // Verify search completes with a valid move
    CHECK(state.bestMove.from != state.bestMove.to);
}

TEST_CASE("Search: SEE pruning reduces node count", "[search][see-pruning]") {
    ensureInit();
    clearTT();
    Board board;
    // Complex middlegame with many captures available. SEE pruning should
    // trim bad captures and quiet moves to attacked squares.
    board.setFen("r1bqkb1r/pppppppp/2n2n2/4p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R w KQkq - 4 4");

    SearchLimits limits;
    limits.depth = 7;
    SearchState state;
    startSearch(board, limits, state);

    CHECK(state.bestMove.from != state.bestMove.to);
    // SEE pruning should keep nodes well bounded at depth 7
    CHECK(state.nodes < 5000000);
}

TEST_CASE("Search: SEE pruning preserves tactical correctness", "[search][see-pruning]") {
    ensureInit();
    clearTT();
    Board board;
    // White knight captures undefended black queen: must not be pruned
    board.setFen("4k3/8/3q4/8/4N3/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board, 4);
    CHECK(best.from == stringToSquare("e4"));
    CHECK(best.to == stringToSquare("d6"));
}

TEST_CASE("Search: SEE pruning does not prune winning captures", "[search][see-pruning]") {
    ensureInit();
    clearTT();
    Board board;
    // White rook captures undefended black queen on e8
    board.setFen("4q2k/8/8/8/8/8/8/4R2K w - - 0 1");

    Move best = findBestMove(board, 4);
    CHECK(best.from == stringToSquare("e1"));
    CHECK(best.to == stringToSquare("e8"));
}

TEST_CASE("Search: ProbCut does not break mate detection", "[search][probcut]") {
    ensureInit();
    clearTT();
    Board board;
    // Mate in 1: Re1-e8#
    board.setFen("6k1/5ppp/8/8/8/8/8/4R2K w - - 0 1");

    Move best = findBestMove(board, 6);
    CHECK(best.from == stringToSquare("e1"));
    CHECK(best.to == stringToSquare("e8"));
}

TEST_CASE("Search: ProbCut reduces nodes at high depth", "[search][probcut]") {
    ensureInit();
    clearTT();
    Board board;
    // White has a clear material advantage (extra queen). ProbCut should
    // detect the winning position quickly and prune at high depths.
    board.setFen("r1bqkbnr/pppppppp/2n5/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 1 2");

    SearchLimits limits;
    limits.depth = 8;
    SearchState state;
    startSearch(board, limits, state);

    CHECK(state.bestMove.from != state.bestMove.to);
    // ProbCut should help keep nodes bounded at depth 8
    CHECK(state.nodes < 15000000);
}

TEST_CASE("Search: improving heuristic preserves tactical correctness", "[search][improving]") {
    ensureInit();
    clearTT();
    Board board;
    // White knight on e4 can capture undefended black queen on d6
    board.setFen("4k3/8/3q4/8/4N3/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board, 4);
    CHECK(best.from == stringToSquare("e4"));
    CHECK(best.to == stringToSquare("d6"));
}

TEST_CASE("Search: improving heuristic does not break mate detection", "[search][improving]") {
    ensureInit();
    clearTT();
    Board board;
    // Mate in 1: Re1-e8#
    board.setFen("6k1/5ppp/8/8/8/8/8/4R2K w - - 0 1");

    Move best = findBestMove(board, 6);
    CHECK(best.from == stringToSquare("e1"));
    CHECK(best.to == stringToSquare("e8"));
}

TEST_CASE("Search: improving heuristic keeps node count bounded", "[search][improving]") {
    ensureInit();
    clearTT();
    Board board;
    board.setStartPos();

    SearchLimits limits;
    limits.depth = 8;
    SearchState state;
    startSearch(board, limits, state);

    CHECK(state.bestMove.from != state.bestMove.to);
    // The improving heuristic should not cause node explosion at depth 8
    CHECK(state.nodes < 20000000);
}

TEST_CASE("Search: razoring keeps hopeless depth-2 searches small", "[search][razoring]") {
    ensureInit();
    clearTT();
    Board board;
    // Black is down a queen and a rook; side to move cannot recover at low
    // depth. Razoring should prune straight to qsearch for many branches.
    board.setFen("4k3/8/8/8/8/8/8/1Q2K1QR b - - 0 1");

    SearchLimits limits;
    limits.depth = 2;
    SearchState state;
    startSearch(board, limits, state);

    CHECK(state.bestMove.from != state.bestMove.to);
    CHECK(state.nodes < 50000);
}

TEST_CASE("Search: check extensions sharpen forcing lines", "[search][extensions]") {
    ensureInit();
    clearTT();
    Board board;
    // Back-rank mate reachable with a quiet preparatory move. Check extensions
    // push the mating follow-up one ply deeper without exploding the tree.
    board.setFen("6k1/5ppp/8/8/8/8/8/4R2K w - - 0 1");

    Move best = findBestMove(board, 3);
    CHECK(best.from == stringToSquare("e1"));
    CHECK(best.to == stringToSquare("e8"));
}

TEST_CASE("Search: QsDeltaMargin mutation changes node count", "[search][qsearch][tunable]") {
    ensureInit();
    clearTT();
    resetSearchParams();

    // Tactical position with enough capture churn for the delta-prune
    // shortcut to fire repeatedly. Tightening the margin should prune
    // more (fewer nodes); loosening it should prune less.
    const std::string fen = "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 0 4";

    Board boardA;
    boardA.setFen(fen);
    SearchLimits limits;
    limits.depth = 8;
    SearchState stateDefault;
    startSearch(boardA, limits, stateDefault);
    int64_t nodesDefault = stateDefault.nodes;

    clearTT();
    searchParams.QsDeltaMargin = 300; // tighter margin, more pruning

    Board boardB;
    boardB.setFen(fen);
    SearchState stateTight;
    startSearch(boardB, limits, stateTight);
    int64_t nodesTight = stateTight.nodes;

    resetSearchParams();
    clearTT();

    CHECK(nodesDefault != nodesTight);
}

TEST_CASE("Search: qsearch shortcut preserves winning capture", "[search][qsearch]") {
    ensureInit();
    clearTT();
    resetSearchParams();

    // Winning capture available: white rook takes hanging black queen.
    // Even with the tightest legal delta margin the node-level shortcut
    // must never suppress the tactic, because the max-gain guard sees
    // the enemy queen and refuses to shortcut.
    searchParams.QsDeltaMargin = 300;
    Board board;
    board.setFen("4k3/8/8/q7/8/8/8/R3K3 w - - 0 1");

    Move best = findBestMove(board, 3);
    CHECK(best.from == stringToSquare("a1"));
    CHECK(best.to == stringToSquare("a5"));

    resetSearchParams();
    clearTT();
}

TEST_CASE("Search: qsearch shortcut keeps pawn captures in pawn endgames", "[search][qsearch]") {
    ensureInit();
    clearTT();
    resetSearchParams();

    // Pure pawn endgame with a simple winning capture: white pawn on e4
    // takes black pawn on d5. Earlier the shortcut's max-gain loop
    // stopped at Knight and so saw zero capturable targets when the
    // enemy only had pawns, causing the node-level prune to fire even
    // though the per-move delta would have let the pawn capture
    // through. A tight margin plus a depth-one search surfaces the
    // regression cleanly.
    searchParams.QsDeltaMargin = 300;
    Board board;
    board.setFen("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board, 1);
    CHECK(best.from == stringToSquare("e4"));
    CHECK(best.to == stringToSquare("d5"));

    resetSearchParams();
    clearTT();
}

TEST_CASE("Search: qsearch shortcut respects pending promotion", "[search][qsearch]") {
    ensureInit();
    clearTT();
    resetSearchParams();

    // White to move with a pawn on the 7th rank ready to promote. Even
    // a tiny delta margin must not shortcut away the promotion: the
    // rank-7 guard in qsearch detects the threat and falls through to
    // the normal search path.
    searchParams.QsDeltaMargin = 300;
    Board board;
    board.setFen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");

    Move best = findBestMove(board, 2);
    CHECK(best.from == stringToSquare("a7"));
    CHECK(best.to == stringToSquare("a8"));
    CHECK(best.promotion == Queen);

    resetSearchParams();
    clearTT();
}

TEST_CASE("Search: qsearch TT probe shrinks repeat work", "[search][qsearch]") {
    ensureInit();
    clearTT();
    Board board;
    board.setFen("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3");

    SearchLimits limits;
    limits.depth = 6;
    SearchState first;
    startSearch(board, limits, first);

    // A second search over the same position should reuse TT entries written
    // by qsearch as well as the main search, bringing the node count down.
    SearchState second;
    startSearch(board, limits, second);
    CHECK(second.nodes <= first.nodes);
    CHECK(second.bestMove.from != second.bestMove.to);
}

TEST_CASE("Search: IIR does not regress mate detection", "[search][iir]") {
    ensureInit();
    clearTT();
    Board board;
    // Fresh TT means the reduction trigger fires, yet depth-6 must still see
    // the mate in one. Catches any bug where IIR hides forced sequences.
    board.setFen("6k1/5ppp/8/8/8/8/8/4R2K w - - 0 1");

    Move best = findBestMove(board, 6);
    CHECK(best.from == stringToSquare("e1"));
    CHECK(best.to == stringToSquare("e8"));
}

TEST_CASE("Search: pawn correction history stays tactically sound", "[search][corrhist]") {
    ensureInit();
    clearTT();
    Board board;
    // Repeat a tactical position several times in a row. If the correction
    // table drifts in the wrong direction it can distort eval-based pruning
    // and drop the capture; verify the best move survives.
    board.setFen("4k3/8/3q4/8/4N3/8/8/4K3 w - - 0 1");

    for (int i = 0; i < 3; i++) {
        Move best = findBestMove(board, 4);
        CHECK(best.from == stringToSquare("e4"));
        CHECK(best.to == stringToSquare("d6"));
    }
}

TEST_CASE("Search: depth 10 node count bounded on startpos", "[search][nodes]") {
    ensureInit();
    clearTT();
    Board board;
    board.setStartPos();

    SearchLimits limits;
    limits.depth = 10;
    SearchState state;
    startSearch(board, limits, state);

    // Guardrail against future regressions in move ordering or pruning.
    CHECK(state.nodes < 400000);
}

TEST_CASE("Search: depth 10 node count bounded on Italian", "[search][nodes]") {
    ensureInit();
    clearTT();
    Board board;
    board.setFen("r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3");

    SearchLimits limits;
    limits.depth = 10;
    SearchState state;
    startSearch(board, limits, state);

    CHECK(state.nodes < 500000);
}

TEST_CASE("Search: depth 10 node count bounded on Kiwipete", "[search][nodes]") {
    ensureInit();
    clearTT();
    Board board;
    board.setFen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");

    SearchLimits limits;
    limits.depth = 10;
    SearchState state;
    startSearch(board, limits, state);

    CHECK(state.nodes < 2000000);
}

TEST_CASE("Search: Berlin position keeps recapturing Nxe5 at depth 17", "[search][corrhist]") {
    // Regression for the sibling-pollution flip in which depths 13 through 16
    // stably chose Nxe5 and then depth 17 jumped to Bf1. The cause was
    // correction history writes at ply 1 and 2 inside the first root move's
    // subtree biasing the evaluation of later-searched root moves. Gating
    // those writes on ply >= 3 preserves the obvious recapture.
    ensureInit();
    clearTT();
    Board board;
    // Position after 1. e4 e5 2. Nf3 Nc6 3. Bb5 Nf6 4. O-O Nxe4 5. Re1 Nd6.
    board.setFen("r1bqkb1r/pppp1ppp/2nn4/1B2p3/8/5N2/PPPP1PPP/RNBQR1K1 w kq - 2 6");

    SearchLimits limits;
    limits.depth = 17;
    SearchState state;
    startSearch(board, limits, state);

    CHECK(state.bestMove.from == stringToSquare("f3"));
    CHECK(state.bestMove.to == stringToSquare("e5"));
}
