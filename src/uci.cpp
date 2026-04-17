#include "uci.h"
#include "board.h"
#include "eval.h"
#include "search.h"
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static void handlePosition(Board &board, std::vector<uint64_t> &posHistory,
                           std::istringstream &ss) {
    std::string token;
    ss >> token;

    if (token == "startpos") {
        board.setStartPos();
        ss >> token; // consume "moves" if present
    } else if (token == "fen") {
        std::string fen;
        while (ss >> token && token != "moves") {
            if (!fen.empty()) fen += " ";
            fen += token;
        }
        board.setFen(fen);
    }

    posHistory.clear();

    // Apply moves, recording position keys for repetition detection
    while (ss >> token) {
        posHistory.push_back(board.key);
        Move m = stringToMove(token);
        board.makeMove(m);
    }
}

static SearchLimits parseGoParams(std::istringstream &ss) {
    SearchLimits limits;
    std::string token;

    while (ss >> token) {
        if (token == "wtime")
            ss >> limits.wtime;
        else if (token == "btime")
            ss >> limits.btime;
        else if (token == "winc")
            ss >> limits.winc;
        else if (token == "binc")
            ss >> limits.binc;
        else if (token == "movestogo")
            ss >> limits.movestogo;
        else if (token == "depth")
            ss >> limits.depth;
        else if (token == "movetime")
            ss >> limits.movetime;
        else if (token == "infinite")
            limits.infinite = true;
    }

    return limits;
}

void uciLoop() {
    Board board;
    SearchState searchState;
    std::vector<uint64_t> posHistory;
    std::thread searchThread;

    auto joinSearch = [&]() {
        if (searchThread.joinable()) {
            searchState.stopped = true;
            searchThread.join();
        }
    };

    std::string line;

    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string command;
        ss >> command;

        if (command == "uci") {
            std::cout << "id name rlngin" << std::endl;
            std::cout << "id author Ryan Lefkowitz" << std::endl;
            std::cout << "option name Hash type spin default 16 min 1 max 1024" << std::endl;
            std::cout << "uciok" << std::endl;
        } else if (command == "isready") {
            joinSearch();
            std::cout << "readyok" << std::endl;
        } else if (command == "setoption") {
            std::string token, name;
            ss >> token; // "name"
            ss >> name;
            if (name == "Hash") {
                std::string valueToken;
                int value;
                ss >> valueToken >> value; // "value" <number>
                setHashSize(static_cast<size_t>(value));
            }
        } else if (command == "ucinewgame") {
            joinSearch();
            board.setStartPos();
            clearTT();
            clearPawnHash();
            clearMaterialHash();
            clearHistory(searchState);
        } else if (command == "position") {
            joinSearch();
            handlePosition(board, posHistory, ss);
        } else if (command == "go") {
            joinSearch();
            SearchLimits limits = parseGoParams(ss);
            searchState.stopped = false;
            searchState.nodes = 0;
            searchState.bestMove = {0, 0, None};
            searchThread = std::thread([board, limits, &searchState, posHistory]() {
                startSearch(board, limits, searchState, posHistory);
                Move best = searchState.bestMove;
                if (best.from == best.to) {
                    std::cout << "bestmove 0000" << std::endl;
                } else {
                    std::cout << "bestmove " << moveToString(best);
                    Move ponder = searchState.ponderMove;
                    if (ponder.from != ponder.to) {
                        std::cout << " ponder " << moveToString(ponder);
                    }
                    std::cout << std::endl;
                }
            });
        } else if (command == "stop") {
            joinSearch();
        } else if (command == "quit") {
            joinSearch();
            break;
        }
    }

    joinSearch();
}
