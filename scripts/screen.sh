#!/usr/bin/env bash
set -euo pipefail

# Usage: screen.sh <rounds> <tc> [concurrency]
#   rounds:      number of game pairs (total games = 2 * rounds)
#   tc:          time control string, e.g. "10+0.1"
#   concurrency: parallel games (default: 2)

ROUNDS="${1:?Usage: screen.sh <rounds> <tc> [concurrency]}"
TC="${2:?Usage: screen.sh <rounds> <tc> [concurrency]}"
CONCURRENCY="${3:-2}"

FASTCHESS="${FASTCHESS:-./fastchess}"
ENGINE="${ENGINE:-./build/rlngin}"
OPENINGS="${OPENINGS:-openings/UHO_Lichess_4852_v1.epd}"

if [ ! -f "$FASTCHESS" ]; then
    echo "Error: fastchess not found at $FASTCHESS"
    echo "Run 'make fetch-fastchess' to download it."
    exit 1
fi

if [ ! -f "$ENGINE" ]; then
    echo "Error: engine not found at $ENGINE"
    echo "Run 'make build' first."
    exit 1
fi

if [ ! -f "$OPENINGS" ]; then
    echo "Error: opening book not found at $OPENINGS"
    echo "Run 'make fetch-openings' to download it."
    exit 1
fi

mkdir -p results

echo "Running $ROUNDS pairs ($((ROUNDS * 2)) games) at tc=$TC, concurrency=$CONCURRENCY"

"$FASTCHESS" \
    -engine cmd="$ENGINE" name=rlngin-dev \
    -engine cmd="$ENGINE" name=rlngin-base \
    -each tc="$TC" \
    -resign movecount=3 score=600 \
    -draw movenumber=34 movecount=8 score=20 \
    -rounds "$ROUNDS" \
    -repeat \
    -concurrency "$CONCURRENCY" \
    -openings file="$OPENINGS" format=epd order=random \
    -pgnout file=results/screen.pgn \
    -recover \
    2>&1 | tee results/screen.log

echo ""
echo "Done. Results saved to results/"
