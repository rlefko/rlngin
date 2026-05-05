#include "tt.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef __linux__
#include <sys/mman.h>
#endif

// Lock the packed entry layout: padding changes would silently alter the
// ratio of entries per megabyte and invalidate hash-sizing assumptions.
static_assert(sizeof(PackedTTEntry) == 10, "PackedTTEntry layout unexpectedly changed");
static_assert(sizeof(TTCluster) == 64, "TTCluster must fit in one cache line");
static_assert(alignof(TTCluster) == 64, "TTCluster must be cache-line aligned");

// `clear()` zeros the table with `memset`, so the cluster and every entry
// must be safe to bit-blit. If a member ever picks up a non-trivial copy or
// destructor (e.g. someone adds a `std::string`), the static_asserts below
// fail at compile time and force the rewrite of the clear path.
static_assert(std::is_trivially_copyable_v<PackedTTEntry>,
              "PackedTTEntry must be trivially copyable for memset-based clear");
static_assert(std::is_trivially_copyable_v<TTCluster>,
              "TTCluster must be trivially copyable for memset-based clear");

namespace {

// `genFlag8` bit layout:
//   bits 0..1 : TTFlag (TT_NONE / TT_EXACT / TT_LOWER_BOUND / TT_UPPER_BOUND)
//   bits 2..7 : 6-bit generation counter (wraps every 64 searches)
constexpr uint8_t TT_FLAG_MASK = 0x3;
constexpr uint8_t TT_GEN_SHIFT = 2;
constexpr uint8_t TT_GEN_MASK = 0xFC;
constexpr uint8_t TT_GEN_RANGE = 0x40;

inline uint16_t encodeMove(const Move &m) {
    return static_cast<uint16_t>((static_cast<uint32_t>(m.from) & 0x3F) |
                                 ((static_cast<uint32_t>(m.to) & 0x3F) << 6) |
                                 ((static_cast<uint32_t>(m.promotion) & 0x7) << 12));
}

inline Move decodeMove(uint16_t encoded) {
    Move m;
    m.from = static_cast<int>(encoded & 0x3F);
    m.to = static_cast<int>((encoded >> 6) & 0x3F);
    m.promotion = static_cast<PieceType>((encoded >> 12) & 0x7);
    return m;
}

inline uint16_t keyCheck(uint64_t key) {
    // Use the low 16 bits as the inside-cluster key check. The cluster index
    // is `(key * num_clusters) >> 64`, which is dominated by the upper bits
    // of the key and at hash sizes >= 16 MB consumes the entire upper 16
    // bits. If the check were taken from the upper 16 bits it would be
    // constant within every cluster, every probe would match the first
    // occupied slot regardless of which position was queried, and the TT
    // would effectively collapse to one slot per cluster. Stockfish uses
    // the same low-16-bit choice for the same reason.
    return static_cast<uint16_t>(key);
}

inline TTFlag flagOf(uint8_t genFlag8) {
    return static_cast<TTFlag>(genFlag8 & TT_FLAG_MASK);
}

inline uint8_t generationOf(uint8_t genFlag8) {
    return static_cast<uint8_t>((genFlag8 & TT_GEN_MASK) >> TT_GEN_SHIFT);
}

inline uint8_t packGenFlag(uint8_t generation, TTFlag flag) {
    return static_cast<uint8_t>(((generation & 0x3F) << TT_GEN_SHIFT) |
                                (static_cast<uint8_t>(flag) & TT_FLAG_MASK));
}

inline int agedQuality(uint8_t depth8, uint8_t storedGen, uint8_t currentGen) {
    int age = (currentGen - storedGen) & (TT_GEN_RANGE - 1);
    return static_cast<int>(depth8) - 8 * age;
}

} // namespace

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

