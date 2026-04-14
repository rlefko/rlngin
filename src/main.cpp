#include "bitboard.h"
#include "uci.h"
#include "zobrist.h"

int main() {
    initBitboards();
    zobrist::init();
    uciLoop();
    return 0;
}
