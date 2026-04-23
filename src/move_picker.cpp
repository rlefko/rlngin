#include "move_picker.h"
#include "bitboard.h"
#include "eval.h"
#include "movegen.h"
#include "see.h"
#include <algorithm>
#include <cstdlib>

// Tier offsets for multi-ply continuation history: 1-ply, 2-ply, and
// 4-ply back. Kept in sync with the updater in search.cpp via the header
// contract; both files read and write the same table layout.
static constexpr int CONT_HIST_OFFSETS[3] = {1, 2, 4};

// Pawn capture-square set for one color. Mirrors the inline helper used
// inside the evaluator's attack-map builder; duplicated here instead of
// un-staticing that helper so the search module stays independent of
// eval internals.
static inline Bitboard enemyPawnAttacks(Bitboard pawns, Color c) {
    if (c == White) {
        return ((pawns & ~FileABB) << 7) | ((pawns & ~FileHBB) << 9);
    }
    return ((pawns & ~FileABB) >> 9) | ((pawns & ~FileHBB) >> 7);
}

void buildThreatMap(const Board &board, ThreatMap &out) {
    Color us = board.sideToMove;
    Color them = (us == White) ? Black : White;
    Bitboard occ = board.occupied;

    Bitboard byPawn = enemyPawnAttacks(board.byPiece[Pawn] & board.byColor[them], them);

    Bitboard knightAttacks = 0;
    Bitboard knights = board.byPiece[Knight] & board.byColor[them];
    while (knights) {
        knightAttacks |= KnightAttacks[popLsb(knights)];
    }

    Bitboard bishopAtk = 0;
    Bitboard bishops = board.byPiece[Bishop] & board.byColor[them];
    while (bishops) {
        bishopAtk |= bishopAttacks(popLsb(bishops), occ);
    }

    Bitboard rookAtk = 0;
    Bitboard rooks = board.byPiece[Rook] & board.byColor[them];
    while (rooks) {
        rookAtk |= rookAttacks(popLsb(rooks), occ);
    }

    out.byPawn = byPawn;
    out.byMinor = byPawn | knightAttacks | bishopAtk;
    out.byRook = out.byMinor | rookAtk;
}

bool isCapture(const Board &board, const Move &m) {
    if (board.squares[m.to].type != None) return true;
    if (board.squares[m.from].type == Pawn && m.to == board.enPassantSquare &&
        board.enPassantSquare != -1)
        return true;
    return false;
}

PieceType capturedType(const Board &board, const Move &m) {
    if (board.squares[m.to].type != None) return board.squares[m.to].type;
    if (board.squares[m.from].type == Pawn && m.to == board.enPassantSquare &&
        board.enPassantSquare != -1)
        return Pawn;
    return None;
}

int contHistoryScore(const SearchState &state, int ply, PieceType currPt, int currTo) {
    int score = 0;
    for (int t = 0; t < 3; t++) {
        int off = CONT_HIST_OFFSETS[t];
        if (ply >= off) {
            PieceType prevPt = state.movedPiece[ply - off];
            int prevTo = state.moveStack[ply - off].to;
            score += state.historyTables->contHistory[t][prevPt][prevTo][currPt][currTo];
        }
    }
    return score;
}

