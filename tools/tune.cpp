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
    out.push_back({"InitiativePasser.eg", &evalParams.InitiativePasser, false, boundsNonNegative()});
    out.push_back({"InitiativePawnCount.eg", &evalParams.InitiativePawnCount, false,
                   boundsNonNegative()});
    out.push_back({"InitiativeOutflank.eg", &evalParams.InitiativeOutflank, false,
                   boundsNonNegative()});
    out.push_back({"InitiativeInfiltrate.eg", &evalParams.InitiativeInfiltrate, false,
                   boundsNonNegative()});
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
    // hashes are now thread_local; each std::thread we spawn below gets
    // a freshly-initialized empty table on first use inside that thread
    // and the table is destroyed when the thread exits at the end of
    // this loss eval. So no clearing is needed and no two threads ever
    // touch the same hash entry, removing the race that previously sat
    // under our 1e-8 acceptance threshold.
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
static void precomputeLeaves(std::vector<LabeledPosition> &positions, int numThreads) {
    std::cerr << "precomputing qsearch leaves for " << positions.size() << " positions on "
              << numThreads << " threads...\n";
    resetQsearchLeafCounters();

    // Workers share an atomic cursor so any uneven per-position cost
    // (some leaves walk a long capture chain, others stand-pat at the
    // root) load-balances naturally. Each worker has its own
    // thread_local TT inside qsearchLeafBoard, so there is no
    // contention on the table itself.
    std::atomic<size_t> nextIndex{0};
    std::atomic<size_t> nextReport{50000};
    const size_t total = positions.size();

    auto worker = [&]() {
        while (true) {
            size_t i = nextIndex.fetch_add(1, std::memory_order_relaxed);
            if (i >= total) break;
            positions[i].board = qsearchLeafBoard(positions[i].board);
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
                                      double K, int numThreads) {
    const size_t N = positions.size();
    const size_t P = theta.size();

    std::vector<int> deltaTheta(P);
    for (size_t j = 0; j < P; j++)
        deltaTheta[j] = theta[j] - cf.theta0[j];

    std::vector<double> partial(numThreads, 0.0);
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (int t = 0; t < numThreads; t++) {
        size_t start = (N * t) / numThreads;
        size_t end = (N * (t + 1)) / numThreads;
        threads.emplace_back([&, start, end, t]() {
            double sum = 0.0;
            for (size_t i = start; i < end; i++) {
                int64_t eval = cf.baseline[i];
                for (const auto &fe : cf.rows[i])
                    eval += static_cast<int64_t>(deltaTheta[fe.paramIdx]) * fe.coef;
                double pred = sigmoid(static_cast<double>(eval), K);
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
    return total / static_cast<double>(N);
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
                              int numThreads, double &bestLoss) {
    const size_t N = positions.size();
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
                    int64_t eval = cf.baseline[i];
                    const auto &row = cf.rows[i];
                    for (const auto &fe : row)
                        eval += static_cast<int64_t>(deltaTheta[fe.paramIdx]) * fe.coef;
                    double pred = sigmoid(static_cast<double>(eval), K);
                    double residual = pred - positions[i].result;
                    double sigprime = pred * (1.0 - pred);
                    double weightR = K * sigprime * residual;
                    double weightH = K * K * sigprime * sigprime;
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
        double L = computeLoss(positions, K, numThreads);
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
                         double &bestLoss) {
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
            Lp = computeLoss(positions, K, numThreads);
        }
        if (canMinus) {
            p.write(orig - delta);
            Lm = computeLoss(positions, K, numThreads);
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
        double L = computeLoss(positions, K, numThreads);
        if (L0 - L > acceptThreshold) {
            bestLoss = L;
            std::cerr << "  newton accepted scale=" << scale << " loss=" << L << "\n";
            return (L0 - L) / L0;
        }
    }

    restore();
    return 0.0;
}

static void tune(std::vector<LabeledPosition> &positions, double K, int numThreads,
                 int maxPasses, int refitKEvery, int refreshLeavesEvery, int newtonPasses,
                 bool useGaussNewton) {
    auto params = collectParams();
    std::cerr << "tuning " << params.size() << " scalars across " << positions.size()
              << " positions with " << numThreads << " threads, K=" << K << "\n";

    projectToConstraints(params);
    validateConstraints(params);

    double bestLoss = computeLoss(positions, K, numThreads);
    std::cerr << "initial loss: " << bestLoss << "\n";

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
    if (newtonPasses > 0 && useGaussNewton) {
        std::cerr << "gauss-newton phase: up to " << newtonPasses << " passes\n";
        CorpusFeatures cf = extractCorpusFeatures(positions, params, numThreads);
        int stalled = 0;
        for (int pass = 0; pass < newtonPasses; pass++, globalPass++) {
            std::cerr << "gauss-newton pass " << globalPass << " starting (loss=" << bestLoss
                      << ")\n";
            double rel = gaussNewtonPass(positions, cf, params, K, numThreads, bestLoss);
            centerPSTGauge();
            std::cerr << "gauss-newton pass " << globalPass << " done, loss=" << bestLoss
                      << " rel-improvement=" << rel << "\n";
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
                K = findBestK(positions, numThreads);
                bestLoss = computeLoss(positions, K, numThreads);
                std::cerr << "K " << oldK << " -> " << K << ", rebased loss=" << bestLoss << "\n";
            }
            if (refreshLeavesEvery > 0 && (pass + 1) % refreshLeavesEvery == 0) {
                std::cerr << "refresh leaves after gauss-newton pass " << globalPass << "\n";
                precomputeLeaves(positions, numThreads);
                double oldK = K;
                K = findBestK(positions, numThreads);
                bestLoss = computeLoss(positions, K, numThreads);
                std::cerr << "post-refresh K " << oldK << " -> " << K
                          << ", rebased loss=" << bestLoss << "\n";
                // Re-extract features against the new qsearch leaves.
                // Without this the Newton step would optimize against
                // stale linearizations.
                cf = extractCorpusFeatures(positions, params, numThreads);
            }
        }
    } else if (newtonPasses > 0) {
        std::cerr << "newton phase: up to " << newtonPasses << " passes\n";
        int stalled = 0;
        for (int pass = 0; pass < newtonPasses; pass++, globalPass++) {
            std::cerr << "newton pass " << globalPass << " starting (loss=" << bestLoss << ")\n";
            double rel = newtonPass(positions, params, K, numThreads, bestLoss);
            centerPSTGauge();
            std::cerr << "newton pass " << globalPass << " done, loss=" << bestLoss
                      << " rel-improvement=" << rel << "\n";
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
                K = findBestK(positions, numThreads);
                bestLoss = computeLoss(positions, K, numThreads);
                std::cerr << "K " << oldK << " -> " << K << ", rebased loss=" << bestLoss << "\n";
            }
            if (refreshLeavesEvery > 0 && (pass + 1) % refreshLeavesEvery == 0) {
                std::cerr << "refresh leaves after newton pass " << globalPass << "\n";
                precomputeLeaves(positions, numThreads);
                double oldK = K;
                K = findBestK(positions, numThreads);
                bestLoss = computeLoss(positions, K, numThreads);
                std::cerr << "post-refresh K " << oldK << " -> " << K
                          << ", rebased loss=" << bestLoss << "\n";
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
                    double loss = computeLoss(positions, K, passNumThreads);
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
                    double loss = computeLoss(positions, K, passNumThreads);
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
        return improved;
    };

    for (int pass = 0; pass < maxPasses; pass++, globalPass++) {
        bool improved = runPass(globalPass, numThreads, relThresholdThreaded);
        // Canonicalize PST/material gauge so the per-term values stay
        // interpretable. Bit-identical eval, and the next pass picks
        // up from the centered point.
        centerPSTGauge();
        std::cerr << "pass " << globalPass << " done, loss=" << bestLoss
                  << (improved ? " (improved)" : " (no change)") << "\n";
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
            K = findBestK(positions, numThreads);
            bestLoss = computeLoss(positions, K, numThreads);
            std::cerr << "K " << oldK << " -> " << K << ", rebased loss=" << bestLoss << "\n";
        }

        // Periodic leaf refresh: qsearch's path through stand-pat and
        // move ordering depends on the current eval params, so leaves
        // computed against the cold-start params drift as the tune
        // moves. Recomputing leaves restores consistency between the
        // labels and the evaluator that fits them. Expensive (full
        // precompute over the whole corpus), so default off.
        if (refreshLeavesEvery > 0 && (pass + 1) % refreshLeavesEvery == 0) {
            std::cerr << "refresh leaves after pass " << globalPass << "\n";
            precomputeLeaves(positions, numThreads);
            // Re-fit K against the new leaves and rebase loss.
            double oldK = K;
            K = findBestK(positions, numThreads);
            bestLoss = computeLoss(positions, K, numThreads);
            std::cerr << "post-refresh K " << oldK << " -> " << K << ", rebased loss=" << bestLoss
                      << "\n";
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
    bestLoss = computeLoss(positions, K, numThreads);
    std::cerr << "finalizer baseline loss: " << bestLoss << "\n";
    for (int finalPass = 0; finalPass < maxPasses; finalPass++, globalPass++) {
        bool improved = runPass(globalPass, numThreads, relThresholdDeterministic);
        // Canonicalize PST/material gauge so the per-term values stay
        // interpretable. Bit-identical eval, and the next pass picks
        // up from the centered point.
        centerPSTGauge();
        std::cerr << "pass " << globalPass << " done, loss=" << bestLoss
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
    std::cout << "};\n";
}

} // namespace

int main(int argc, char **argv) {
    auto usage = [] {
        std::cerr << "usage: tune [--from <ckpt>] [--refit-k-every N] "
                     "[--refresh-leaves-every N]\n";
        std::cerr << "            [--newton-passes N] [--gauss-newton {0,1}]\n";
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
    int refitKEvery = 5;        // refit K every N completed passes; 0 disables
    int refreshLeavesEvery = 0; // recompute leaves every N passes; 0 disables
    int newtonPasses = 0;       // run N Newton-style passes before CD; 0 disables
    bool useGaussNewton = true; // true: Gauss-Newton, false: diagonal Newton
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

    precomputeLeaves(positions, numThreads);

    std::cerr << "finding K...\n";
    double K = findBestK(positions, numThreads);
    std::cerr << "K=" << K << "\n";

    tune(positions, K, numThreads, maxPasses, refitKEvery, refreshLeavesEvery, newtonPasses,
         useGaussNewton);
    printCurrentValues();
    return 0;
}
