#ifndef PAWN_HASH_H
#define PAWN_HASH_H

#include <cstdint>
#include <vector>

struct PawnHashEntry {
    uint64_t key = 0;
    int16_t mgScore = 0;
    int16_t egScore = 0;
};

class PawnHashTable {
  public:
    PawnHashTable(size_t size_mb = 2);

    void resize(size_t size_mb);
    void clear();

    void store(uint64_t key, int mg, int eg);
    bool probe(uint64_t key, int &mg, int &eg) const;

  private:
    std::vector<PawnHashEntry> table_;
    size_t numEntries_ = 0;

    size_t index(uint64_t key) const;
};

#endif
