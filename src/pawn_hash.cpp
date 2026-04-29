#include "pawn_hash.h"

PawnHashTable::PawnHashTable(size_t size_mb) {
    resize(size_mb);
}

void PawnHashTable::resize(size_t size_mb) {
    numEntries_ = (size_mb * 1024 * 1024) / sizeof(PawnHashEntry);
    if (numEntries_ == 0) numEntries_ = 1;
    table_.resize(numEntries_);
    clear();
}

void PawnHashTable::clear() {
    std::fill(table_.begin(), table_.end(), PawnHashEntry{});
}

size_t PawnHashTable::index(uint64_t key) const {
    // Multiplicative hashing: avoids the DIV instruction that modulo would
    // emit and distributes keys evenly over arbitrary table sizes.
    return static_cast<size_t>((static_cast<__uint128_t>(key) * numEntries_) >> 64);
}

void PawnHashTable::store(uint64_t key, int mg, int eg, uint64_t whitePassers,
                          uint64_t blackPassers) {
    size_t i = index(key);
    bool sameKey = table_[i].key == key;
    table_[i].key = key;
    table_[i].mgScore = static_cast<int16_t>(mg);
    table_[i].egScore = static_cast<int16_t>(eg);
    table_[i].passers[0] = whitePassers;
    table_[i].passers[1] = blackPassers;
    if (!sameKey) {
        // New entry replaces a different position's data; the shelter
        // cache stamped under the old key is no longer trustworthy.
        table_[i].shelterValid[0] = false;
        table_[i].shelterValid[1] = false;
    }
}

bool PawnHashTable::probe(uint64_t key, int &mg, int &eg, uint64_t &whitePassers,
                          uint64_t &blackPassers) const {
    size_t i = index(key);
    if (table_[i].key == key) {
        mg = table_[i].mgScore;
        eg = table_[i].egScore;
        whitePassers = table_[i].passers[0];
        blackPassers = table_[i].passers[1];
        return true;
    }
    return false;
}

bool PawnHashTable::probeShelter(uint64_t key, int side, int kingFile, int castlingMask,
                                 int &mg, int &eg) const {
    size_t i = index(key);
    const PawnHashEntry &e = table_[i];
    if (e.key != key) return false;
    if (!e.shelterValid[side]) return false;
    if (e.shelterKingFile[side] != static_cast<uint8_t>(kingFile)) return false;
    if (e.shelterCastling[side] != static_cast<uint8_t>(castlingMask)) return false;
    mg = e.shelterMg[side];
    eg = e.shelterEg[side];
    return true;
}

void PawnHashTable::storeShelter(uint64_t key, int side, int kingFile, int castlingMask,
                                 int mg, int eg) {
    size_t i = index(key);
    PawnHashEntry &e = table_[i];
    if (e.key != key) return;
    e.shelterMg[side] = static_cast<int16_t>(mg);
    e.shelterEg[side] = static_cast<int16_t>(eg);
    e.shelterKingFile[side] = static_cast<uint8_t>(kingFile);
    e.shelterCastling[side] = static_cast<uint8_t>(castlingMask);
    e.shelterValid[side] = true;
}
