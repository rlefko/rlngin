// Texel-style local tuner for the new eval parameters.
//
// Reads a labeled dataset of `FEN | result` lines, where result is 1.0,
// 0.5, or 0.0 from White's perspective. Finds a scaling constant K that
// minimizes the mean squared error between sigmoid(K * eval) and the
// game result, then runs coordinate descent over every mg / eg half of
// every Score in `evalParams`. The final parameter values are printed
// in a form that can be pasted back into `src/eval_params.cpp`.

#include "board.h"
#include "eval.h"
#include "eval_params.h"
#include "types.h"
#include "zobrist.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct LabeledPosition {
    Board board;
    double result; // 1.0 / 0.5 / 0.0 from White POV
};

// Accessor for a single mg/eg half of a Score field.
struct ParamRef {
    std::string name;
    Score *target;
    bool isMg; // true = modify mg half, false = eg half

    int read() const {
        return isMg ? mg_value(*target) : eg_value(*target);
    }
    void write(int v) const {
        int mg = mg_value(*target);
        int eg = eg_value(*target);
        if (isMg)
            mg = v;
        else
            eg = v;
        *target = S(mg, eg);
    }
};

static std::vector<ParamRef> collectParams() {
    std::vector<ParamRef> out;
    auto addMgEg = [&](const std::string &name, Score *s, bool mg = true, bool eg = true) {
        if (mg) out.push_back({name + ".mg", s, true});
        if (eg) out.push_back({name + ".eg", s, false});
    };

    // --- Threats ---
    addMgEg("ThreatByPawn", &evalParams.ThreatByPawn);
    for (int v = Rook; v <= Queen; v++)
        addMgEg("ThreatByMinor[" + std::to_string(v) + "]", &evalParams.ThreatByMinor[v]);
    addMgEg("ThreatByRook[Queen]", &evalParams.ThreatByRook[Queen]);
    addMgEg("ThreatByKing", &evalParams.ThreatByKing);
    addMgEg("Hanging", &evalParams.Hanging);
    addMgEg("WeakQueen", &evalParams.WeakQueen);
    addMgEg("SafePawnPush", &evalParams.SafePawnPush);

    // --- Passed pawn extras (rank 3..6 inclusive are the interesting
    // slots -- ranks 0/1/2 and 7 stay at zero).
    for (int r = 3; r <= 6; r++) {
        addMgEg("PassedKingProxBonus[" + std::to_string(r) + "]",
                &evalParams.PassedKingProxBonus[r], false, true); // eg only
        addMgEg("PassedEnemyKingProxPenalty[" + std::to_string(r) + "]",
                &evalParams.PassedEnemyKingProxPenalty[r], false, true);
        addMgEg("PassedBlockedPenalty[" + std::to_string(r) + "]",
                &evalParams.PassedBlockedPenalty[r]);
        addMgEg("PassedSupportedBonus[" + std::to_string(r) + "]",
                &evalParams.PassedSupportedBonus[r]);
        addMgEg("ConnectedPassersBonus[" + std::to_string(r) + "]",
                &evalParams.ConnectedPassersBonus[r]);
    }

    addMgEg("RookOn7thBonus", &evalParams.RookOn7thBonus);
    addMgEg("BadBishopPenalty", &evalParams.BadBishopPenalty);
    addMgEg("Tempo", &evalParams.Tempo, true, false); // mg only

    return out;
}

static std::vector<LabeledPosition> loadDataset(const std::string &path) {
    std::vector<LabeledPosition> out;
    std::ifstream in(path);
    if (!in) {
        std::cerr << "could not open " << path << std::endl;
        std::exit(1);
    }
    std::string line;
    while (std::getline(in, line)) {
        auto sep = line.find('|');
        if (sep == std::string::npos) continue;
        std::string fen = line.substr(0, sep);
        std::string result = line.substr(sep + 1);
        while (!fen.empty() && fen.back() == ' ')
            fen.pop_back();
        while (!result.empty() && (result.front() == ' '))
            result.erase(result.begin());
        LabeledPosition lp;
        lp.board.setFen(fen);
        lp.result = std::stod(result);
        out.push_back(std::move(lp));
    }
    return out;
}

static double sigmoid(double x, double K) {
    return 1.0 / (1.0 + std::exp(-K * x));
}

static double computeLoss(const std::vector<LabeledPosition> &positions, double K,
                          int numThreads) {
    size_t n = positions.size();
    std::vector<double> partial(numThreads, 0.0);
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int t = 0; t < numThreads; t++) {
        size_t start = (n * t) / numThreads;
        size_t end = (n * (t + 1)) / numThreads;
        threads.emplace_back([&, start, end, t]() {
            double sum = 0.0;
            for (size_t i = start; i < end; i++) {
                Board board = positions[i].board;
                int raw = evaluate(board);
                // evaluate() returns side-to-move relative; convert to
                // White POV so the tuner can compare directly to the
                // White-POV game result.
                if (board.sideToMove == Black) raw = -raw;
                double pred = sigmoid(static_cast<double>(raw), K);
                double err = pred - positions[i].result;
                sum += err * err;
            }
            partial[t] = sum;
        });
    }
    for (auto &th : threads)
        th.join();
    double total = 0.0;
    for (double p : partial)
        total += p;
    return total / static_cast<double>(n);
}

static double findBestK(const std::vector<LabeledPosition> &positions, int numThreads) {
    // Golden-section-style bracket search. Internal scale is ~228 per
    // pawn, so K in the range [0.0005, 0.01] covers the plausible span
    // of Texel scaling constants.
    double lo = 0.0005, hi = 0.02;
    for (int iter = 0; iter < 40; iter++) {
        double m1 = lo + (hi - lo) / 3.0;
        double m2 = hi - (hi - lo) / 3.0;
        double l1 = computeLoss(positions, m1, numThreads);
        double l2 = computeLoss(positions, m2, numThreads);
        if (l1 < l2)
            hi = m2;
        else
            lo = m1;
        if (hi - lo < 1e-6) break;
    }
    return (lo + hi) / 2.0;
}

