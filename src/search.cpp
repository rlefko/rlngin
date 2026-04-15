#include "search.h"
#include "bitboard.h"
#include "eval.h"
#include "movegen.h"
#include "see.h"
#include "tt.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

static constexpr int MAX_HISTORY = 16384;
static constexpr int MAX_CONT_HISTORY = 16384;

static TranspositionTable tt(16);

static void updateHistory(int &entry, int bonus) {
    entry += bonus - entry * std::abs(bonus) / MAX_HISTORY;
}

static void updateContHistory(int16_t &entry, int bonus) {
    entry += static_cast<int16_t>(bonus - entry * std::abs(bonus) / MAX_CONT_HISTORY);
}

static bool isCapture(const Board &board, const Move &m) {
    if (board.squares[m.to].type != None) return true;
    if (board.squares[m.from].type == Pawn && m.to == board.enPassantSquare &&
        board.enPassantSquare != -1)
        return true;
    return false;
}

static PieceType capturedType(const Board &board, const Move &m) {
    if (board.squares[m.to].type != None) return board.squares[m.to].type;
    if (board.squares[m.from].type == Pawn && m.to == board.enPassantSquare &&
        board.enPassantSquare != -1)
        return Pawn;
    return None;
}

static int scoreMove(const Move &m, const Board &board, const Move &ttMove, int ply,
                     const SearchState &state) {
    // TT move gets highest priority
    if (m.from == ttMove.from && m.to == ttMove.to && m.promotion == ttMove.promotion) {
        return 10000000;
    }

    PieceType pt = board.squares[m.from].type;

    // Promotions
    if (m.promotion != None) {
        if (m.promotion == Queen) return 9000000;
        return -5000000;
    }

    // Captures: use SEE to separate good from bad (skip SEE in quiescence, ply == -1)
    if (isCapture(board, m)) {
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

    // Quiet moves: use continuation history
    int score = 0;
    if (ply >= 1) {
        PieceType prevPt = state.movedPiece[ply - 1];
        int prevTo = state.moveStack[ply - 1].to;
        score += state.contHistory->data[prevPt][prevTo][pt][m.to];
    }

    return score;
}

static void checkTime(SearchState &state) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state.startTime).count();
    if (elapsed >= state.allocatedTimeMs) {
        state.stopped = true;
    }
}

static bool isInCheck(const Board &board) {
    Color side = board.sideToMove;
    Color opponent = (side == White) ? Black : White;
    Bitboard kingBB = board.byPiece[King] & board.byColor[side];
    if (!kingBB) return false;
    return isSquareAttacked(board, lsb(kingBB), opponent);
}

static int quiescence(Board &board, int alpha, int beta, int ply, SearchState &state) {
    state.nodes++;
    if (ply > state.seldepth) state.seldepth = ply;

    if (ply >= MAX_PLY) return evaluate(board);

    state.pvLength[ply] = ply;

    if (state.nodes % 1024 == 0) checkTime(state);
    if (state.stopped) return 0;

    bool inCheck = isInCheck(board);

    if (!inCheck) {
        int standPat = evaluate(board);
        if (standPat >= beta) return beta;
        if (standPat > alpha) alpha = standPat;
    }

    // When in check, search all legal moves (must escape check).
    // Otherwise, search only captures.
    std::vector<Move> moves = inCheck ? generateLegalMoves(board) : generateLegalCaptures(board);

    if (inCheck && moves.empty()) return -(MATE_SCORE - ply);

    // MVV-LVA ordering for captures
    Move noTTMove = {0, 0, None};
    std::sort(moves.begin(), moves.end(), [&](const Move &a, const Move &b) {
        return scoreMove(a, board, noTTMove, -1, state) > scoreMove(b, board, noTTMove, -1, state);
    });

    for (const Move &m : moves) {
        // Prune losing captures when not in check
        if (!inCheck && isCapture(board, m) && see(board, m) < 0) continue;

        UndoInfo undo = board.makeMove(m);
        int score = -quiescence(board, -beta, -alpha, ply + 1, state);
        board.unmakeMove(m, undo);
        if (state.stopped) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }

    return alpha;
}