#ifdef __linux__
    // Ask the kernel for transparent huge pages over the TT region. The
    // allocation is only 64-byte aligned (alignof(TTCluster)), not 2 MiB
    // aligned, so the kernel applies the hint to the huge-page-aligned
    // interior and falls back to 4 KiB pages for the head and tail. At
    // typical 1+ GB hashes that interior is essentially the whole table,
    // and the TLB-miss reduction is meaningful (the search hits the TT on
    // every node). Below ~2 MiB the hint cannot apply at all, so skip it.
    const size_t bytes = num_clusters_ * sizeof(TTCluster);
    if (bytes >= 2ULL * 1024 * 1024) {
        madvise(table_.data(), bytes, MADV_HUGEPAGE);
    }
#endif

    clear();
}

void TranspositionTable::clear() {
    // memset is safe because the cluster is trivially copyable and every
    // member's all-zero byte pattern is a valid empty state: `genFlag8 == 0`
    // encodes TT_NONE, so probes filter the slot out before ever inspecting
    // score / eval / move. Above the threshold the memset is sharded across
    // up to eight worker threads so ucinewgame on a multi-gigabyte hash
    // does not stall the engine on a serial DRAM wipe.
    const size_t bytes = num_clusters_ * sizeof(TTCluster);
    constexpr size_t PARALLEL_THRESHOLD = 64ULL * 1024 * 1024;

    if (bytes < PARALLEL_THRESHOLD) {
        std::memset(table_.data(), 0, bytes);
    } else {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 1;
        const unsigned numThreads = std::min(hw, 8u);
        const size_t chunkClusters = (num_clusters_ + numThreads - 1) / numThreads;
        std::vector<std::thread> workers;
        workers.reserve(numThreads);
        for (unsigned t = 0; t < numThreads; t++) {
            const size_t startCluster = static_cast<size_t>(t) * chunkClusters;
            if (startCluster >= num_clusters_) break;
            const size_t endCluster = std::min(startCluster + chunkClusters, num_clusters_);
            workers.emplace_back([this, startCluster, endCluster] {
                std::memset(&table_[startCluster], 0,
                            (endCluster - startCluster) * sizeof(TTCluster));
            });
        }
        for (auto &w : workers)
            w.join();
    }
    generation_ = 0;
}

