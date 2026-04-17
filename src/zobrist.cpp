#include "zobrist.h"
#include <random>

namespace zobrist {

uint64_t piece_keys[2][7][64];
uint64_t side_to_move_key;
uint64_t castling_keys[16];
uint64_t en_passant_keys[8];
uint64_t material_keys[2][7][17];

void init() {
    std::mt19937_64 rng(0x12345678DEADBEEF);

    for (int color = 0; color < 2; color++) {
        for (int piece = 0; piece < 7; piece++) {
            for (int sq = 0; sq < 64; sq++) {
                piece_keys[color][piece][sq] = rng();
            }
        }
    }

    side_to_move_key = rng();

    for (int i = 0; i < 16; i++) {
        castling_keys[i] = rng();
    }

    for (int i = 0; i < 8; i++) {
        en_passant_keys[i] = rng();
    }

    for (int color = 0; color < 2; color++) {
        for (int piece = 0; piece < 7; piece++) {
            for (int count = 0; count < 17; count++) {
                material_keys[color][piece][count] = rng();
            }
        }
    }
}

} // namespace zobrist
