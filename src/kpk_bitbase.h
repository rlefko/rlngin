#ifndef KPK_BITBASE_H
#define KPK_BITBASE_H

#include "types.h"

// Endgame bitbase for king and pawn versus king. The table is built once
// at engine init via retrograde analysis and supports both color
// polarities through vertical mirroring at lookup time. Each entry is a
// single bit: 1 means the strong (pawn) side wins with optimal play, 0
// means the position is drawn.
namespace Kpk {

// Build the bitbase. Idempotent; calls after the first are no-ops.
// Allocates roughly 200 KB of working memory during the build and
// retains a 24 KB packed bit array for the lifetime of the process.
void init();

// Probe the bitbase. `strongSide` is the side with the pawn,
// `strongKing` and `weakKing` are the square indices for the kings
// (0..63, a1 = 0), `pawn` is the pawn square on the strong side, and
// `stm` is the side to move. Returns true when the strong side wins
// with optimal play.
bool probe(Color strongSide, int strongKingSq, int pawnSq, int weakKingSq, Color stm);

} // namespace Kpk

#endif
