#include "zobrist.h"
#include <random>

namespace zobrist {

uint64_t piece_keys[2][7][64];
uint64_t side_to_move_key;
uint64_t castling_keys[16];
uint64_t en_passant_keys[8];
uint64_t material_keys[2][7][17];

void init() {
    static bool inited = false;
    if (inited) return;
    inited = true;

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

// Static initializer that forces zobrist::init() to run at program
// startup, before any Board operation can read the random tables.
// Without this, a board constructed or set via FEN before the first
// evaluate() call computes its keys from zero-initialized tables and
// ends up with materialKey == 0, which then collides with the default-
// initialized materialHashTable entry and returns bogus cached
// material values. Wrapping the init call in a namespace-scope object
// guarantees it runs before main() and is idempotent.
namespace {
struct EagerInit {
    EagerInit() { init(); }
};
EagerInit g_eager;
} // namespace

} // namespace zobrist
