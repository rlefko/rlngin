#include "endgame.h"

#include "kpk_bitbase.h"
#include "zobrist.h"

#include <unordered_map>

namespace Endgame {

namespace {

// Material-key registries populated once in init(). Lookups are read-only
// after init() returns, so no synchronization is required on probe.
std::unordered_map<uint64_t, ValueEntry> g_valueMap;
std::unordered_map<uint64_t, ScaleEntry> g_scaleMap;

// Compute the same material zobrist key Board::computeMaterialKey
// produces for a position with the given piece counts. The argument
// ordering follows the natural piece progression (pawn, knight, bishop,
// rook, queen) for each color; the king count is always 1 and is folded
// in implicitly via material_keys[c][King][1].
uint64_t makeKey(int wp, int wn, int wb, int wr, int wq, int bp, int bn, int bb, int br, int bq) {
    uint64_t k = 0;
    k ^= zobrist::material_keys[White][Pawn][wp];
    k ^= zobrist::material_keys[White][Knight][wn];
    k ^= zobrist::material_keys[White][Bishop][wb];
    k ^= zobrist::material_keys[White][Rook][wr];
    k ^= zobrist::material_keys[White][Queen][wq];
    k ^= zobrist::material_keys[White][King][1];
    k ^= zobrist::material_keys[Black][Pawn][bp];
    k ^= zobrist::material_keys[Black][Knight][bn];
    k ^= zobrist::material_keys[Black][Bishop][bb];
    k ^= zobrist::material_keys[Black][Rook][br];
    k ^= zobrist::material_keys[Black][Queen][bq];
    k ^= zobrist::material_keys[Black][King][1];
    // The empty-slot zobrist entries XOR in for every piece type whose
    // count is zero; they cancel by symmetry against the natural board
    // setup since computeMaterialKey iterates every piece type and looks
    // up material_keys[c][pt][pieceCount[c][pt]] regardless of whether
    // the count is zero. The lookup above mirrors that behavior so the
    // generated key matches Board::computeMaterialKey byte for byte.
    return k;
}

bool g_initialized = false;

} // namespace

void init() {
    if (g_initialized) return;
    g_initialized = true;
    Kpk::init();
    // Subsequent commits will populate g_valueMap and g_scaleMap with
    // registrations for the recognized material configurations. The
    // scaffold leaves both maps empty so dispatch always falls through
    // to the generic eval until evaluators land.
    (void)makeKey;
}

const ValueEntry *probeValue(uint64_t materialKey) {
    auto it = g_valueMap.find(materialKey);
    return it == g_valueMap.end() ? nullptr : &it->second;
}

const ScaleEntry *probeScale(uint64_t materialKey) {
    auto it = g_scaleMap.find(materialKey);
    return it == g_scaleMap.end() ? nullptr : &it->second;
}

} // namespace Endgame
