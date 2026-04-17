#ifndef MATERIAL_HASH_H
#define MATERIAL_HASH_H

#include <cstdint>
#include <vector>

struct MaterialHashEntry {
    uint64_t key = 0;
    int16_t mgScore = 0;
    int16_t egScore = 0;
    int16_t phase = 0;
    int16_t _pad = 0;
};

class MaterialHashTable {
  public:
    MaterialHashTable(size_t size_mb = 1);

    void resize(size_t size_mb);
    void clear();

    void store(uint64_t key, int mg, int eg, int phase);
    bool probe(uint64_t key, int &mg, int &eg, int &phase) const;

  private:
    std::vector<MaterialHashEntry> table_;
    size_t numEntries_ = 0;

    size_t index(uint64_t key) const;
};

#endif
