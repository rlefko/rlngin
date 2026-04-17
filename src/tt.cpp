#include "tt.h"

// Lock the TT entry layout: padding changes would silently alter the ratio of
// entries per megabyte and invalidate hash-sizing assumptions.
static_assert(sizeof(TTEntry) == 32, "TTEntry layout unexpectedly changed");

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
    // Multiplicative hashing: map key into [0, num_entries_) using the upper
    // 64 bits of the 128-bit product. Avoids the DIV instruction that modulo
    // would emit and distributes well over arbitrary, non-power-of-two sizes.
    return static_cast<size_t>((static_cast<__uint128_t>(key) * num_entries_) >> 64);
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

void TranspositionTable::store(uint64_t key, int score, int eval, int depth, TTFlag flag,
                               const Move &best_move, int ply) {
    size_t i = index(key);
    // Depth-preferred replacement: only overwrite if the slot is empty or the
    // new search is at least as deep.  This prevents shallow searches (such as
    // the singular extension subtree) from evicting valuable deep entries.
    if (table_[i].flag == TT_NONE || depth >= table_[i].depth) {
        table_[i].key = key;
        table_[i].score = scoreToTT(score, ply);
        table_[i].depth = static_cast<int16_t>(depth);
        table_[i].eval = static_cast<int16_t>(eval);
        table_[i].flag = flag;
        table_[i].best_move = best_move;
    }
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
