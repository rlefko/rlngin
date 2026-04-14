#include "uci.h"
#include "board.h"
#include "movegen.h"
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void handlePosition(Board &board, std::istringstream &ss) {
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

    // Apply moves
    while (ss >> token) {
        Move m = stringToMove(token);
        board.makeMove(m);
    }
}

static void handleGo(const Board &board) {
    std::vector<Move> moves = generateLegalMoves(board);
    if (moves.empty()) {
        std::cout << "bestmove 0000" << std::endl;
    } else {
        std::cout << "bestmove " << moveToString(moves[0]) << std::endl;
    }
}

void uciLoop() {
    Board board;
    std::string line;

    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string command;
        ss >> command;

        if (command == "uci") {
            std::cout << "id name rlngin" << std::endl;
            std::cout << "id author Ryan Lefkowitz" << std::endl;
            std::cout << "uciok" << std::endl;
        } else if (command == "isready") {
            std::cout << "readyok" << std::endl;
        } else if (command == "ucinewgame") {
            board.setStartPos();
        } else if (command == "position") {
            handlePosition(board, ss);
        } else if (command == "go") {
            handleGo(board);
        } else if (command == "stop") {
            // No-op: engine responds instantly, so stop is never needed
        } else if (command == "quit") {
            break;
        }
    }
}
