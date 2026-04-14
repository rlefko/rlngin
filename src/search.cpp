#include "search.h"
#include "bitboard.h"
#include "eval.h"
#include "movegen.h"
#include "tt.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

static TranspositionTable tt(16);

static int scoreMove(const Move &m, const Board &board, const Move &ttMove, int ply,
                     const SearchState &state) {
    // TT move gets highest priority
    if (m.from == ttMove.from && m.to == ttMove.to && m.promotion == ttMove.promotion) {
        return 10000000;
    }

    // Promotions
    if (m.promotion != None) {
        return 950000 + PieceValue[m.promotion];
    }

    Piece captured = board.squares[m.to];

    // En passant capture
    if (captured.type == None && board.squares[m.from].type == Pawn &&
        m.to == board.enPassantSquare && board.enPassantSquare != -1) {
        return 1000000 + PieceValue[Pawn] * 100 - PieceValue[Pawn];
    }

    // Captures scored by MVV-LVA
    if (captured.type != None) {
        return 1000000 + PieceValue[captured.type] * 100 - PieceValue[board.squares[m.from].type];
    }

    // Killer moves
    if (ply >= 0) {
        for (int k = 0; k < 2; k++) {
            const Move &killer = state.killers[ply][k];
            if (m.from == killer.from && m.to == killer.to && m.promotion == killer.promotion) {
                return 900000 - k;
            }
        }
    }

    return 0;
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
        UndoInfo undo = board.makeMove(m);
        int score = -quiescence(board, -beta, -alpha, ply + 1, state);
        board.unmakeMove(m, undo);
        if (state.stopped) return 0;
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }

    return alpha;
}

static int negamax(Board &board, int depth, int ply, int alpha, int beta, SearchState &state) {
    state.nodes++;
    if (ply > state.seldepth) state.seldepth = ply;
    state.pvLength[ply] = ply;

    if (ply >= MAX_PLY - 1) return evaluate(board);

    if (state.nodes % 1024 == 0) checkTime(state);
    if (state.stopped) return 0;

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

    for (const Move &m : moves) {
        UndoInfo undo = board.makeMove(m);
        int score = -negamax(board, depth - 1, ply + 1, -beta, -alpha, state);
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
            // Store killer move if it's a quiet move (not a capture or en passant)
            if (board.squares[m.to].type == None &&
                !(board.squares[m.from].type == Pawn && m.to == board.enPassantSquare &&
                  board.enPassantSquare != -1)) {
                state.killers[ply][1] = state.killers[ply][0];
                state.killers[ply][0] = m;
            }
            break;
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

void startSearch(const Board &board, const SearchLimits &limits, SearchState &state) {
    state.stopped = false;
    state.nodes = 0;
    state.bestMove = {0, 0, None};
    memset(state.killers, 0, sizeof(state.killers));
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

        for (const Move &m : rootMoves) {
            UndoInfo undo = pos.makeMove(m);
            int score = -negamax(pos, depth - 1, 1, -beta, -alpha, state);
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

        std::cout << " nodes " << state.nodes << " nps " << nps << " time " << timeMs << " pv";
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
