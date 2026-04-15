#include "uci.h"
#include "board.h"
#include "search.h"
#include <algorithm>
#include <cctype>
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

static std::string trim(std::string value) {
    auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

static bool parseBoolValue(std::string value, bool &out) {
    value = trim(value);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (value == "true" || value == "1" || value == "on") {
        out = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

static void handleSetOption(std::istringstream &ss, SearchConfig &searchConfig) {
    std::string token;
    if (!(ss >> token) || token != "name") return;

    std::string name;
    while (ss >> token && token != "value") {
        if (!name.empty()) name += " ";
        name += token;
    }

    std::string value;
    std::getline(ss, value);
    value = trim(value);

    if (name == "Hash") {
        if (!value.empty()) {
            int hashMb = std::stoi(value);
            setHashSize(static_cast<size_t>(hashMb));
        }
        return;
    }

    bool boolValue = false;
    if (!parseBoolValue(value, boolValue)) return;

    if (name == "UseQSEEPruning") {
        searchConfig.useQseePruning = boolValue;
    } else if (name == "UseRFP") {
        searchConfig.useRfp = boolValue;
    } else if (name == "UseNullMove") {
        searchConfig.useNullMove = boolValue;
    } else if (name == "UseFutilityPruning") {
        searchConfig.useFutilityPruning = boolValue;
    } else if (name == "UseMoveCountPruning") {
        searchConfig.useMoveCountPruning = boolValue;
    } else if (name == "UseLMR") {
        searchConfig.useLmr = boolValue;
    } else if (name == "UseAspirationWindows") {
        searchConfig.useAspirationWindows = boolValue;
    } else if (name == "DebugSearchStats") {
        searchConfig.debugSearchStats = boolValue;
    }
}

void uciLoop() {
    Board board;
    SearchState searchState;
    SearchConfig searchConfig;
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
            std::cout << "option name UseQSEEPruning type check default true" << std::endl;
            std::cout << "option name UseRFP type check default true" << std::endl;
            std::cout << "option name UseNullMove type check default true" << std::endl;
            std::cout << "option name UseFutilityPruning type check default true" << std::endl;
            std::cout << "option name UseMoveCountPruning type check default true" << std::endl;
            std::cout << "option name UseLMR type check default true" << std::endl;
            std::cout << "option name UseAspirationWindows type check default true" << std::endl;
            std::cout << "option name DebugSearchStats type check default false" << std::endl;
            std::cout << "uciok" << std::endl;
        } else if (command == "isready") {
            joinSearch();
            std::cout << "readyok" << std::endl;
        } else if (command == "setoption") {
            handleSetOption(ss, searchConfig);
        } else if (command == "ucinewgame") {
            joinSearch();
            board.setStartPos();
            clearTT();
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
            SearchConfig activeConfig = searchConfig;
            searchThread = std::thread([board, limits, &searchState, posHistory, activeConfig]() {
                startSearch(board, limits, searchState, posHistory, activeConfig);
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
