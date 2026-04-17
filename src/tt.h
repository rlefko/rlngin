#ifndef TT_H
#define TT_H

#include "types.h"
#include <cstdint>
#include <limits>
#include <vector>

enum TTFlag : uint8_t { TT_NONE, TT_EXACT, TT_LOWER_BOUND, TT_UPPER_BOUND };

// Sentinel indicating that no static eval is cached with the entry (for example,
// nodes where the side to move was in check and eval was skipped).
constexpr int16_t TT_NO_EVAL = std::numeric_limits<int16_t>::min();

// Number of entries packed into each transposition-table cluster. Two 32-byte
// entries fit exactly into a single 64-byte cache line, which gives us
// set-associative replacement without ever straddling a cache line on probe.
constexpr int TT_CLUSTER_SIZE = 2;

struct TTEntry {
    uint64_t key = 0;
    int16_t score = 0;
    int16_t depth = 0;
    int16_t eval = TT_NO_EVAL;
    TTFlag flag = TT_NONE;
    uint8_t generation = 0;
    Move best_move = {0, 0, None};
};

struct alignas(64) TTCluster {
    TTEntry entries[TT_CLUSTER_SIZE];
};

class TranspositionTable {
  public:
    TranspositionTable(size_t size_mb = 16);

    void resize(size_t size_mb);
    void clear();
    void new_search();

    void store(uint64_t key, int score, int eval, int depth, TTFlag flag, const Move &best_move,
               int ply);
    bool probe(uint64_t key, TTEntry &entry, int ply) const;
    void prefetch(uint64_t key) const;
    int hashfull() const;
    uint8_t generation() const { return generation_; }

  private:
    std::vector<TTCluster> table_;
    size_t num_clusters_ = 0;
    uint8_t generation_ = 0;

    size_t index(uint64_t key) const;
    static int16_t scoreToTT(int score, int ply);
    static int scoreFromTT(int16_t score, int ply);
};

#endif
