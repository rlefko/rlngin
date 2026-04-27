// Texel-style local tuner for the new eval parameters.
//
// Reads a labeled dataset of `FEN | result` lines, where result is 1.0,
// 0.5, or 0.0 from White's perspective. Finds a scaling constant K that
// minimizes the mean squared error between sigmoid(K * eval) and the
// game result, then runs coordinate descent over every mg / eg half of
// every Score in `evalParams`. The final parameter values are printed
// in a form that can be pasted back into `src/eval_params.cpp`.

#include "bitboard.h"
#include "board.h"
#include "eval.h"
#include "eval_params.h"
#include "search.h"
#include "types.h"
#include "zobrist.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct LabeledPosition {
    Board board;
    double result; // 1.0 / 0.5 / 0.0 from White POV
};

// Inclusive integer bounds for a parameter half. The wide defaults are
// effectively unconstrained while still letting callers safely clamp
// without overflow when scalars run unbounded.
struct Bounds {
    int lo = -1000000;
    int hi = 1000000;
};

// Accessor for a single mg/eg half of a Score field. `bounds()` returns
// the live feasible range, which can read sibling values out of
// `evalParams` for chain constraints (e.g. mobility neighbors, passer
// rank chains). `allow()` and `clamp()` derive from `bounds()` so the
// strict CD predicate, the constraint validator, and the projection
// clamp all see the same definition of "feasible".
struct ParamRef {
    std::string name;
    Score *target;
    bool isMg; // true = modify mg half, false = eg half
    std::function<Bounds()> bounds = [] { return Bounds{}; };

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
    bool allow(int v) const {
        Bounds b = bounds();
        return b.lo <= v && v <= b.hi;
    }
    int clampToBounds(int v) const {
        Bounds b = bounds();
        return std::max(b.lo, std::min(b.hi, v));
    }
};

// Convenience bounds factories for the constraint catalog. Wider
// `Bounds{}` defaults stand in for "unconstrained".
static std::function<Bounds()> boundsAny() {
    return [] { return Bounds{}; };
}
static std::function<Bounds()> boundsNonPositive() {
    return [] { return Bounds{-1000000, 0}; };
}
static std::function<Bounds()> boundsNonNegative() {
    return [] { return Bounds{0, 1000000}; };
}
static std::function<Bounds()> boundsRange(int lo, int hi) {
    return [lo, hi] { return Bounds{lo, hi}; };
}

