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
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct LabeledPosition {
    Board board;
    double result;                    // 1.0 / 0.5 / 0.0 from White POV
    std::vector<uint32_t> gameIds;    // every source game that produced this row
    double weight = 1.0;              // per game inverse weight, normalised post load
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

    // --- Threats: every term here is a true bonus on attacker side, so
    // none of them should ever go negative regardless of corpus drift.
    addMgEgConstr("ThreatByPawn", &evalParams.ThreatByPawn, boundsNonNegative());
    for (int v = Rook; v <= Queen; v++)
        addMgEgConstr("ThreatByMinor[" + std::to_string(v) + "]",
                      &evalParams.ThreatByMinor[v], boundsNonNegative());
    addMgEgConstr("ThreatByRook[Queen]", &evalParams.ThreatByRook[Queen], boundsNonNegative());
    addMgEgConstr("ThreatByKing", &evalParams.ThreatByKing, boundsNonNegative());
    addMgEgConstr("Hanging", &evalParams.Hanging, boundsNonNegative());
    addMgEgConstr("WeakQueen", &evalParams.WeakQueen, boundsNonNegative());
    addMgEgConstr("SafePawnPush", &evalParams.SafePawnPush, boundsNonNegative());
    addMgEgConstr("ThreatByPawnPush", &evalParams.ThreatByPawnPush, boundsNonNegative());
    addMgEgConstr("WeakQueenDefender", &evalParams.WeakQueenDefender, boundsNonPositive());
    addMgEgConstr("KnightOnQueen", &evalParams.KnightOnQueen, boundsNonNegative());
    addMgEgConstr("PawnlessFlank", &evalParams.PawnlessFlank, boundsNonPositive());
    addMgEgConstr("QueenInfiltration", &evalParams.QueenInfiltration, boundsNonNegative());
    // Align Texel bounds with the matching SPSA spec ranges so both
    // tuners explore the same feasible region. Otherwise Texel can push
    // a parameter far past SPSA's clamp and the compiled defaults
    // silently violate the spec.
    out.push_back(
        {"KingPawnDistEg.eg", &evalParams.KingPawnDistEg, false, boundsRange(-50, 0)});
    out.push_back(
        {"KBNKCornerEg.eg", &evalParams.KBNKCornerEg, false, boundsRange(0, 40)});
    out.push_back(
        {"LucenaEg.eg", &evalParams.LucenaEg, false, boundsRange(0, 250)});
    // Mating-conversion push gradients: per-square eg weights. After
    // the pushToEdge feature was normalized to 0..3 the per-unit
    // signal got smaller, so the old 0..50 cap let Texel turn each
    // gradient into a residual sink. Caps match the tighter SPSA
    // ranges below.
    out.push_back(
        {"KXKPushToEdgeEg.eg", &evalParams.KXKPushToEdge, false, boundsRange(0, 30)});
    out.push_back(
        {"KXKPushCloseEg.eg", &evalParams.KXKPushClose, false, boundsRange(0, 25)});
    out.push_back(
        {"KBNKPushCloseEg.eg", &evalParams.KBNKPushClose, false, boundsRange(0, 25)});
    out.push_back(
        {"KQKRPushToEdgeEg.eg", &evalParams.KQKRPushToEdge, false, boundsRange(0, 30)});
    out.push_back(
        {"KQKRPushCloseEg.eg", &evalParams.KQKRPushClose, false, boundsRange(0, 15)});

    // Drawishness scales: ScaleResult multiplier in 0..64 (values
    // above 64 would amplify the eg rather than damp it). Lower
    // bound matches the SPSA range for the parameter; full-fortress
    // configurations can tune down to zero, partial-fortress shapes
    // stay at the small positive floor.
    out.push_back(
        {"KPsKFortressScaleEg.eg", &evalParams.KPsKFortressScale, false, boundsRange(0, 32)});
    out.push_back(
        {"KBPKNDrawishScaleEg.eg", &evalParams.KBPKNDrawishScale, false, boundsRange(0, 32)});
    out.push_back(
        {"KRKPDrawishScaleEg.eg", &evalParams.KRKPDrawishScale, false, boundsRange(16, 48)});
    out.push_back(
        {"KRKMinorScaleEg.eg", &evalParams.KRKMinorScale, false, boundsRange(16, 48)});
    out.push_back(
        {"KNNKDrawScaleEg.eg", &evalParams.KNNKDrawScale, false, boundsRange(0, 32)});
    out.push_back({"EscapableThreatScaleEg.eg", &evalParams.EscapableThreatScale, false,
                   boundsRange(0, 64)});

    // --- Passed pawn extras (rank 3..6 inclusive are the interesting
    // slots -- ranks 0/1/2 and 7 stay at zero).
    // Rank chains: more advanced passers carry at least as much weight
    // as less advanced ones, so support / connected / supported bonuses
    // are non-decreasing in rank, and PassedBlockedPenalty (stored
    // negative) is non-increasing in rank (more advanced block hurts
    // more). PassedEnemyKingProxPenalty is stored as a positive
    // magnitude the eval subtracts; the prior is monotone non-decreasing
    // so the storage stays consistent with "closer enemy king = bigger
    // penalty".
    for (int r = 3; r <= 6; r++) {
        // PassedKingProxBonus.eg: non-decreasing in rank, plus >= 0.
        out.push_back({"PassedKingProxBonus[" + std::to_string(r) + "].eg",
                       &evalParams.PassedKingProxBonus[r], false,
                       [r] {
                           Bounds b{0, 1000000};
                           if (r > 3)
                               b.lo = std::max(b.lo,
                                               eg_value(evalParams.PassedKingProxBonus[r - 1]));
                           if (r < 6)
                               b.hi = std::min(b.hi,
                                               eg_value(evalParams.PassedKingProxBonus[r + 1]));
                           return b;
                       }});
        // PassedEnemyKingProxPenalty.eg: stored positive, non-decreasing.
        out.push_back(
            {"PassedEnemyKingProxPenalty[" + std::to_string(r) + "].eg",
             &evalParams.PassedEnemyKingProxPenalty[r], false,
             [r] {
                 Bounds b{0, 1000000};
                 if (r > 3)
                     b.lo = std::max(b.lo,
                                     eg_value(evalParams.PassedEnemyKingProxPenalty[r - 1]));
                 if (r < 6)
                     b.hi = std::min(b.hi,
                                     eg_value(evalParams.PassedEnemyKingProxPenalty[r + 1]));
                 return b;
             }});
        // PassedBlockedPenalty: <= 0 and non-increasing in rank.
        for (bool isMg : {true, false}) {
            std::string name =
                "PassedBlockedPenalty[" + std::to_string(r) + (isMg ? "].mg" : "].eg");
            out.push_back({name, &evalParams.PassedBlockedPenalty[r], isMg, [r, isMg] {
                               Bounds b{-1000000, 0};
                               auto half = [&](int idx) {
                                   return isMg ? mg_value(evalParams.PassedBlockedPenalty[idx])
                                               : eg_value(evalParams.PassedBlockedPenalty[idx]);
                               };
                               if (r > 3) b.hi = std::min(b.hi, half(r - 1));
                               if (r < 6) b.lo = std::max(b.lo, half(r + 1));
                               return b;
                           }});
        }
        // PassedSupportedBonus: non-decreasing chain, both halves, plus
        // a non-negative floor. A passer supported by friendly pieces
        // is universally a positive feature; the chain alone permits
        // a low-rank entry to drift negative as long as the next rank
        // dominates it, which contradicts the prior.
        for (bool isMg : {true, false}) {
            std::string name =
                "PassedSupportedBonus[" + std::to_string(r) + (isMg ? "].mg" : "].eg");
            out.push_back({name, &evalParams.PassedSupportedBonus[r], isMg, [r, isMg] {
                               Bounds b{0, 1000000};
                               auto half = [&](int idx) {
                                   return isMg ? mg_value(evalParams.PassedSupportedBonus[idx])
                                               : eg_value(evalParams.PassedSupportedBonus[idx]);
                               };
                               if (r > 3) b.lo = std::max(b.lo, half(r - 1));
                               if (r < 6) b.hi = std::min(b.hi, half(r + 1));
                               return b;
                           }});
        }
        // ConnectedPassersBonus: non-decreasing chain, both halves.
        for (bool isMg : {true, false}) {
            std::string name =
                "ConnectedPassersBonus[" + std::to_string(r) + (isMg ? "].mg" : "].eg");
            out.push_back({name, &evalParams.ConnectedPassersBonus[r], isMg, [r, isMg] {
                               Bounds b;
                               auto half = [&](int idx) {
                                   return isMg ? mg_value(evalParams.ConnectedPassersBonus[idx])
                                               : eg_value(evalParams.ConnectedPassersBonus[idx]);
                               };
                               if (r > 3) b.lo = std::max(b.lo, half(r - 1));
                               if (r < 6) b.hi = std::min(b.hi, half(r + 1));
                               return b;
                           }});
        }
    }

    // Rook on the 7th: universal Tarrasch / Capablanca prior on both
    // halves. Two Texel runs settle the mg slightly negative because
    // the rook PST already credits 7th-rank squares heavily and the
    // bonus becomes a residual correction. Pinning >= 0 reallocates
    // that signal cleanly into the PST without changing the total
    // eval contribution, and stops a "Bonus"-named term from
    // representing a penalty.
    addMgEgConstr("RookOn7thBonus", &evalParams.RookOn7thBonus, boundsNonNegative());
    addMgEgConstr("BadBishop", &evalParams.BadBishop, boundsNonPositive());
    addMgEgConstr("BishopPawns", &evalParams.BishopPawns, boundsNonPositive());
    addMgEg("Tempo", &evalParams.Tempo, true, false); // mg only

    // --- Material (skip None and King; both are structurally zero) ---
    for (int pt = Pawn; pt <= Queen; pt++)
        addMgEg("PieceScore[" + std::to_string(pt) + "]", &evalParams.PieceScore[pt]);

    // --- Piece-square tables. Each PST has 64 squares; skip the back/
    // front ranks of the pawn PST because they are always zero. King
    // PST squares stay tunable because the king appears on them.
    // Stopgap range cap at +/-300 prevents corner squares from drifting
    // into implausible magnitudes during long Texel runs.
    auto addPST = [&](const std::string &name, Score *arr, int start, int end) {
        for (int sq = start; sq < end; sq++) {
            out.push_back({name + "[" + std::to_string(sq) + "].mg", &arr[sq], true,
                           boundsRange(-300, 300)});
            out.push_back({name + "[" + std::to_string(sq) + "].eg", &arr[sq], false,
                           boundsRange(-300, 300)});
        }
    };
    addPST("PawnPST", evalParams.PawnPST, 8, 56);
    addPST("KnightPST", evalParams.KnightPST, 0, 32);
    addPST("BishopPST", evalParams.BishopPST, 0, 32);
    addPST("RookPST", evalParams.RookPST, 0, 32);
    addPST("QueenPST", evalParams.QueenPST, 0, 32);
    addPST("KingPST", evalParams.KingPST, 0, 32);

    // --- Mobility bonuses (only Knight..Queen rows carry values).
    // Soft chess prior: more attacked mobility-area squares is at least
    // as good as fewer of them. Enforced as a non-decreasing chain on
    // both halves, indexed by attack count. Per-entry sign is left
    // unconstrained because the lowest counts (a knight with 0..2 moves)
    // legitimately carry a negative contribution.
    static const int mobilityCounts[7] = {0, 0, 9, 14, 15, 28, 0};
    // Per piece slope caps on the mobility chain. The non decreasing
    // prior alone lets the tuner spike a single count by 50+ cp when a
    // recurring corpus motif rewards exactly that count. Capping the
    // delta between adjacent counts keeps the curve readable as a
    // smooth gradient instead of a step function. Values are loose
    // enough that real diminishing return shifts are still expressible
    // (knight mobility goes from 0 to 9 counts over 350 cp at the
    // extreme; capping per step at 40 still permits a 360 cp span).
    static const int mobilitySlopeMax[7] = {0, 0, 40, 35, 30, 25, 0};
    for (int pt = Knight; pt <= Queen; pt++) {
        const int n = mobilityCounts[pt];
        const int maxStep = mobilitySlopeMax[pt];
        for (int i = 0; i < n; i++) {
            auto mgChain = [pt, i, n, maxStep] {
                Bounds b;
                if (i > 0) {
                    int prev = mg_value(evalParams.MobilityBonus[pt][i - 1]);
                    b.lo = std::max(b.lo, prev);
                    b.hi = std::min(b.hi, prev + maxStep);
                }
                if (i < n - 1) {
                    int next = mg_value(evalParams.MobilityBonus[pt][i + 1]);
                    b.hi = std::min(b.hi, next);
                    b.lo = std::max(b.lo, next - maxStep);
                }
                return b;
            };
            auto egChain = [pt, i, n, maxStep] {
                Bounds b;
                if (i > 0) {
                    int prev = eg_value(evalParams.MobilityBonus[pt][i - 1]);
                    b.lo = std::max(b.lo, prev);
                    b.hi = std::min(b.hi, prev + maxStep);
                }
                if (i < n - 1) {
                    int next = eg_value(evalParams.MobilityBonus[pt][i + 1]);
                    b.hi = std::min(b.hi, next);
                    b.lo = std::max(b.lo, next - maxStep);
                }
                return b;
            };
            std::string base =
                "MobilityBonus[" + std::to_string(pt) + "][" + std::to_string(i) + "]";
            out.push_back({base + ".mg", &evalParams.MobilityBonus[pt][i], true, mgChain});
            out.push_back({base + ".eg", &evalParams.MobilityBonus[pt][i], false, egChain});
        }
    }

    // --- Passed pawn base bonus and connected pawn bonus (ranks 1..6
    // carry non-zero defaults; rank 0/7 stay at zero). Both arrays
    // enforce non-decreasing chains in rank: an advanced passer is
    // never less valuable than a less advanced one. The mg defaults at
    // ranks 1..2 may be slightly negative (early-promotion exposure
    // costs), so the lo bound is the previous neighbor, not zero.
    // PassedPawnBonus: chain non-decreasing in rank. Mg ranks 4..6 are
    // floored at 0 (a passer past midfield should never be a middlegame
    // penalty). Eg ranks 1..6 stay floor-free since the eg defaults are
    // already strongly positive and the chain is enough.
    for (int r = 1; r <= 6; r++) {
        for (bool isMg : {true, false}) {
            std::string name = "PassedPawnBonus[" + std::to_string(r) + (isMg ? "].mg" : "].eg");
            out.push_back({name, &evalParams.PassedPawnBonus[r], isMg, [r, isMg] {
                               Bounds b;
                               if (isMg && r >= 4) b.lo = 0;
                               auto half = [&](int idx) {
                                   return isMg ? mg_value(evalParams.PassedPawnBonus[idx])
                                               : eg_value(evalParams.PassedPawnBonus[idx]);
                               };
                               if (r > 1) b.lo = std::max(b.lo, half(r - 1));
                               if (r < 6) b.hi = std::min(b.hi, half(r + 1));
                               return b;
                           }});
        }
    }
    // ConnectedPawnBonus: chain non-decreasing in rank. Eg ranks 2..6
    // are floored at 0 (defended/phalanx pawn from rank 2 onward is a
    // positive feature in eg). Rank 1 eg is left unconstrained because
    // the default sits negative and the corpus signal there is genuine.
    for (int r = 1; r <= 6; r++) {
        for (bool isMg : {true, false}) {
            std::string name =
                "ConnectedPawnBonus[" + std::to_string(r) + (isMg ? "].mg" : "].eg");
            out.push_back({name, &evalParams.ConnectedPawnBonus[r], isMg, [r, isMg] {
                               Bounds b;
                               if (!isMg && r >= 2) b.lo = 0;
                               auto half = [&](int idx) {
                                   return isMg ? mg_value(evalParams.ConnectedPawnBonus[idx])
                                               : eg_value(evalParams.ConnectedPawnBonus[idx]);
                               };
                               if (r > 1) b.lo = std::max(b.lo, half(r - 1));
                               if (r < 6) b.hi = std::min(b.hi, half(r + 1));
                               return b;
                           }});
        }
    }

    // --- Rook files: open file is at least as good as semi-open, both
    // non-negative. Cross-field chain implemented as bounds that read
    // the sibling Score's matching half.
    for (bool isMg : {true, false}) {
        out.push_back({isMg ? "RookOpenFileBonus.mg" : "RookOpenFileBonus.eg",
                       &evalParams.RookOpenFileBonus, isMg, [isMg] {
                           Bounds b;
                           int semi = isMg ? mg_value(evalParams.RookSemiOpenFileBonus)
                                           : eg_value(evalParams.RookSemiOpenFileBonus);
                           b.lo = std::max(b.lo, semi);
                           return b;
                       }});
        out.push_back({isMg ? "RookSemiOpenFileBonus.mg" : "RookSemiOpenFileBonus.eg",
                       &evalParams.RookSemiOpenFileBonus, isMg, [isMg] {
                           Bounds b{0, 1000000};
                           int open = isMg ? mg_value(evalParams.RookOpenFileBonus)
                                           : eg_value(evalParams.RookOpenFileBonus);
                           b.hi = std::min(b.hi, open);
                           return b;
                       }});
    }
    addMgEgConstr("RookOnQueenFile", &evalParams.RookOnQueenFile, boundsNonNegative());
    addMgEgConstr("KnightOutpostBonus", &evalParams.KnightOutpostBonus, boundsNonNegative());
    addMgEgConstr("BishopOutpostBonus", &evalParams.BishopOutpostBonus, boundsNonNegative());
    out.push_back({"TrappedRookByKingPenalty.mg", &evalParams.TrappedRookByKingPenalty, true,
                   boundsNonPositive()}); // mg only, must stay a penalty
    // Tarrasch's-rule rook-behind-passer. RookBehindOurPasserBonus is
    // a pure bonus on both halves; the eg half especially carries the
    // canonical "escort the passer to promotion" prior. For the
    // RookBehindTheirPasserBonus side, only the eg half is a strong
    // prior (rook blockading from behind an enemy passer is the
    // textbook endgame technique). The mg half is left unconstrained
    // because in middlegame, a rook stuck behind the opponent's passer
    // is often genuinely passive; the corpus' negative MG signal there
    // is plausibly real, not a prior violation.
    addMgEgConstr("RookBehindOurPasserBonus", &evalParams.RookBehindOurPasserBonus,
                  boundsNonNegative());
    addMgEg("RookBehindTheirPasserBonus", &evalParams.RookBehindTheirPasserBonus, true,
            false); // mg unconstrained
    out.push_back({"RookBehindTheirPasserBonus.eg", &evalParams.RookBehindTheirPasserBonus, false,
                   boundsNonNegative()});
    addMgEgConstr("MinorBehindPawnBonus", &evalParams.MinorBehindPawnBonus, boundsNonNegative());
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

    // --- Shelter and storm grids (mg only). Shelter is structurally a
    // middlegame concept and the eg halves stay at their compile-time
    // zero defaults. The Shelter[d][0] entry is the semi-open file
    // penalty (no own pawn on the shield file), so it is the only
    // non-positive Shelter slot; ranks 1-6 stay non-negative because
    // a pawn shield can only help. UnblockedStorm and BlockedStorm
    // are subtracted at the call site, so magnitudes stay non-negative
    // to preserve the prior that an enemy advance hurts us.
    for (int d = 0; d < 4; d++) {
        out.push_back({"Shelter[" + std::to_string(d) + "][0].mg",
                       &evalParams.Shelter[d][0], true, boundsNonPositive()});
        for (int r = 1; r < 7; r++) {
            out.push_back({"Shelter[" + std::to_string(d) + "][" + std::to_string(r) + "].mg",
                           &evalParams.Shelter[d][r], true, boundsNonNegative()});
        }
        for (int r = 1; r < 7; r++) {
            out.push_back({"UnblockedStorm[" + std::to_string(d) + "][" + std::to_string(r) +
                               "].mg",
                           &evalParams.UnblockedStorm[d][r], true, boundsNonNegative()});
        }
    }
    for (int r = 1; r < 7; r++) {
        // Blocked storm: subtracted at the call site, so the magnitude
        // stays non-negative. Left otherwise free of cross-field chain
        // priors because main's legacy BlockedPawnStorm registration
        // had no such coupling and the tuner must be free to refit
        // both halves of the storm on the new shape independently.
        out.push_back({"BlockedStorm[" + std::to_string(r) + "].mg", &evalParams.BlockedStorm[r],
                       true, boundsNonNegative()});
    }
    addMgEgConstr("UndefendedKingZoneSq", &evalParams.UndefendedKingZoneSq, boundsNonPositive());
    // KingMobilityFactor: linear weight subtracted from the king-danger
    // accumulator per safe king move. Subtracted at the call site so
    // both halves stay non-negative.
    addMgEgConstr("KingMobilityFactor", &evalParams.KingMobilityFactor, boundsNonNegative());

    // --- King-danger accumulator weights. Each per-attacker weight
    // feeds the quadratic king-danger term, so all are non-negative.
    // Cross-field piece ordering: queen attack is at least as dangerous
    // as rook, and rook is at least as dangerous as a knight. Bishops
    // are released from the chain because they deliver pressure through
    // a different geometric pattern; the prior that bishop weight must
    // sit below rook conflated supporting attackers with mainline ones.
    auto kingAttackBounds = [](Score *self, Score *atLeast, Score *atMost, bool isMg) {
        return [self, atLeast, atMost, isMg] {
            Bounds b{0, 1000000};
            (void)self;
            if (atLeast)
                b.lo = std::max(b.lo, isMg ? mg_value(*atLeast) : eg_value(*atLeast));
            if (atMost)
                b.hi = std::min(b.hi, isMg ? mg_value(*atMost) : eg_value(*atMost));
            return b;
        };
    };
    for (bool isMg : {true, false}) {
        // Knight: bounded above by Rook (preserves the heavier-attacker
        // ordering for the only non-bishop minor).
        out.push_back({isMg ? "KingAttackByKnight.mg" : "KingAttackByKnight.eg",
                       &evalParams.KingAttackByKnight, isMg,
                       kingAttackBounds(&evalParams.KingAttackByKnight, nullptr,
                                        &evalParams.KingAttackByRook, isMg)});
        // Bishop: free of the rook chain. Tuner can place bishop above
        // or below rook based on its own signal.
        out.push_back({isMg ? "KingAttackByBishop.mg" : "KingAttackByBishop.eg",
                       &evalParams.KingAttackByBishop, isMg, boundsNonNegative()});
        // Rook: at least Knight, at most Queen. Bishop is no longer a
        // floor on rook because the chain prior was wrong.
        out.push_back({isMg ? "KingAttackByRook.mg" : "KingAttackByRook.eg",
                       &evalParams.KingAttackByRook, isMg, [isMg] {
                           Bounds b{0, 1000000};
                           int knight = isMg ? mg_value(evalParams.KingAttackByKnight)
                                             : eg_value(evalParams.KingAttackByKnight);
                           int queen = isMg ? mg_value(evalParams.KingAttackByQueen)
                                            : eg_value(evalParams.KingAttackByQueen);
                           b.lo = std::max(b.lo, knight);
                           b.hi = std::min(b.hi, queen);
                           return b;
                       }});
        // Queen: at least Rook (heaviest attacker bound).
        out.push_back({isMg ? "KingAttackByQueen.mg" : "KingAttackByQueen.eg",
                       &evalParams.KingAttackByQueen, isMg,
                       kingAttackBounds(&evalParams.KingAttackByQueen,
                                        &evalParams.KingAttackByRook, nullptr, isMg)});
    }
    // KingSafeCheck: Knight stays capped above by Rook; Bishop is free
    // of the rook prior because bishop checks travel through diagonals
    // that the rook cannot deliver and vice versa. Rook stays bounded
    // by Knight below and Queen above; Queen stays bounded below by
    // Rook.
    for (bool isMg : {true, false}) {
        out.push_back({"KingSafeCheck[2]." + std::string(isMg ? "mg" : "eg"),
                       &evalParams.KingSafeCheck[2], isMg, [isMg] {
                           Bounds b{0, 1000000};
                           int rook = isMg ? mg_value(evalParams.KingSafeCheck[4])
                                           : eg_value(evalParams.KingSafeCheck[4]);
                           b.hi = std::min(b.hi, rook);
                           return b;
                       }});
        out.push_back({"KingSafeCheck[3]." + std::string(isMg ? "mg" : "eg"),
                       &evalParams.KingSafeCheck[3], isMg, boundsNonNegative()});
        out.push_back({"KingSafeCheck[4]." + std::string(isMg ? "mg" : "eg"),
                       &evalParams.KingSafeCheck[4], isMg, [isMg] {
                           Bounds b{0, 1000000};
                           int knight = isMg ? mg_value(evalParams.KingSafeCheck[2])
                                             : eg_value(evalParams.KingSafeCheck[2]);
                           int queen = isMg ? mg_value(evalParams.KingSafeCheck[5])
                                            : eg_value(evalParams.KingSafeCheck[5]);
                           b.lo = std::max(b.lo, knight);
                           b.hi = std::min(b.hi, queen);
                           return b;
                       }});
        out.push_back({"KingSafeCheck[5]." + std::string(isMg ? "mg" : "eg"),
                       &evalParams.KingSafeCheck[5], isMg, [isMg] {
                           Bounds b{0, 1000000};
                           int rook = isMg ? mg_value(evalParams.KingSafeCheck[4])
                                           : eg_value(evalParams.KingSafeCheck[4]);
                           b.lo = std::max(b.lo, rook);
                           return b;
                       }});
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
    // eval_params.h:184; only the mg half is exposed. Chain prior:
    // primary center (d4/e4 for white) is at least as valuable as
    // extended center (c4/f4) since the primary squares more directly
    // contest the centerlock. Both halves stay non-negative.
    out.push_back({"CentralPawnBonus[0].mg", &evalParams.CentralPawnBonus[0], true, [] {
                       Bounds b{0, 1000000};
                       b.lo = std::max(b.lo, mg_value(evalParams.CentralPawnBonus[1]));
                       return b;
                   }});
    out.push_back({"CentralPawnBonus[1].mg", &evalParams.CentralPawnBonus[1], true, [] {
                       Bounds b{0, 1000000};
                       b.hi = std::min(b.hi, mg_value(evalParams.CentralPawnBonus[0]));
                       return b;
                   }});

    // --- Bishop long diagonal sweep ---
    addMgEgConstr("BishopLongDiagonalBonus", &evalParams.BishopLongDiagonalBonus, boundsNonNegative());

    // --- Bishop x-ray pawns: penalty per enemy pawn on the bishop's
    // diagonals (own pieces transparent). Both halves stay <= 0.
    addMgEgConstr("BishopXrayPawns", &evalParams.BishopXrayPawns, boundsNonPositive());

    // --- Initiative system. All seven scalars carry mg=0 by construction
    // (see eval_params.h:206-208) and live entirely in the eg half. The
    // first six are positive features; InitiativeConstant is the
    // negative baseline shift.
    //
    // Per-feature upper bounds were added after evaluateVerbose probes of
    // the Scandinavian Mieses gambit (1.e4 d5 2.exd5 c6 3.dxc6 Nxc6) and
    // the Marshall recapture (1.e4 d5 2.exd5 Nf6 3.Nf3 Nxd5) showed the
    // Initiative term swinging by ~450 cp between the two positions in
    // the eg half. The dominant offender was `InitiativePawnCount * 14`
    // (14 pawns on the board) producing an egMag of ~700 cp by itself,
    // amplified by the sign-of-eg ramp into a 100-300 cp directional
    // bonus that flipped the engine's verdict on otherwise balanced
    // openings. Stockfish-lineage reference values for `popcount(allPawns)`
    // bonuses sit around 7-10 in the same magnitude space, so the cap
    // here forces Texel to reallocate the term across the other features
    // (passers, infiltration, outflank) instead of letting raw pawn
    // count dominate.
    out.push_back({"InitiativePasser.eg", &evalParams.InitiativePasser, false, boundsRange(0, 25)});
    out.push_back({"InitiativePawnCount.eg", &evalParams.InitiativePawnCount, false,
                   boundsRange(0, 12)});
    out.push_back({"InitiativeOutflank.eg", &evalParams.InitiativeOutflank, false, boundsRange(0, 4)});
    out.push_back({"InitiativeInfiltrate.eg", &evalParams.InitiativeInfiltrate, false,
                   boundsRange(0, 48)});
    // InitiativePureBase fires only in pure-pawn endgames; it is a
    // binary feature that can absorb a lot of correlation if left
    // unbounded. Cap at 48 (~2.5x the original default of 18) to keep
    // it from acting as a residual sink for the rest of the eg eval.
    out.push_back({"InitiativePureBase.eg", &evalParams.InitiativePureBase, false,
                   boundsRange(0, 48)});
    // InitiativeConstant is the negative baseline shift; force it to
    // stay strictly negative so the Initiative system does not
    // collapse the baseline into the other six features.
    out.push_back({"InitiativeConstant.eg", &evalParams.InitiativeConstant, false,
                   boundsRange(-1000000, -1)});

    // --- Slider on queen x-ray: pure bonus, both halves >= 0.
    addMgEgConstr("SliderOnQueenBishop", &evalParams.SliderOnQueenBishop, boundsNonNegative());
    addMgEgConstr("SliderOnQueenRook", &evalParams.SliderOnQueenRook, boundsNonNegative());

    // --- Restricted piece: rewards mutual attack on enemy non-pawns.
    addMgEgConstr("RestrictedPiece", &evalParams.RestrictedPiece, boundsNonNegative());

    return out;
}

// Strip leading and trailing whitespace from a string in place.
static void trim(std::string &s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
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
        // Format: "{FEN} | {result}" (legacy) or
        //         "{FEN} | {result} | {gameId,gameId,...}" (current).
        auto sep1 = line.find('|');
        if (sep1 == std::string::npos) continue;
        auto sep2 = line.find('|', sep1 + 1);

        std::string fen = line.substr(0, sep1);
        std::string result = (sep2 == std::string::npos)
                                 ? line.substr(sep1 + 1)
                                 : line.substr(sep1 + 1, sep2 - sep1 - 1);
        std::string gameIdsField =
            (sep2 == std::string::npos) ? std::string() : line.substr(sep2 + 1);
        trim(fen);
        trim(result);
        trim(gameIdsField);

        LabeledPosition lp;
        lp.board.setFen(fen);
        lp.result = std::stod(result);
        if (!gameIdsField.empty()) {
            size_t pos = 0;
            while (pos < gameIdsField.size()) {
                size_t comma = gameIdsField.find(',', pos);
                std::string token = (comma == std::string::npos)
                                        ? gameIdsField.substr(pos)
                                        : gameIdsField.substr(pos, comma - pos);
                if (!token.empty()) {
                    lp.gameIds.push_back(static_cast<uint32_t>(std::stoul(token)));
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        out.push_back(std::move(lp));
    }
    return out;
}

// Compute per game inverse weights for every position. A position
// extracted from games {a, b, c} where game a contributed 80
// positions, game b 40, and game c 60 receives weight
// 1/80 + 1/40 + 1/60. After all per position weights are populated
// the corpus is renormalised so the average weight is 1.0, which
// keeps absolute loss numbers comparable to the unweighted
// pipeline (so K refit and the relative-improvement gate stay
// calibrated).
static void assignGameWeights(std::vector<LabeledPosition> &positions) {
    std::unordered_map<uint32_t, uint64_t> gameSizes;
    for (const auto &lp : positions) {
        for (uint32_t gid : lp.gameIds) {
            gameSizes[gid] += 1;
        }
    }
    if (gameSizes.empty()) {
        // Legacy corpus with no game id metadata: every position
        // already has weight 1.0 from default-construction; no
        // renormalisation needed.
        return;
    }
    double totalWeight = 0.0;
    for (auto &lp : positions) {
        if (lp.gameIds.empty()) {
            lp.weight = 1.0;
        } else {
            double w = 0.0;
            for (uint32_t gid : lp.gameIds) {
                auto it = gameSizes.find(gid);
                if (it != gameSizes.end() && it->second > 0) {
                    w += 1.0 / static_cast<double>(it->second);
                }
            }
            lp.weight = w;
        }
        totalWeight += lp.weight;
    }
    if (totalWeight <= 0.0) return;
    double scale = static_cast<double>(positions.size()) / totalWeight;
    for (auto &lp : positions) {
        lp.weight *= scale;
    }
    std::cerr << "assignGameWeights: " << gameSizes.size() << " games, "
              << positions.size() << " positions, mean weight 1.000\n";
}

// Stable splitmix64-flavoured hash; used to bucket game ids into the
// train / val partitions deterministically. Mixing the seed lets the
// operator try multiple splits without changing the corpus on disk.
static uint64_t splitHash(uint32_t key, uint64_t seed) {
    uint64_t x = static_cast<uint64_t>(key) ^ seed;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return x;
}

// Partition the corpus into a training slice and a held-out validation
// slice using stratified sampling over (phase, result) buckets.
//
// Why stratified: a flat random-by-game split holds out approximately
// `valFraction` of every position type *in expectation*, but the
// realised bucket coverage varies. Bucketed val reporting then sees
// noisy per-cell loss numbers driven by sample-size variance instead
// of real signal differences, and rare buckets (e.g. opposite-colour
// bishop endgames, KQK with a passed pawn) can land severely
// under-covered.
//
// Algorithm: classify every position into nine cells = three phases
// (mgPhase >= 18 / 8..17 / < 8) crossed with three result classes
// (win 1.0 / draw 0.5 / loss 0.0), matching the reportValidation
// bucket layout exactly so the per-cell coverage we engineer here
// shows up directly in that reporter's output. Then for each cell,
// hash the games contributing to it (game id XOR cell-salt), sort
// deterministically, and pull games into the val partition until the
// cell's coverage hits `valFraction * cell_size`. Crucially, each
// game's full position contribution lands in val regardless of which
// cell selected it -- a game can only be wholly val or wholly train,
// preserving the no-spans-both-partitions invariant. So most cells
// reach their target organically from games already chosen for
// larger neighbouring cells.
//
// Legacy corpora without game-id metadata fall back to the previous
// per-position-index split. valFraction edge cases preserved:
//   * 0.0: every position goes to train (val empty, val gate disabled).
//   * 1.0: every position goes to val (only useful for diagnostics).
static std::pair<std::vector<size_t>, std::vector<size_t>>
splitCorpus(const std::vector<LabeledPosition> &positions, double valFraction,
            uint64_t seed) {
    std::vector<size_t> train;
    std::vector<size_t> val;
    train.reserve(positions.size());
    if (valFraction <= 0.0) {
        for (size_t i = 0; i < positions.size(); i++)
            train.push_back(i);
        std::cerr << "splitCorpus: " << train.size() << " train, 0 val (val gate off)\n";
        return {std::move(train), std::move(val)};
    }
    if (valFraction >= 1.0) {
        for (size_t i = 0; i < positions.size(); i++)
            val.push_back(i);
        std::cerr << "splitCorpus: 0 train, " << val.size()
                  << " val (training disabled, diagnostics only)\n";
        return {std::move(train), std::move(val)};
    }

    // Inline phase / result bucketing so this function stays
    // self-contained at the file's ordering. Must match
    // reportValidation's classifications exactly.
    static const int phaseInc[7] = {0, 0, 1, 1, 2, 4, 0};
    auto phaseBucketOf = [&](const Board &b) -> int {
        int p = 0;
        for (int pt = Knight; pt <= Queen; pt++) {
            p += phaseInc[pt] * (b.pieceCount[White][pt] + b.pieceCount[Black][pt]);
        }
        if (p > 24) p = 24;
        return p >= 18 ? 0 : (p >= 8 ? 1 : 2);
    };
    auto resultBucketOf = [](double r) -> int {
        if (r > 0.75) return 0;  // win
        if (r < 0.25) return 2;  // loss
        return 1;                // draw
    };

    constexpr int N_BUCKETS = 9;
    static const char *bucketLabels[N_BUCKETS] = {
        "open-win",  "open-draw", "open-loss",
        "mid-win",   "mid-draw",  "mid-loss",
        "end-win",   "end-draw",  "end-loss",
    };

    // First pass: assign each position to a (phase, result) cell, count
    // cell sizes, and detect whether any game-id metadata exists.
    std::vector<uint8_t> posBucket(positions.size());
    std::array<size_t, N_BUCKETS> bucketSize{};
    bool anyMetadata = false;
    for (size_t i = 0; i < positions.size(); i++) {
        const auto &lp = positions[i];
        int b = phaseBucketOf(lp.board) * 3 + resultBucketOf(lp.result);
        posBucket[i] = static_cast<uint8_t>(b);
        bucketSize[b]++;
        if (!lp.gameIds.empty()) anyMetadata = true;
    }

    // Legacy fallback: corpus has no game-id metadata, so we cannot
    // honour the no-game-spans-partitions invariant. Fall back to a
    // deterministic per-position-index hash split.
    if (!anyMetadata) {
        constexpr uint64_t bucketScale = 1ULL << 16;
        uint64_t valCutoff = static_cast<uint64_t>(valFraction * bucketScale);
        for (size_t i = 0; i < positions.size(); i++) {
            uint64_t h = splitHash(static_cast<uint32_t>(i), seed);
            if ((h % bucketScale) < valCutoff) val.push_back(i);
            else train.push_back(i);
        }
        std::cerr << "splitCorpus: " << train.size() << " train, " << val.size()
                  << " val (by position index, no metadata, target val fraction "
                  << valFraction << ")\n";
        return {std::move(train), std::move(val)};
    }

    // Per-game contribution table: gameContrib[gid][b] is the count of
    // positions from game `gid` that fell into cell `b`.
    std::unordered_map<uint32_t, std::array<uint32_t, N_BUCKETS>> gameContrib;
    for (size_t i = 0; i < positions.size(); i++) {
        if (positions[i].gameIds.empty()) continue;
        uint32_t gid = positions[i].gameIds.front();
        gameContrib[gid][posBucket[i]]++;
    }

    // Greedy stratified game selection. Each cell salts the hash with
    // its bucket index so neighbouring cells walk games in different
    // orders, but every game's val/train decision is final on first
    // selection -- a game already chosen by an earlier cell is skipped
    // for later cells (its contribution still counts toward those
    // cells' coverage thanks to the running tally).
    std::unordered_set<uint32_t> valGames;
    std::array<uint32_t, N_BUCKETS> bucketCoverage{};
    for (int b = 0; b < N_BUCKETS; b++) {
        if (bucketSize[b] == 0) continue;
        uint64_t target = static_cast<uint64_t>(bucketSize[b] * valFraction);
        if (target == 0 || bucketCoverage[b] >= target) continue;

        std::vector<std::pair<uint64_t, uint32_t>> sortedGames;
        sortedGames.reserve(gameContrib.size());
        for (const auto &kv : gameContrib) {
            if (kv.second[b] > 0) {
                uint64_t h = splitHash(kv.first, seed ^ static_cast<uint64_t>(b + 1));
                sortedGames.emplace_back(h, kv.first);
            }
        }
        std::sort(sortedGames.begin(), sortedGames.end());

        for (const auto &entry : sortedGames) {
            uint32_t gid = entry.second;
            if (valGames.count(gid)) continue;
            valGames.insert(gid);
            const auto &c = gameContrib[gid];
            for (int bb = 0; bb < N_BUCKETS; bb++) {
                bucketCoverage[bb] += c[bb];
            }
            if (bucketCoverage[b] >= target) break;
        }
    }

    // Final partition: walk the corpus and route each position based
    // on whether its first game id was selected for val. Positions
    // without metadata in an otherwise-tagged corpus default to
    // training so they cannot dilute the held-out slice.
    for (size_t i = 0; i < positions.size(); i++) {
        const auto &lp = positions[i];
        if (!lp.gameIds.empty() && valGames.count(lp.gameIds.front())) {
            val.push_back(i);
        } else {
            train.push_back(i);
        }
    }

    std::cerr << "splitCorpus (stratified by phase x result): " << train.size()
              << " train, " << val.size() << " val (target " << valFraction
              << ", " << valGames.size() << "/" << gameContrib.size()
              << " games held out)\n";
    for (int b = 0; b < N_BUCKETS; b++) {
        if (bucketSize[b] == 0) continue;
        double cov = 100.0 * static_cast<double>(bucketCoverage[b]) /
                     static_cast<double>(bucketSize[b]);
        std::cerr << "  " << bucketLabels[b] << ": " << bucketCoverage[b]
                  << "/" << bucketSize[b] << " val (" << cov << "%)\n";
    }
    return {std::move(train), std::move(val)};
}

static double sigmoid(double x, double K) {
    return 1.0 / (1.0 + std::exp(-K * x));
}

// Per piece smoothness weights for the PST L2 regulariser. The pawn
// PST is full board (file structure matters) and the king eg is the
// term that most often picks up corpus motifs (corners drift away
// from the center on rare endgames), so those two carry the heaviest
// weights. Knight values are second priority because the corner /
// edge cells typically capture rare-attack motifs. Queen smoothness
// is intentionally lighter because the queen's tactical reach
// genuinely produces sharp local jumps. Index by piece type.
static const double PstSmoothMg[7] = {0.0, 1.0, 0.6, 0.5, 0.4, 0.3, 0.4};
static const double PstSmoothEg[7] = {0.0, 1.0, 0.6, 0.5, 0.4, 0.3, 1.0};

// Read tunable env vars once on program start so subsequent loss
// evaluations do not pay the getenv / strtod cost.
//
// The PST smoothness penalty sums squared adjacent-cell differences
// over every PST. With typical 50 cp adjacent diffs that sum lands
// around 2.5e2 to 3e2 cp^2 even on a tuned snapshot, while the data
// MSE on a Texel corpus sits around 5e-2 to 1e-1. Default lambda
// 1e-9 puts the regulariser at a few percent of the data loss; the
// previous 1e-4 default was wildly over-scaled and let the regulariser
// dominate every accept gate. PST_SMOOTH_LAMBDA in the env still
// dials it in or out without rebuilding when the operator wants
// stronger smoothness pressure.
static double pstSmoothLambda() {
    static const double cached = [] {
        const char *env = std::getenv("PST_SMOOTH_LAMBDA");
        if (!env) return 1e-9;
        char *endp = nullptr;
        double v = std::strtod(env, &endp);
        return (endp == env) ? 1e-9 : v;
    }();
    return cached;
}

// Pawn PST mirror prior magnitude. Non-pawn PSTs are stored half
// board so they are mirror-identical by construction, but the pawn
// PST is full board because pawn structure is genuinely asymmetric
// (g and h pawns play differently from a and b pawns once castling
// directions matter). A soft mirror prior caps how far the tuner
// can drift the two halves apart from corpus signal alone.
//
// Mirror penalty is naturally smaller than the smoothness penalty
// because there are fewer pairs (24 vs hundreds), so a slightly
// looser default (1e-8) keeps the prior at a similar fraction of
// the data loss. Previous default 1e-4 was over-scaled.
static double pawnMirrorLambda() {
    static const double cached = [] {
        const char *env = std::getenv("PAWN_MIRROR_LAMBDA");
        if (!env) return 1e-8;
        char *endp = nullptr;
        double v = std::strtod(env, &endp);
        return (endp == env) ? 1e-8 : v;
    }();
    return cached;
}

// Sum of squared file-mirror differences across the pawn PST. Skips
// the back ranks (forced zero). Each pair contributes once.
static double pawnMirrorPenalty() {
    double total = 0.0;
    auto sqr = [](double x) { return x * x; };
    for (int rank = 1; rank <= 6; rank++) {
        for (int file = 0; file < 4; file++) {
            int sq = rank * 8 + file;
            int mirror = rank * 8 + (7 - file);
            total += sqr(mg_value(evalParams.PawnPST[sq]) -
                         mg_value(evalParams.PawnPST[mirror]));
            total += sqr(eg_value(evalParams.PawnPST[sq]) -
                         eg_value(evalParams.PawnPST[mirror]));
        }
    }
    return total;
}

// Sum of squared differences between adjacent squares in every PST,
// scaled by per piece weights. Adjacency for the pawn 64-entry PST
// is the standard 8x8 grid (rank +/- 1 same file, or file +/- 1 same
// rank). Adjacency for the 32-entry half-board PSTs is over (rank,
// fileIdx) where fileIdx in [0..3]. Only the rank 2..7 region of the
// pawn PST is walked because the back ranks are forced to zero.
static double pstSmoothnessPenalty() {
    double total = 0.0;

    auto sqr = [](double x) { return x * x; };

    // Pawn full-board (skip rank 0 and rank 7; both are forced zero).
    for (int rank = 1; rank <= 6; rank++) {
        for (int file = 0; file < 8; file++) {
            int sq = rank * 8 + file;
            if (file < 7) {
                int neighbour = sq + 1;
                int rankNeighbour = neighbour / 8;
                if (rankNeighbour >= 1 && rankNeighbour <= 6) {
                    total += PstSmoothMg[Pawn] *
                             sqr(mg_value(evalParams.PawnPST[sq]) -
                                 mg_value(evalParams.PawnPST[neighbour]));
                    total += PstSmoothEg[Pawn] *
                             sqr(eg_value(evalParams.PawnPST[sq]) -
                                 eg_value(evalParams.PawnPST[neighbour]));
                }
            }
            if (rank < 6) {
                int neighbour = sq + 8;
                total += PstSmoothMg[Pawn] *
                         sqr(mg_value(evalParams.PawnPST[sq]) -
                             mg_value(evalParams.PawnPST[neighbour]));
                total += PstSmoothEg[Pawn] *
                         sqr(eg_value(evalParams.PawnPST[sq]) -
                             eg_value(evalParams.PawnPST[neighbour]));
            }
        }
    }

    // Non-pawn half-board PSTs: index = (rank << 2) | fileIdx where
    // fileIdx in [0..3]. Adjacency over the half-board grid.
    auto walkHalfBoard = [&](const Score *pst, int pieceType) {
        const double wmg = PstSmoothMg[pieceType];
        const double weg = PstSmoothEg[pieceType];
        for (int rank = 0; rank < 8; rank++) {
            for (int fIdx = 0; fIdx < 4; fIdx++) {
                int idx = (rank << 2) | fIdx;
                if (fIdx < 3) {
                    int neighbour = idx + 1;
                    total += wmg * sqr(mg_value(pst[idx]) - mg_value(pst[neighbour]));
                    total += weg * sqr(eg_value(pst[idx]) - eg_value(pst[neighbour]));
                }
                if (rank < 7) {
                    int neighbour = idx + 4;
                    total += wmg * sqr(mg_value(pst[idx]) - mg_value(pst[neighbour]));
                    total += weg * sqr(eg_value(pst[idx]) - eg_value(pst[neighbour]));
                }
            }
        }
    };
    walkHalfBoard(evalParams.KnightPST, Knight);
    walkHalfBoard(evalParams.BishopPST, Bishop);
    walkHalfBoard(evalParams.RookPST, Rook);
    walkHalfBoard(evalParams.QueenPST, Queen);
    walkHalfBoard(evalParams.KingPST, King);

    return total;
}

// Pure data MSE. The inner loop walks an explicit index list when one
// is supplied so the train and val partitions can each take their own
// loss without touching the other half of the corpus. Positions here
// already hold qsearch-resolved leaf boards, so we only need a static
// evaluate(). The pawn and material hashes are thread_local: each
// std::thread spawned below gets a fresh empty table on first use and
// destroys it on exit, so no clearing is needed and no two threads
// ever touch the same hash entry. That removes the race that
// previously sat under our 1e-8 acceptance threshold.
static double computeDataLoss(const std::vector<LabeledPosition> &positions, double K,
                              int numThreads,
                              const std::vector<size_t> *indices = nullptr) {
    size_t n = indices ? indices->size() : positions.size();
    if (n == 0) return 0.0;
    std::vector<double> partial(numThreads, 0.0);
    std::vector<double> partialWeight(numThreads, 0.0);
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int t = 0; t < numThreads; t++) {
        size_t start = (n * t) / numThreads;
        size_t end = (n * (t + 1)) / numThreads;
        threads.emplace_back([&, start, end, t]() {
            double sum = 0.0;
            double sumWeight = 0.0;
            for (size_t i = start; i < end; i++) {
                size_t pi = indices ? (*indices)[i] : i;
                Board board = positions[pi].board;
                int raw = evaluate(board);
                // evaluate returns side-to-move relative; convert to
                // White POV so the tuner can compare to the White-POV
                // game result. The leaf's side-to-move may differ from
                // the root if an odd number of captures were resolved
                // during qsearch.
                if (board.sideToMove == Black) raw = -raw;
                double pred = sigmoid(static_cast<double>(raw), K);
                double err = pred - positions[pi].result;
                double w = positions[pi].weight;
                sum += w * err * err;
                sumWeight += w;
            }
            partial[t] = sum;
            partialWeight[t] = sumWeight;
        });
    }
    for (auto &th : threads)
        th.join();
    double total = 0.0;
    double totalWeight = 0.0;
    for (int t = 0; t < numThreads; t++) {
        total += partial[t];
        totalWeight += partialWeight[t];
    }
    // assignGameWeights renormalises the corpus so the sum of weights
    // equals position count, but legacy corpora and edge cases may
    // leave totalWeight at zero; fall back to position count.
    return totalWeight > 0.0 ? total / totalWeight : total / static_cast<double>(n);
}

// Total loss including the parameter-only PST smoothness and pawn
// mirror priors. Used for the training-side accept gate so that a step
// which makes the data loss slightly worse but reduces an outsize
// regulariser by enough to net out is still allowed -- the priors are
// designed to shape parameters, not just measure them.
//
// PST_SMOOTH_LAMBDA / PAWN_MIRROR_LAMBDA env vars control the prior
// strength without rebuilding. The regularisers are constant over the
// corpus (parameter-only) so they cancel out of any indices-based
// difference; we still include them so the training loss stays in the
// same units regardless of which slice it is computed on.
static double computeLoss(const std::vector<LabeledPosition> &positions, double K,
                          int numThreads,
                          const std::vector<size_t> *indices = nullptr) {
    double dataLoss = computeDataLoss(positions, K, numThreads, indices);
    double regLoss = pstSmoothLambda() * pstSmoothnessPenalty();
    regLoss += pawnMirrorLambda() * pawnMirrorPenalty();
    return dataLoss + regLoss;
}

// Phase increments mirror the engine's `GamePhaseInc` (see eval.cpp)
// so the reporter buckets val positions by the same opening / middle /
// endgame definition the search uses.
static const int TunerPhaseInc[7] = {0, 0, 1, 1, 2, 4, 0};

// Compute the same `mgPhase` value the engine clamps to [0, 24]. Used
// purely for val bucketing; the engine still does its own phase work
// during evaluate().
static int phaseFromBoard(const Board &board) {
    int phase = 0;
    for (int pt = Knight; pt <= Queen; pt++) {
        phase += TunerPhaseInc[pt] * (board.pieceCount[White][pt] + board.pieceCount[Black][pt]);
    }
    return phase > 24 ? 24 : phase;
}

// Signed material delta from White's POV in pawn units. Used as a
// rough material-imbalance bucket for the val reporter (sign tells us
// who's ahead; magnitude buckets balanced / one-pawn / one-minor /
// rook-or-better).
static int materialDeltaCpFromBoard(const Board &board) {
    static const int valueCp[7] = {0, 100, 320, 320, 500, 900, 0};
    int delta = 0;
    for (int pt = Pawn; pt <= Queen; pt++) {
        delta += valueCp[pt] * (board.pieceCount[White][pt] - board.pieceCount[Black][pt]);
    }
    return delta;
}

// One bucket of (weight, weighted MSE numerator, count). Buckets merge
// over threads by simple field-wise add.
struct BucketStat {
    double sumWeight = 0.0;
    double sumWErr2 = 0.0;
    size_t count = 0;
    void operator+=(const BucketStat &o) {
        sumWeight += o.sumWeight;
        sumWErr2 += o.sumWErr2;
        count += o.count;
    }
    double meanLoss() const { return sumWeight > 0.0 ? sumWErr2 / sumWeight : 0.0; }
};

// Categorical bucket buckets for the val reporter. Indices are kept
// stable so the log lines stay machine-greppable.
namespace bucket {
constexpr int PhaseOpen = 0; // mgPhase >= 18
constexpr int PhaseMid = 1;  // 8 <= mgPhase < 18
constexpr int PhaseEnd = 2;  // mgPhase < 8
constexpr int PhaseN = 3;

constexpr int ResultWin = 0;  // 1.0
constexpr int ResultDraw = 1; // 0.5
constexpr int ResultLoss = 2; // 0.0
constexpr int ResultN = 3;

// Material delta from White's POV in cp:
//   < -300  : down a minor or worse
//   [-300, -50] : down a pawn-ish (pawn or fragmentary)
//   [-50, 50]   : balanced (within +/- half a pawn)
//   [50, 300]   : up a pawn-ish
//   > 300       : up a minor or better
constexpr int MatDownMinor = 0;
constexpr int MatDownPawn = 1;
constexpr int MatBalanced = 2;
constexpr int MatUpPawn = 3;
constexpr int MatUpMinor = 4;
constexpr int MatN = 5;

// Eval magnitude buckets in cp (post side-to-move flip, White POV):
//   <= -300, (-300, 0], (0, 300], > 300
constexpr int EvalDeep = 0;     // <= -300
constexpr int EvalNegSmall = 1; // (-300, 0]
constexpr int EvalPosSmall = 2; // (0, 300]
constexpr int EvalDeepPos = 3;  // > 300
constexpr int EvalN = 4;
} // namespace bucket

// Per-pass bucketed val reporter. Walks the val partition once,
// classifying each position by phase, result, signed material delta,
// and signed eval magnitude. Output goes to std::cerr in a stable
// machine-greppable layout so the log can be diffed pass over pass.
//
// Uses the same threaded loop pattern as computeDataLoss so per-pass
// overhead scales with thread count. Heavy lambda body but the loop
// is bound by static evaluate() so the wins are linear.
//
// Returns the overall val loss (data MSE on the held-out slice). The
// val gate uses this number directly, so the bucketed walk doubles
// as the gate's val measurement -- one pass over the slice instead
// of two.
static double reportValidation(const std::vector<LabeledPosition> &positions,
                               const std::vector<size_t> &valIndices, double K, int numThreads,
                               double trainLoss, int globalPass, const std::string &tag) {
    if (valIndices.empty()) return 0.0;
    const size_t n = valIndices.size();
    struct ThreadAcc {
        std::array<BucketStat, bucket::PhaseN> phase{};
        std::array<BucketStat, bucket::ResultN> result{};
        std::array<BucketStat, bucket::MatN> material{};
        std::array<BucketStat, bucket::EvalN> evalMag{};
        BucketStat overall{};
    };
    std::vector<ThreadAcc> tacc(numThreads);
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int t = 0; t < numThreads; t++) {
        size_t start = (n * t) / numThreads;
        size_t end = (n * (t + 1)) / numThreads;
        threads.emplace_back([&, start, end, t]() {
            auto &acc = tacc[t];
            for (size_t i = start; i < end; i++) {
                size_t pi = valIndices[i];
                const auto &lp = positions[pi];
                Board board = lp.board;
                int raw = evaluate(board);
                if (board.sideToMove == Black) raw = -raw;
                double pred = sigmoid(static_cast<double>(raw), K);
                double err = pred - lp.result;
                double w = lp.weight;
                double w_err2 = w * err * err;
                BucketStat sample{w, w_err2, 1};
                acc.overall += sample;

                int phase = phaseFromBoard(board);
                int phaseIdx;
                if (phase >= 18) phaseIdx = bucket::PhaseOpen;
                else if (phase >= 8) phaseIdx = bucket::PhaseMid;
                else phaseIdx = bucket::PhaseEnd;
                acc.phase[phaseIdx] += sample;

                int resultIdx;
                if (lp.result > 0.75) resultIdx = bucket::ResultWin;
                else if (lp.result < 0.25) resultIdx = bucket::ResultLoss;
                else resultIdx = bucket::ResultDraw;
                acc.result[resultIdx] += sample;

                int matDelta = materialDeltaCpFromBoard(board);
                int matIdx;
                if (matDelta < -300) matIdx = bucket::MatDownMinor;
                else if (matDelta < -50) matIdx = bucket::MatDownPawn;
                else if (matDelta <= 50) matIdx = bucket::MatBalanced;
                else if (matDelta <= 300) matIdx = bucket::MatUpPawn;
                else matIdx = bucket::MatUpMinor;
                acc.material[matIdx] += sample;

                int evalIdx;
                if (raw <= -300) evalIdx = bucket::EvalDeep;
                else if (raw <= 0) evalIdx = bucket::EvalNegSmall;
                else if (raw <= 300) evalIdx = bucket::EvalPosSmall;
                else evalIdx = bucket::EvalDeepPos;
                acc.evalMag[evalIdx] += sample;
            }
        });
    }
    for (auto &th : threads)
        th.join();

    ThreadAcc total;
    for (int t = 0; t < numThreads; t++) {
        for (int i = 0; i < bucket::PhaseN; i++) total.phase[i] += tacc[t].phase[i];
        for (int i = 0; i < bucket::ResultN; i++) total.result[i] += tacc[t].result[i];
        for (int i = 0; i < bucket::MatN; i++) total.material[i] += tacc[t].material[i];
        for (int i = 0; i < bucket::EvalN; i++) total.evalMag[i] += tacc[t].evalMag[i];
        total.overall += tacc[t].overall;
    }

    auto fmt = [](const BucketStat &b) {
        std::ostringstream os;
        os << b.meanLoss() << "(" << b.count << ")";
        return os.str();
    };

    std::cerr << "  " << tag << " pass " << globalPass << " train_loss=" << trainLoss
              << " val_loss=" << total.overall.meanLoss() << "\n";
    std::cerr << "    by_phase open=" << fmt(total.phase[bucket::PhaseOpen])
              << " mid=" << fmt(total.phase[bucket::PhaseMid])
              << " end=" << fmt(total.phase[bucket::PhaseEnd]) << "\n";
    std::cerr << "    by_result win=" << fmt(total.result[bucket::ResultWin])
              << " draw=" << fmt(total.result[bucket::ResultDraw])
              << " loss=" << fmt(total.result[bucket::ResultLoss]) << "\n";
    std::cerr << "    by_material down_minor=" << fmt(total.material[bucket::MatDownMinor])
              << " down_pawn=" << fmt(total.material[bucket::MatDownPawn])
              << " balanced=" << fmt(total.material[bucket::MatBalanced])
              << " up_pawn=" << fmt(total.material[bucket::MatUpPawn])
              << " up_minor=" << fmt(total.material[bucket::MatUpMinor]) << "\n";
    std::cerr << "    by_eval [<=-300]=" << fmt(total.evalMag[bucket::EvalDeep])
              << " [-300,0]=" << fmt(total.evalMag[bucket::EvalNegSmall])
              << " [0,300]=" << fmt(total.evalMag[bucket::EvalPosSmall])
              << " [>300]=" << fmt(total.evalMag[bucket::EvalDeepPos]) << "\n";
    return total.overall.meanLoss();
}

// Replace each training position's root board with its quiet leaf so
// the loss loop can use a fast static evaluate() call. Two modes:
//
//   leafDepth = 0 : pure qsearch leaf (existing default). Handles
//                   capture chains but cannot see one-ply tactics.
//   leafDepth > 0 : run a fixed-depth alpha-beta search and follow
//                   the PV to its terminal, then qsearch from there.
//                   Resolves more tactical noise (Andrew-Grant style
//                   PV-terminal corpus) at the cost of a noticeably
//                   longer precompute -- a depth-6 walk over 5M
//                   positions on 14 threads is several minutes on
//                   top of the qsearch-only baseline.
//
// Both modes route TT traffic through a per-thread leaf TT inside the
// leaf function (a thread-local override of the engine's main TT) so the
// outer worker pool can run concurrently without contention and without
// disturbing the persistent UCI-search TT.
static void precomputeLeaves(std::vector<LabeledPosition> &positions, int numThreads,
                             int leafDepth = 0) {
    if (leafDepth > 0) {
        std::cerr << "precomputing PV-terminal leaves (depth=" << leafDepth << ") for "
                  << positions.size() << " positions on " << numThreads << " threads...\n";
    } else {
        std::cerr << "precomputing qsearch leaves for " << positions.size() << " positions on "
                  << numThreads << " threads...\n";
    }
    resetQsearchLeafCounters();

    // Workers share an atomic cursor so any uneven per-position cost
    // (some leaves walk a long capture chain, others stand-pat at the
    // root) load-balances naturally. Each worker routes through a
    // per-thread leaf TT inside qsearchLeafBoard / pvLeafBoard, so there
    // is no contention on the table itself.
    std::atomic<size_t> nextIndex{0};
    std::atomic<size_t> nextReport{50000};
    const size_t total = positions.size();

    auto worker = [&]() {
        while (true) {
            size_t i = nextIndex.fetch_add(1, std::memory_order_relaxed);
            if (i >= total) break;
            positions[i].board = leafDepth > 0
                                     ? pvLeafBoard(positions[i].board, leafDepth)
                                     : qsearchLeafBoard(positions[i].board);
            size_t threshold = nextReport.load(std::memory_order_relaxed);
            if (i + 1 >= threshold) {
                if (nextReport.compare_exchange_strong(threshold, threshold + 50000)) {
                    std::cerr << "  " << (i + 1) << " leaves computed\n";
                }
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(numThreads);
    for (int t = 0; t < numThreads; t++)
        workers.emplace_back(worker);
    for (auto &th : workers)
        th.join();

    auto stats = qsearchLeafCounters();
    std::cerr << "leaf stats: " << stats.total << " leaves, " << stats.inCheck
              << " returned in check, " << stats.ttMiss << " TT-miss exits, "
              << stats.cappedIterations << " hit iteration cap\n";
}

static double findBestK(const std::vector<LabeledPosition> &positions, int numThreads,
                        const std::vector<size_t> *indices = nullptr) {
    // Golden-section-style bracket search. Internal scale is ~228 per
    // pawn, so K in the range [0.0005, 0.01] covers the plausible span
    // of Texel scaling constants. When `indices` is non-null the fit
    // runs on the training partition only so the val slice never
    // contributes to the sigmoid scale.
    double lo = 0.0005, hi = 0.02;
    for (int iter = 0; iter < 40; iter++) {
        double m1 = lo + (hi - lo) / 3.0;
        double m2 = hi - (hi - lo) / 3.0;
        double l1 = computeLoss(positions, m1, numThreads, indices);
        double l2 = computeLoss(positions, m2, numThreads, indices);
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

// Canonicalize the PST/material gauge. The eval is invariant under
// shifts that subtract a constant `mean` from every PST entry of a
// piece type while adding the same `mean` (times piece count) back via
// PieceScore. Coordinate descent has been freely exploring this null
// direction, leaving material values that drift far from defaults
// while the actual eval stays the same. Recentering each PST around
// zero by mean and pushing the mean into PieceScore keeps the eval
// bit-identical and pulls material values back to interpretable
// magnitudes. The pawn PST excludes ranks 1 and 8 (forced zero
// squares); the king PST is centered without touching PieceScore[King]
// because there is exactly one king per side, so any constant shift
// in the king PST cancels in the white-minus-black eval. Run after
// every accepted pass so the gauge never wanders far between
// snapshots.
static void centerPSTGauge() {
    auto centerWithMaterial = [](Score *pst, int firstSq, int lastSq, Score &pieceScore) {
        int n = lastSq - firstSq;
        int sumMg = 0, sumEg = 0;
        for (int sq = firstSq; sq < lastSq; sq++) {
            sumMg += mg_value(pst[sq]);
            sumEg += eg_value(pst[sq]);
        }
        int meanMg = sumMg / n;
        int meanEg = sumEg / n;
        if (meanMg == 0 && meanEg == 0) return;
        for (int sq = firstSq; sq < lastSq; sq++) {
            pst[sq] = S(mg_value(pst[sq]) - meanMg, eg_value(pst[sq]) - meanEg);
        }
        pieceScore = S(mg_value(pieceScore) + meanMg, eg_value(pieceScore) + meanEg);
    };
    auto centerNoMaterial = [](Score *pst, int firstSq, int lastSq) {
        int n = lastSq - firstSq;
        int sumMg = 0, sumEg = 0;
        for (int sq = firstSq; sq < lastSq; sq++) {
            sumMg += mg_value(pst[sq]);
            sumEg += eg_value(pst[sq]);
        }
        int meanMg = sumMg / n;
        int meanEg = sumEg / n;
        if (meanMg == 0 && meanEg == 0) return;
        for (int sq = firstSq; sq < lastSq; sq++) {
            pst[sq] = S(mg_value(pst[sq]) - meanMg, eg_value(pst[sq]) - meanEg);
        }
    };

    centerWithMaterial(evalParams.PawnPST, 8, 56, evalParams.PieceScore[Pawn]);
    centerWithMaterial(evalParams.KnightPST, 0, 32, evalParams.PieceScore[Knight]);
    centerWithMaterial(evalParams.BishopPST, 0, 32, evalParams.PieceScore[Bishop]);
    centerWithMaterial(evalParams.RookPST, 0, 32, evalParams.PieceScore[Rook]);
    centerWithMaterial(evalParams.QueenPST, 0, 32, evalParams.PieceScore[Queen]);
    centerNoMaterial(evalParams.KingPST, 0, 32);
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
    // Canonicalize the PST / material gauge so the written checkpoint
    // is in the same canonical form `tune()` produces between passes.
    centerPSTGauge();

    writeCheckpoint(outPath, params);
    std::cerr << "checkpoint written to " << outPath << "\n";
}

// Sparse feature representation. Each position carries a list of
// non-zero per-parameter linearization coefficients. uint16 fits the
// 778-parameter index space; int16 fits every post-taper finite-
// difference delta we have observed (max magnitudes for piece-score
// and material params with all queens of the side present land
// comfortably under 32K).
struct FeatureEntry {
    uint16_t paramIdx;
    int16_t coef;
};
using FeatureVector = std::vector<FeatureEntry>;

// Per-corpus feature cache. baseline[i] is the white-POV eval of
// position i at theta_0. rows[i] is the sparse vector of (paramIdx,
// coef) pairs such that
//   eval(theta, position_i) ≈ baseline[i] + sum_j (theta_j - theta0[j]) * rows[i][j].coef
// The approximation is exact for parameters the eval is linear in
// (PSTs, material, mobility, threats, pawn structure, most rook /
// bishop terms) and a first-order Taylor expansion at theta_0 for
// the king-safety quadratic. Re-extracting after each leaf refresh
// keeps drift bounded.
struct CorpusFeatures {
    std::vector<int> baseline;
    std::vector<FeatureVector> rows;
    std::vector<int> theta0;
};

// Extract per-position linearization features for the entire corpus.
// One serial outer loop over parameters with a parallel inner loop
// over positions. Per parameter we perturb theta_j by +1 (or -1 if
// the upper neighbor is infeasible), evaluate every position on
// `numThreads` threads, and append the non-zero deltas to the
// per-position sparse feature vector. Each parameter's perturbation
// is restored before moving on to the next, so the global evalParams
// is unchanged when extraction returns.
//
// Cost: roughly (P / 1) * (N / numThreads) microseconds. For the
// 778-parameter / 5.4M-position corpus on 14 threads, ~5 minutes.
// Memory: 5.4M * avg ~100 active params * 4 bytes = ~2 GB plus the
// 22 MB baseline vector.
static CorpusFeatures extractCorpusFeatures(std::vector<LabeledPosition> &positions,
                                            std::vector<ParamRef> &params, int numThreads) {
    const size_t N = positions.size();
    const size_t P = params.size();

    CorpusFeatures cf;
    cf.baseline.assign(N, 0);
    cf.rows.assign(N, FeatureVector{});
    cf.theta0.resize(P);
    for (size_t j = 0; j < P; j++)
        cf.theta0[j] = params[j].read();

    auto runOverPositions = [&](auto &&fn) {
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (int t = 0; t < numThreads; t++) {
            size_t start = (N * t) / numThreads;
            size_t end = (N * (t + 1)) / numThreads;
            threads.emplace_back([start, end, &positions, fn]() {
                for (size_t i = start; i < end; i++) {
                    Board board = positions[i].board;
                    int raw = evaluate(board);
                    if (board.sideToMove == Black) raw = -raw;
                    fn(i, raw);
                }
            });
        }
        for (auto &th : threads)
            th.join();
    };

    std::cerr << "extracting features for " << N << " positions across " << P
              << " parameters on " << numThreads << " threads\n";

    runOverPositions([&](size_t i, int raw) { cf.baseline[i] = raw; });

    std::vector<int> delta(N, 0);
    const size_t reportEvery = std::max<size_t>(1, P / 20);
    const int int16Min = std::numeric_limits<int16_t>::min();
    const int int16Max = std::numeric_limits<int16_t>::max();

    for (size_t j = 0; j < P; j++) {
        auto &p = params[j];
        int orig = cf.theta0[j];
        int sign = 0;
        if (p.allow(orig + 1)) {
            sign = 1;
            p.write(orig + 1);
        } else if (p.allow(orig - 1)) {
            sign = -1;
            p.write(orig - 1);
        } else {
            // Pinned parameter contributes no linearization feature.
            continue;
        }

        runOverPositions([&, sign](size_t i, int raw) {
            // sign = +1 means feature = raw - baseline.
            // sign = -1 means feature = baseline - raw (preserves
            // the d(eval) / d(theta) sign so the Newton step is
            // independent of which neighbor we sampled).
            delta[i] = sign > 0 ? (raw - cf.baseline[i]) : (cf.baseline[i] - raw);
        });

        p.write(orig);

        // Append non-zero entries in parallel by sharding the row
        // index range across threads. Each thread only writes to its
        // own slice of cf.rows so there is no contention.
        std::vector<std::thread> appenders;
        appenders.reserve(numThreads);
        for (int t = 0; t < numThreads; t++) {
            size_t start = (N * t) / numThreads;
            size_t end = (N * (t + 1)) / numThreads;
            appenders.emplace_back([start, end, &delta, &cf, j, int16Min, int16Max]() {
                for (size_t i = start; i < end; i++) {
                    int d = delta[i];
                    if (d == 0) continue;
                    if (d > int16Max) d = int16Max;
                    if (d < int16Min) d = int16Min;
                    cf.rows[i].push_back({static_cast<uint16_t>(j), static_cast<int16_t>(d)});
                }
            });
        }
        for (auto &th : appenders)
            th.join();

        if ((j + 1) % reportEvery == 0 || j + 1 == P)
            std::cerr << "  feature extract: " << (j + 1) << "/" << P << " parameters\n";
    }

    // Lightweight self-check: at theta_0 the prediction collapses to
    // the baseline, so re-evaluating a sample of positions and
    // comparing should mismatch nowhere. A non-zero count points at a
    // non-deterministic eval (e.g., a forgotten thread_local hash
    // race) before the first GN iteration.
    size_t checkCount = 0, mismatchCount = 0;
    size_t step = std::max<size_t>(1, N / 1000);
    for (size_t i = 0; i < N; i += step) {
        Board board = positions[i].board;
        int raw = evaluate(board);
        if (board.sideToMove == Black) raw = -raw;
        if (raw != cf.baseline[i]) mismatchCount++;
        checkCount++;
    }
    std::cerr << "feature extraction done: " << mismatchCount << "/" << checkCount
              << " baseline mismatches\n";

    return cf;
}

// In-place Cholesky decomposition of a symmetric positive-definite
// matrix A (row-major n x n) with an optional ridge lambda added to
// the diagonal for numerical safety. The lower triangle of A is
// overwritten with the Cholesky factor L; the upper triangle is
// untouched. Returns false on a non-positive pivot, which the caller
// treats as "step rejected" and falls back to the next phase.
static bool choleskyDecompose(std::vector<double> &A, size_t n, double lambda) {
    if (lambda > 0.0) {
        for (size_t i = 0; i < n; i++)
            A[i * n + i] += lambda;
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j <= i; j++) {
            double sum = A[i * n + j];
            for (size_t k = 0; k < j; k++)
                sum -= A[i * n + k] * A[j * n + k];
            if (i == j) {
                if (sum <= 0.0) return false;
                A[i * n + i] = std::sqrt(sum);
            } else {
                A[i * n + j] = sum / A[j * n + j];
            }
        }
    }
    return true;
}

// Solve L * L^T * x = b in place using the lower-triangular L
// produced by choleskyDecompose. The right-hand-side b is
// overwritten with the solution x. Cost is O(n^2).
static void choleskySolve(const std::vector<double> &A, std::vector<double> &b, size_t n) {
    // Forward solve L * y = b. y reuses b's storage.
    for (size_t i = 0; i < n; i++) {
        double sum = b[i];
        for (size_t j = 0; j < i; j++)
            sum -= A[i * n + j] * b[j];
        b[i] = sum / A[i * n + i];
    }
    // Back solve L^T * x = y. x reuses b's storage.
    for (size_t i = n; i-- > 0;) {
        double sum = b[i];
        for (size_t j = i + 1; j < n; j++)
            sum -= A[j * n + i] * b[j];
        b[i] = sum / A[i * n + i];
    }
}

// Convenience wrapper: decompose and solve with a single ridge.
// Returns true iff the system was solved (i.e., A + lambda*I was
// positive definite). On false, b is untouched.
static bool choleskySolveSymmetric(std::vector<double> &A, std::vector<double> &b, size_t n,
                                   double lambda) {
    if (!choleskyDecompose(A, n, lambda)) return false;
    choleskySolve(A, b, n);
    return true;
}

// Loss evaluated against the cached corpus features instead of the
// engine. Exact for parameters the eval is linear in (the dominant
// majority) and a first-order Taylor approximation for the king-
// safety quadratic. Used inside the Gauss-Newton line search to avoid
// repeated engine evaluations of the same corpus.
static double computeLossFromFeatures(const std::vector<LabeledPosition> &positions,
                                      const CorpusFeatures &cf, const std::vector<int> &theta,
                                      double K, int numThreads,
                                      const std::vector<size_t> *indices = nullptr) {
    const size_t N = indices ? indices->size() : positions.size();
    const size_t P = theta.size();

    std::vector<int> deltaTheta(P);
    for (size_t j = 0; j < P; j++)
        deltaTheta[j] = theta[j] - cf.theta0[j];

    std::vector<double> partial(numThreads, 0.0);
    std::vector<double> partialWeight(numThreads, 0.0);
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int t = 0; t < numThreads; t++) {
        size_t start = (N * t) / numThreads;
        size_t end = (N * (t + 1)) / numThreads;
        threads.emplace_back([&, start, end, t]() {
            double sum = 0.0;
            double sumWeight = 0.0;
            for (size_t i = start; i < end; i++) {
                size_t pi = indices ? (*indices)[i] : i;
                int64_t eval = cf.baseline[pi];
                for (const auto &fe : cf.rows[pi])
                    eval += static_cast<int64_t>(deltaTheta[fe.paramIdx]) * fe.coef;
                double pred = sigmoid(static_cast<double>(eval), K);
                double err = pred - positions[pi].result;
                double w = positions[pi].weight;
                sum += w * err * err;
                sumWeight += w;
            }
            partial[t] = sum;
            partialWeight[t] = sumWeight;
        });
    }
    for (auto &th : threads)
        th.join();
    double total = 0.0;
    double totalWeight = 0.0;
    for (int t = 0; t < numThreads; t++) {
        total += partial[t];
        totalWeight += partialWeight[t];
    }
    return totalWeight > 0.0 ? total / totalWeight : total / static_cast<double>(N);
}

// One full Gauss-Newton iteration over the parameter vector. With
// pre-extracted features the iteration becomes:
//
//   1. Predict eval per position from cached baseline and features.
//   2. Compute residual r_i = sigmoid(K * eval_i) - target_i.
//   3. Accumulate J^T J = K^2 * sum sigmoid'^2 * f_i f_i^T and
//      J^T r = K * sum sigmoid' * r_i * f_i in parallel over the
//      corpus.
//   4. Solve (J^T J + lambda * I) * x = J^T r with Cholesky. The
//      ridge lambda = 1e-3 * trace(J^T J) / P keeps the solver
//      stable when the Hessian is rank-deficient or near-singular
//      under tightly correlated parameters.
//   5. Clip per-parameter steps to +/-32 cp for robustness against
//      pathological corner moves the linearization gets wrong.
//   6. Backtracking line search at scales [1.0, 0.5, 0.25] using the
//      exact engine loss; accept the first scale that beats the
//      threaded-noise threshold.
//
// Returns the relative loss decrease. Updates bestLoss in place when
// an improvement is accepted; restores the parameter vector
// otherwise.
static double gaussNewtonPass(const std::vector<LabeledPosition> &positions,
                              const CorpusFeatures &cf, std::vector<ParamRef> &params, double K,
                              int numThreads, double &bestLoss,
                              const std::vector<size_t> *trainIndices = nullptr) {
    const size_t N = trainIndices ? trainIndices->size() : positions.size();
    const size_t P = params.size();

    constexpr int maxStepPerParam = 32;

    const double L0 = bestLoss;
    const double acceptThreshold = L0 * 1e-7;

    std::vector<int> theta(P);
    for (size_t j = 0; j < P; j++)
        theta[j] = params[j].read();
    std::vector<int> deltaTheta(P);
    for (size_t j = 0; j < P; j++)
        deltaTheta[j] = theta[j] - cf.theta0[j];

    // Per-thread accumulators. P * P doubles per thread; for P=778
    // that is about 4.8 MB per thread, comfortably under any cache
    // line budget but also small enough to merge serially in <10 ms.
    std::vector<std::vector<double>> threadJtJ(numThreads, std::vector<double>(P * P, 0.0));
    std::vector<std::vector<double>> threadJtr(numThreads, std::vector<double>(P, 0.0));

    {
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (int t = 0; t < numThreads; t++) {
            size_t start = (N * t) / numThreads;
            size_t end = (N * (t + 1)) / numThreads;
            threads.emplace_back([&, start, end, t]() {
                auto &JtJ = threadJtJ[t];
                auto &Jtr = threadJtr[t];
                for (size_t i = start; i < end; i++) {
                    size_t pi = trainIndices ? (*trainIndices)[i] : i;
                    int64_t eval = cf.baseline[pi];
                    const auto &row = cf.rows[pi];
                    for (const auto &fe : row)
                        eval += static_cast<int64_t>(deltaTheta[fe.paramIdx]) * fe.coef;
                    double pred = sigmoid(static_cast<double>(eval), K);
                    double residual = pred - positions[pi].result;
                    double sigprime = pred * (1.0 - pred);
                    double w = positions[pi].weight;
                    // Per-position weights enter the weighted least-
                    // squares normal equations symmetrically: each
                    // residual contributes w * sigprime^2 * f f^T to
                    // J^T J and w * sigprime * residual * f to J^T r.
                    double weightR = w * K * sigprime * residual;
                    double weightH = w * K * K * sigprime * sigprime;
                    const size_t M = row.size();
                    for (size_t a = 0; a < M; a++) {
                        int idxA = row[a].paramIdx;
                        double cA = static_cast<double>(row[a].coef);
                        Jtr[idxA] += weightR * cA;
                        double rowScale = weightH * cA;
                        for (size_t b = 0; b < a; b++) {
                            int idxB = row[b].paramIdx;
                            double cB = static_cast<double>(row[b].coef);
                            double v = rowScale * cB;
                            JtJ[idxA * P + idxB] += v;
                            JtJ[idxB * P + idxA] += v;
                        }
                        JtJ[idxA * P + idxA] += rowScale * cA;
                    }
                }
            });
        }
        for (auto &th : threads)
            th.join();
    }

    // Merge thread accumulators.
    std::vector<double> JtJ(P * P, 0.0);
    std::vector<double> Jtr(P, 0.0);
    for (int t = 0; t < numThreads; t++) {
        const auto &tj = threadJtJ[t];
        const auto &tr = threadJtr[t];
        for (size_t k = 0; k < P * P; k++)
            JtJ[k] += tj[k];
        for (size_t j = 0; j < P; j++)
            Jtr[j] += tr[j];
    }

    // Ridge regularization sized to the average diagonal so the
    // ridge does not dominate strong parameters or vanish on weak
    // ones.
    double diagSum = 0.0;
    for (size_t j = 0; j < P; j++)
        diagSum += JtJ[j * P + j];
    double avgDiag = diagSum / static_cast<double>(P);
    double ridge = 1e-3 * avgDiag;
    if (ridge < 1e-12) ridge = 1e-12;

    std::vector<double> step = Jtr;
    if (!choleskySolveSymmetric(JtJ, step, P, ridge)) {
        std::cerr << "  gauss-newton: Cholesky failed (non-positive pivot); skipping\n";
        return 0.0;
    }

    // step now contains x in (J^T J + lambda I) x = J^T r. The
    // Newton update is theta -= x with backtracking. clampToBounds
    // keeps the step inside the constraint region.
    auto applyScaled = [&](double scale) {
        for (size_t j = 0; j < P; j++) {
            double s = -scale * step[j];
            if (s > maxStepPerParam) s = static_cast<double>(maxStepPerParam);
            if (s < -maxStepPerParam) s = static_cast<double>(-maxStepPerParam);
            int newVal = theta[j] + static_cast<int>(std::round(s));
            params[j].write(params[j].clampToBounds(newVal));
        }
    };
    auto restore = [&]() {
        for (size_t j = 0; j < P; j++)
            params[j].write(theta[j]);
    };

    static const std::array<double, 3> backtrackScales = {1.0, 0.5, 0.25};
    for (double scale : backtrackScales) {
        applyScaled(scale);
        double L = computeLoss(positions, K, numThreads, trainIndices);
        if (L0 - L > acceptThreshold) {
            bestLoss = L;
            std::cerr << "  gauss-newton accepted scale=" << scale << " loss=" << L << "\n";
            return (L0 - L) / L0;
        }
    }

    restore();
    return 0.0;
}

// One Newton-Raphson pass over the parameter vector. Estimates per-
// parameter first and second derivatives of the loss via central finite
// differences (delta = 1 cp, the natural integer unit of these scalars),
// builds a batched Newton step, and accepts the largest backtracking
// scale whose loss is below the pre-pass baseline by more than the
// threaded-noise threshold the coordinate-descent loop already uses.
//
// On accept the loop moves the entire parameter vector at once, so a
// converged Newton phase can collapse what would be many coordinate-
// descent passes into a single sweep. On reject the loop restores the
// pre-pass vector and reports zero improvement; the caller treats two
// consecutive zero-improvement passes as the signal to fall back to the
// coordinate-descent ladder.
//
// Returns the relative loss decrease (`(L0 - Lnew) / L0`). Updates
// `bestLoss` in place when an improvement is accepted.
static double newtonPass(std::vector<LabeledPosition> &positions,
                         std::vector<ParamRef> &params, double K, int numThreads,
                         double &bestLoss,
                         const std::vector<size_t> *trainIndices = nullptr) {
    constexpr int delta = 1;             // finite-difference step size
    constexpr int maxStep = 16;          // per-parameter clip
    constexpr double minHessian = 1e-12; // floor for Newton division

    const double L0 = bestLoss;
    const double acceptThreshold = L0 * 1e-7; // matches the threaded CD threshold

    std::vector<int> theta(params.size());
    for (size_t i = 0; i < params.size(); i++)
        theta[i] = params[i].read();

    std::vector<double> dtheta(params.size(), 0.0);

    for (size_t i = 0; i < params.size(); i++) {
        auto &p = params[i];
        int orig = theta[i];
        bool canPlus = p.allow(orig + delta);
        bool canMinus = p.allow(orig - delta);
        if (!canPlus && !canMinus) continue;

        double Lp = L0;
        double Lm = L0;
        if (canPlus) {
            p.write(orig + delta);
            Lp = computeLoss(positions, K, numThreads, trainIndices);
        }
        if (canMinus) {
            p.write(orig - delta);
            Lm = computeLoss(positions, K, numThreads, trainIndices);
        }
        p.write(orig); // restore for the next parameter and the line search

        double g, h;
        if (canPlus && canMinus) {
            g = (Lp - Lm) / (2.0 * delta);
            h = (Lp - 2.0 * L0 + Lm) / (delta * delta);
        } else if (canPlus) {
            g = (Lp - L0) / delta;
            h = minHessian;
        } else {
            g = (L0 - Lm) / delta;
            h = minHessian;
        }

        double step;
        if (h > minHessian) {
            step = -g / h;
        } else {
            // Flat or concave: gradient-descent fallback, capped at one
            // cp so a noisy region cannot launch the parameter.
            step = (g > 0.0 ? -1.0 : 1.0);
        }
        if (step > maxStep) step = maxStep;
        if (step < -maxStep) step = -maxStep;
        dtheta[i] = step;
    }

    auto applyScaled = [&](double scale) {
        for (size_t i = 0; i < params.size(); i++) {
            int newVal = theta[i] + static_cast<int>(std::round(dtheta[i] * scale));
            params[i].write(params[i].clampToBounds(newVal));
        }
    };

    auto restore = [&]() {
        for (size_t i = 0; i < params.size(); i++)
            params[i].write(theta[i]);
    };

    static const std::array<double, 3> backtrackScales = {1.0, 0.5, 0.25};
    for (double scale : backtrackScales) {
        applyScaled(scale);
        double L = computeLoss(positions, K, numThreads, trainIndices);
        if (L0 - L > acceptThreshold) {
            bestLoss = L;
            std::cerr << "  newton accepted scale=" << scale << " loss=" << L << "\n";
            return (L0 - L) / L0;
        }
    }

    restore();
    return 0.0;
}

// Adam optimiser state. We carry a real-valued shadow parameter vector
// because Adam updates are sub-cp by design and the integer params
// would otherwise round most of them to zero. The shadow vector is
// rounded to integers, written to params, projected to constraints,
// then synced back so the next epoch sees the in-bounds values.
struct AdamState {
    std::vector<double> theta;  // real-valued shadow params
    std::vector<double> m;      // first moment, size P
    std::vector<double> v;      // second moment, size P
    int t = 0;                  // step counter for bias correction
};

// One full-batch Adam epoch over the cached corpus features. Computes
// the gradient of the data MSE in parallel across `numThreads` workers
// (each with a P-sized local accumulator, merged serially), applies a
// bias-corrected Adam update to the shadow theta, rounds + writes the
// integer params, projects onto the constraint region, and resyncs the
// shadow theta from the projected values.
//
// Concurrency: per-thread P-sized gradient accumulator (~6 KB at P=791,
// fits in L1). Final merge is O(P * numThreads), negligible compared
// to the O(N * sparse-features) main loop. Loss + total weight are
// reduced in the same pass to avoid a second corpus walk.
//
// Why no regulariser gradient: Gauss-Newton's normal equations don't
// include the regulariser either, and at our default lambdas
// (1e-9 / 1e-8) the data MSE gradient dominates by 6+ orders of
// magnitude. Keeping Adam pure-data-gradient lets it converge
// alongside GN consistently, and the next CD ladder cleans up any
// residual regulariser-driven motion.
//
// Returns relative loss decrease; updates `bestLoss` in place.
static double adamEpoch(std::vector<LabeledPosition> &positions, const CorpusFeatures &cf,
                        std::vector<ParamRef> &params, AdamState &adam, double K, int numThreads,
                        double lr, double beta1, double beta2, double eps, double &bestLoss,
                        const std::vector<size_t> *trainIndices = nullptr) {
    const size_t N = trainIndices ? trainIndices->size() : positions.size();
    const size_t P = params.size();

    adam.t++;
    const double L0 = bestLoss;

    // Build deltaTheta in feature-cache units. We round the shadow
    // theta to integers before forming deltaTheta so the linearised
    // eval matches what an integer-rounded write+evaluate would give.
    std::vector<int> deltaTheta(P);
    for (size_t p = 0; p < P; p++) {
        int rounded = static_cast<int>(std::lround(adam.theta[p]));
        deltaTheta[p] = rounded - cf.theta0[p];
    }

    // Threaded gradient + loss accumulation. Each worker walks its
    // slice and accumulates into a private gradient vector + scalar
    // loss / weight totals.
    std::vector<std::vector<double>> threadGrad(numThreads, std::vector<double>(P, 0.0));
    std::vector<double> threadLoss(numThreads, 0.0);
    std::vector<double> threadWeight(numThreads, 0.0);
    {
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (int t = 0; t < numThreads; t++) {
            size_t start = (N * t) / numThreads;
            size_t end = (N * (t + 1)) / numThreads;
            threads.emplace_back([&, start, end, t]() {
                auto &grad = threadGrad[t];
                double lossSum = 0.0;
                double weightSum = 0.0;
                for (size_t i = start; i < end; i++) {
                    size_t pi = trainIndices ? (*trainIndices)[i] : i;
                    int64_t eval = cf.baseline[pi];
                    const auto &row = cf.rows[pi];
                    for (const auto &fe : row)
                        eval += static_cast<int64_t>(deltaTheta[fe.paramIdx]) * fe.coef;
                    double pred = sigmoid(static_cast<double>(eval), K);
                    double residual = pred - positions[pi].result;
                    double sigprime = pred * (1.0 - pred);
                    double w = positions[pi].weight;
                    lossSum += w * residual * residual;
                    weightSum += w;
                    // Gradient contribution: partial of (w * residual^2)
                    // with respect to theta_p is 2 * w * residual *
                    // K * sigprime * coef_p. We omit the leading 2 to
                    // mirror the Gauss-Newton residual scaling, which
                    // means the effective Adam learning rate is in
                    // GN-comparable units.
                    double scale = K * sigprime * residual * w;
                    for (const auto &fe : row) {
                        grad[fe.paramIdx] += scale * static_cast<double>(fe.coef);
                    }
                }
                threadLoss[t] = lossSum;
                threadWeight[t] = weightSum;
            });
        }
        for (auto &th : threads)
            th.join();
    }

    std::vector<double> grad(P, 0.0);
    double dataLoss = 0.0;
    double totalWeight = 0.0;
    for (int t = 0; t < numThreads; t++) {
        const auto &g = threadGrad[t];
        for (size_t p = 0; p < P; p++)
            grad[p] += g[p];
        dataLoss += threadLoss[t];
        totalWeight += threadWeight[t];
    }
    if (totalWeight > 0.0) {
        const double inv = 1.0 / totalWeight;
        dataLoss *= inv;
        for (size_t p = 0; p < P; p++)
            grad[p] *= inv;
    } else if (N > 0) {
        const double inv = 1.0 / static_cast<double>(N);
        dataLoss *= inv;
        for (size_t p = 0; p < P; p++)
            grad[p] *= inv;
    }

    // Adam update with bias correction. The two precomputed factors
    // (1 - beta^t) approach 1 quickly; for small t they keep the
    // first-step magnitude calibrated.
    const double oneMinusBeta1T = 1.0 - std::pow(beta1, adam.t);
    const double oneMinusBeta2T = 1.0 - std::pow(beta2, adam.t);
    const double biasInv1 = oneMinusBeta1T > 0.0 ? 1.0 / oneMinusBeta1T : 1.0;
    const double biasInv2 = oneMinusBeta2T > 0.0 ? 1.0 / oneMinusBeta2T : 1.0;
    for (size_t p = 0; p < P; p++) {
        adam.m[p] = beta1 * adam.m[p] + (1.0 - beta1) * grad[p];
        adam.v[p] = beta2 * adam.v[p] + (1.0 - beta2) * grad[p] * grad[p];
        double mHat = adam.m[p] * biasInv1;
        double vHat = adam.v[p] * biasInv2;
        adam.theta[p] -= lr * mHat / (std::sqrt(vHat) + eps);
    }

    // Round, write, project, resync. Projection may snap chain entries
    // and slope-cap violators; we copy the projected integer values
    // back into the shadow theta so the next epoch's linearization
    // and Adam state stay consistent with the in-bounds parameters.
    for (size_t p = 0; p < P; p++) {
        int newVal = static_cast<int>(std::lround(adam.theta[p]));
        params[p].write(params[p].clampToBounds(newVal));
    }
    projectToConstraints(params);
    for (size_t p = 0; p < P; p++) {
        adam.theta[p] = static_cast<double>(params[p].read());
    }

    // Total loss includes the parameter-only regulariser so the val
    // gate's bestLoss tracking stays in the same units used by
    // computeLoss elsewhere in the tuner.
    double regLoss = pstSmoothLambda() * pstSmoothnessPenalty();
    regLoss += pawnMirrorLambda() * pawnMirrorPenalty();
    double totalLoss = dataLoss + regLoss;
    bestLoss = totalLoss;

    return L0 > 0.0 ? (L0 - totalLoss) / L0 : 0.0;
}

// ============================================================================
// Feature-cached coordinate descent
//
// Standard CD calls computeLoss() per step attempt -- a full corpus walk
// (~240ms on 14 threads / 11M positions). With the corpus feature cache we
// can do ~80x better on sparse params and ~5-10x better in aggregate by:
//
//   1. Maintaining per-position cached state: eval[i] (current eval),
//      residual[i] = sigmoid(K*eval[i]) - target[i], plus the global
//      data MSE that those residuals sum to.
//   2. Building an inverted index (param -> list of positions whose
//      feature row contains that param, plus the per-pair coef).
//   3. For a CD step on param p with delta d, only the positions in
//      paramPositions[p] need recomputation -- their eval shifts by
//      d * coef, residual is recomputed, and the per-position weighted
//      err^2 contribution to the MSE is updated incrementally.
//   4. The regulariser is recomputed in full each step (cheap; just
//      walks PST cells, no corpus pass), since one PST cell change
//      perturbs every adjacent-pair / mirror-pair it participates in.
//
// Memory: inverted index is ~6 bytes per (position, feature) pair using
// SoA. For an 11M-position corpus with ~100 features each, that's ~7 GB.
// We free cf.rows immediately after the index is built so peak memory
// during the GN/Adam->CD transition stays bounded by max(cf.rows,
// inverted-index) plus eval/residual state (~200 MB).
//
// Concurrency: the inner loop over paramPositions[p] is threaded across
// `numThreads` workers when |positions(p)| is large enough to amortize
// thread setup overhead (~50 us). Below the threshold we go
// single-threaded since per-call cost is already < 1 ms. The state
// arrays are only written for accepted steps and the writes never
// overlap across threads (each thread owns a contiguous slice of the
// inverted index for the current param), so there are no locks or
// atomics anywhere in the hot path.

struct CDInvertedIndex {
    // CSR-style layout. For param p, the active (position, coef) pairs
    // live in [rowStart[p], rowStart[p+1]) inside `positions` /
    // `coefs`. Indexed positions are train-slice-relative (0..nTrain).
    std::vector<uint64_t> rowStart;  // size = numParams + 1
    std::vector<uint32_t> positions; // train-slice indices
    std::vector<int16_t> coefs;
};

struct CDFeatureState {
    size_t nTrain = 0;
    double K = 0.0;

    // Per-train-slice cached state. Each is size nTrain.
    std::vector<double> eval;       // current eval = baseline + sum(delta_theta * coef)
    std::vector<double> residual;   // sigmoid(K * eval) - target
    std::vector<double> targets;    // result label per train position
    std::vector<double> weights;    // assignGameWeights output

    double totalWeight = 0.0;
    double dataLoss = 0.0;          // weighted MSE over train
    double regLoss = 0.0;           // PST smoothness + pawn mirror
    double totalLoss = 0.0;         // dataLoss + regLoss

    CDInvertedIndex inv;
};

// Build the inverted index from cf.rows over the train slice. Two-pass
// CSR construction: first pass counts entries per param to size the
// flat arrays; second pass fills them via a writePos cursor per param.
// Both passes are serial -- index build is one-time and 5 sec single-
// threaded on the 11M-position corpus is fine.
static void buildInvertedIndex(CDInvertedIndex &inv, const CorpusFeatures &cf,
                               const std::vector<size_t> &trainIndices, size_t numParams) {
    const size_t nTrain = trainIndices.size();
    inv.rowStart.assign(numParams + 1, 0);

    for (size_t i = 0; i < nTrain; i++) {
        const auto &row = cf.rows[trainIndices[i]];
        for (const auto &fe : row) {
            inv.rowStart[fe.paramIdx + 1]++;
        }
    }
    for (size_t p = 0; p < numParams; p++) {
        inv.rowStart[p + 1] += inv.rowStart[p];
    }
    const uint64_t totalEntries = inv.rowStart[numParams];
    inv.positions.assign(totalEntries, 0);
    inv.coefs.assign(totalEntries, 0);

    std::vector<uint64_t> writePos = inv.rowStart;
    for (size_t i = 0; i < nTrain; i++) {
        const auto &row = cf.rows[trainIndices[i]];
        for (const auto &fe : row) {
            uint64_t w = writePos[fe.paramIdx]++;
            inv.positions[w] = static_cast<uint32_t>(i);
            inv.coefs[w] = fe.coef;
        }
    }
}

// Initialize the per-position eval/residual cache from cf.baseline +
// the current parameter values. Threaded over the train slice; each
// worker walks a contiguous range and accumulates into a private
// loss / weight pair, merged serially.
static void initCDFeatureState(CDFeatureState &state, const std::vector<LabeledPosition> &positions,
                               const CorpusFeatures &cf, std::vector<ParamRef> &params, double K,
                               int numThreads, const std::vector<size_t> &trainIndices) {
    const size_t nTrain = trainIndices.size();
    const size_t P = params.size();

    state.nTrain = nTrain;
    state.K = K;
    state.eval.assign(nTrain, 0.0);
    state.residual.assign(nTrain, 0.0);
    state.targets.assign(nTrain, 0.0);
    state.weights.assign(nTrain, 0.0);

    std::vector<int> deltaTheta(P);
    for (size_t p = 0; p < P; p++) {
        deltaTheta[p] = params[p].read() - cf.theta0[p];
    }

    std::vector<double> partialLoss(numThreads, 0.0);
    std::vector<double> partialWeight(numThreads, 0.0);
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int t = 0; t < numThreads; t++) {
        size_t start = (nTrain * t) / numThreads;
        size_t end = (nTrain * (t + 1)) / numThreads;
        threads.emplace_back([&, start, end, t]() {
            double sumLoss = 0.0;
            double sumWeight = 0.0;
            for (size_t i = start; i < end; i++) {
                size_t pi = trainIndices[i];
                int64_t evalI = cf.baseline[pi];
                for (const auto &fe : cf.rows[pi])
                    evalI += static_cast<int64_t>(deltaTheta[fe.paramIdx]) * fe.coef;
                double evalF = static_cast<double>(evalI);
                state.eval[i] = evalF;
                double pred = sigmoid(evalF, K);
                double target = positions[pi].result;
                state.targets[i] = target;
                double res = pred - target;
                state.residual[i] = res;
                double w = positions[pi].weight;
                state.weights[i] = w;
                sumLoss += w * res * res;
                sumWeight += w;
            }
            partialLoss[t] = sumLoss;
            partialWeight[t] = sumWeight;
        });
    }
    for (auto &th : threads)
        th.join();

    state.totalWeight = 0.0;
    state.dataLoss = 0.0;
    for (int t = 0; t < numThreads; t++) {
        state.dataLoss += partialLoss[t];
        state.totalWeight += partialWeight[t];
    }
    if (state.totalWeight > 0.0) {
        state.dataLoss /= state.totalWeight;
    } else if (nTrain > 0) {
        state.dataLoss /= static_cast<double>(nTrain);
    }
    state.regLoss = pstSmoothLambda() * pstSmoothnessPenalty();
    state.regLoss += pawnMirrorLambda() * pawnMirrorPenalty();
    state.totalLoss = state.dataLoss + state.regLoss;
}

// Try a CD step on `paramIdx` with `delta` cp added to the live
// parameter (the caller has already done `param.write(new_value)`).
// Computes the proposed new total loss using only the positions in
// the inverted index for this param; if the improvement clears the
// caller's threshold, commits the per-position eval/residual updates
// and returns true. On rejection, leaves state unchanged and returns
// false; the caller restores the parameter via `param.write(original)`.
//
// Threading: when the param touches more than `kThreadingThreshold`
// positions the inner loops are split across `numThreads`. Below that
// threshold the thread setup overhead dominates so we go
// single-threaded.
static bool cdFeatureTryStep(CDFeatureState &state, int paramIdx, int delta, double threshold,
                             int numThreads) {
    const auto &inv = state.inv;
    const uint64_t lo = inv.rowStart[paramIdx];
    const uint64_t hi = inv.rowStart[paramIdx + 1];
    const uint64_t span = hi - lo;
    constexpr uint64_t kThreadingThreshold = 100000;
    const double K = state.K;
    const double *targets = state.targets.data();
    const double *weights = state.weights.data();
    const uint32_t *invPos = inv.positions.data();
    const int16_t *invCoef = inv.coefs.data();
    double *evalArr = state.eval.data();
    double *resArr = state.residual.data();

    auto computeDelta = [&](uint64_t s, uint64_t e) -> double {
        double sum = 0.0;
        for (uint64_t k = s; k < e; k++) {
            uint32_t i = invPos[k];
            double newEval = evalArr[i] + static_cast<double>(delta) * invCoef[k];
            double newPred = sigmoid(newEval, K);
            double newRes = newPred - targets[i];
            sum += weights[i] * (newRes * newRes - resArr[i] * resArr[i]);
        }
        return sum;
    };
    auto applyCommit = [&](uint64_t s, uint64_t e) {
        for (uint64_t k = s; k < e; k++) {
            uint32_t i = invPos[k];
            evalArr[i] += static_cast<double>(delta) * invCoef[k];
            resArr[i] = sigmoid(evalArr[i], K) - targets[i];
        }
    };

    double deltaWErr2 = 0.0;
    if (span < kThreadingThreshold || numThreads <= 1) {
        deltaWErr2 = computeDelta(lo, hi);
    } else {
        std::vector<double> partial(numThreads, 0.0);
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (int t = 0; t < numThreads; t++) {
            uint64_t s = lo + (span * t) / numThreads;
            uint64_t e = lo + (span * (t + 1)) / numThreads;
            threads.emplace_back([&, s, e, t]() { partial[t] = computeDelta(s, e); });
        }
        for (auto &th : threads)
            th.join();
        for (double p : partial)
            deltaWErr2 += p;
    }

    const double newDataLoss = state.dataLoss + deltaWErr2 / state.totalWeight;
    const double newRegLoss = pstSmoothLambda() * pstSmoothnessPenalty()
                              + pawnMirrorLambda() * pawnMirrorPenalty();
    const double newTotalLoss = newDataLoss + newRegLoss;

    if (state.totalLoss - newTotalLoss <= threshold) {
        return false;
    }

    if (span < kThreadingThreshold || numThreads <= 1) {
        applyCommit(lo, hi);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (int t = 0; t < numThreads; t++) {
            uint64_t s = lo + (span * t) / numThreads;
            uint64_t e = lo + (span * (t + 1)) / numThreads;
            threads.emplace_back([&, s, e]() { applyCommit(s, e); });
        }
        for (auto &th : threads)
            th.join();
    }

    state.dataLoss = newDataLoss;
    state.regLoss = newRegLoss;
    state.totalLoss = newTotalLoss;
    return true;
}

// Resync the eval/residual cache from current params after an event
// that wrote params outside of `cdFeatureTryStep` -- gauge centering
// (PSTs shifted by complementary constants, eval is bit-identical so
// state.eval would still be correct in theory but rounding makes it
// safer to reinit), constraint projection that snapped chain
// violators, etc. Cheap: one threaded pass over the train slice. The
// inverted index is unchanged.
static void resyncCDFeatureState(CDFeatureState &state,
                                 const std::vector<LabeledPosition> &positions,
                                 const CorpusFeatures &cf, std::vector<ParamRef> &params,
                                 int numThreads, const std::vector<size_t> &trainIndices) {
    initCDFeatureState(state, positions, cf, params, state.K, numThreads, trainIndices);
}

static void tune(std::vector<LabeledPosition> &positions, double K, int numThreads,
                 int maxPasses, int refitKEvery, int refreshLeavesEvery, int newtonPasses,
                 bool useGaussNewton, int adamEpochs, double adamLr,
                 const std::vector<size_t> &trainIndices,
                 const std::vector<size_t> &valIndices,
                 int valGateWarmup, int valGatePatience, bool valGateEnabled,
                 int leafDepth) {
    auto params = collectParams();
    const std::vector<size_t> *trainPtr = trainIndices.empty() ? nullptr : &trainIndices;
    const std::vector<size_t> *valPtr = valIndices.empty() ? nullptr : &valIndices;
    std::cerr << "tuning " << params.size() << " scalars across "
              << (trainPtr ? trainIndices.size() : positions.size())
              << " train positions";
    if (valPtr) std::cerr << " (" << valIndices.size() << " val held out)";
    std::cerr << " with " << numThreads << " threads, K=" << K << "\n";
    if (valPtr) {
        if (valGateEnabled) {
            std::cerr << "val gate: on, warmup=" << valGateWarmup
                      << " passes, patience=" << valGatePatience
                      << " consecutive non-improvements\n";
        } else {
            std::cerr << "val gate: off (diagnostics only)\n";
        }
    }

    projectToConstraints(params);
    validateConstraints(params);

    double bestLoss = computeLoss(positions, K, numThreads, trainPtr);

    auto snapshotParams = [&]() {
        std::vector<int> snap(params.size());
        for (size_t i = 0; i < params.size(); i++) snap[i] = params[i].read();
        return snap;
    };
    auto restoreParams = [&](const std::vector<int> &snap) {
        for (size_t i = 0; i < params.size(); i++) params[i].write(snap[i]);
    };

    // Best-val tracking. We never revert mid-run -- coordinate descent
    // is deterministic given the params, so a mid-run revert would
    // just replay the same step ladder and revert again. Instead we
    // remember the param vector at the lowest val loss seen so far,
    // let training continue (so a noisy bad pass cannot prematurely
    // stop the trajectory), and at the end of tune() restore the
    // final params to the best-val snapshot. That captures the most
    // generalising point along the trajectory rather than wherever
    // the run happened to terminate.
    double bestValLoss = std::numeric_limits<double>::infinity();
    std::vector<int> bestValSnapshot;
    int noValImprovement = 0;
    if (valPtr) {
        bestValLoss = computeDataLoss(positions, K, numThreads, valPtr);
        bestValSnapshot = snapshotParams();
    }
    std::cerr << "initial loss: " << bestLoss;
    if (valPtr) std::cerr << " val_loss=" << bestValLoss;
    std::cerr << "\n";

    // Bucketed val report; returns the overall val loss so the caller
    // can feed the gate without paying the per-position pass twice.
    auto reportVal = [&](const std::string &tag, int pass) -> double {
        if (!valPtr) return 0.0;
        return reportValidation(positions, valIndices, K, numThreads, bestLoss, pass, tag);
    };

    // Best-val accept gate. Three cases per pass:
    //
    //   * Val improved (new minimum): update bestValLoss and snapshot
    //     the current params as the best-val state. Reset the
    //     consecutive-non-improvement counter. Also write a
    //     `checkpoint_bestval.txt` so the operator has an on-disk
    //     copy of the best-val state if the run is interrupted.
    //   * Val did not improve: log the counter and let training
    //     continue. We do NOT revert the pass, because mid-run
    //     reverts replay the same deterministic CD step ladder and
    //     just produce the same regression next pass. Other
    //     parameters in the same pass might genuinely have improved
    //     even if the aggregate val measurement happened to land
    //     above the previous best -- not chopping them off lets the
    //     trajectory continue exploring.
    //   * Patience exceeded (post-warmup, `noValImprovement >=
    //     patience`): break the loop. The end-of-tune handler will
    //     restore params to the best-val snapshot regardless of
    //     where the loop exits.
    //
    // Returns the (possibly overridden) `improved` flag the outer
    // loop should believe: true if training should continue, false
    // if patience hit.
    auto applyValGate = [&](double valLoss, int pass, const std::string &tag,
                            bool improved) -> bool {
        if (!valPtr || !valGateEnabled) return improved;
        if (valLoss < bestValLoss * (1.0 - 1e-6)) {
            bestValLoss = valLoss;
            bestValSnapshot = snapshotParams();
            noValImprovement = 0;
            std::cerr << "  " << tag << " pass " << pass
                      << " new best val_loss=" << valLoss << "\n";
            writeCheckpoint("tuning/checkpoint_bestval.txt", params);
            return improved;
        }
        if (pass < valGateWarmup) return improved;
        noValImprovement++;
        std::cerr << "  " << tag << " pass " << pass
                  << " no val improvement (" << noValImprovement << "/"
                  << valGatePatience << " toward patience limit, best_val_loss="
                  << bestValLoss << ")\n";
        if (noValImprovement >= valGatePatience) {
            std::cerr << "  " << tag << " pass " << pass
                      << " val gate stop: " << valGatePatience
                      << " consecutive passes without val improvement\n";
            return false;
        }
        return improved;
    };

    // Reset the patience counter at the start of each phase. GN /
    // Newton / CD-ladder / CD-finalizer each explore the loss
    // landscape differently, so a phase that exhausts patience
    // without progress should not pre-empt the next phase from
    // running at all.
    auto resetPatience = [&]() { noValImprovement = 0; };

    // Rebase bestValLoss after a K refit or leaf refresh. Both events
    // deliberately reshape the loss surface (sigmoid scale change for
    // K refit, eval-target redistribution for leaf refresh), so the
    // existing bestValLoss scalar -- measured under the old surface
    // -- is no longer a valid baseline for the patience gate. Without
    // a rebase, the very first post-refresh val measurement looks
    // worse than the stale baseline by construction, the patience
    // counter trips immediately, and the rest of the tune is wasted.
    //
    // We re-evaluate bestValLoss by temporarily restoring
    // bestValSnapshot (the params at the historical best val), so the
    // new baseline reflects how that same snapshot scores under the
    // new K and new leaves -- preserving the "is the current point
    // better than the historical best?" semantics across the surface
    // shift. The patience counter is reset because the previous
    // non-improvements were measured against the obsolete baseline.
    //
    // No-op when val tracking is off, the gate is disabled, or no
    // best snapshot has been recorded yet.
    auto rebaseBestValLoss = [&](const std::string &reason) {
        if (!valPtr || !valGateEnabled || bestValSnapshot.empty()) return;
        std::vector<int> live = snapshotParams();
        restoreParams(bestValSnapshot);
        bestValLoss = computeDataLoss(positions, K, numThreads, valPtr);
        restoreParams(live);
        noValImprovement = 0;
        std::cerr << "  rebased bestValLoss=" << bestValLoss
                  << " (after " << reason << ", patience reset)\n";
    };

    // Optional Newton-style initial phase. Two flavors share the same
    // pass-count budget and the same K-refit / leaf-refresh cadences:
    //
    //   * Gauss-Newton (default when useGaussNewton is true) caches
    //     per-position linearization features once, then every
    //     iteration is one parallel pass to accumulate J^T J and
    //     J^T r plus a Cholesky solve. Per iteration: roughly four
    //     seconds plus three exact-loss evaluations for the line
    //     search, versus ten minutes for the diagonal Newton path on
    //     the same corpus.
    //   * Diagonal Newton (useGaussNewton false) uses central finite
    //     differences on the full loss to estimate per-parameter
    //     first and second derivatives. No feature cache, no linear
    //     algebra dependency, but per-pass cost is 2P + 3 loss
    //     evaluations.
    //
    // Both paths stop early when two consecutive passes fail to clear
    // the threaded-noise threshold or when the relative improvement
    // drops below 1e-6. After Newton-style passes finish, the
    // coordinate-descent ladder below picks up any residual.
    int globalPass = 0;
    // Feature cache shared across the Gauss-Newton and Adam phases.
    // Extracted lazily by whichever phase runs first; reused by the
    // other if both run (the common case under default flags).
    CorpusFeatures cf;
    bool cfInitialized = false;
    if (newtonPasses > 0 && useGaussNewton) {
        std::cerr << "gauss-newton phase: up to " << newtonPasses << " passes\n";
        cf = extractCorpusFeatures(positions, params, numThreads);
        cfInitialized = true;
        resetPatience();
        int stalled = 0;
        for (int pass = 0; pass < newtonPasses; pass++, globalPass++) {
            std::cerr << "gauss-newton pass " << globalPass << " starting (loss=" << bestLoss
                      << ")\n";
            double rel = gaussNewtonPass(positions, cf, params, K, numThreads, bestLoss, trainPtr);
            centerPSTGauge();
            std::cerr << "gauss-newton pass " << globalPass << " done, loss=" << bestLoss
                      << " rel-improvement=" << rel << "\n";
            double valLoss = reportVal("gauss-newton", globalPass);
            bool improved = rel >= 1e-6;
            improved = applyValGate(valLoss, globalPass, "gauss-newton", improved);
            // If the gate signalled patience-exhaustion, treat as a
            // stalled pass for the train-side convergence check too
            // so the existing "two stalled passes -> exit" path runs
            // in lockstep with the val gate.
            if (!improved) rel = 0.0;
            writeCheckpoint("tuning/checkpoint.txt", params);

            if (rel < 1e-6) {
                if (++stalled >= 2) {
                    std::cerr << "gauss-newton convergence; switching to coordinate descent\n";
                    pass++;
                    globalPass++;
                    break;
                }
            } else {
                stalled = 0;
            }

            if (refitKEvery > 0 && (pass + 1) % refitKEvery == 0) {
                std::cerr << "refit K after gauss-newton pass " << globalPass << "\n";
                double oldK = K;
                K = findBestK(positions, numThreads, trainPtr);
                bestLoss = computeLoss(positions, K, numThreads, trainPtr);
                std::cerr << "K " << oldK << " -> " << K << ", rebased loss=" << bestLoss << "\n";
                rebaseBestValLoss("K refit");
            }
            if (refreshLeavesEvery > 0 && (pass + 1) % refreshLeavesEvery == 0) {
                std::cerr << "refresh leaves after gauss-newton pass " << globalPass << "\n";
                precomputeLeaves(positions, numThreads, leafDepth);
                double oldK = K;
                K = findBestK(positions, numThreads, trainPtr);
                bestLoss = computeLoss(positions, K, numThreads, trainPtr);
                std::cerr << "post-refresh K " << oldK << " -> " << K
                          << ", rebased loss=" << bestLoss << "\n";
                rebaseBestValLoss("leaf refresh");
                // Re-extract features against the new qsearch leaves.
                // Without this the Newton step would optimize against
                // stale linearizations.
                cf = extractCorpusFeatures(positions, params, numThreads);
            }
        }
    } else if (newtonPasses > 0) {
        std::cerr << "newton phase: up to " << newtonPasses << " passes\n";
        resetPatience();
        int stalled = 0;
        for (int pass = 0; pass < newtonPasses; pass++, globalPass++) {
            std::cerr << "newton pass " << globalPass << " starting (loss=" << bestLoss << ")\n";
            double rel = newtonPass(positions, params, K, numThreads, bestLoss, trainPtr);
            centerPSTGauge();
            std::cerr << "newton pass " << globalPass << " done, loss=" << bestLoss
                      << " rel-improvement=" << rel << "\n";
            double valLoss = reportVal("newton", globalPass);
            bool improved = rel >= 1e-6;
            improved = applyValGate(valLoss, globalPass, "newton", improved);
            if (!improved) rel = 0.0;
            writeCheckpoint("tuning/checkpoint.txt", params);

            if (rel < 1e-6) {
                if (++stalled >= 2) {
                    std::cerr << "newton convergence; switching to coordinate descent\n";
                    pass++; globalPass++;
                    break;
                }
            } else {
                stalled = 0;
            }

            if (refitKEvery > 0 && (pass + 1) % refitKEvery == 0) {
                std::cerr << "refit K after newton pass " << globalPass << "\n";
                double oldK = K;
                K = findBestK(positions, numThreads, trainPtr);
                bestLoss = computeLoss(positions, K, numThreads, trainPtr);
                std::cerr << "K " << oldK << " -> " << K << ", rebased loss=" << bestLoss << "\n";
                rebaseBestValLoss("K refit");
            }
            if (refreshLeavesEvery > 0 && (pass + 1) % refreshLeavesEvery == 0) {
                std::cerr << "refresh leaves after newton pass " << globalPass << "\n";
                precomputeLeaves(positions, numThreads, leafDepth);
                double oldK = K;
                K = findBestK(positions, numThreads, trainPtr);
                bestLoss = computeLoss(positions, K, numThreads, trainPtr);
                std::cerr << "post-refresh K " << oldK << " -> " << K
                          << ", rebased loss=" << bestLoss << "\n";
                rebaseBestValLoss("leaf refresh");
                // Diagonal Newton doesn't keep a feature cache, so any
                // staleness flag the Adam phase set is moot here -- but
                // if Adam runs after this loop it will need a fresh
                // extract.
                cfInitialized = false;
            }
        }
    }

    // Adam fine-tuning over the cached features. Runs after the
    // Newton-style phase (when configured) and before coordinate
    // descent. The conventional modern HCE Texel approach (Andrew
    // Grant's Tune for Ethereal, Berserk, etc.): once GN has settled
    // into a coarse minimum, Adam takes ~10x more iterations at ~100x
    // less cost-per-iteration than CD, hitting sub-cp residuals that
    // CD would otherwise grind out one parameter at a time.
    //
    // Each epoch is one full-batch pass through the train slice with
    // a single bias-corrected Adam update applied at the end. The val
    // gate runs once per epoch (consistent with GN / CD), so the
    // existing patience + best-val-restore semantics apply.
    if (adamEpochs > 0) {
        if (!cfInitialized) {
            cf = extractCorpusFeatures(positions, params, numThreads);
            cfInitialized = true;
        }
        const size_t P = params.size();
        AdamState adam;
        adam.theta.resize(P);
        adam.m.assign(P, 0.0);
        adam.v.assign(P, 0.0);
        adam.t = 0;
        for (size_t p = 0; p < P; p++) {
            adam.theta[p] = static_cast<double>(params[p].read());
        }

        std::cerr << "adam phase: up to " << adamEpochs << " epochs at lr=" << adamLr << "\n";
        resetPatience();
        int stalled = 0;
        constexpr double kBeta1 = 0.9;
        constexpr double kBeta2 = 0.999;
        constexpr double kEps = 1e-8;
        for (int epoch = 0; epoch < adamEpochs; epoch++, globalPass++) {
            double rel = adamEpoch(positions, cf, params, adam, K, numThreads, adamLr,
                                   kBeta1, kBeta2, kEps, bestLoss, trainPtr);
            centerPSTGauge();
            // centerPSTGauge() shifts integer PSTs by complementary
            // constants; pull the new values into the shadow theta so
            // the next epoch's gradient is computed against the
            // gauge-centered state.
            for (size_t p = 0; p < P; p++)
                adam.theta[p] = static_cast<double>(params[p].read());

            std::cerr << "adam epoch " << globalPass << " done, loss=" << bestLoss
                      << " rel-improvement=" << rel << "\n";
            double valLoss = reportVal("adam", globalPass);
            bool improved = rel >= 1e-7;
            improved = applyValGate(valLoss, globalPass, "adam", improved);
            if (!improved) rel = 0.0;
            writeCheckpoint("tuning/checkpoint.txt", params);

            if (rel < 1e-7) {
                if (++stalled >= 4) {
                    std::cerr << "adam convergence; switching to coordinate descent\n";
                    epoch++;
                    globalPass++;
                    break;
                }
            } else {
                stalled = 0;
            }

            // Optional periodic K refit, mirroring the GN / Newton /
            // CD cadence. Adam itself doesn't refit K; we leave it to
            // the operator's `--refit-k-every` to keep K aligned with
            // the moving params, with the rebase helper handling the
            // val gate side.
            if (refitKEvery > 0 && (epoch + 1) % refitKEvery == 0) {
                std::cerr << "refit K after adam epoch " << globalPass << "\n";
                double oldK = K;
                K = findBestK(positions, numThreads, trainPtr);
                bestLoss = computeLoss(positions, K, numThreads, trainPtr);
                std::cerr << "K " << oldK << " -> " << K << ", rebased loss=" << bestLoss << "\n";
                rebaseBestValLoss("K refit");
            }
            if (refreshLeavesEvery > 0 && (epoch + 1) % refreshLeavesEvery == 0) {
                std::cerr << "refresh leaves after adam epoch " << globalPass << "\n";
                precomputeLeaves(positions, numThreads, leafDepth);
                double oldK = K;
                K = findBestK(positions, numThreads, trainPtr);
                bestLoss = computeLoss(positions, K, numThreads, trainPtr);
                std::cerr << "post-refresh K " << oldK << " -> " << K
                          << ", rebased loss=" << bestLoss << "\n";
                rebaseBestValLoss("leaf refresh");
                // New leaves invalidate the linearization the feature
                // cache was extracted around; re-extract before the
                // next Adam epoch.
                cf = extractCorpusFeatures(positions, params, numThreads);
                for (size_t p = 0; p < P; p++)
                    adam.theta[p] = static_cast<double>(params[p].read());
            }
        }
    }

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
    //
    // Threshold strategy: threaded passes use a slightly looser
    // relative threshold (1e-7) to avoid chasing improvements smaller
    // than the residual noise floor of the parallel loss. Once the
    // ladder bails out, a deterministic single-thread finalizer at
    // 1e-8 picks up any improvements that survive the tighter cut.
    const double relThresholdThreaded = 1e-7;
    const double relThresholdDeterministic = 1e-8;
    static const std::array<int, 4> stepLadder = {8, 4, 2, 1};

    // Feature-cached CD state. Built once before the CD ladder and
    // reused across both the ladder and the finalizer. Replaces the
    // full-corpus computeLoss() per step attempt with an O(|positions
    // touching this param|) delta-loss computation -- ~5-10x speedup
    // in aggregate, ~80x for sparse params.
    //
    // We use the feature cache only when it's available (Gauss-Newton
    // or Adam ran beforehand and we have a populated cf). For the
    // legacy path where neither phase is configured, the cache is
    // built on-demand below; that costs one extra
    // extractCorpusFeatures call (~5 min on the full corpus) but
    // pays back many times over in CD speedup.
    if (!cfInitialized) {
        cf = extractCorpusFeatures(positions, params, numThreads);
        cfInitialized = true;
    }
    // Resolve the train slice once. When the corpus has no
    // train/val split (`trainPtr == nullptr`) we fall back to an
    // identity index over the full corpus so the rest of the cached
    // path can treat the slice uniformly.
    std::vector<size_t> identityTrain;
    auto cdSliceRef = [&]() -> const std::vector<size_t> & {
        if (trainPtr) return trainIndices;
        if (identityTrain.empty()) {
            identityTrain.resize(positions.size());
            for (size_t i = 0; i < positions.size(); i++) identityTrain[i] = i;
        }
        return identityTrain;
    };
    CDFeatureState cdState;
    {
        const std::vector<size_t> &slice = cdSliceRef();
        std::cerr << "building CD feature cache (" << slice.size() << " train positions x "
                  << params.size() << " params)\n";
        buildInvertedIndex(cdState.inv, cf, slice, params.size());
        std::cerr << "  inverted index: " << cdState.inv.positions.size()
                  << " (position, coef) entries\n";
        initCDFeatureState(cdState, positions, cf, params, K, numThreads, slice);
        // Sanity: cdState.totalLoss should match the bestLoss the
        // val gate / Newton phases computed via the engine path. Any
        // drift here would be a feature-cache bug, not a tuner bug.
        bestLoss = cdState.totalLoss;
    }

    // Per-pass body. Returns true iff at least one scalar moved. The
    // log line format is identical to the prior strict CPW output so
    // `--replay` keeps reconstructing checkpoints from a tune.log
    // without caring whether the line came from a threaded or a
    // deterministic pass.
    auto runPass = [&](int pass, int passNumThreads, double relThreshold) {
        bool improved = false;
        for (size_t pi = 0; pi < params.size(); pi++) {
            auto &p = params[pi];
            int original = p.read();
            double threshold = bestLoss * relThreshold;
            bool accepted = false;

            for (int step : stepLadder) {
                if (p.allow(original + step)) {
                    p.write(original + step);
                    if (cdFeatureTryStep(cdState, static_cast<int>(pi), step, threshold,
                                         passNumThreads)) {
                        bestLoss = cdState.totalLoss;
                        improved = true;
                        accepted = true;
                        std::cerr << "  pass " << pass << " " << p.name << ": " << original
                                  << " -> " << (original + step) << " loss=" << bestLoss << "\n";
                        break;
                    }
                    p.write(original);
                }
                if (p.allow(original - step)) {
                    p.write(original - step);
                    if (cdFeatureTryStep(cdState, static_cast<int>(pi), -step, threshold,
                                         passNumThreads)) {
                        bestLoss = cdState.totalLoss;
                        improved = true;
                        accepted = true;
                        std::cerr << "  pass " << pass << " " << p.name << ": " << original
                                  << " -> " << (original - step) << " loss=" << bestLoss << "\n";
                        break;
                    }
                    p.write(original);
                }
            }

            (void)accepted;
        }
        return improved;
    };

    resetPatience();
    for (int pass = 0; pass < maxPasses; pass++, globalPass++) {
        bool improved = runPass(globalPass, numThreads, relThresholdThreaded);
        // Canonicalize PST/material gauge so the per-term values stay
        // interpretable. Bit-identical eval, and the next pass picks
        // up from the centered point.
        centerPSTGauge();
        std::cerr << "pass " << globalPass << " done, loss=" << bestLoss
                  << (improved ? " (improved)" : " (no change)") << "\n";
        double valLoss = reportVal("cd", globalPass);
        improved = applyValGate(valLoss, globalPass, "cd", improved);
        writeCheckpoint("tuning/checkpoint.txt", params);
        if (!improved) break;

        // Periodic K refit: as parameters move, the sigmoid scale that
        // best fits the current eval distribution drifts. Refitting K
        // every few passes keeps the loss surface honest. Cheap (one
        // golden-section search, ~40 loss evals) compared to a CD
        // pass.
        if (refitKEvery > 0 && (pass + 1) % refitKEvery == 0) {
            std::cerr << "refit K after pass " << globalPass << "\n";
            double oldK = K;
            K = findBestK(positions, numThreads, trainPtr);
            bestLoss = computeLoss(positions, K, numThreads, trainPtr);
            std::cerr << "K " << oldK << " -> " << K << ", rebased loss=" << bestLoss << "\n";
            rebaseBestValLoss("K refit");
            // K refit changes the sigmoid scale, so every cached
            // residual in the CD state is wrong under the new K.
            // Resync rebuilds eval / residual / dataLoss from scratch
            // -- cheap (one threaded corpus walk).
            initCDFeatureState(cdState, positions, cf, params, K, numThreads, cdSliceRef());
            bestLoss = cdState.totalLoss;
        }

        // Periodic leaf refresh: qsearch's path through stand-pat and
        // move ordering depends on the current eval params, so leaves
        // computed against the cold-start params drift as the tune
        // moves. Recomputing leaves restores consistency between the
        // labels and the evaluator that fits them. Expensive (full
        // precompute over the whole corpus), so default off.
        if (refreshLeavesEvery > 0 && (pass + 1) % refreshLeavesEvery == 0) {
            std::cerr << "refresh leaves after pass " << globalPass << "\n";
            precomputeLeaves(positions, numThreads, leafDepth);
            // Re-fit K against the new leaves and rebase loss.
            double oldK = K;
            K = findBestK(positions, numThreads, trainPtr);
            bestLoss = computeLoss(positions, K, numThreads, trainPtr);
            std::cerr << "post-refresh K " << oldK << " -> " << K << ", rebased loss=" << bestLoss
                      << "\n";
            rebaseBestValLoss("leaf refresh");
            // New leaves invalidate cf (the linearization was around
            // the old leaves) and the CD state. Re-extract cf, rebuild
            // the inverted index, and resync state from scratch.
            cf = extractCorpusFeatures(positions, params, numThreads);
            buildInvertedIndex(cdState.inv, cf, cdSliceRef(), params.size());
            initCDFeatureState(cdState, positions, cf, params, K, numThreads, cdSliceRef());
            bestLoss = cdState.totalLoss;
        }
    }

    // Tight-threshold finalizer. The main passes use 1e-7 to skim above
    // the (now small, since pawn / material hashes are thread_local)
    // floating-point summation noise; the finalizer uses 1e-8 to
    // recover smaller real improvements. With thread_local hashes the
    // loss is bit-deterministic for a given thread count, so we keep
    // running at the user-requested numThreads instead of dropping to
    // single-thread; the result is the same canonical convergence
    // point at far less wall time. Loops until a pass produces no
    // movement. Global pass numbering continues so log replay stays
    // valid.
    std::cerr << "tight-threshold finalizer at " << relThresholdDeterministic
              << " (numThreads=" << numThreads << ")\n";
    // The CD state's totalLoss is already authoritative for the
    // current params (incrementally maintained through every accepted
    // step in the ladder). Reuse it instead of paying for another
    // full-corpus computeLoss().
    bestLoss = cdState.totalLoss;
    std::cerr << "finalizer baseline loss: " << bestLoss << "\n";
    resetPatience();
    for (int finalPass = 0; finalPass < maxPasses; finalPass++, globalPass++) {
        bool improved = runPass(globalPass, numThreads, relThresholdDeterministic);
        // Canonicalize PST/material gauge so the per-term values stay
        // interpretable. Bit-identical eval, and the next pass picks
        // up from the centered point.
        centerPSTGauge();
        std::cerr << "pass " << globalPass << " done, loss=" << bestLoss
                  << (improved ? " (improved)" : " (no change)") << "\n";
        double valLoss = reportVal("cd-final", globalPass);
        improved = applyValGate(valLoss, globalPass, "cd-final", improved);
        writeCheckpoint("tuning/checkpoint.txt", params);
        if (!improved) break;
    }

    // End-of-tune: restore the param vector to the best-val snapshot
    // we observed along the trajectory, re-center the gauge, and
    // overwrite the live checkpoint so the dump that follows reflects
    // the most-generalising point rather than wherever the last pass
    // happened to land. No-op when val tracking was off (legacy
    // pipeline) or when the very first val measurement was the best
    // (params already match the snapshot).
    if (valPtr && valGateEnabled && !bestValSnapshot.empty()) {
        bool needsRestore = false;
        for (size_t i = 0; i < params.size(); i++) {
            if (params[i].read() != bestValSnapshot[i]) {
                needsRestore = true;
                break;
            }
        }
        if (needsRestore) {
            restoreParams(bestValSnapshot);
            centerPSTGauge();
            bestLoss = computeLoss(positions, K, numThreads, trainPtr);
            std::cerr << "tune complete: restored params to best-val snapshot, val_loss="
                      << bestValLoss << " train_loss=" << bestLoss << "\n";
            writeCheckpoint("tuning/checkpoint.txt", params);
        } else {
            std::cerr << "tune complete: final params already match best-val snapshot, val_loss="
                      << bestValLoss << "\n";
        }
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

static void printPST64(const std::string &name, const Score *arr) {
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

static void printPST32(const std::string &name, const Score *arr) {
    std::cout << "    // " << name << " (half-board)\n";
    std::cout << "    {\n";
    for (int row = 0; row < 8; row++) {
        std::cout << "        ";
        for (int col = 0; col < 4; col++) {
            std::cout << fmtScore(arr[row * 4 + col]);
            if (row != 7 || col != 3) std::cout << ",";
            if (col < 3) std::cout << " ";
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
    std::cout << "    " << fmtScore(evalParams.BadBishop) << ", // BadBishop\n";
    std::cout << "    " << fmtScore(evalParams.BishopPawns) << ", // BishopPawns\n";
    std::cout << "    " << fmtScore(evalParams.Tempo) << ", // Tempo\n";

    // PieceScore
    std::cout << "    {";
    for (int i = 0; i < 7; i++) {
        std::cout << fmtScore(evalParams.PieceScore[i]);
        if (i < 6) std::cout << ", ";
    }
    std::cout << "}, // PieceScore\n";

    printPST64("PawnPST", evalParams.PawnPST);
    printPST32("KnightPST", evalParams.KnightPST);
    printPST32("BishopPST", evalParams.BishopPST);
    printPST32("RookPST", evalParams.RookPST);
    printPST32("QueenPST", evalParams.QueenPST);
    printPST32("KingPST", evalParams.KingPST);

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
    std::cout << "    " << fmtScore(evalParams.RookOnQueenFile) << ", // RookOnQueenFile\n";
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

    std::cout << "    {\n";
    for (int d = 0; d < 4; d++) {
        std::cout << "        {";
        for (int r = 0; r < 7; r++) {
            std::cout << fmtScore(evalParams.Shelter[d][r]);
            if (r < 6) std::cout << ", ";
        }
        std::cout << "}";
        if (d < 3) std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "    }, // Shelter\n";
    std::cout << "    {\n";
    for (int d = 0; d < 4; d++) {
        std::cout << "        {";
        for (int r = 0; r < 7; r++) {
            std::cout << fmtScore(evalParams.UnblockedStorm[d][r]);
            if (r < 6) std::cout << ", ";
        }
        std::cout << "}";
        if (d < 3) std::cout << ",";
        std::cout << "\n";
    }
    std::cout << "    }, // UnblockedStorm\n";
    std::cout << "    {";
    for (int r = 0; r < 7; r++) {
        std::cout << fmtScore(evalParams.BlockedStorm[r]);
        if (r < 6) std::cout << ", ";
    }
    std::cout << "}, // BlockedStorm\n";
    std::cout << "    " << fmtScore(evalParams.UndefendedKingZoneSq)
              << ", // UndefendedKingZoneSq\n";
    std::cout << "    " << fmtScore(evalParams.KingMobilityFactor)
              << ", // KingMobilityFactor\n";

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
    std::cout << "    " << fmtScore(evalParams.BishopXrayPawns) << ", // BishopXrayPawns\n";
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
    std::cout << "    " << fmtScore(evalParams.ThreatByPawnPush) << ", // ThreatByPawnPush\n";
    std::cout << "    " << fmtScore(evalParams.WeakQueenDefender) << ", // WeakQueenDefender\n";
    std::cout << "    " << fmtScore(evalParams.KnightOnQueen) << ", // KnightOnQueen\n";
    std::cout << "    " << fmtScore(evalParams.PawnlessFlank) << ", // PawnlessFlank\n";
    std::cout << "    " << fmtScore(evalParams.QueenInfiltration) << ", // QueenInfiltration\n";
    std::cout << "    " << fmtScore(evalParams.KingPawnDistEg) << ", // KingPawnDistEg\n";
    std::cout << "    " << fmtScore(evalParams.KBNKCornerEg) << ", // KBNKCornerEg\n";
    std::cout << "    " << fmtScore(evalParams.LucenaEg) << ", // LucenaEg\n";
    std::cout << "    " << fmtScore(evalParams.KXKPushToEdge) << ", // KXKPushToEdge\n";
    std::cout << "    " << fmtScore(evalParams.KXKPushClose) << ", // KXKPushClose\n";
    std::cout << "    " << fmtScore(evalParams.KBNKPushClose) << ", // KBNKPushClose\n";
    std::cout << "    " << fmtScore(evalParams.KQKRPushToEdge) << ", // KQKRPushToEdge\n";
    std::cout << "    " << fmtScore(evalParams.KQKRPushClose) << ", // KQKRPushClose\n";
    std::cout << "    " << fmtScore(evalParams.KPsKFortressScale) << ", // KPsKFortressScale\n";
    std::cout << "    " << fmtScore(evalParams.KBPKNDrawishScale) << ", // KBPKNDrawishScale\n";
    std::cout << "    " << fmtScore(evalParams.KRKPDrawishScale) << ", // KRKPDrawishScale\n";
    std::cout << "    " << fmtScore(evalParams.KRKMinorScale) << ", // KRKMinorScale\n";
    std::cout << "    " << fmtScore(evalParams.KNNKDrawScale) << ", // KNNKDrawScale\n";
    std::cout << "    " << fmtScore(evalParams.EscapableThreatScale)
              << ", // EscapableThreatScale\n";
    std::cout << "};\n";
}

} // namespace

int main(int argc, char **argv) {
    auto usage = [] {
        std::cerr << "usage: tune [--from <ckpt>] [--refit-k-every N] "
                     "[--refresh-leaves-every N]\n";
        std::cerr << "            [--newton-passes N] [--gauss-newton {0,1}]\n";
        std::cerr << "            [--adam-epochs N] [--adam-lr X]\n";
        std::cerr << "            [--val-fraction X] [--val-gate-warmup N] [--val-gate-patience N]\n";
        std::cerr << "            [--val-gate] [--leaf-depth N]\n";
        std::cerr << "            <dataset> [threads=6] [maxPasses=30]\n";
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
        // Canonicalize before printing: project violators onto the
        // current bounds, validate (warn-only so the dump still emits
        // a copy-paste-ready snapshot even on a slightly stale ckpt),
        // then center the PST/material gauge so the printed values are
        // in the same canonical form `tune()` produces between passes.
        // The eval is bit-identical to the input checkpoint after both
        // operations.
        projectToConstraints(params);
        validateConstraints(params, /*fatal=*/false);
        centerPSTGauge();
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

    // Optional flag prefixes before the positional <dataset> arg. Each
    // is independently optional and order-insensitive among themselves.
    std::string fromCheckpoint;
    int refitKEvery = 0;        // refit K every N completed passes; 0 disables
    int refreshLeavesEvery = 0; // recompute leaves every N passes; 0 disables
    int newtonPasses = 0;       // run N Newton-style passes before CD; 0 disables
    bool useGaussNewton = true; // true: Gauss-Newton, false: diagonal Newton
    double valFraction = 0.10;  // game-id-based train / val split fraction
    // Default warmup: VAL_GATE_WARMUP env var, else 5 passes. The
    // warmup window lets the initial coarse moves settle before the
    // gate's patience counter starts ticking on non-improving passes.
    int valGateWarmup = [] {
        const char *env = std::getenv("VAL_GATE_WARMUP");
        if (!env) return 5;
        char *endp = nullptr;
        long v = std::strtol(env, &endp, 10);
        return (endp == env) ? 5 : static_cast<int>(v);
    }();
    // Patience: number of consecutive post-warmup passes without val
    // improvement before the loop breaks. Default 8 so a couple of
    // noisy passes do not chop off real later improvements; the
    // best-val snapshot is restored on exit regardless of where the
    // loop terminated.
    int valGatePatience = [] {
        const char *env = std::getenv("VAL_GATE_PATIENCE");
        if (!env) return 8;
        char *endp = nullptr;
        long v = std::strtol(env, &endp, 10);
        return (endp == env) ? 8 : static_cast<int>(v);
    }();
    bool valGateEnabled = false;
    // PV walk depth for the leaf precompute. Default 0 keeps the
    // existing qsearch-only behaviour; positive values re-enter
    // alpha-beta from the root and walk the PV to its terminal
    // before falling into qsearch. Read once from CLI / env var so
    // the operator does not have to recompile to swap modes.
    int leafDepth = [] {
        const char *env = std::getenv("LEAF_DEPTH");
        if (!env) return 0;
        char *endp = nullptr;
        long v = std::strtol(env, &endp, 10);
        return (endp == env) ? 0 : static_cast<int>(v);
    }();
    // Adam fine-tuning between GN and CD. Default 100 epochs at lr=1.0
    // which is standard for Texel-style sparse-feature gradient
    // optimisation; setting epochs=0 falls through directly from GN
    // to CD (legacy behaviour).
    int adamEpochs = [] {
        const char *env = std::getenv("ADAM_EPOCHS");
        if (!env) return 100;
        char *endp = nullptr;
        long v = std::strtol(env, &endp, 10);
        return (endp == env) ? 100 : static_cast<int>(v);
    }();
    double adamLr = [] {
        const char *env = std::getenv("ADAM_LR");
        if (!env) return 1.0;
        char *endp = nullptr;
        double v = std::strtod(env, &endp);
        return (endp == env) ? 1.0 : v;
    }();
    int argIdx = 1;
    while (argIdx < argc) {
        std::string a = argv[argIdx];
        if (a == "--from" && argIdx + 1 < argc) {
            fromCheckpoint = argv[argIdx + 1];
            argIdx += 2;
        } else if (a == "--refit-k-every" && argIdx + 1 < argc) {
            refitKEvery = std::atoi(argv[argIdx + 1]);
            argIdx += 2;
        } else if (a == "--refresh-leaves-every" && argIdx + 1 < argc) {
            refreshLeavesEvery = std::atoi(argv[argIdx + 1]);
            argIdx += 2;
        } else if (a == "--newton-passes" && argIdx + 1 < argc) {
            newtonPasses = std::atoi(argv[argIdx + 1]);
            argIdx += 2;
        } else if (a == "--gauss-newton" && argIdx + 1 < argc) {
            useGaussNewton = std::atoi(argv[argIdx + 1]) != 0;
            argIdx += 2;
        } else if (a == "--adam-epochs" && argIdx + 1 < argc) {
            adamEpochs = std::atoi(argv[argIdx + 1]);
            argIdx += 2;
        } else if (a == "--adam-lr" && argIdx + 1 < argc) {
            adamLr = std::atof(argv[argIdx + 1]);
            argIdx += 2;
        } else if (a == "--val-fraction" && argIdx + 1 < argc) {
            valFraction = std::atof(argv[argIdx + 1]);
            argIdx += 2;
        } else if (a == "--val-gate-warmup" && argIdx + 1 < argc) {
            valGateWarmup = std::atoi(argv[argIdx + 1]);
            argIdx += 2;
        } else if (a == "--val-gate-patience" && argIdx + 1 < argc) {
            valGatePatience = std::atoi(argv[argIdx + 1]);
            argIdx += 2;
        } else if (a == "--val-gate") {
            valGateEnabled = true;
            argIdx += 1;
        } else if (a == "--leaf-depth" && argIdx + 1 < argc) {
            leafDepth = std::atoi(argv[argIdx + 1]);
            argIdx += 2;
        } else {
            break;
        }
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
        // Center the PST/material gauge so the first post-resume pass
        // starts from the same canonical form `tune()` produces between
        // passes; loss is bit-identical, but the gauge stops drifting.
        projectToConstraints(params);
        validateConstraints(params);
        centerPSTGauge();
    }

    std::cerr << "loading " << dataset << "...\n";
    auto positions = loadDataset(dataset);
    std::cerr << "loaded " << positions.size() << " positions\n";
    // Per-game inverse weighting so a 250-ply draw stops out-voting a
    // sharp 30-move win in coordinate descent. Renormalises the corpus
    // so the sum of weights equals position count, keeping K refit and
    // absolute loss numbers comparable to the unweighted pipeline.
    // No-op (every weight stays 1.0) on legacy corpora that lack the
    // game id metadata field.
    assignGameWeights(positions);

    // Held-out validation slice. Two paths:
    //
    //   1. **External master val** (preferred): if `VAL_EPD` is set or
    //      the default file `tuning/val/master_positions.epd` exists,
    //      load that as the entire val partition. The in-corpus
    //      self-play stays wholly in train. This tests generalisation
    //      to a *different distribution* (master games at unseen
    //      strength), which catches overfitting that in-corpus splits
    //      cannot -- e.g. an engine that systematically avoids 3.Nc3
    //      in self-play has zero coverage of the resulting French
    //      mainlines under any same-distribution split, but a master
    //      val partition contains them by construction.
    //
    //   2. **In-corpus stratified split** (fallback): the (phase x
    //      result) stratified split, used when no external val EPD is
    //      available (no internet on first run, fetch script failed,
    //      etc.). This still tests generalisation to *unseen games*
    //      within the self-play distribution.
    //
    // valFraction <= 0 disables the in-corpus fallback's split (all
    // positions go to train, val empty, gate off); external val is
    // unaffected by valFraction.
    std::vector<size_t> trainIndices;
    std::vector<size_t> valIndices;

    auto resolveValEpd = [](std::string &out) -> bool {
        const char *env = std::getenv("VAL_EPD");
        if (env && *env) {
            out = env;
            std::ifstream check(out);
            return check.good();
        }
        // Default location populated by scripts/fetch_val_corpus.sh.
        const std::string defaultPath = "tuning/val/master_positions.epd";
        std::ifstream check(defaultPath);
        if (check.good()) {
            out = defaultPath;
            return true;
        }
        return false;
    };

    std::string valEpdPath;
    if (resolveValEpd(valEpdPath)) {
        std::cerr << "loading external val corpus from " << valEpdPath << "...\n";
        auto valCorpus = loadDataset(valEpdPath);
        if (valCorpus.empty()) {
            std::cerr << "warning: external val EPD at " << valEpdPath
                      << " is empty; falling back to in-corpus stratified split\n";
            auto split = splitCorpus(positions, valFraction, /*seed=*/0xc0ffeeULL);
            trainIndices = std::move(split.first);
            valIndices = std::move(split.second);
        } else {
            // Per-game inverse weighting on the val corpus so a 200-ply
            // master draw doesn't out-vote a sharp 25-move tactical
            // win inside the val partition. Game-ids in master corpora
            // come from the extract pipeline; no-metadata legacy files
            // get default weight 1.0 from loadDataset.
            assignGameWeights(valCorpus);

            const size_t valStart = positions.size();
            positions.reserve(valStart + valCorpus.size());
            for (auto &lp : valCorpus) {
                positions.push_back(std::move(lp));
            }

            trainIndices.reserve(valStart);
            for (size_t i = 0; i < valStart; i++) trainIndices.push_back(i);
            valIndices.reserve(positions.size() - valStart);
            for (size_t i = valStart; i < positions.size(); i++) valIndices.push_back(i);

            std::cerr << "external val corpus: " << trainIndices.size()
                      << " train (in-corpus self-play), " << valIndices.size()
                      << " val (master positions, distribution-shift sanity check)\n";
        }
    } else {
        std::cerr << "no external val corpus found; falling back to in-corpus "
                     "stratified split (run scripts/fetch_val_corpus.sh to "
                     "enable external val)\n";
        auto split = splitCorpus(positions, valFraction, /*seed=*/0xc0ffeeULL);
        trainIndices = std::move(split.first);
        valIndices = std::move(split.second);
    }

    // Optional curated EPD pack. CURATED_EPD points to a small balanced
    // position file (same `FEN | result` parser as the main corpus,
    // game-id metadata not expected). Curated rows always go to the
    // training set -- never the validation slice -- because the whole
    // point is to bias the gradient toward positions the operator
    // hand-selected. Per-position weight defaults to 5.0 so a 100-row
    // pack punches above its row count without dominating a 5M
    // corpus; CURATED_WEIGHT in the env raises or lowers that.
    if (const char *curatedPath = std::getenv("CURATED_EPD")) {
        if (*curatedPath) {
            double curatedWeight = 5.0;
            if (const char *wEnv = std::getenv("CURATED_WEIGHT")) {
                char *endp = nullptr;
                double v = std::strtod(wEnv, &endp);
                if (endp != wEnv) curatedWeight = v;
            }
            std::cerr << "loading curated pack from " << curatedPath << "...\n";
            auto curated = loadDataset(curatedPath);
            const size_t baseIdx = positions.size();
            for (auto &lp : curated) {
                lp.gameIds.clear();
                lp.weight = curatedWeight;
                positions.push_back(std::move(lp));
            }
            for (size_t i = baseIdx; i < positions.size(); i++) {
                trainIndices.push_back(i);
            }
            std::cerr << "loaded " << (positions.size() - baseIdx) << " curated positions"
                      << ", weight=" << curatedWeight << " each (training only)\n";
        }
    }

    precomputeLeaves(positions, numThreads, leafDepth);

    std::cerr << "finding K...\n";
    const std::vector<size_t> *trainPtr =
        trainIndices.empty() ? nullptr : &trainIndices;
    double K = findBestK(positions, numThreads, trainPtr);
    std::cerr << "K=" << K << "\n";

    tune(positions, K, numThreads, maxPasses, refitKEvery, refreshLeavesEvery, newtonPasses,
         useGaussNewton, adamEpochs, adamLr, trainIndices, valIndices, valGateWarmup,
         valGatePatience, valGateEnabled, leafDepth);
    printCurrentValues();
    return 0;
}
