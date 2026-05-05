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

// Six 10-byte packed entries plus four bytes of padding fit exactly into one
// 64-byte cache line. Six candidates per cluster gives the replacement policy
// more room to keep a deep entry through age while still putting every probe
// on a single cache line.
constexpr int TT_CLUSTER_SIZE = 6;

// Internal storage layout. `key16` is the low 16 bits of the full hash key,
// used as the inside-cluster collision check (Stockfish-style). The cluster
// index is taken from the high bits of the key by multiplicative hashing, so
// the low 16 bits are independent of the cluster and uniquely identify the
// entry within it with 65535/65536 probability; the rare false-hit is
// filtered out by the move-legality check on the search side. `move16`
// packs the move into `from | (to << 6) | (promo << 12)`. `genFlag8` packs
// the generation into the upper six bits and the bound flag into the lower
// two bits.
struct PackedTTEntry {
    uint16_t key16 = 0;
    uint16_t move16 = 0;
    int16_t score = 0;
    int16_t eval = TT_NO_EVAL;
    uint8_t depth8 = 0;
    uint8_t genFlag8 = 0;
};

struct alignas(64) TTCluster {
    PackedTTEntry entries[TT_CLUSTER_SIZE];
    uint8_t padding[4];
};

// Public-facing entry returned by `probe`. The internal storage is the packed
// form above; `probe` inflates a packed slot into this struct on hit so
// callers can read full-width score / eval / move / flag without bit fiddling.
struct TTEntry {
    uint64_t key = 0;
    int16_t score = 0;
    int16_t depth = 0;
    int16_t eval = TT_NO_EVAL;
    TTFlag flag = TT_NONE;
    uint8_t generation = 0;
    Move best_move = {0, 0, None};
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