static bool isRepetition(const Board &board, const SearchState &state, int ply) {
    uint64_t key = board.key;
    int hmc = board.halfmoveClock;

    // Only positions within the halfmove clock window can possibly repeat,
    // since captures and pawn moves make earlier positions unreachable.

    // Check positions during the current search (step by 2 for same side to move)
    int searchBack = std::min(ply, hmc);
    for (int i = 2; i <= searchBack; i += 2) {
        if (state.searchKeys[ply - i] == key) return true;
    }

    // Check game history (positions before search started)
    int gameBack = hmc - ply;
    if (gameBack > 0) {
        int histSize = static_cast<int>(state.positionHistory.size());
        int end = std::max(0, histSize - gameBack);
        for (int i = histSize - 1; i >= end; i--) {
            if (state.positionHistory[i] == key) return true;
        }
    }

    return false;
}

static int negamax(Board &board, int depth, int ply, int alpha, int beta, SearchState &state) {
    state.nodes++;
    if (ply > state.seldepth) state.seldepth = ply;
    state.pvLength[ply] = ply;
    state.searchKeys[ply] = board.key;

    if (ply >= MAX_PLY - 1) return evaluate(board);

    if (state.nodes % 1024 == 0) checkTime(state);
    if (state.stopped) return 0;

    // Draw detection: repetition and 50-move rule
    if (ply > 0) {
        if (board.halfmoveClock >= 100) return 0;
        if (isRepetition(board, state, ply)) return 0;
    }

    int origAlpha = alpha;

    // TT probe
    TTEntry ttEntry;
    Move ttMove = {0, 0, None};
    if (tt.probe(board.key, ttEntry, ply)) {
        ttMove = ttEntry.best_move;
        if (ttEntry.depth >= depth) {
            if (ttEntry.flag == TT_EXACT) {
                return ttEntry.score;
            }
            if (ttEntry.flag == TT_LOWER_BOUND && ttEntry.score >= beta) {
                return ttEntry.score;
            }
            if (ttEntry.flag == TT_UPPER_BOUND && ttEntry.score <= alpha) {
                return ttEntry.score;
            }
        }
    }

    std::vector<Move> moves = generateLegalMoves(board);

    if (moves.empty()) {
        if (isInCheck(board)) return -(MATE_SCORE - ply);
        return 0;
    }

    if (depth == 0) return quiescence(board, alpha, beta, ply, state);

    // Move ordering: TT move first, then MVV-LVA for captures, then quiet moves
    std::sort(moves.begin(), moves.end(), [&](const Move &a, const Move &b) {
        return scoreMove(a, board, ttMove, ply, state) > scoreMove(b, board, ttMove, ply, state);
    });

    int bestScore = -INF_SCORE;
    Move bestMove = moves[0];

    Move searchedCaptures[64];
    Move searchedQuiets[64];
    int numSearchedCaptures = 0;
    int numSearchedQuiets = 0;
    int bonus = std::min(depth * depth, 400);

    for (int moveIndex = 0; moveIndex < static_cast<int>(moves.size()); moveIndex++) {
        const Move &m = moves[moveIndex];
        state.moveStack[ply] = m;
        state.movedPiece[ply] = board.squares[m.from].type;

        UndoInfo undo = board.makeMove(m);

        int score;
        if (moveIndex == 0) {
            score = -negamax(board, depth - 1, ply + 1, -beta, -alpha, state);
        } else {
            // PVS: null-window search for non-first moves
            score = -negamax(board, depth - 1, ply + 1, -alpha - 1, -alpha, state);
            if (score > alpha && score < beta) {
                score = -negamax(board, depth - 1, ply + 1, -beta, -alpha, state);
            }
        }

        board.unmakeMove(m, undo);
        if (state.stopped) return 0;
        if (score > bestScore) {
            bestScore = score;
            bestMove = m;
        }
        if (score > alpha) {
            alpha = score;
            state.pv[ply][ply] = m;
            for (int i = ply + 1; i < state.pvLength[ply + 1]; i++) {
                state.pv[ply][i] = state.pv[ply + 1][i];
            }
            state.pvLength[ply] = state.pvLength[ply + 1];
        }
        if (alpha >= beta) {
            bool cap = isCapture(board, m);
            if (cap) {
                // Reward the capture that caused the cutoff
                PieceType pt = board.squares[m.from].type;
                PieceType ct = capturedType(board, m);
                updateHistory(state.captureHistory[pt][m.to][ct], bonus);
                // Penalize previously searched captures
                for (int i = 0; i < numSearchedCaptures; i++) {
                    const Move &prev = searchedCaptures[i];
                    PieceType prevPt = board.squares[prev.from].type;
                    PieceType prevCt = capturedType(board, prev);
                    updateHistory(state.captureHistory[prevPt][prev.to][prevCt], -bonus);
                }
            } else {
                // Killer move update
                state.killers[ply][1] = state.killers[ply][0];
                state.killers[ply][0] = m;
                // Continuation history reward
                if (ply >= 1) {
                    PieceType prevPt = state.movedPiece[ply - 1];
                    int prevTo = state.moveStack[ply - 1].to;
                    PieceType currPt = board.squares[m.from].type;
                    updateContHistory(state.contHistory->data[prevPt][prevTo][currPt][m.to], bonus);
                    // Penalize previously searched quiets
                    for (int i = 0; i < numSearchedQuiets; i++) {
                        const Move &prev = searchedQuiets[i];
                        PieceType qPt = board.squares[prev.from].type;
                        updateContHistory(state.contHistory->data[prevPt][prevTo][qPt][prev.to],
                                          -bonus);
                    }
                }
            }
            break;
        }

        // Track searched moves for malus
        if (isCapture(board, m)) {
            if (numSearchedCaptures < 64) searchedCaptures[numSearchedCaptures++] = m;
        } else {
            if (numSearchedQuiets < 64) searchedQuiets[numSearchedQuiets++] = m;
        }
    }

    // TT store
    TTFlag flag;
    if (bestScore <= origAlpha) {
        flag = TT_UPPER_BOUND;
    } else if (bestScore >= beta) {
        flag = TT_LOWER_BOUND;
    } else {
        flag = TT_EXACT;
    }
    tt.store(board.key, bestScore, depth, flag, bestMove, ply);

    return bestScore;
}