int scoreMove(const Move &m, const Board &board, const Move &ttMove, int ply,
              const SearchState &state, int *outQuietHistory) {
    PieceType pt = board.squares[m.from].type;
    bool capture = isCapture(board, m);
    bool quietCandidate = !capture && m.promotion == None;

    // Compute the quiet history once and expose it to the caller; LMR reads
    // this instead of recomputing the same lookups after move ordering.
    int quietHist = 0;
    if (quietCandidate) {
        quietHist = state.historyTables->mainHistory[board.sideToMove][m.from][m.to];
        if (ply >= 0) {
            quietHist += contHistoryScore(state, ply, pt, m.to);
        }
        if (outQuietHistory) *outQuietHistory = quietHist;
    }

    // TT move gets highest priority
    if (m.from == ttMove.from && m.to == ttMove.to && m.promotion == ttMove.promotion) {
        return 10000000;
    }

    // Promotions
    if (m.promotion != None) {
        if (m.promotion == Queen) return 9000000;
        return -5000000;
    }

    // Captures: use SEE to separate good from bad (skip SEE in quiescence, ply == -1)
    if (capture) {
        PieceType ct = capturedType(board, m);
        int capHist = state.captureHistory[pt][m.to][ct];
        if (ply < 0) {
            // Quiescence: use MVV-LVA + capture history (no SEE for speed)
            return 5000000 + PieceValue[ct] * 100 - PieceValue[pt] + capHist / 32;
        }
        int seeVal = see(board, m);
        if (seeVal >= 0) {
            return 5000000 + seeVal + capHist / 32;
        } else {
            return -2000000 + seeVal + capHist / 32;
        }
    }

    // Killer moves
    if (ply >= 0) {
        for (int k = 0; k < 2; k++) {
            const Move &killer = state.killers[ply][k];
            if (m.from == killer.from && m.to == killer.to && m.promotion == killer.promotion) {
                return 4000000 - k;
            }
        }
    }

    // Counter-move: the quiet reply that previously refuted the prior move
    if (ply >= 1) {
        Color prevColor = (board.sideToMove == White) ? Black : White;
        PieceType prevPt = state.movedPiece[ply - 1];
        int prevTo = state.moveStack[ply - 1].to;
        const Move &counter = state.historyTables->counterMoves[prevColor][prevPt][prevTo];
        if (m.from == counter.from && m.to == counter.to && m.promotion == counter.promotion) {
            return 3500000;
        }
    }

    // Plain quiet move: fall back to the precomputed history score.
    return quietHist;
}

static inline bool sameMove(const Move &a, const Move &b) {
    return a.from == b.from && a.to == b.to && a.promotion == b.promotion;
}

static inline bool isNullMove(const Move &m) {
    return m.from == 0 && m.to == 0 && m.promotion == None;
}

