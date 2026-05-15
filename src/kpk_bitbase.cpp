#include "kpk_bitbase.h"

#include "bitboard.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace Kpk {

namespace {

// Folded-file encoding: pawn files a..d (0..3) cover the board through
// horizontal mirroring. Pawn ranks span 2..7 (rank indices 1..6) since a
// pawn on rank 1 or 8 cannot occur in a legal middle-of-game position.
// Side to move multiplies the index space by 2. The final indexable
// space is `stm * 24 * 64 * 64 + pawnIdx * 64 * 64 + wkSq * 64 + bkSq`.
constexpr int kPawnSlots = 4 * 6;                     // 24
constexpr int kIndexCount = 2 * kPawnSlots * 64 * 64; // 196608

enum Result : uint8_t {
    INVALID = 0,
    UNKNOWN = 1,
    DRAW = 2,
    WIN = 3,
};

uint64_t g_winBits[kIndexCount / 64];
bool g_initialized = false;

inline int pawnIdxOf(int file, int rank) {
    return file * 6 + (rank - 1);
}

inline int indexOf(int stm, int pawnIdx, int wkSq, int bkSq) {
    return ((stm * kPawnSlots + pawnIdx) * 64 + wkSq) * 64 + bkSq;
}

// Determine whether the white pawn on wpSq attacks the square `target`.
// Pawns attack only the two forward diagonals, so the test is purely
// geometric and does not need the board state.
inline bool pawnAttacks(int wpSq, int target) {
    int wpFile = squareFile(wpSq);
    int wpRank = squareRank(wpSq);
    if (wpRank >= 7) return false;
    int tFile = squareFile(target);
    int tRank = squareRank(target);
    return tRank == wpRank + 1 && (tFile == wpFile - 1 || tFile == wpFile + 1);
}

// Classify a single position based on the current results buffer. The
// same function runs in the initial pass (where every successor is still
// UNKNOWN) and in the fixpoint iterations (where successors gradually
// resolve to WIN or DRAW). Terminal cases like stalemate, mate, and
// promotion outcomes resolve immediately without looking at successors.
Result classify(int stm, int wpSq, int wkSq, int bkSq, const std::vector<Result> &results) {
    if (wkSq == bkSq || wkSq == wpSq || bkSq == wpSq) return INVALID;
    if (chebyshev(wkSq, bkSq) <= 1) return INVALID;

    int wpFile = squareFile(wpSq);
    int wpRank = squareRank(wpSq);
    if (wpRank < 1 || wpRank > 6) return INVALID;

    bool wpAttacksBk = pawnAttacks(wpSq, bkSq);
    // With white to move, the black king cannot already be in check
    // from the white pawn: that would mean black left the king in check
    // on the previous move.
    if (stm == 0 && wpAttacksBk) return INVALID;

    if (stm == 0) {
        bool anyLegal = false;
        bool anyWin = false;
        bool anyUnknown = false;

        Bitboard wkMoves = KingAttacks[wkSq];
        while (wkMoves) {
            int to = popLsb(wkMoves);
            if (to == wpSq) continue;
            if (chebyshev(to, bkSq) <= 1) continue;
            anyLegal = true;
            int succ = indexOf(1, pawnIdxOf(wpFile, wpRank), to, bkSq);
            Result rs = results[succ];
            if (rs == WIN) {
                anyWin = true;
            } else if (rs == UNKNOWN) {
                anyUnknown = true;
            }
        }

        int oneFwd = wpSq + 8;
        bool pushClear = (oneFwd != wkSq && oneFwd != bkSq);
        if (pushClear) {
            anyLegal = true;
            if (wpRank == 6) {
                // Promotion to queen. The post-promotion KQK position
                // is treated as won unless the black king can capture
                // the freshly minted queen with no white-king recapture
                // available, which collapses to a bare-kings draw.
                bool bkAdjacent = chebyshev(oneFwd, bkSq) == 1;
                bool wkDefends = chebyshev(oneFwd, wkSq) == 1;
                bool capturable = bkAdjacent && !wkDefends;
                if (!capturable) anyWin = true;
            } else {
                int succ = indexOf(1, pawnIdxOf(wpFile, wpRank + 1), wkSq, bkSq);
                Result rs = results[succ];
                if (rs == WIN) {
                    anyWin = true;
                } else if (rs == UNKNOWN) {
                    anyUnknown = true;
                }
            }
        }

        if (wpRank == 1) {
            int twoFwd = wpSq + 16;
            bool clear = (oneFwd != wkSq && oneFwd != bkSq) && (twoFwd != wkSq && twoFwd != bkSq);
            if (clear) {
                anyLegal = true;
                int succ = indexOf(1, pawnIdxOf(wpFile, 3), wkSq, bkSq);
                Result rs = results[succ];
                if (rs == WIN) {
                    anyWin = true;
                } else if (rs == UNKNOWN) {
                    anyUnknown = true;
                }
            }
        }

        // White cannot be in check in any legal KPK position, so no
        // legal moves means stalemate.
        if (!anyLegal) return DRAW;
        if (anyWin) return WIN;
        if (anyUnknown) return UNKNOWN;
        return DRAW;
    }

    bool anyLegal = false;
    bool anyDraw = false;
    bool anyUnknown = false;

    Bitboard bkMoves = KingAttacks[bkSq];
    while (bkMoves) {
        int to = popLsb(bkMoves);
        if (to == wkSq) continue;
        if (chebyshev(to, wkSq) <= 1) continue;
        bool attackedByPawn = pawnAttacks(wpSq, to);
        if (to == wpSq) {
            if (chebyshev(wpSq, wkSq) <= 1) continue;
            anyLegal = true;
            anyDraw = true; // capturing the pawn collapses to KK
            continue;
        }
        if (attackedByPawn) continue;
        anyLegal = true;
        int succ = indexOf(0, pawnIdxOf(wpFile, wpRank), wkSq, to);
        Result rs = results[succ];
        if (rs == DRAW) {
            anyDraw = true;
        } else if (rs == UNKNOWN) {
            anyUnknown = true;
        }
    }

    if (!anyLegal) {
        return wpAttacksBk ? WIN : DRAW;
    }
    if (anyDraw) return DRAW;
    if (anyUnknown) return UNKNOWN;
    return WIN;
}

} // namespace