static void tune(std::vector<LabeledPosition> &positions, double K, int numThreads,
                 int maxPasses) {
    auto params = collectParams();
    std::cerr << "tuning " << params.size() << " scalars across " << positions.size()
              << " positions with " << numThreads << " threads, K=" << K << "\n";

    double bestLoss = computeLoss(positions, K, numThreads);
    std::cerr << "initial loss: " << bestLoss << "\n";

    // Relative improvement threshold -- changes smaller than this are
    // treated as noise, preventing coordinate descent from running away
    // on parameters that barely appear in the dataset. Also cap how far
    // a single parameter can move in one pass so no one scalar explodes.
    const double relThreshold = 1e-6;
    const int maxStepsPerPass = 20;

    for (int pass = 0; pass < maxPasses; pass++) {
        bool improved = false;
        for (size_t pi = 0; pi < params.size(); pi++) {
            auto &p = params[pi];
            int original = p.read();

            double threshold = bestLoss * relThreshold;

            // Try +1
            p.write(original + 1);
            double lossUp = computeLoss(positions, K, numThreads);
            int best = original;
            double bestHere = bestLoss;
            if (bestHere - lossUp > threshold) {
                bestHere = lossUp;
                best = original + 1;
                int steps = 1;
                while (steps < maxStepsPerPass) {
                    p.write(best + 1);
                    double l = computeLoss(positions, K, numThreads);
                    if (bestHere - l > threshold) {
                        bestHere = l;
                        best = best + 1;
                        steps++;
                    } else {
                        break;
                    }
                }
            } else {
                p.write(original - 1);
                double lossDown = computeLoss(positions, K, numThreads);
                if (bestHere - lossDown > threshold) {
                    bestHere = lossDown;
                    best = original - 1;
                    int steps = 1;
                    while (steps < maxStepsPerPass) {
                        p.write(best - 1);
                        double l = computeLoss(positions, K, numThreads);
                        if (bestHere - l > threshold) {
                            bestHere = l;
                            best = best - 1;
                            steps++;
                        } else {
                            break;
                        }
                    }
                }
            }

            p.write(best);
            if (best != original) {
                bestLoss = bestHere;
                improved = true;
                std::cerr << "  pass " << pass << " " << p.name << ": " << original << " -> "
                          << best << " loss=" << bestLoss << "\n";
            }
        }
        std::cerr << "pass " << pass << " done, loss=" << bestLoss
                  << (improved ? " (improved)" : " (no change)") << "\n";
        if (!improved) break;
    }
}

static void printCurrentValues() {
    std::cout << "// Updated evalParams (paste into src/eval_params.cpp)\n";
    auto fmt = [](Score s) {
        return "S(" + std::to_string(mg_value(s)) + ", " + std::to_string(eg_value(s)) + ")";
    };
    std::cout << "ThreatByPawn = " << fmt(evalParams.ThreatByPawn) << ";\n";
    std::cout << "ThreatByMinor[Rook] = " << fmt(evalParams.ThreatByMinor[Rook]) << ";\n";
    std::cout << "ThreatByMinor[Queen] = " << fmt(evalParams.ThreatByMinor[Queen]) << ";\n";
    std::cout << "ThreatByRook[Queen] = " << fmt(evalParams.ThreatByRook[Queen]) << ";\n";
    std::cout << "ThreatByKing = " << fmt(evalParams.ThreatByKing) << ";\n";
    std::cout << "Hanging = " << fmt(evalParams.Hanging) << ";\n";
    std::cout << "WeakQueen = " << fmt(evalParams.WeakQueen) << ";\n";
    std::cout << "SafePawnPush = " << fmt(evalParams.SafePawnPush) << ";\n";

    auto printArr = [&](const std::string &name, Score arr[8]) {
        std::cout << name << " = { ";
        for (int i = 0; i < 8; i++) {
            std::cout << fmt(arr[i]);
            if (i < 7) std::cout << ", ";
        }
        std::cout << " };\n";
    };
    printArr("PassedKingProxBonus", evalParams.PassedKingProxBonus);
    printArr("PassedEnemyKingProxPenalty", evalParams.PassedEnemyKingProxPenalty);
    printArr("PassedBlockedPenalty", evalParams.PassedBlockedPenalty);
    printArr("PassedSupportedBonus", evalParams.PassedSupportedBonus);
    printArr("ConnectedPassersBonus", evalParams.ConnectedPassersBonus);

    std::cout << "RookOn7thBonus = " << fmt(evalParams.RookOn7thBonus) << ";\n";
    std::cout << "BadBishopPenalty = " << fmt(evalParams.BadBishopPenalty) << ";\n";
    std::cout << "Tempo = " << fmt(evalParams.Tempo) << ";\n";
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: tune <dataset> [threads=6] [maxPasses=30]\n";
        return 1;
    }
    std::string dataset = argv[1];
    int numThreads = argc >= 3 ? std::atoi(argv[2]) : 6;
    int maxPasses = argc >= 4 ? std::atoi(argv[3]) : 30;

    zobrist::init();

    std::cerr << "loading " << dataset << "...\n";
    auto positions = loadDataset(dataset);
    std::cerr << "loaded " << positions.size() << " positions\n";

    std::cerr << "finding K...\n";
    double K = findBestK(positions, numThreads);
    std::cerr << "K=" << K << "\n";

    tune(positions, K, numThreads, maxPasses);
    printCurrentValues();
    return 0;
}