void TranspositionTable::new_search() {
    // Bump once per root search so stored entries carry the generation tag
    // used by the aging replacement rule. The stored field is six bits, so
    // ages wrap every 64 searches; the runtime counter stays uint8_t and
    // wraps every 256, which the public `generation()` accessor preserves.
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
    const uint16_t k16 = keyCheck(key);

    // Prefer three slots in priority order: the slot already holding this key
    // (refresh in place so the newest info wins), then any empty slot, then
    // the entry whose age-adjusted depth is lowest. Each generation older
    // subtracts 8 ply of effective depth, so a stale deep result loses to a
    // fresh shallow one once the stale entry trails the current search by
    // several generations.
    PackedTTEntry *target = nullptr;
    bool sameKey = false;
    for (int i = 0; i < TT_CLUSTER_SIZE; i++) {
        PackedTTEntry &slot = cluster.entries[i];
        if (slot.key16 == k16 && flagOf(slot.genFlag8) != TT_NONE) {
            target = &slot;
            sameKey = true;
            break;
        }
    }
    if (target == nullptr) {
        for (int i = 0; i < TT_CLUSTER_SIZE; i++) {
            if (flagOf(cluster.entries[i].genFlag8) == TT_NONE) {
                target = &cluster.entries[i];
                break;
            }
        }
    }
    if (target == nullptr) {
        target = &cluster.entries[0];
        int best_quality = agedQuality(target->depth8, generationOf(target->genFlag8), generation_);
        for (int i = 1; i < TT_CLUSTER_SIZE; i++) {
            PackedTTEntry &slot = cluster.entries[i];
            int q = agedQuality(slot.depth8, generationOf(slot.genFlag8), generation_);
            if (q < best_quality) {
                target = &slot;
                best_quality = q;
            }
        }
    }

    // Same-key replacement gate. A shallow non-EXACT write must not clobber a
    // deeper same-key entry from a more thorough search: doing so bleeds depth
    // out of the table every time a PV node re-searches a transposition at a
    // smaller remaining depth than its prior visit. An EXACT bound always
    // wins because it carries maximally useful information; otherwise the new
    // depth must land within 3 ply of the existing entry to take over. The
    // bestMove hint is preserved across rejected overwrites: a valid prior
    // move stays put rather than being erased by a store that could not supply
    // one of its own (for instance a leaf-level stand-pat write).
    TTFlag targetFlag = flagOf(target->genFlag8);
    if (sameKey && flag != TT_EXACT && targetFlag == TT_EXACT &&
        depth < static_cast<int>(target->depth8)) {
        // Preserve the deeper exact bound. Only refresh the generation so the
        // aging replacement logic does not treat it as stale.
        target->genFlag8 = packGenFlag(generation_, targetFlag);
        return;
    }
    if (sameKey && flag != TT_EXACT && depth + 4 <= static_cast<int>(target->depth8)) {
        // New entry is meaningfully shallower than the existing same-key bound
        // and carries no stronger flag, so keep the deeper result intact.
        target->genFlag8 = packGenFlag(generation_, targetFlag);
        return;
    }

    bool incomingHasMove = best_move.from != 0 || best_move.to != 0;
    uint16_t preservedMove = target->move16;
    bool preservedHasMove = sameKey && preservedMove != 0;

    int clampedDepth = depth;
    if (clampedDepth < 0) clampedDepth = 0;
    if (clampedDepth > 0xFF) clampedDepth = 0xFF;

    target->key16 = k16;
    target->score = scoreToTT(score, ply);
    target->depth8 = static_cast<uint8_t>(clampedDepth);
    target->eval = static_cast<int16_t>(eval);
    target->genFlag8 = packGenFlag(generation_, flag);
    target->move16 = (!incomingHasMove && preservedHasMove) ? preservedMove : encodeMove(best_move);
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
    const uint16_t k16 = keyCheck(key);
    for (int i = 0; i < TT_CLUSTER_SIZE; i++) {
        const PackedTTEntry &slot = cluster.entries[i];
        TTFlag slotFlag = flagOf(slot.genFlag8);
        if (slot.key16 == k16 && slotFlag != TT_NONE) {
            entry.key = key;
            entry.score = static_cast<int16_t>(scoreFromTT(slot.score, ply));
            entry.depth = static_cast<int16_t>(slot.depth8);
            entry.eval = slot.eval;
            entry.flag = slotFlag;
            entry.generation = generationOf(slot.genFlag8);
            entry.best_move = decodeMove(slot.move16);
            return true;
        }
    }
    return false;
}

int TranspositionTable::hashfull() const {
    // Sample the first 1000 slots so the reading is cheap and stable across
    // table sizes. Only entries tagged with the current search generation
    // count as full, so the number reflects table pressure from the live
    // search rather than residue from prior searches.
    const size_t total_slots = num_clusters_ * TT_CLUSTER_SIZE;
    const size_t sample_slots = std::min(total_slots, static_cast<size_t>(1000));
    const size_t sample_clusters = (sample_slots + TT_CLUSTER_SIZE - 1) / TT_CLUSTER_SIZE;
    const uint8_t currentGen = static_cast<uint8_t>(generation_ & 0x3F);
    int used = 0;
    for (size_t c = 0; c < sample_clusters; c++) {
        for (int e = 0; e < TT_CLUSTER_SIZE; e++) {
            const size_t slot = c * TT_CLUSTER_SIZE + e;
            if (slot >= sample_slots) break;
            const PackedTTEntry &entry = table_[c].entries[e];
            TTFlag slotFlag = flagOf(entry.genFlag8);
            if (slotFlag != TT_NONE && generationOf(entry.genFlag8) == currentGen) used++;
        }
    }
    return static_cast<int>(used * 1000 / sample_slots);
}
