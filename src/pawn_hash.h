#ifndef PAWN_HASH_H
#define PAWN_HASH_H

#include <cstdint>
#include <vector>

struct PawnHashEntry {
    uint64_t key = 0;
    uint64_t passers[2] = {0, 0};
    int16_t mgScore = 0;
    int16_t egScore = 0;
    // Per side shelter cache, keyed jointly by the pawn key, the king
    // file at compute time, and the castling rights mask. The cache
    // misses on a king move or castle change even with an unchanged
    // pawn structure, so each store also stamps the matching key.
    int16_t shelterMg[2] = {0, 0};
    int16_t shelterEg[2] = {0, 0};
    uint8_t shelterKingFile[2] = {0xFF, 0xFF};
    uint8_t shelterCastling[2] = {0, 0};
    bool shelterValid[2] = {false, false};
};

class PawnHashTable {
  public:
    PawnHashTable(size_t size_mb = 2);

    void resize(size_t size_mb);
    void clear();

    void store(uint64_t key, int mg, int eg, uint64_t whitePassers, uint64_t blackPassers);
    bool probe(uint64_t key, int &mg, int &eg, uint64_t &whitePassers,
               uint64_t &blackPassers) const;

    // Shelter cache. Returns true on hit; only valid when the entry's
    // pawn key, king file, and castling mask all match the stored
    // values. Storing requires the pawn entry to already be present
    // (the basic pawn evaluation must have run first).
    bool probeShelter(uint64_t key, int side, int kingFile, int castlingMask, int &mg,
                      int &eg) const;
    void storeShelter(uint64_t key, int side, int kingFile, int castlingMask, int mg, int eg);

  private:
    std::vector<PawnHashEntry> table_;
    size_t numEntries_ = 0;

    size_t index(uint64_t key) const;
};

#endif