static int64_t computeTimeAllocation(const Board &board, const SearchLimits &limits) {
    if (limits.infinite) return std::numeric_limits<int64_t>::max();
    if (limits.depth > 0) return std::numeric_limits<int64_t>::max();
    if (limits.movetime > 0) return limits.movetime;

    int64_t timeLeft = (board.sideToMove == White) ? limits.wtime : limits.btime;
    int64_t increment = (board.sideToMove == White) ? limits.winc : limits.binc;
    int movesToGo = (limits.movestogo > 0) ? limits.movestogo : 25;

    int64_t allocated = timeLeft / movesToGo + increment;
    allocated = std::min(allocated, timeLeft * 4 / 5);
    allocated = std::max(allocated, static_cast<int64_t>(10));

    return allocated;
}

void startSearch(const Board &board, const SearchLimits &limits, SearchState &state,
                 const std::vector<uint64_t> &positionHistory) {
    state.stopped = false;
    state.nodes = 0;
    state.bestMove = {0, 0, None};
    memset(state.killers, 0, sizeof(state.killers));
    state.positionHistory = positionHistory;
    state.searchKeys[0] = board.key;
    state.startTime = std::chrono::steady_clock::now();
    state.allocatedTimeMs = computeTimeAllocation(board, limits);

    int maxDepth = (limits.depth > 0) ? limits.depth : 100;

    Board pos = board;
    std::vector<Move> rootMoves = generateLegalMoves(pos);
    if (rootMoves.empty()) return;

    for (int depth = 1; depth <= maxDepth; depth++) {
        state.seldepth = 0;
        Move currentBest = rootMoves[0];
        int currentBestScore = -INF_SCORE;
        int alpha = -INF_SCORE;
        int beta = INF_SCORE;

        state.pvLength[0] = 0;

        for (size_t mi = 0; mi < rootMoves.size(); mi++) {
            const Move &m = rootMoves[mi];

            if (depth >= 2) {
                std::cout << "info depth " << depth << " currmove " << moveToString(m)
                          << " currmovenumber " << (mi + 1) << std::endl;
            }

            state.moveStack[0] = m;
            state.movedPiece[0] = pos.squares[m.from].type;

            UndoInfo undo = pos.makeMove(m);

            int score;
            if (mi == 0) {
                score = -negamax(pos, depth - 1, 1, -beta, -alpha, state);
            } else {
                // PVS: null-window search for non-first moves
                score = -negamax(pos, depth - 1, 1, -alpha - 1, -alpha, state);
                if (score > alpha && score < beta) {
                    score = -negamax(pos, depth - 1, 1, -beta, -alpha, state);
                }
            }

            pos.unmakeMove(m, undo);
            if (state.stopped) break;
            if (score > currentBestScore) {
                currentBestScore = score;
                currentBest = m;
            }
            if (score > alpha) {
                alpha = score;
                state.pv[0][0] = m;
                for (int i = 1; i < state.pvLength[1]; i++) {
                    state.pv[0][i] = state.pv[1][i];
                }
                state.pvLength[0] = state.pvLength[1];
            }
        }

        if (state.stopped) break;

        state.bestMove = currentBest;

        if (state.pvLength[0] >= 2) {
            state.ponderMove = state.pv[0][1];
        } else {
            state.ponderMove = {0, 0, None};
        }

        // Move the best move to the front for the next iteration
        for (size_t i = 0; i < rootMoves.size(); i++) {
            if (rootMoves[i].from == currentBest.from && rootMoves[i].to == currentBest.to &&
                rootMoves[i].promotion == currentBest.promotion) {
                std::swap(rootMoves[0], rootMoves[i]);
                break;
            }
        }

        auto now = std::chrono::steady_clock::now();
        int64_t timeMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - state.startTime).count();
        int64_t nps = (timeMs > 0) ? (state.nodes * 1000 / timeMs) : state.nodes;

        std::cout << "info depth " << depth << " seldepth " << state.seldepth;

        if (std::abs(currentBestScore) >= MATE_SCORE - 100) {
            int matePly = MATE_SCORE - std::abs(currentBestScore);
            int mateInMoves = (matePly + 1) / 2;
            if (currentBestScore < 0) mateInMoves = -mateInMoves;
            std::cout << " score mate " << mateInMoves;
        } else {
            std::cout << " score cp " << currentBestScore;
        }

        std::cout << " nodes " << state.nodes << " nps " << nps << " time " << timeMs
                  << " hashfull " << getHashfull() << " pv";
        for (int i = 0; i < state.pvLength[0]; i++) {
            std::cout << " " << moveToString(state.pv[0][i]);
        }
        std::cout << std::endl;

        if (std::abs(currentBestScore) >= MATE_SCORE - 100) break;
    }

    if (state.bestMove.from == 0 && state.bestMove.to == 0 && !rootMoves.empty()) {
        state.bestMove = rootMoves[0];
    }
}

Move findBestMove(const Board &board, int depth) {
    SearchLimits limits;
    limits.depth = depth;
    SearchState state;
    startSearch(board, limits, state);
    return state.bestMove;
}

void setHashSize(size_t mb) {
    tt.resize(mb);
}

void clearTT() {
    tt.clear();
}

int getHashfull() {
    return tt.hashfull();
}

void clearHistory(SearchState &state) {
    memset(state.captureHistory, 0, sizeof(state.captureHistory));
    memset(state.contHistory->data, 0, sizeof(state.contHistory->data));
}
