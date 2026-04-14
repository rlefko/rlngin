#ifndef TT_H
#define TT_H

#include "types.h"
#include <cstdint>
#include <vector>

enum TTFlag : uint8_t { TT_NONE, TT_EXACT, TT_LOWER_BOUND, TT_UPPER_BOUND };

struct TTEntry {
    uint64_t key = 0;
    int16_t score = 0;
    int16_t depth = 0;
    TTFlag flag = TT_NONE;
    Move best_move = {0, 0, None};
};

class TranspositionTable {
  public:
    TranspositionTable(size_t size_mb = 16);

    void resize(size_t size_mb);
    void clear();

    void store(uint64_t key, int score, int depth, TTFlag flag, const Move &best_move, int ply);
    bool probe(uint64_t key, TTEntry &entry, int ply) const;

  private:
    std::vector<TTEntry> table_;
    size_t num_entries_ = 0;

    size_t index(uint64_t key) const;
    static int16_t scoreToTT(int score, int ply);
    static int scoreFromTT(int16_t score, int ply);
};

#endif
