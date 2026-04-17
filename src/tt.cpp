#include "tt.h"

#include <algorithm>
#include <cassert>
#include <cstdint>

// Lock the TT entry layout: padding changes would silently alter the ratio of
// entries per megabyte and invalidate hash-sizing assumptions.
static_assert(sizeof(TTEntry) == 32, "TTEntry layout unexpectedly changed");
static_assert(sizeof(TTCluster) == 64, "TTCluster must fit in one cache line");
static_assert(alignof(TTCluster) == 64, "TTCluster must be cache-line aligned");

TranspositionTable::TranspositionTable(size_t size_mb) {
    resize(size_mb);
}

void TranspositionTable::resize(size_t size_mb) {
    num_clusters_ = (size_mb * 1024 * 1024) / sizeof(TTCluster);
    if (num_clusters_ == 0) num_clusters_ = 1;
    table_.resize(num_clusters_);
    // std::allocator honors over-aligned types under C++17; assert it so the
    // single-cache-line-per-cluster invariant holds at runtime as well as the
    // layout static_asserts hold at compile time.
    assert(reinterpret_cast<std::uintptr_t>(table_.data()) % alignof(TTCluster) == 0);
    clear();
}

void TranspositionTable::clear() {
    std::fill(table_.begin(), table_.end(), TTCluster{});
    generation_ = 0;
}

void TranspositionTable::new_search() {
    // Bump once per root search so stored entries carry the generation tag
    // used by the aging replacement rule. Natural uint8_t overflow wraps at
    // 256, which is sufficient given search-to-search generations stay well
    // within the ageing horizon we penalize against.
    generation_++;
}

size_t TranspositionTable::index(uint64_t key) const {
    // Multiplicative hashing: map key into [0, num_clusters_) using the upper
    // 64 bits of the 128-bit product. Avoids the DIV instruction that modulo
    // would emit and distributes well over arbitrary, non-power-of-two sizes.
    return static_cast<size_t>((static_cast<__uint128_t>(key) * num_clusters_) >> 64);
}

int16_t TranspositionTable::scoreToTT(int score, int ply) {
    if (score >= MATE_SCORE - 100) return static_cast<int16_t>(score + ply);
    if (score <= -(MATE_SCORE - 100)) return static_cast<int16_t>(score - ply);
    return static_cast<int16_t>(score);
}

int TranspositionTable::scoreFromTT(int16_t score, int ply) {
    if (score >= MATE_SCORE - 100) return score - ply;
    if (score <= -(MATE_SCORE - 100)) return score + ply;
    return score;
}

void TranspositionTable::store(uint64_t key, int score, int eval, int depth, TTFlag flag,
                               const Move &best_move, int ply) {
    TTCluster &cluster = table_[index(key)];

    // Prefer three slots in priority order: the slot already holding this key
    // (refresh in place so the newest info wins), then any empty slot, then
    // the entry whose age-adjusted depth is lowest. Each generation older
    // subtracts 8 ply of effective depth, so a stale deep result loses to a
    // fresh shallow one once the stale entry trails the current search by
    // several generations.
    TTEntry *target = nullptr;
    for (int i = 0; i < TT_CLUSTER_SIZE; i++) {
        if (cluster.entries[i].key == key && cluster.entries[i].flag != TT_NONE) {
            target = &cluster.entries[i];
            break;
        }
    }
    if (target == nullptr) {
        for (int i = 0; i < TT_CLUSTER_SIZE; i++) {
            if (cluster.entries[i].flag == TT_NONE) {
                target = &cluster.entries[i];
                break;
            }
        }
    }
    if (target == nullptr) {
        auto quality = [this](const TTEntry &e) {
            int age = static_cast<uint8_t>(generation_ - e.generation);
            return static_cast<int>(e.depth) - 8 * age;
        };
        target = &cluster.entries[0];
        int best_quality = quality(cluster.entries[0]);
        for (int i = 1; i < TT_CLUSTER_SIZE; i++) {
            int q = quality(cluster.entries[i]);
            if (q < best_quality) {
                target = &cluster.entries[i];
                best_quality = q;
            }
        }
    }

    target->key = key;
    target->score = scoreToTT(score, ply);
    target->depth = static_cast<int16_t>(depth);
    target->eval = static_cast<int16_t>(eval);
    target->flag = flag;
    target->generation = generation_;
    target->best_move = best_move;
}

void TranspositionTable::prefetch(uint64_t key) const {
    // Issue a non-temporal L1 hint for the cluster a future probe would read.
    // Call this right after a move is played so the child's probe finds the
    // line hot, overlapping the DRAM fetch with work the caller still needs
    // to do before recursing.
    __builtin_prefetch(&table_[index(key)], 0, 0);
}

bool TranspositionTable::probe(uint64_t key, TTEntry &entry, int ply) const {
    const TTCluster &cluster = table_[index(key)];
    for (int i = 0; i < TT_CLUSTER_SIZE; i++) {
        const TTEntry &candidate = cluster.entries[i];
        if (candidate.key == key && candidate.flag != TT_NONE) {
            entry = candidate;
            entry.score = static_cast<int16_t>(scoreFromTT(candidate.score, ply));
            return true;
        }
    }
    return false;
}

int TranspositionTable::hashfull() const {
    // Sample the first 1000 slots (500 clusters) so the reading is cheap and
    // stable across table sizes. Only entries tagged with the current search
    // generation count as full, so the number reflects table pressure from
    // the live search rather than residue from prior searches.
    const size_t total_slots = num_clusters_ * TT_CLUSTER_SIZE;
    const size_t sample_slots = std::min(total_slots, static_cast<size_t>(1000));
    const size_t sample_clusters = (sample_slots + TT_CLUSTER_SIZE - 1) / TT_CLUSTER_SIZE;
    int used = 0;
    for (size_t c = 0; c < sample_clusters; c++) {
        for (int e = 0; e < TT_CLUSTER_SIZE; e++) {
            const size_t slot = c * TT_CLUSTER_SIZE + e;
            if (slot >= sample_slots) break;
            const TTEntry &entry = table_[c].entries[e];
            if (entry.flag != TT_NONE && entry.generation == generation_) used++;
        }
    }
    return static_cast<int>(used * 1000 / sample_slots);
}