bool isPseudoLegalMove(const Board &board, const Move &m) {
    if (isNullMove(m)) return false;
    if (m.from < 0 || m.from >= 64 || m.to < 0 || m.to >= 64) return false;
    if (m.from == m.to) return false;

    Color us = board.sideToMove;
    Color them = (us == White) ? Black : White;
    const Piece &fromPiece = board.squares[m.from];
    if (fromPiece.type == None || fromPiece.color != us) return false;

    const Piece &toPiece = board.squares[m.to];
    if (toPiece.type != None && toPiece.color == us) return false;

    PieceType pt = fromPiece.type;
    Bitboard occ = board.occupied;
    Bitboard target = squareBB(m.to);

    // Promotion gate: only pawns reaching the far rank may promote, and a
    // pawn reaching that rank without a promotion is also invalid.
    Bitboard promoRank = (us == White) ? Rank8BB : Rank1BB;
    if (m.promotion != None) {
        if (pt != Pawn) return false;
        if (!(target & promoRank)) return false;
        if (m.promotion == Pawn || m.promotion == King || m.promotion == None) return false;
    } else if (pt == Pawn && (target & promoRank)) {
        return false;
    }

    switch (pt) {
    case Pawn: {
        int from = m.from;
        int to = m.to;
        int forward = (us == White) ? 8 : -8;
        int startRank = (us == White) ? 1 : 6;

        // Single push
        if (to - from == forward) {
            return toPiece.type == None;
        }
        // Double push
        if (to - from == 2 * forward && squareRank(from) == startRank) {
            int mid = from + forward;
            return board.squares[mid].type == None && toPiece.type == None;
        }
        // Diagonal capture or en passant
        int fileDiff = squareFile(to) - squareFile(from);
        int rankDiff = squareRank(to) - squareRank(from);
        int expectedRank = (us == White) ? 1 : -1;
        if ((fileDiff == 1 || fileDiff == -1) && rankDiff == expectedRank) {
            if (toPiece.type != None && toPiece.color == them) return true;
            if (m.to == board.enPassantSquare && board.enPassantSquare != -1) return true;
        }
        return false;
    }
    case Knight:
        return (KnightAttacks[m.from] & target) != 0;
    case Bishop:
        return (bishopAttacks(m.from, occ) & target) != 0;
    case Rook:
        return (rookAttacks(m.from, occ) & target) != 0;
    case Queen:
        return (queenAttacks(m.from, occ) & target) != 0;
    case King: {
        if (KingAttacks[m.from] & target) return true;
        // Castling requires full validation here: `Board::makeMove` treats
        // any two-square king move as castling and blindly relocates the
        // rook, so waving a castling move through without checking rights,
        // path occupancy, and path-through-check would corrupt the board
        // when `isLegalMove` runs the move for verification. Mirror the
        // gates `addKingMoves` already uses at generation time.
        Color enemy = (us == White) ? Black : White;
        if (us == White && m.from == 4) {
            if (m.to == 6 && board.castleWK && board.squares[5].type == None &&
                board.squares[6].type == None && !isSquareAttacked(board, 4, enemy) &&
                !isSquareAttacked(board, 5, enemy) && !isSquareAttacked(board, 6, enemy)) {
                return true;
            }
            if (m.to == 2 && board.castleWQ && board.squares[3].type == None &&
                board.squares[2].type == None && board.squares[1].type == None &&
                !isSquareAttacked(board, 4, enemy) && !isSquareAttacked(board, 3, enemy) &&
                !isSquareAttacked(board, 2, enemy)) {
                return true;
            }
        }
        if (us == Black && m.from == 60) {
            if (m.to == 62 && board.castleBK && board.squares[61].type == None &&
                board.squares[62].type == None && !isSquareAttacked(board, 60, enemy) &&
                !isSquareAttacked(board, 61, enemy) && !isSquareAttacked(board, 62, enemy)) {
                return true;
            }
            if (m.to == 58 && board.castleBQ && board.squares[59].type == None &&
                board.squares[58].type == None && board.squares[57].type == None &&
                !isSquareAttacked(board, 60, enemy) && !isSquareAttacked(board, 59, enemy) &&
                !isSquareAttacked(board, 58, enemy)) {
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

// Strictly checks whether `m` is a fully legal move in `board`. The
// pseudo-legal screen guards the makeMove call inside `isLegalMove`
// against garbage TT moves that would otherwise write into arbitrary
// board cells, and the real legality test then confirms the move also
// satisfies check / pin constraints.
static bool isFullyLegal(Board &board, const Move &m) {
    if (!isPseudoLegalMove(board, m)) return false;
    return isLegalMove(board, m);
}

MovePicker::MovePicker(Board &board, const SearchState &state, int ply, Move ttMove, bool inCheck,
                       const ThreatMap *threats)
    : board_(board), state_(state), ply_(ply), ttMove_(ttMove), phase_(PickPhase::TTMove),
      inCheck_(inCheck), threats_(threats), caps_(state.pickerBuffers[ply].caps),
      quiets_(state.pickerBuffers[ply].quiets), badCapIdx_(state.pickerBuffers[ply].badCapIdx) {
    // Skip the TT-move phase outright when no candidate is on hand. Saves
    // the isPseudoLegalMove / isFullyLegal cost at every node that has no TT
    // hit for the position.
    if (isNullMove(ttMove_) || !isFullyLegal(board_, ttMove_)) {
        ttMove_ = {0, 0, None};
        phase_ = PickPhase::GenCaptures;
    }
    killer1_ = state.killers[ply][0];
    killer2_ = state.killers[ply][1];
    if (ply >= 1) {
        Color prevColor = (board.sideToMove == White) ? Black : White;
        PieceType prevPt = state.movedPiece[ply - 1];
        int prevTo = state.moveStack[ply - 1].to;
        counterMove_ = state.historyTables->counterMoves[prevColor][prevPt][prevTo];
    }
}

MovePicker::MovePicker(Board &board, const SearchState &state, int ply, Move ttMove, bool inCheck,
                       bool /*qsearchTag*/)
    : board_(board), state_(state), ply_(ply), ttMove_(ttMove), phase_(PickPhase::QsTTMove),
      inCheck_(inCheck), caps_(state.pickerBuffers[ply].caps),
      quiets_(state.pickerBuffers[ply].quiets), badCapIdx_(state.pickerBuffers[ply].badCapIdx) {
    if (inCheck_) {
        // In-check qsearch needs every legal evasion, not just captures, so
        // the pipeline routes through the evasion phase. The TT move is
        // still tried first when valid because the stored best move is
        // often the strongest evasion.
        if (isNullMove(ttMove_) || !isFullyLegal(board_, ttMove_)) {
            ttMove_ = {0, 0, None};
            phase_ = PickPhase::QsGenEvasions;
        }
    } else {
        if (isNullMove(ttMove_) || !isFullyLegal(board_, ttMove_)) {
            ttMove_ = {0, 0, None};
            phase_ = PickPhase::QsGenCaptures;
        }
    }
}

void MovePicker::genCaptures() {
    std::vector<Move> pseudo = generateLegalCaptures(board_);
    numCaps_ = 0;
    for (const Move &m : pseudo) {
        if (numCaps_ >= MOVE_PICKER_BUFFER_SIZE) break;
        int score = scoreMove(m, board_, ttMove_, ply_, state_, nullptr);
        caps_[numCaps_++] = {score, 0, m};
    }
    std::sort(caps_, caps_ + numCaps_,
              [](const ScoredMove &a, const ScoredMove &b) { return a.score > b.score; });
}

void MovePicker::genQuiets() {
    // In-check positions need full legal move generation because evasions
    // include non-capturing king moves and blocks that `generateLegalQuiets`
    // covers, but also captures that the capture phase already consumed.
    // Using `generateLegalQuiets` keeps the quiet set free of duplicates.
    std::vector<Move> pseudo = generateLegalQuiets(board_);
    numQuiets_ = 0;
    for (const Move &m : pseudo) {
        if (numQuiets_ >= MOVE_PICKER_BUFFER_SIZE) break;
        int hist = 0;
        int score = scoreMove(m, board_, ttMove_, ply_, state_, &hist);
        quiets_[numQuiets_++] = {score, hist, m};
    }
    std::sort(quiets_, quiets_ + numQuiets_,
              [](const ScoredMove &a, const ScoredMove &b) { return a.score > b.score; });
}

bool MovePicker::selectNextCapture(PickedMove &out) {
    // Captures are pre-sorted descending by score in `genCaptures`, so a
    // linear walk is enough. Good captures (SEE >= 0) are served inline;
    // bad captures are indexed for the later BadCaptures phase without
    // being removed from `caps_` so their ordering is preserved.
    while (capCursor_ < numCaps_) {
        const ScoredMove &picked = caps_[capCursor_++];
        if (picked.score >= 0) {
            out = {picked.move, picked.score, 0, PickPhase::GoodCaptures};
            return true;
        }
        if (numBadCaps_ < MOVE_PICKER_BUFFER_SIZE) {
            badCapIdx_[numBadCaps_++] = capCursor_ - 1;
        }
    }
    return false;
}

bool MovePicker::selectNextQuiet(PickedMove &out) {
    // Quiets are pre-sorted descending by score in `genQuiets`.
    if (quietCursor_ >= numQuiets_) return false;
    const ScoredMove &picked = quiets_[quietCursor_++];
    out = {picked.move, picked.score, picked.histScore, PickPhase::Quiets};
    return true;
}

bool MovePicker::selectNextBadCapture(PickedMove &out) {
    // Bad captures were already scored during the good-capture partition.
    // Walk them in the order they were discovered; re-sorting them by
    // score would not strengthen ordering because every bad capture is
    // below every quiet that survives history selection.
    while (badCapCursor_ < numBadCaps_) {
        const ScoredMove &sm = caps_[badCapIdx_[badCapCursor_++]];
        out = {sm.move, sm.score, 0, PickPhase::BadCaptures};
        return true;
    }
    return false;
}

bool MovePicker::next(PickedMove &out, const Move &skipMove) {
    auto emittedAlready = [&](const Move &m) {
        return sameMove(m, ttMove_) || sameMove(m, killer1_) || sameMove(m, killer2_) ||
               sameMove(m, counterMove_);
    };

    for (;;) {
        switch (phase_) {
        case PickPhase::TTMove:
            phase_ = PickPhase::GenCaptures;
            if (!sameMove(ttMove_, skipMove)) {
                out = {ttMove_, 10000000, 0, PickPhase::TTMove};
                return true;
            }
            break;

        case PickPhase::GenCaptures:
            genCaptures();
            phase_ = PickPhase::GoodCaptures;
            break;

        case PickPhase::GoodCaptures:
            if (selectNextCapture(out)) {
                if (sameMove(out.move, ttMove_) || sameMove(out.move, skipMove)) continue;
                return true;
            }
            phase_ = PickPhase::Killer1;
            break;

        case PickPhase::Killer1:
            phase_ = PickPhase::Killer2;
            if (!isNullMove(killer1_) && !sameMove(killer1_, ttMove_) &&
                !sameMove(killer1_, skipMove) && !isCapture(board_, killer1_) &&
                killer1_.promotion == None && isFullyLegal(board_, killer1_)) {
                int hist = 0;
                int score = scoreMove(killer1_, board_, ttMove_, ply_, state_, &hist);
                out = {killer1_, score, hist, PickPhase::Killer1};
                return true;
            }
            break;

        case PickPhase::Killer2:
            phase_ = PickPhase::CounterMove;
            if (!isNullMove(killer2_) && !sameMove(killer2_, ttMove_) &&
                !sameMove(killer2_, killer1_) && !sameMove(killer2_, skipMove) &&
                !isCapture(board_, killer2_) && killer2_.promotion == None &&
                isFullyLegal(board_, killer2_)) {
                int hist = 0;
                int score = scoreMove(killer2_, board_, ttMove_, ply_, state_, &hist);
                out = {killer2_, score, hist, PickPhase::Killer2};
                return true;
            }
            break;

        case PickPhase::CounterMove:
            phase_ = PickPhase::GenQuiets;
            if (!isNullMove(counterMove_) && !sameMove(counterMove_, ttMove_) &&
                !sameMove(counterMove_, killer1_) && !sameMove(counterMove_, killer2_) &&
                !sameMove(counterMove_, skipMove) && !isCapture(board_, counterMove_) &&
                counterMove_.promotion == None && isFullyLegal(board_, counterMove_)) {
                int hist = 0;
                int score = scoreMove(counterMove_, board_, ttMove_, ply_, state_, &hist);
                out = {counterMove_, score, hist, PickPhase::CounterMove};
                return true;
            }
            break;

        case PickPhase::GenQuiets:
            genQuiets();
            phase_ = PickPhase::Quiets;
            break;

        case PickPhase::Quiets:
            if (selectNextQuiet(out)) {
                if (emittedAlready(out.move) || sameMove(out.move, skipMove)) continue;
                return true;
            }
            phase_ = PickPhase::BadCaptures;
            break;

        case PickPhase::BadCaptures:
            if (selectNextBadCapture(out)) {
                if (sameMove(out.move, ttMove_) || sameMove(out.move, skipMove)) continue;
                return true;
            }
            phase_ = PickPhase::Done;
            return false;

        case PickPhase::QsTTMove:
            phase_ = inCheck_ ? PickPhase::QsGenEvasions : PickPhase::QsGenCaptures;
            if (!sameMove(ttMove_, skipMove)) {
                out = {ttMove_, 10000000, 0, PickPhase::QsTTMove};
                return true;
            }
            break;

        case PickPhase::QsGenCaptures:
            genCaptures();
            phase_ = PickPhase::QsCaptures;
            break;

        case PickPhase::QsCaptures: {
            // Qsearch reuses the capture scoring pipeline but streams
            // everything, good or bad, because the caller applies its own
            // SEE and delta-pruning gates per capture. The buffer is
            // already sorted by `genCaptures` so a linear walk suffices.
            while (capCursor_ < numCaps_) {
                const ScoredMove &picked = caps_[capCursor_++];
                if (sameMove(picked.move, ttMove_) || sameMove(picked.move, skipMove)) continue;
                out = {picked.move, picked.score, 0, PickPhase::QsCaptures};
                return true;
            }
            phase_ = PickPhase::QsDone;
            return false;
        }

        case PickPhase::QsGenEvasions: {
            // The in-check qsearch path must emit every legal move so the
            // search can find a check escape. We reuse the quiets buffer
            // as a scratch list because it is otherwise unused on this
            // branch, and run the full movegen path through the normal
            // scoring routine. Sort up front so the QsEvasions phase can
            // do a straight linear walk.
            std::vector<Move> all = generateLegalMoves(board_);
            numQuiets_ = 0;
            for (const Move &m : all) {
                if (numQuiets_ >= MOVE_PICKER_BUFFER_SIZE) break;
                int hist = 0;
                int score = scoreMove(m, board_, ttMove_, ply_, state_, &hist);
                quiets_[numQuiets_++] = {score, hist, m};
            }
            std::sort(quiets_, quiets_ + numQuiets_,
                      [](const ScoredMove &a, const ScoredMove &b) { return a.score > b.score; });
            phase_ = PickPhase::QsEvasions;
            break;
        }

        case PickPhase::QsEvasions: {
            while (quietCursor_ < numQuiets_) {
                const ScoredMove &picked = quiets_[quietCursor_++];
                if (sameMove(picked.move, ttMove_) || sameMove(picked.move, skipMove)) continue;
                out = {picked.move, picked.score, picked.histScore, PickPhase::QsEvasions};
                return true;
            }
            phase_ = PickPhase::QsDone;
            return false;
        }

        case PickPhase::Done:
        case PickPhase::QsDone:
            return false;
        }
    }
}
