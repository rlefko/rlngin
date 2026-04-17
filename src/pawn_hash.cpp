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

void PawnHashTable::store(uint64_t key, int mg, int eg) {
    size_t i = index(key);
    table_[i].key = key;
    table_[i].mgScore = static_cast<int16_t>(mg);
    table_[i].egScore = static_cast<int16_t>(eg);
}

bool PawnHashTable::probe(uint64_t key, int &mg, int &eg) const {
    size_t i = index(key);
    if (table_[i].key == key) {
        mg = table_[i].mgScore;
        eg = table_[i].egScore;
        return true;
    }
    return false;
}
