#include "bitboard.h"
#include "board.h"
#include "catch_amalgamated.hpp"
#include "eval.h"
#include "search.h"
#include "zobrist.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

static void ensureInit() {
    static bool done = false;
    if (!done) {
        initBitboards();
        zobrist::init();
        initSearch();
        done = true;
    }
}

// Vertical mirror of a FEN: flip the board ranks, swap each piece character's
// case, toggle side-to-move, swap castling-right cases, and flip the en-passant
// square's rank. The resulting position is color-equivalent to the original,
// so evaluate() must return the same stm-POV value for both.
static std::string mirrorFen(const std::string &fen) {
    std::istringstream ss(fen);
    std::string boardStr, side, castling, ep;
    ss >> boardStr >> side >> castling >> ep;

    std::vector<std::string> ranks;
    std::string current;
    for (char c : boardStr) {
        if (c == '/') {
            ranks.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    ranks.push_back(current);
    REQUIRE(ranks.size() == 8);

    auto swapCase = [](char c) -> char {
        if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
        return c;
    };

    std::string mirroredBoard;
    for (int i = 7; i >= 0; i--) {
        if (!mirroredBoard.empty()) mirroredBoard += '/';
        for (char c : ranks[i])
            mirroredBoard += swapCase(c);
    }

    std::string newSide = (side == "w") ? "b" : "w";

    std::string newCastling;
    for (char c : castling)
        newCastling += swapCase(c);

    std::string newEp = ep;
    if (ep.size() == 2 && ep[0] >= 'a' && ep[0] <= 'h') {
        newEp[1] = static_cast<char>('8' - (ep[1] - '1'));
    }

    std::ostringstream out;
    out << mirroredBoard << ' ' << newSide << ' ' << newCastling << ' ' << newEp;

    std::string hm, fm;
    if (ss >> hm) out << ' ' << hm;
    if (ss >> fm) out << ' ' << fm;

    return out.str();
}

// Mirror of a from-to square pair: flip rank via sq ^ 56.
static Move mirrorMove(const Move &m) {
    Move out = m;
    out.from = m.from ^ 56;
    out.to = m.to ^ 56;
    return out;
}

// Embedded slice of the UHO_Lichess_4852_v1 opening corpus. Kept inline
// rather than read from disk so the test stays self-contained: CI's `test`
// job does not run `make fetch-openings`, and even local users without the
// opening book pulled down can run `[symmetry]` directly. The first 30
// positions cover Sicilians, French, Caro-Kann, English, and several gambits,
// which is enough variety to catch any color-asymmetric search behavior.
static const std::vector<std::string> &sampleFens() {
    static const std::vector<std::string> fens = {
        "rnb1kbnr/ppp1pppp/8/4q3/8/2N5/PPPP1PPP/R1BQKBNR w KQkq - 2 4",
        "rnbqkbnr/pp2pppp/2p5/8/2pP4/4P3/PP3PPP/RNBQKBNR w KQkq - 0 4",
        "rnbqk1nr/pppp1ppp/3b4/4p3/3PP3/5N2/PPP2PPP/RNBQKB1R b KQkq - 0 3",
        "r1bqk2r/ppp1bppp/3p1n2/8/3QP3/2N5/PPP1BPPP/R1B1K2R w KQkq - 2 8",
        "rnbqkbnr/ppp2ppp/4P3/8/8/8/PPPP1PPP/RNBQKBNR b KQkq - 0 3",
        "rn1qkbnr/ppp2ppp/4b3/8/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 4",
        "rn1qkb1r/ppp2ppp/4bn2/8/8/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 5",
        "r1bqkbnr/ppp1pppp/3p4/3Pn3/4P3/8/PPP2PPP/RNBQKBNR w KQkq - 1 4",
        "rnbqkbnr/p1pp1ppp/1p2p3/8/3PP3/8/PPP2PPP/RNBQKBNR w KQkq - 0 3",
        "r1bqk1nr/pppp1pp1/2n4p/2b5/2BPP3/5N2/PP3PPP/RNBQK2R b KQkq - 0 6",
        "rnbqkbnr/ppp2ppp/8/3Pp3/8/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 3",
        "rn2kbnr/ppp1pppp/8/q7/6b1/2N2N2/PPPP1PPP/R1BQKB1R w KQkq - 4 5",
        "r1b1kbnr/ppppqppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
        "rn1qkb1r/pbpppppp/1p3n2/8/3PP3/3B4/PPP2PPP/RNBQK1NR w KQkq - 3 4",
        "rnbqkbnr/p1pppppp/8/1p6/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2",
        "rnbqkbnr/p2p1ppp/4p3/2pP4/2B1P3/8/PP3PPP/RNBQK1NR b KQkq - 0 5",
        "r1bqkb1r/pppp1ppp/2n2n2/4P3/4P3/5N2/PPP2PPP/RNBQKB1R b KQkq - 0 4",
        "rnbqk1nr/p1ppppbp/1p4p1/8/3PP3/5N2/PPP2PPP/RNBQKB1R w KQkq - 0 4",
        "rnbqkbnr/pp3ppp/3p4/2pPp3/2P5/2N5/PP2PPPP/R1BQKBNR b KQkq - 1 4",
        "rnbqkbnr/ppp3pp/4p3/3p1p2/2PP4/2N5/PP2PPPP/R1BQKBNR w KQkq - 0 4",
        "r1bqk2r/ppppbppp/5n2/6B1/3QP3/8/PPP2PPP/RN2KB1R w KQkq - 3 7",
        "rnbqkb1r/ppp2ppp/4Pn2/8/8/2N5/PPPP1PPP/R1BQKBNR b KQkq - 0 4",
        "r2qkbnr/ppp3pp/2p5/4Nb2/4p3/8/PPPP1PPP/RNBQK2R w KQkq - 0 7",
        "rn1qk1nr/pbppppbp/1p4p1/8/3PP3/2NB4/PPP2PPP/R1BQK1NR w KQkq - 2 5",
        "r1bqkb1r/pp1p1ppp/2n1pn2/2p3N1/2B1P3/8/PPPP1PPP/RNBQK2R w KQkq - 0 5",
        "r1bqkbnr/p4ppp/2ppp3/2p5/4PP2/3P1N2/PPP3PP/RNBQK2R b KQkq - 0 6",
        "rn1qk1nr/pbppppbp/1p4p1/8/2PPP3/2N2N2/PP3PPP/R1BQKB1R b KQkq - 4 5",
        "rnb1kb1r/pppqpppp/5n2/8/3P4/2N5/PPP2PPP/R1BQKBNR w KQkq - 1 5",
        "rnbqkbnr/ppp1pppp/8/3p4/8/4PQ2/PPPP1PPP/RNB1KBNR b KQkq - 1 2",
        "r1bqkbnr/ppp1nppp/3p4/3Pp3/2P1P3/5N2/PP3PPP/RNBQKB1R b KQkq - 0 5",
    };
    return fens;
}

TEST_CASE("Symmetry: mirrorFen is an involution", "[symmetry]") {
    ensureInit();

    // A round-trip of mirrorFen must yield the original FEN, or the helper
    // itself is busted and every test below is meaningless.
    std::vector<std::string> samples = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "4k3/8/8/3Q4/8/8/8/4K3 w - - 0 1",
        "4k3/8/8/8/3p4/8/3P4/4K3 b - d3 0 1",
        "r3k2r/pppqbppp/2n1bn2/3pp3/3PP3/2N1BN2/PPPQBPPP/R3K2R w KQkq - 0 1",
    };
    for (const std::string &fen : samples) {
        CAPTURE(fen);
        std::string roundTrip = mirrorFen(mirrorFen(fen));
        CHECK(roundTrip == fen);
    }
}

TEST_CASE("Symmetry: evaluate(P) == evaluate(mirror(P)) on startpos", "[symmetry][eval]") {
    ensureInit();
    Board original, mirrored;
    original.setStartPos();
    mirrored.setFen(mirrorFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));
    CHECK(evaluate(original) == evaluate(mirrored));
}

