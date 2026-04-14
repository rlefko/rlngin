#include "uci.h"
#include "zobrist.h"

int main() {
    zobrist::init();
    uciLoop();
    return 0;
}
