#include "material_hash.h"

MaterialHashTable::MaterialHashTable(size_t size_mb) {
    resize(size_mb);
}

void MaterialHashTable::resize(size_t size_mb) {
    numEntries_ = (size_mb * 1024 * 1024) / sizeof(MaterialHashEntry);
    if (numEntries_ == 0) numEntries_ = 1;
    table_.resize(numEntries_);
    clear();
}

void MaterialHashTable::clear() {
    std::fill(table_.begin(), table_.end(), MaterialHashEntry{});
}

size_t MaterialHashTable::index(uint64_t key) const {
    return key % numEntries_;
}

void MaterialHashTable::store(uint64_t key, int mg, int eg, int phase) {
    size_t i = index(key);
    table_[i].key = key;
    table_[i].mgScore = static_cast<int16_t>(mg);
    table_[i].egScore = static_cast<int16_t>(eg);
    table_[i].phase = static_cast<int16_t>(phase);
}

bool MaterialHashTable::probe(uint64_t key, int &mg, int &eg, int &phase) const {
    size_t i = index(key);
    if (table_[i].key == key) {
        mg = table_[i].mgScore;
        eg = table_[i].egScore;
        phase = table_[i].phase;
        return true;
    }
    return false;
}