void init() {
    if (g_initialized) return;
    g_initialized = true;

    // Initialize every slot as UNKNOWN so a not-yet-classified successor
    // looked up during the initial pass cannot be mistaken for a draw.
    // classify() flips the slot to INVALID for kings-overlap and other
    // structurally impossible configurations.
    std::vector<Result> results(kIndexCount, UNKNOWN);

    auto idxFromPawn = [](int pawnIdx) {
        int file = pawnIdx / 6;
        int rank = (pawnIdx % 6) + 1;
        return rank * 8 + file;
    };

    for (int stm = 0; stm < 2; stm++) {
        for (int pawnIdx = 0; pawnIdx < kPawnSlots; pawnIdx++) {
            int wpSq = idxFromPawn(pawnIdx);
            for (int wk = 0; wk < 64; wk++) {
                for (int bk = 0; bk < 64; bk++) {
                    results[indexOf(stm, pawnIdx, wk, bk)] = classify(stm, wpSq, wk, bk, results);
                }
            }
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (int stm = 0; stm < 2; stm++) {
            for (int pawnIdx = 0; pawnIdx < kPawnSlots; pawnIdx++) {
                int wpSq = idxFromPawn(pawnIdx);
                for (int wk = 0; wk < 64; wk++) {
                    for (int bk = 0; bk < 64; bk++) {
                        int idx = indexOf(stm, pawnIdx, wk, bk);
                        if (results[idx] != UNKNOWN) continue;
                        Result r = classify(stm, wpSq, wk, bk, results);
                        if (r != UNKNOWN) {
                            results[idx] = r;
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    std::memset(g_winBits, 0, sizeof(g_winBits));
    for (int i = 0; i < kIndexCount; i++) {
        if (results[i] == WIN) {
            g_winBits[i >> 6] |= 1ULL << (i & 63);
        }
    }
}

bool probe(Color strongSide, int strongKingSq, int pawnSq, int weakKingSq, Color stm) {
    if (strongSide == Black) {
        strongKingSq ^= 56;
        pawnSq ^= 56;
        weakKingSq ^= 56;
        stm = (stm == White) ? Black : White;
    }
    if (squareFile(pawnSq) >= 4) {
        strongKingSq ^= 7;
        pawnSq ^= 7;
        weakKingSq ^= 7;
    }
    int pawnRank = squareRank(pawnSq);
    if (pawnRank < 1 || pawnRank > 6) return false;
    int pawnFile = squareFile(pawnSq);
    int pawnIdx = pawnIdxOf(pawnFile, pawnRank);
    int idx = indexOf(stm == White ? 0 : 1, pawnIdx, strongKingSq, weakKingSq);
    return ((g_winBits[idx >> 6] >> (idx & 63)) & 1ULL) != 0;
}

} // namespace Kpk
