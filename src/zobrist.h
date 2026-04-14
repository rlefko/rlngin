#ifndef ZOBRIST_H
#define ZOBRIST_H

#include <cstdint>

namespace zobrist {

extern uint64_t piece_keys[2][7][64];
extern uint64_t side_to_move_key;
extern uint64_t castling_keys[16];
extern uint64_t en_passant_keys[8];

void init();

} // namespace zobrist

#endif
