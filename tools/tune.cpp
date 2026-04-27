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
    addMgEgConstr("KnightOutpostBonus", &evalParams.KnightOutpostBonus, boundsNonNegative());
    addMgEgConstr("BishopOutpostBonus", &evalParams.BishopOutpostBonus, boundsNonNegative());
    out.push_back({"TrappedRookByKingPenalty.mg", &evalParams.TrappedRookByKingPenalty, true,
                   boundsNonPositive()}); // mg only, must stay a penalty
    // Tarrasch's-rule rook-behind-passer: pure bonus on either side.
    addMgEgConstr("RookBehindOurPasserBonus", &evalParams.RookBehindOurPasserBonus,
                  boundsNonNegative());
    addMgEgConstr("RookBehindTheirPasserBonus", &evalParams.RookBehindTheirPasserBonus,
                  boundsNonNegative());
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

    // --- Pawn shield: both halves stay non-negative, plus a chain
    // [0] >= [1] (back-rank shield is at least as valuable as rank-3
    // shield). Both halves remain tunable so endgame data still gets
    // a say; the chain stops the tuner from treating a more advanced
    // shield as more valuable than the back-rank one.
    for (bool isMg : {true, false}) {
        out.push_back({isMg ? "PawnShieldBonus[0].mg" : "PawnShieldBonus[0].eg",
                       &evalParams.PawnShieldBonus[0], isMg, [isMg] {
                           Bounds b{0, 1000000};
                           int next = isMg ? mg_value(evalParams.PawnShieldBonus[1])
                                           : eg_value(evalParams.PawnShieldBonus[1]);
                           b.lo = std::max(b.lo, next);
                           return b;
                       }});
        out.push_back({isMg ? "PawnShieldBonus[1].mg" : "PawnShieldBonus[1].eg",
                       &evalParams.PawnShieldBonus[1], isMg, [isMg] {
                           Bounds b{0, 1000000};
                           int prev = isMg ? mg_value(evalParams.PawnShieldBonus[0])
                                           : eg_value(evalParams.PawnShieldBonus[0]);
                           b.hi = std::min(b.hi, prev);
                           return b;
                       }});
    }
    for (int i = 0; i < 5; i++) {
        // Consumed with `scores -= <Blocked|Unblocked>PawnStorm[idx]`, so
        // magnitudes must stay non-negative to preserve the "enemy advance
        // hurts us" prior.
        out.push_back({"BlockedPawnStorm[" + std::to_string(i) + "].mg",
                       &evalParams.BlockedPawnStorm[i], true, boundsNonNegative()});
        out.push_back({"UnblockedPawnStorm[" + std::to_string(i) + "].mg",
                       &evalParams.UnblockedPawnStorm[i], true, boundsNonNegative()});
    }
    // King-zone files: open file is at least as bad as semi-open; both
    // are penalties (<= 0). Chain enforced so the tuner cannot end up
    // with semi-open scoring worse than open.
    out.push_back({"SemiOpenFileNearKing.mg", &evalParams.SemiOpenFileNearKing, true, [] {
                       Bounds b{-1000000, 0};
                       b.lo = std::max(b.lo, mg_value(evalParams.OpenFileNearKing));
                       return b;
                   }});
    out.push_back({"OpenFileNearKing.mg", &evalParams.OpenFileNearKing, true, [] {
                       Bounds b{-1000000, 0};
                       b.hi = std::min(b.hi, mg_value(evalParams.SemiOpenFileNearKing));
                       return b;
                   }});
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

    // --- King-danger accumulator weights. Each per-attacker weight
    // feeds the quadratic king-danger term, so all are non-negative.
    // Cross-field piece ordering: queen attack is at least as dangerous
    // as rook, and rook is at least as dangerous as either minor. Same
    // ordering for safe-checks. Without these chains the tuner has been
    // pushing queen weight below rook on this corpus, which the chess
    // prior does not allow.
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
        // Knight and Bishop: bounded above by Rook (sibling minor floor
        // stays unconstrained so the two minors can rebalance freely).
        out.push_back({isMg ? "KingAttackByKnight.mg" : "KingAttackByKnight.eg",
                       &evalParams.KingAttackByKnight, isMg,
                       kingAttackBounds(&evalParams.KingAttackByKnight, nullptr,
                                        &evalParams.KingAttackByRook, isMg)});
        out.push_back({isMg ? "KingAttackByBishop.mg" : "KingAttackByBishop.eg",
                       &evalParams.KingAttackByBishop, isMg,
                       kingAttackBounds(&evalParams.KingAttackByBishop, nullptr,
                                        &evalParams.KingAttackByRook, isMg)});
        // Rook: at least max(Knight, Bishop), at most Queen.
        out.push_back({isMg ? "KingAttackByRook.mg" : "KingAttackByRook.eg",
                       &evalParams.KingAttackByRook, isMg, [isMg] {
                           Bounds b{0, 1000000};
                           int knight = isMg ? mg_value(evalParams.KingAttackByKnight)
                                             : eg_value(evalParams.KingAttackByKnight);
                           int bishop = isMg ? mg_value(evalParams.KingAttackByBishop)
                                             : eg_value(evalParams.KingAttackByBishop);
                           int queen = isMg ? mg_value(evalParams.KingAttackByQueen)
                                            : eg_value(evalParams.KingAttackByQueen);
                           b.lo = std::max({b.lo, knight, bishop});
                           b.hi = std::min(b.hi, queen);
                           return b;
                       }});
        // Queen: at least Rook (heaviest attacker bound).
        out.push_back({isMg ? "KingAttackByQueen.mg" : "KingAttackByQueen.eg",
                       &evalParams.KingAttackByQueen, isMg,
                       kingAttackBounds(&evalParams.KingAttackByQueen,
                                        &evalParams.KingAttackByRook, nullptr, isMg)});
    }
    // KingSafeCheck: same ordering by piece weight. Knight and Bishop
    // capped above by Rook; Rook between max(minors) and Queen; Queen
    // bounded below by Rook.
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
                       &evalParams.KingSafeCheck[3], isMg, [isMg] {
                           Bounds b{0, 1000000};
                           int rook = isMg ? mg_value(evalParams.KingSafeCheck[4])
                                           : eg_value(evalParams.KingSafeCheck[4]);
                           b.hi = std::min(b.hi, rook);
                           return b;
                       }});
        out.push_back({"KingSafeCheck[4]." + std::string(isMg ? "mg" : "eg"),
                       &evalParams.KingSafeCheck[4], isMg, [isMg] {
                           Bounds b{0, 1000000};
                           int knight = isMg ? mg_value(evalParams.KingSafeCheck[2])
                                             : eg_value(evalParams.KingSafeCheck[2]);
                           int bishop = isMg ? mg_value(evalParams.KingSafeCheck[3])
                                             : eg_value(evalParams.KingSafeCheck[3]);
                           int queen = isMg ? mg_value(evalParams.KingSafeCheck[5])
                                            : eg_value(evalParams.KingSafeCheck[5]);
                           b.lo = std::max({b.lo, knight, bishop});
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
static void precomputeLeaves(std::vector<LabeledPosition> &positions) {
    std::cerr << "precomputing qsearch leaves for " << positions.size() << " positions...\n";
    resetQsearchLeafCounters();
    size_t reported = 0;
    for (size_t i = 0; i < positions.size(); i++) {
        positions[i].board = qsearchLeafBoard(positions[i].board);
        if (i - reported >= 50000) {
            std::cerr << "  " << i << " leaves computed\n";
            reported = i;
        }
    }
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
    centerWithMaterial(evalParams.KnightPST, 0, 64, evalParams.PieceScore[Knight]);
    centerWithMaterial(evalParams.BishopPST, 0, 64, evalParams.PieceScore[Bishop]);
    centerWithMaterial(evalParams.RookPST, 0, 64, evalParams.PieceScore[Rook]);
    centerWithMaterial(evalParams.QueenPST, 0, 64, evalParams.PieceScore[Queen]);
    centerNoMaterial(evalParams.KingPST, 0, 64);
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

static void tune(std::vector<LabeledPosition> &positions, double K, int numThreads,
                 int maxPasses, int refitKEvery, int refreshLeavesEvery) {
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

    int globalPass = 0;
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
            precomputeLeaves(positions);
            // Re-fit K against the new leaves and rebase loss.
            double oldK = K;
            K = findBestK(positions, numThreads);
            bestLoss = computeLoss(positions, K, numThreads);
            std::cerr << "post-refresh K " << oldK << " -> " << K << ", rebased loss=" << bestLoss
                      << "\n";
        }
    }

    // Deterministic single-thread finalizer at the tighter threshold.
    // The threaded passes throw away small candidate improvements that
    // sit below their loss noise; deterministic passes recover them.
    // Loops until a finalizer pass produces no further movement. Pass
    // numbers continue the global sequence so log replay stays valid.
    std::cerr << "deterministic finalizer at threshold " << relThresholdDeterministic
              << " (single-thread)\n";
    bestLoss = computeLoss(positions, K, 1);
    std::cerr << "deterministic baseline loss: " << bestLoss << "\n";
    for (int finalPass = 0; finalPass < maxPasses; finalPass++, globalPass++) {
        bool improved = runPass(globalPass, /*passNumThreads=*/1, relThresholdDeterministic);
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
        std::cerr << "usage: tune [--from <ckpt>] [--refit-k-every N] "
                     "[--refresh-leaves-every N]\n";
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

    precomputeLeaves(positions);

    std::cerr << "finding K...\n";
    double K = findBestK(positions, numThreads);
    std::cerr << "K=" << K << "\n";

    tune(positions, K, numThreads, maxPasses, refitKEvery, refreshLeavesEvery);
    printCurrentValues();
    return 0;
}
