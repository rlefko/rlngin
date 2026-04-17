#ifndef ZOBRIST_H
#define ZOBRIST_H

#include <cstdint>

namespace zobrist {

extern uint64_t piece_keys[2][7][64];
extern uint64_t side_to_move_key;
extern uint64_t castling_keys[16];
extern uint64_t en_passant_keys[8];

// Material keys are indexed by [color][pieceType][count]. The material-hash
// probe takes the XOR of material_keys[color][pt][pieceCount[color][pt]] for
// every color and piece type, so the size 17 covers the theoretical maximum
// of 8 pawns + 8 promotions + 1 original for pawns / majors and bishops.
extern uint64_t material_keys[2][7][17];

void init();

} // namespace zobrist

#endif
