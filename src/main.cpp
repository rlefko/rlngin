#include "bitboard.h"
#include "search.h"
#include "uci.h"
#include "zobrist.h"

int main() {
    initBitboards();
    zobrist::init();
    initSearch();
    uciLoop();
    return 0;
}