TEST_CASE("Symmetry: evaluate is color-mirror invariant over EPD corpus", "[symmetry][eval]") {
    ensureInit();

    // Probe every embedded position. Any miss here is a static-eval asymmetry.
    const std::vector<std::string> &fens = sampleFens();
    REQUIRE(!fens.empty());

    int mismatches = 0;
    std::string firstBadOrig;
    int firstBadA = 0, firstBadB = 0;
    for (const std::string &fen : fens) {
        Board a, b;
        a.setFen(fen);
        b.setFen(mirrorFen(fen));
        int ea = evaluate(a);
        int eb = evaluate(b);
        if (ea != eb) {
            if (mismatches == 0) {
                firstBadOrig = fen;
                firstBadA = ea;
                firstBadB = eb;
            }
            mismatches++;
        }
    }
    if (mismatches) {
        CAPTURE(firstBadOrig);
        CAPTURE(firstBadA);
        CAPTURE(firstBadB);
        CAPTURE(mismatches);
    }
    CHECK(mismatches == 0);
}

TEST_CASE("Symmetry: search picks the mirrored best move at fixed depth", "[symmetry][search]") {
    ensureInit();

    // generateLegalMoves now sorts its output in a side-to-move-relative
    // order, and every search-time sort has been switched to std::stable_sort
    // so that ordering is preserved through scoring. The combination is
    // mirror-invariant end-to-end: at any fixed depth, a position and its
    // color mirror must pick mirrored best moves.
    const size_t N = 20;
    const int DEPTH = 6;
    const std::vector<std::string> &all = sampleFens();
    REQUIRE(all.size() >= N);
    std::vector<std::string> fens(all.begin(), all.begin() + N);

    int mismatches = 0;
    std::string firstBadFen, firstBadOrig, firstBadMirror, firstBadExpected;
    for (const std::string &fen : fens) {
        clearTT();
        clearPawnHash();
        clearMaterialHash();
        Board a;
        a.setFen(fen);
        Move ma = findBestMove(a, DEPTH);

        clearTT();
        clearPawnHash();
        clearMaterialHash();
        Board b;
        b.setFen(mirrorFen(fen));
        Move mb = findBestMove(b, DEPTH);

        Move maMirrored = mirrorMove(ma);
        bool same = (mb.from == maMirrored.from && mb.to == maMirrored.to &&
                     mb.promotion == maMirrored.promotion);
        if (!same && mismatches == 0) {
            firstBadFen = fen;
            firstBadOrig = moveToString(ma);
            firstBadMirror = moveToString(mb);
            firstBadExpected = moveToString(maMirrored);
        }
        if (!same) mismatches++;
    }
    if (mismatches > 0) {
        CAPTURE(firstBadFen);
        CAPTURE(firstBadOrig);
        CAPTURE(firstBadMirror);
        CAPTURE(firstBadExpected);
        CAPTURE(mismatches);
    }
    CHECK(mismatches == 0);
}
