#!/usr/bin/env bash
set -euo pipefail

ROUNDS="${1:-10}"
FASTCHESS="${FASTCHESS:-./fastchess}"
ENGINE="./build/rlngin"

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

mkdir -p results

echo "Running $ROUNDS rounds of self-play..."

"$FASTCHESS" \
    -engine cmd="$ENGINE" name=rlngin-current \
    -engine cmd="$ENGINE" name=rlngin-baseline \
    -each tc=10+0.1 \
    -rounds "$ROUNDS" \
    -pgnout file=results/selfplay.pgn \
    -recover \
    2>&1 | tee results/selfplay.log

echo "Results saved to results/"