static std::vector<ParamRef> collectParams() {
    std::vector<ParamRef> out;
    auto addMgEg = [&](const std::string &name, Score *s, bool mg = true, bool eg = true) {
        if (mg) out.push_back({name + ".mg", s, true});
        if (eg) out.push_back({name + ".eg", s, false});
    };
    // Overload that stamps the same bounds factory on both halves. The
    // factory is invoked at every constraint check so chain bounds can
    // read live sibling values without going stale across pass updates.
    auto addMgEgConstr = [&](const std::string &name, Score *s,
                             std::function<Bounds()> bounds, bool mg = true, bool eg = true) {
        if (mg) out.push_back({name + ".mg", s, true, bounds});
        if (eg) out.push_back({name + ".eg", s, false, bounds});
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
    // PassedEnemyKingProxPenalty is stored as a positive magnitude that
    // the eval subtracts, so the chess prior "enemy king close to our
    // passer is bad for the passer" maps to keeping the stored value
    // non-negative. Without the constraint a tuner with a noisy corpus
    // could flip it to negative and silently invert the term.
    for (int r = 3; r <= 6; r++) {
        addMgEg("PassedKingProxBonus[" + std::to_string(r) + "]",
                &evalParams.PassedKingProxBonus[r], false, true); // eg only
        out.push_back({"PassedEnemyKingProxPenalty[" + std::to_string(r) + "].eg",
                       &evalParams.PassedEnemyKingProxPenalty[r], false, boundsNonNegative()});
        addMgEgConstr("PassedBlockedPenalty[" + std::to_string(r) + "]",
                      &evalParams.PassedBlockedPenalty[r], boundsNonPositive());
        addMgEg("PassedSupportedBonus[" + std::to_string(r) + "]",
                &evalParams.PassedSupportedBonus[r]);
        addMgEg("ConnectedPassersBonus[" + std::to_string(r) + "]",
                &evalParams.ConnectedPassersBonus[r]);
    }

    addMgEg("RookOn7thBonus", &evalParams.RookOn7thBonus);
    addMgEgConstr("BadBishopPenalty", &evalParams.BadBishopPenalty, boundsNonPositive());
    addMgEg("Tempo", &evalParams.Tempo, true, false); // mg only

    // --- Material (skip None and King; both are structurally zero) ---
    for (int pt = Pawn; pt <= Queen; pt++)
        addMgEg("PieceScore[" + std::to_string(pt) + "]", &evalParams.PieceScore[pt]);

    // --- Piece-square tables. Each PST has 64 squares; skip the back/
    // front ranks of the pawn PST because they are always zero. King
    // PST squares stay tunable because the king appears on them.
    auto addPST = [&](const std::string &name, Score *arr, int start, int end) {
        for (int sq = start; sq < end; sq++) {
            addMgEg(name + "[" + std::to_string(sq) + "]", &arr[sq]);
        }
    };
    addPST("PawnPST", evalParams.PawnPST, 8, 56);
    addPST("KnightPST", evalParams.KnightPST, 0, 64);
    addPST("BishopPST", evalParams.BishopPST, 0, 64);
    addPST("RookPST", evalParams.RookPST, 0, 64);
    addPST("QueenPST", evalParams.QueenPST, 0, 64);
    addPST("KingPST", evalParams.KingPST, 0, 64);

    // --- Mobility bonuses (only Knight..Queen rows carry values).
    // Soft chess prior: more attacked mobility-area squares is at least
    // as good as fewer of them. Enforced as a non-decreasing chain on
    // both halves, indexed by attack count. Per-entry sign is left
    // unconstrained because the lowest counts (a knight with 0..2 moves)
    // legitimately carry a negative contribution.
    static const int mobilityCounts[7] = {0, 0, 9, 14, 15, 28, 0};
    for (int pt = Knight; pt <= Queen; pt++) {
        const int n = mobilityCounts[pt];
        for (int i = 0; i < n; i++) {
            auto mgChain = [pt, i, n] {
                Bounds b;
                if (i > 0)
                    b.lo = std::max(b.lo, mg_value(evalParams.MobilityBonus[pt][i - 1]));
                if (i < n - 1)
                    b.hi = std::min(b.hi, mg_value(evalParams.MobilityBonus[pt][i + 1]));
                return b;
            };
            auto egChain = [pt, i, n] {
                Bounds b;
                if (i > 0)
                    b.lo = std::max(b.lo, eg_value(evalParams.MobilityBonus[pt][i - 1]));
                if (i < n - 1)
                    b.hi = std::min(b.hi, eg_value(evalParams.MobilityBonus[pt][i + 1]));
                return b;
            };
            std::string base =
                "MobilityBonus[" + std::to_string(pt) + "][" + std::to_string(i) + "]";
            out.push_back({base + ".mg", &evalParams.MobilityBonus[pt][i], true, mgChain});
            out.push_back({base + ".eg", &evalParams.MobilityBonus[pt][i], false, egChain});
        }
    }

    // --- Passed pawn base bonus and connected pawn bonus (ranks 1..6
    // carry non-zero defaults; rank 0/7 stay at zero). ---
    for (int r = 1; r <= 6; r++)
        addMgEg("PassedPawnBonus[" + std::to_string(r) + "]", &evalParams.PassedPawnBonus[r]);
    for (int r = 1; r <= 6; r++)
        addMgEg("ConnectedPawnBonus[" + std::to_string(r) + "]", &evalParams.ConnectedPawnBonus[r]);

    // --- Rook files, outposts, trapped rook ---
    addMgEg("RookOpenFileBonus", &evalParams.RookOpenFileBonus);
    addMgEg("RookSemiOpenFileBonus", &evalParams.RookSemiOpenFileBonus);
    addMgEg("KnightOutpostBonus", &evalParams.KnightOutpostBonus);
    addMgEg("BishopOutpostBonus", &evalParams.BishopOutpostBonus);
    out.push_back({"TrappedRookByKingPenalty.mg", &evalParams.TrappedRookByKingPenalty, true,
                   boundsNonPositive()}); // mg only, must stay a penalty
    addMgEg("RookBehindOurPasserBonus", &evalParams.RookBehindOurPasserBonus);
    addMgEg("RookBehindTheirPasserBonus", &evalParams.RookBehindTheirPasserBonus);
    addMgEg("MinorBehindPawnBonus", &evalParams.MinorBehindPawnBonus);
    // King-ring pressure: a piece whose attack set intersects the enemy
    // king zone is a positive influence on our score by definition.
    // Locked non-negative so the tuner cannot use these terms as a
    // residual sink for unrelated correlations.
    addMgEgConstr("MinorOnKingRing", &evalParams.MinorOnKingRing, boundsNonNegative());
    addMgEgConstr("RookOnKingRing", &evalParams.RookOnKingRing, boundsNonNegative());
    addMgEgConstr("KingProtector", &evalParams.KingProtector, boundsNonPositive());

    // --- Bishop pair: universally accepted positive imbalance, locked
    // non-negative so the tuner cannot push the term into a residual
    // role that contradicts the prior.
    addMgEgConstr("BishopPair", &evalParams.BishopPair, boundsNonNegative());

    // --- Pawn shield and storm, king-zone scalars ---
    for (int i = 0; i < 2; i++)
        addMgEg("PawnShieldBonus[" + std::to_string(i) + "]", &evalParams.PawnShieldBonus[i]);
    for (int i = 0; i < 5; i++) {
        // Consumed with `scores -= <Blocked|Unblocked>PawnStorm[idx]`, so
        // magnitudes must stay non-negative to preserve the "enemy advance
        // hurts us" prior.
        out.push_back({"BlockedPawnStorm[" + std::to_string(i) + "].mg",
                       &evalParams.BlockedPawnStorm[i], true, boundsNonNegative()});
        out.push_back({"UnblockedPawnStorm[" + std::to_string(i) + "].mg",
                       &evalParams.UnblockedPawnStorm[i], true, boundsNonNegative()});
    }
    out.push_back({"SemiOpenFileNearKing.mg", &evalParams.SemiOpenFileNearKing, true,
                   boundsNonPositive()});
    out.push_back({"OpenFileNearKing.mg", &evalParams.OpenFileNearKing, true, boundsNonPositive()});
    addMgEgConstr("UndefendedKingZoneSq", &evalParams.UndefendedKingZoneSq, boundsNonPositive());
    // KingSafeSqPenalty: each slot must stay a penalty (<= 0), and the
    // chain is monotonically non-decreasing -- more safe king-move
    // squares can never score lower than fewer. Predicate closes over
    // the index so it can consult the live neighboring Score values.
    for (int i = 0; i < 9; i++) {
        auto mgChain = [i] {
            Bounds b{-1000000, 0}; // every slot is a penalty (<= 0)
            if (i > 0)
                b.lo = std::max(b.lo, mg_value(evalParams.KingSafeSqPenalty[i - 1]));
            if (i < 8)
                b.hi = std::min(b.hi, mg_value(evalParams.KingSafeSqPenalty[i + 1]));
            return b;
        };
        auto egChain = [i] {
            Bounds b{-1000000, 0};
            if (i > 0)
                b.lo = std::max(b.lo, eg_value(evalParams.KingSafeSqPenalty[i - 1]));
            if (i < 8)
                b.hi = std::min(b.hi, eg_value(evalParams.KingSafeSqPenalty[i + 1]));
            return b;
        };
        out.push_back({"KingSafeSqPenalty[" + std::to_string(i) + "].mg",
                       &evalParams.KingSafeSqPenalty[i], true, mgChain});
        out.push_back({"KingSafeSqPenalty[" + std::to_string(i) + "].eg",
                       &evalParams.KingSafeSqPenalty[i], false, egChain});
    }

    // --- King-danger accumulator weights. Each per-attacker weight feeds
    // the quadratic king-danger term; non-negative so an extra attacker
    // never reduces danger. KingNoQueenDiscount is subtracted from the
    // accumulator, so its magnitude must also stay non-negative.
    addMgEgConstr("KingAttackByKnight", &evalParams.KingAttackByKnight, boundsNonNegative());
    addMgEgConstr("KingAttackByBishop", &evalParams.KingAttackByBishop, boundsNonNegative());
    addMgEgConstr("KingAttackByRook", &evalParams.KingAttackByRook, boundsNonNegative());
    addMgEgConstr("KingAttackByQueen", &evalParams.KingAttackByQueen, boundsNonNegative());
    for (int pt = Knight; pt <= Queen; pt++) {
        addMgEgConstr("KingSafeCheck[" + std::to_string(pt) + "]",
                      &evalParams.KingSafeCheck[pt], boundsNonNegative());
    }
    addMgEgConstr("KingRingWeakWeight", &evalParams.KingRingWeakWeight, boundsNonNegative());
    addMgEgConstr("KingNoQueenDiscount", &evalParams.KingNoQueenDiscount, boundsNonNegative());

    // --- Pawn-structure penalties ---
    addMgEgConstr("IsolatedPawnPenalty", &evalParams.IsolatedPawnPenalty, boundsNonPositive());
    addMgEgConstr("DoubledPawnPenalty", &evalParams.DoubledPawnPenalty, boundsNonPositive());
    addMgEgConstr("BackwardPawnPenalty", &evalParams.BackwardPawnPenalty, boundsNonPositive());
    addMgEgConstr("WeakUnopposedPenalty", &evalParams.WeakUnopposedPenalty, boundsNonPositive());
    addMgEgConstr("DoubledIsolatedPenalty", &evalParams.DoubledIsolatedPenalty, boundsNonPositive());
    for (int i = 0; i < 2; i++)
        addMgEgConstr("BlockedPawnPenalty[" + std::to_string(i) + "]",
                      &evalParams.BlockedPawnPenalty[i], boundsNonPositive());
    addMgEgConstr("PawnIslandPenalty", &evalParams.PawnIslandPenalty, boundsNonPositive());
    // PhalanxBonus is disabled in eval (see eval_params.h); skip tuning it.
    // addMgEg("PhalanxBonus", &evalParams.PhalanxBonus);

    // --- Central pawn occupancy. Eg structurally zero per
    // eval_params.h:184; only the mg half is exposed.
    out.push_back({"CentralPawnBonus[0].mg", &evalParams.CentralPawnBonus[0], true,
                   boundsNonNegative()});
    out.push_back({"CentralPawnBonus[1].mg", &evalParams.CentralPawnBonus[1], true,
                   boundsNonNegative()});

    // --- Bishop long diagonal sweep ---
    addMgEgConstr("BishopLongDiagonalBonus", &evalParams.BishopLongDiagonalBonus, boundsNonNegative());

    // --- Initiative system. All seven scalars carry mg=0 by construction
    // (see eval_params.h:206-208) and live entirely in the eg half. The
    // first six are positive features; InitiativeConstant is the
    // negative baseline shift.
    out.push_back({"InitiativePasser.eg", &evalParams.InitiativePasser, false, boundsNonNegative()});
    out.push_back({"InitiativePawnCount.eg", &evalParams.InitiativePawnCount, false,
                   boundsNonNegative()});
    out.push_back({"InitiativeOutflank.eg", &evalParams.InitiativeOutflank, false,
                   boundsNonNegative()});
    out.push_back({"InitiativeTension.eg", &evalParams.InitiativeTension, false, boundsNonNegative()});
    out.push_back({"InitiativeInfiltrate.eg", &evalParams.InitiativeInfiltrate, false,
                   boundsNonNegative()});
    out.push_back({"InitiativePureBase.eg", &evalParams.InitiativePureBase, false,
                   boundsNonNegative()});
    out.push_back({"InitiativeConstant.eg", &evalParams.InitiativeConstant, false,
                   boundsNonPositive()});

    // --- Slider on queen x-ray ---
    addMgEg("SliderOnQueenBishop", &evalParams.SliderOnQueenBishop);
    addMgEg("SliderOnQueenRook", &evalParams.SliderOnQueenRook);

    // --- Restricted piece ---
    addMgEg("RestrictedPiece", &evalParams.RestrictedPiece);

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
    // Positions here already hold qsearch-resolved leaf boards, so the
    // inner loop only needs static evaluate(). The pawn and material
    // hashes still cache per-eval-param state, so clear them on each
    // loss evaluation; threads within a single loss call race on the
    // shared hash writes, which we accept as tuner noise.
    clearPawnHash();
    clearMaterialHash();
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
                // evaluate returns side-to-move relative; convert to
                // White POV so the tuner can compare to the White-POV
                // game result. The leaf's side-to-move may differ from
                // the root if an odd number of captures were resolved
                // during qsearch.
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

// Replace each training position's root board with its qsearch-resolved
// leaf so the loss loop can use a fast static evaluate() call. Runs
// sequentially because qsearchLeafBoard clears and rewrites the global
// TT per position.
static void precomputeLeaves(std::vector<LabeledPosition> &positions) {
    std::cerr << "precomputing qsearch leaves for " << positions.size() << " positions...\n";
    size_t reported = 0;
    for (size_t i = 0; i < positions.size(); i++) {
        positions[i].board = qsearchLeafBoard(positions[i].board);
        if (i - reported >= 50000) {
            std::cerr << "  " << i << " leaves computed\n";
            reported = i;
        }
    }
    std::cerr << "  done\n";
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

// Project every scalar onto its current feasible region. Each ParamRef
// has a live bounds() factory that may read sibling values (chain
// constraints). We sweep all params, calling clampToBounds() to move
// every out-of-bounds scalar to the nearest feasible value, and repeat
// until a sweep produces no change. Because each clamp moves a scalar
// toward (and reaches) its allowed range -- never out of it -- the
// iteration converges in at most a handful of sweeps for the chains we
// register; the cap is purely a guardrail.
//
// This is the replacement for the prior `clampToConstraints` whose
// repair direction was sign-derived and could not climb a chain to
// satisfy a non-decreasing prior. Iterative clamp from live bounds
// handles both directions naturally.
static void projectToConstraints(std::vector<ParamRef> &params) {
    int snapped = 0;
    for (int sweep = 0; sweep < 50; sweep++) {
        bool changed = false;
        for (auto &p : params) {
            int v = p.read();
            int target = p.clampToBounds(v);
            if (target != v) {
                std::cerr << "  project " << p.name << ": " << v << " -> " << target << "\n";
                p.write(target);
                snapped++;
                changed = true;
            }
        }
        if (!changed) break;
    }
    std::cerr << "projected " << snapped << " scalars into the constraint region\n";
}

// Hard validation step. Walks every ParamRef, prints any value outside
// its live bounds, and exits with non-zero status if any violator
// survives. Called from every entry path that loads or applies a
// checkpoint so an invalid snapshot never gets used or written.
static bool validateConstraints(const std::vector<ParamRef> &params, bool fatal = true) {
    int violations = 0;
    for (const auto &p : params) {
        int v = p.read();
        if (!p.allow(v)) {
            Bounds b = p.bounds();
            std::cerr << "constraint violation: " << p.name << " = " << v << " (bounds [" << b.lo
                      << ", " << b.hi << "])\n";
            violations++;
        }
    }
    if (violations == 0) {
        std::cerr << "validateConstraints: all " << params.size() << " params in bounds\n";
        return true;
    }
    std::cerr << "validateConstraints: " << violations << " violators\n";
    if (fatal) std::exit(1);
    return false;
}

// Dump every collected scalar to `path` as `<name> <value>\n`. Acts as a
// crash-safe per-pass checkpoint so a long tune that gets killed mid-run
// can resume without redoing the work that already landed.
static void writeCheckpoint(const std::string &path, const std::vector<ParamRef> &params) {
    std::ofstream out(path);
    if (!out) {
        std::cerr << "warning: failed to open checkpoint path " << path << "\n";
        return;
    }
    for (const auto &p : params)
        out << p.name << " " << p.read() << "\n";
}

// Inverse of writeCheckpoint: read `<name> <value>` lines and stamp the
// values into `evalParams` via the matching ParamRef setters. Names that
// no longer exist in the registry are skipped with a warning.
static void loadCheckpoint(const std::string &path, const std::vector<ParamRef> &params) {
    std::unordered_map<std::string, const ParamRef *> byName;
    for (const auto &p : params)
        byName[p.name] = &p;
    std::ifstream in(path);
    if (!in) {
        std::cerr << "could not open checkpoint " << path << "\n";
        std::exit(1);
    }
    std::string name;
    int value;
    int loaded = 0, missing = 0;
    while (in >> name >> value) {
        auto it = byName.find(name);
        if (it == byName.end()) {
            missing++;
            continue;
        }
        it->second->write(value);
        loaded++;
    }
    std::cerr << "loaded " << loaded << " params from " << path;
    if (missing) std::cerr << " (" << missing << " unknown names skipped)";
    std::cerr << "\n";
}

// Reconstruct in-memory tuner state by replaying every accepted change
// from a previous tune.log against the compile-time defaults. Picks up
// both the constraint clamps that fire before pass 0 and the
// coordinate-descent steps that follow. Writes the resulting state out
// as a checkpoint so the resumed tune can pick up via --from.
static void replayLog(const std::string &logPath, const std::string &outPath) {
    auto params = collectParams();
    std::unordered_map<std::string, const ParamRef *> byName;
    for (const auto &p : params)
        byName[p.name] = &p;

    std::ifstream in(logPath);
    if (!in) {
        std::cerr << "could not open log " << logPath << "\n";
        std::exit(1);
    }

    static const std::regex passRe(R"(\s*pass\s+\d+\s+(\S+):\s+(-?\d+)\s+->\s+(-?\d+))");
    static const std::regex clampRe(R"(\s*clamp\s+(\S+):\s+(-?\d+)\s+->\s+(-?\d+))");

    std::string line;
    int clamps = 0, replays = 0, mismatches = 0, unknown = 0;
    while (std::getline(in, line)) {
        std::smatch m;
        bool isClamp = std::regex_search(line, m, clampRe);
        if (!isClamp && !std::regex_search(line, m, passRe)) continue;

        std::string name = m[1].str();
        int from = std::stoi(m[2].str());
        int to = std::stoi(m[3].str());
        auto it = byName.find(name);
        if (it == byName.end()) {
            unknown++;
            continue;
        }
        if (it->second->read() != from) mismatches++;
        it->second->write(to);
        if (isClamp)
            clamps++;
        else
            replays++;
    }
    std::cerr << "replay: " << clamps << " clamp updates, " << replays
              << " coordinate-descent steps applied, " << mismatches
              << " from-value mismatches, " << unknown << " unknown names\n";

    // Project the replayed state onto current constraint bounds and
    // validate before writing. The replayed snapshot is captured under
    // whatever bounds were in force during the original run; new
    // constraints registered since may render some halves invalid.
    projectToConstraints(params);
    validateConstraints(params);

    writeCheckpoint(outPath, params);
    std::cerr << "checkpoint written to " << outPath << "\n";
}

static void tune(std::vector<LabeledPosition> &positions, double K, int numThreads,
                 int maxPasses) {
    auto params = collectParams();
    std::cerr << "tuning " << params.size() << " scalars across " << positions.size()
              << " positions with " << numThreads << " threads, K=" << K << "\n";

    projectToConstraints(params);
    validateConstraints(params);

    double bestLoss = computeLoss(positions, K, numThreads);
    std::cerr << "initial loss: " << bestLoss << "\n";

    // Texel's Tuning Method with a step ladder. The strict CPW pseudocode
    // tries `+1` then `-1` per scalar; we generalize to a descending
    // ladder `[8, 4, 2, 1]` so larger plateaus get crossed in early
    // passes without losing the strict-improvement guarantee. The
    // bottom rung is still `+/-1`, so a pass that produces no movement
    // at any step is at least as tight a local optimum as strict CPW
    // would ever reach -- anything `+/-1` would catch is still caught,
    // plus larger jumps that strict `+/-1` would miss are now seen.
    // Each accepted step still strictly decreases the global loss, and
    // constrained scalars skip any direction that would leave their
    // feasible region. maxPasses caps runtime when convergence stalls.
    const double relThreshold = 1e-8;
    static const std::array<int, 4> stepLadder = {8, 4, 2, 1};

    for (int pass = 0; pass < maxPasses; pass++) {
        bool improved = false;
        for (size_t pi = 0; pi < params.size(); pi++) {
            auto &p = params[pi];
            int original = p.read();
            double threshold = bestLoss * relThreshold;
            bool accepted = false;

            for (int step : stepLadder) {
                if (p.allow(original + step)) {
                    p.write(original + step);
                    double loss = computeLoss(positions, K, numThreads);
                    if (bestLoss - loss > threshold) {
                        bestLoss = loss;
                        improved = true;
                        accepted = true;
                        std::cerr << "  pass " << pass << " " << p.name << ": " << original
                                  << " -> " << (original + step) << " loss=" << bestLoss << "\n";
                        break;
                    }
                }
                if (p.allow(original - step)) {
                    p.write(original - step);
                    double loss = computeLoss(positions, K, numThreads);
                    if (bestLoss - loss > threshold) {
                        bestLoss = loss;
                        improved = true;
                        accepted = true;
                        std::cerr << "  pass " << pass << " " << p.name << ": " << original
                                  << " -> " << (original - step) << " loss=" << bestLoss << "\n";
                        break;
                    }
                }
            }

            if (!accepted) p.write(original);
        }
        std::cerr << "pass " << pass << " done, loss=" << bestLoss
                  << (improved ? " (improved)" : " (no change)") << "\n";
        writeCheckpoint("tuning/checkpoint.txt", params);
        if (!improved) break;
    }
}

static std::string fmtScore(Score s) {
    return "S(" + std::to_string(mg_value(s)) + ", " + std::to_string(eg_value(s)) + ")";
}

static void printArr8(const std::string &indent, Score arr[8]) {
    std::cout << indent << "{";
    for (int i = 0; i < 8; i++) {
        std::cout << fmtScore(arr[i]);
        if (i < 7) std::cout << ", ";
    }
    std::cout << "},\n";
}

static void printPST(const std::string &name, Score arr[64]) {
    std::cout << "    // " << name << "\n";
    std::cout << "    {\n";
    for (int row = 0; row < 8; row++) {
        std::cout << "        ";
        for (int col = 0; col < 8; col++) {
            std::cout << fmtScore(arr[row * 8 + col]);
            if (row != 7 || col != 7) std::cout << ",";
            if (col < 7) std::cout << " ";
        }
        std::cout << "\n";
    }
    std::cout << "    },\n";
}

static void printCurrentValues() {
    std::cout << "// Paste as the full body of kDefaultEvalParams in src/eval_params.cpp\n";
    std::cout << "static const EvalParams kDefaultEvalParams = {\n";

    std::cout << "    " << fmtScore(evalParams.ThreatByPawn) << ", // ThreatByPawn\n";
    std::cout << "    {";
    for (int i = 0; i < 7; i++) {
        std::cout << fmtScore(evalParams.ThreatByMinor[i]);
        if (i < 6) std::cout << ", ";
    }
    std::cout << "},\n";
    std::cout << "    {";
    for (int i = 0; i < 7; i++) {
        std::cout << fmtScore(evalParams.ThreatByRook[i]);
        if (i < 6) std::cout << ", ";
    }
    std::cout << "},\n";
    std::cout << "    " << fmtScore(evalParams.ThreatByKing) << ", // ThreatByKing\n";
    std::cout << "    " << fmtScore(evalParams.Hanging) << ", // Hanging\n";
    std::cout << "    " << fmtScore(evalParams.WeakQueen) << ", // WeakQueen\n";
    std::cout << "    " << fmtScore(evalParams.SafePawnPush) << ", // SafePawnPush\n";

    printArr8("    ", evalParams.PassedKingProxBonus);
    printArr8("    ", evalParams.PassedEnemyKingProxPenalty);
    printArr8("    ", evalParams.PassedBlockedPenalty);
    printArr8("    ", evalParams.PassedSupportedBonus);
    printArr8("    ", evalParams.ConnectedPassersBonus);

    std::cout << "    " << fmtScore(evalParams.RookOn7thBonus) << ", // RookOn7thBonus\n";
    std::cout << "    " << fmtScore(evalParams.BadBishopPenalty) << ", // BadBishopPenalty\n";
    std::cout << "    " << fmtScore(evalParams.Tempo) << ", // Tempo\n";

    // PieceScore
    std::cout << "    {";
    for (int i = 0; i < 7; i++) {
        std::cout << fmtScore(evalParams.PieceScore[i]);
        if (i < 6) std::cout << ", ";
    }
    std::cout << "}, // PieceScore\n";

    printPST("PawnPST", evalParams.PawnPST);
    printPST("KnightPST", evalParams.KnightPST);
    printPST("BishopPST", evalParams.BishopPST);
    printPST("RookPST", evalParams.RookPST);
    printPST("QueenPST", evalParams.QueenPST);
    printPST("KingPST", evalParams.KingPST);

    // MobilityBonus
    std::cout << "    {\n";
    static const int mobilityCounts[7] = {0, 0, 9, 14, 15, 28, 0};
    for (int pt = 0; pt < 7; pt++) {
        std::cout << "        {";
        for (int i = 0; i < mobilityCounts[pt]; i++) {
            std::cout << fmtScore(evalParams.MobilityBonus[pt][i]);
            if (i < mobilityCounts[pt] - 1) std::cout << ", ";
        }
        std::cout << "},\n";
    }
    std::cout << "    },\n";

    printArr8("    ", evalParams.PassedPawnBonus);
    printArr8("    ", evalParams.ConnectedPawnBonus);

    std::cout << "    " << fmtScore(evalParams.RookOpenFileBonus) << ", // RookOpenFileBonus\n";
    std::cout << "    " << fmtScore(evalParams.RookSemiOpenFileBonus)
              << ", // RookSemiOpenFileBonus\n";
    std::cout << "    " << fmtScore(evalParams.KnightOutpostBonus) << ", // KnightOutpostBonus\n";
    std::cout << "    " << fmtScore(evalParams.BishopOutpostBonus) << ", // BishopOutpostBonus\n";
    std::cout << "    " << fmtScore(evalParams.TrappedRookByKingPenalty)
              << ", // TrappedRookByKingPenalty\n";
    std::cout << "    " << fmtScore(evalParams.RookBehindOurPasserBonus)
              << ", // RookBehindOurPasserBonus\n";
    std::cout << "    " << fmtScore(evalParams.RookBehindTheirPasserBonus)
              << ", // RookBehindTheirPasserBonus\n";
    std::cout << "    " << fmtScore(evalParams.MinorBehindPawnBonus)
              << ", // MinorBehindPawnBonus\n";
    std::cout << "    " << fmtScore(evalParams.MinorOnKingRing) << ", // MinorOnKingRing\n";
    std::cout << "    " << fmtScore(evalParams.RookOnKingRing) << ", // RookOnKingRing\n";
    std::cout << "    " << fmtScore(evalParams.KingProtector) << ", // KingProtector\n";
    std::cout << "    " << fmtScore(evalParams.BishopPair) << ", // BishopPair\n";

    std::cout << "    {" << fmtScore(evalParams.PawnShieldBonus[0]) << ", "
              << fmtScore(evalParams.PawnShieldBonus[1]) << "}, // PawnShieldBonus\n";
    std::cout << "    {";
    for (int i = 0; i < 5; i++) {
        std::cout << fmtScore(evalParams.BlockedPawnStorm[i]);
        if (i < 4) std::cout << ", ";
    }
    std::cout << "}, // BlockedPawnStorm\n";
    std::cout << "    {";
    for (int i = 0; i < 5; i++) {
        std::cout << fmtScore(evalParams.UnblockedPawnStorm[i]);
        if (i < 4) std::cout << ", ";
    }
    std::cout << "}, // UnblockedPawnStorm\n";
    std::cout << "    " << fmtScore(evalParams.SemiOpenFileNearKing)
              << ", // SemiOpenFileNearKing\n";
    std::cout << "    " << fmtScore(evalParams.OpenFileNearKing) << ", // OpenFileNearKing\n";
    std::cout << "    " << fmtScore(evalParams.UndefendedKingZoneSq)
              << ", // UndefendedKingZoneSq\n";
    std::cout << "    {";
    for (int i = 0; i < 9; i++) {
        std::cout << fmtScore(evalParams.KingSafeSqPenalty[i]);
        if (i < 8) std::cout << ", ";
    }
    std::cout << "}, // KingSafeSqPenalty\n";

    std::cout << "    " << fmtScore(evalParams.KingAttackByKnight)
              << ", // KingAttackByKnight\n";
    std::cout << "    " << fmtScore(evalParams.KingAttackByBishop)
              << ", // KingAttackByBishop\n";
    std::cout << "    " << fmtScore(evalParams.KingAttackByRook) << ", // KingAttackByRook\n";
    std::cout << "    " << fmtScore(evalParams.KingAttackByQueen) << ", // KingAttackByQueen\n";
    std::cout << "    {";
    for (int i = 0; i < 7; i++) {
        std::cout << fmtScore(evalParams.KingSafeCheck[i]);
        if (i < 6) std::cout << ", ";
    }
    std::cout << "}, // KingSafeCheck\n";
    std::cout << "    " << fmtScore(evalParams.KingRingWeakWeight)
              << ", // KingRingWeakWeight\n";
    std::cout << "    " << fmtScore(evalParams.KingNoQueenDiscount)
              << ", // KingNoQueenDiscount\n";

    std::cout << "    " << fmtScore(evalParams.IsolatedPawnPenalty) << ", // IsolatedPawnPenalty\n";
    std::cout << "    " << fmtScore(evalParams.DoubledPawnPenalty) << ", // DoubledPawnPenalty\n";
    std::cout << "    " << fmtScore(evalParams.BackwardPawnPenalty) << ", // BackwardPawnPenalty\n";
    std::cout << "    " << fmtScore(evalParams.WeakUnopposedPenalty)
              << ", // WeakUnopposedPenalty\n";
    std::cout << "    " << fmtScore(evalParams.DoubledIsolatedPenalty)
              << ", // DoubledIsolatedPenalty\n";
    std::cout << "    {" << fmtScore(evalParams.BlockedPawnPenalty[0]) << ", "
              << fmtScore(evalParams.BlockedPawnPenalty[1])
              << "}, // BlockedPawnPenalty (rel rank 5, 6)\n";
    std::cout << "    " << fmtScore(evalParams.PawnIslandPenalty) << ", // PawnIslandPenalty\n";
    // PhalanxBonus is disabled in eval_params.h; re-enable the dump when the
    // field and tuner entry come back.
    // std::cout << "    " << fmtScore(evalParams.PhalanxBonus) << ", // PhalanxBonus\n";
    std::cout << "    {" << fmtScore(evalParams.CentralPawnBonus[0]) << ", "
              << fmtScore(evalParams.CentralPawnBonus[1]) << "}, // CentralPawnBonus\n";
    std::cout << "    " << fmtScore(evalParams.BishopLongDiagonalBonus)
              << ", // BishopLongDiagonalBonus\n";
    std::cout << "    " << fmtScore(evalParams.InitiativePasser) << ", // InitiativePasser\n";
    std::cout << "    " << fmtScore(evalParams.InitiativePawnCount)
              << ", // InitiativePawnCount\n";
    std::cout << "    " << fmtScore(evalParams.InitiativeOutflank)
              << ", // InitiativeOutflank\n";
    std::cout << "    " << fmtScore(evalParams.InitiativeTension) << ", // InitiativeTension\n";
    std::cout << "    " << fmtScore(evalParams.InitiativeInfiltrate)
              << ", // InitiativeInfiltrate\n";
    std::cout << "    " << fmtScore(evalParams.InitiativePureBase)
              << ", // InitiativePureBase\n";
    std::cout << "    " << fmtScore(evalParams.InitiativeConstant)
              << ", // InitiativeConstant\n";
    std::cout << "    " << fmtScore(evalParams.SliderOnQueenBishop)
              << ", // SliderOnQueenBishop\n";
    std::cout << "    " << fmtScore(evalParams.SliderOnQueenRook) << ", // SliderOnQueenRook\n";
    std::cout << "    " << fmtScore(evalParams.RestrictedPiece) << ", // RestrictedPiece\n";
    std::cout << "};\n";
}

} // namespace

int main(int argc, char **argv) {
    auto usage = [] {
        std::cerr << "usage: tune [--from <ckpt>] <dataset> [threads=6] [maxPasses=30]\n";
        std::cerr << "       tune --replay <log> <ckpt-out>\n";
        std::cerr << "       tune --dump <ckpt>\n";
    };

    // Dump subcommand: load a checkpoint and emit the printCurrentValues
    // initializer to stdout. Useful for snapshotting an in-flight tune
    // without disturbing it - the running tuner writes a checkpoint after
    // every pass, so this gives a clean handoff point.
    if (argc >= 2 && std::string(argv[1]) == "--dump") {
        if (argc < 3) {
            usage();
            return 1;
        }
        zobrist::init();
        initBitboards();
        auto params = collectParams();
        loadCheckpoint(argv[2], params);
        // Warn-only validation: emit the snapshot even if it has stale
        // bound violations so the caller can splice into eval_params.cpp
        // before bounds are tightened. The next `--from` load will project
        // and validate fatally.
        validateConstraints(params, /*fatal=*/false);
        printCurrentValues();
        return 0;
    }

    // Replay subcommand: parse a previous tune.log and write a checkpoint
    // that captures every accepted clamp / coordinate-descent step.
    if (argc >= 2 && std::string(argv[1]) == "--replay") {
        if (argc < 4) {
            usage();
            return 1;
        }
        zobrist::init();
        initBitboards();
        replayLog(argv[2], argv[3]);
        return 0;
    }

    // Optional --from <ckpt> prefix loads a checkpoint over the
    // compile-time defaults before any tuning starts.
    std::string fromCheckpoint;
    int argIdx = 1;
    if (argc >= 3 && std::string(argv[1]) == "--from") {
        fromCheckpoint = argv[2];
        argIdx = 3;
    }

    if (argc <= argIdx) {
        usage();
        return 1;
    }

    std::string dataset = argv[argIdx];
    int numThreads = argc > argIdx + 1 ? std::atoi(argv[argIdx + 1]) : 6;
    int maxPasses = argc > argIdx + 2 ? std::atoi(argv[argIdx + 2]) : 30;

    zobrist::init();
    // Qsearch calls movegen (isSquareAttacked, generateLegalCaptures)
    // which depends on the bitboard attack tables; evaluate()'s usual
    // lazy init can race in the tuner's multi-threaded loss loop.
    initBitboards();

    if (!fromCheckpoint.empty()) {
        auto params = collectParams();
        loadCheckpoint(fromCheckpoint, params);
        // Project + validate before any tuning starts. tune() also runs
        // these but doing them up front means a malformed checkpoint
        // never reaches the corpus load / leaf precompute stages.
        projectToConstraints(params);
        validateConstraints(params);
    }

    std::cerr << "loading " << dataset << "...\n";
    auto positions = loadDataset(dataset);
    std::cerr << "loaded " << positions.size() << " positions\n";

    precomputeLeaves(positions);

    std::cerr << "finding K...\n";
    double K = findBestK(positions, numThreads);
    std::cerr << "K=" << K << "\n";

    tune(positions, K, numThreads, maxPasses);
    printCurrentValues();
    return 0;
}
