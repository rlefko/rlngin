#include "tt.h"

TranspositionTable::TranspositionTable(size_t size_mb) {
    resize(size_mb);
}

void TranspositionTable::resize(size_t size_mb) {
    num_entries_ = (size_mb * 1024 * 1024) / sizeof(TTEntry);
    if (num_entries_ == 0) num_entries_ = 1;
    table_.resize(num_entries_);
    clear();
}

void TranspositionTable::clear() {
    std::fill(table_.begin(), table_.end(), TTEntry{});
}

size_t TranspositionTable::index(uint64_t key) const {
    return key % num_entries_;
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

void TranspositionTable::store(uint64_t key, int score, int depth, TTFlag flag,
                               const Move &best_move, int ply) {
    size_t i = index(key);
    table_[i].key = key;
    table_[i].score = scoreToTT(score, ply);
    table_[i].depth = static_cast<int16_t>(depth);
    table_[i].flag = flag;
    table_[i].best_move = best_move;
}

bool TranspositionTable::probe(uint64_t key, TTEntry &entry, int ply) const {
    size_t i = index(key);
    if (table_[i].key == key && table_[i].flag != TT_NONE) {
        entry = table_[i];
        entry.score = static_cast<int16_t>(scoreFromTT(table_[i].score, ply));
        return true;
    }
    return false;
}

int TranspositionTable::hashfull() const {
    size_t sample = std::min(num_entries_, static_cast<size_t>(1000));
    int used = 0;
    for (size_t i = 0; i < sample; i++) {
        if (table_[i].flag != TT_NONE) {
            used++;
        }
    }
    if (sample < 1000) {
        return static_cast<int>(used * 1000 / sample);
    }
    return used;
}
