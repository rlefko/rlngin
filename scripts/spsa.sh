#!/usr/bin/env bash
set -euo pipefail

# Usage: spsa.sh [iterations] [tc] [concurrency] [seed]
#
# Thin wrapper around tools/spsa/spsa.py so ad hoc runs share one
# entry point with make spsa.
#
# Environment variables:
#   ENGINE:    path to the engine (default: ./build/rlngin)
#   FASTCHESS: path to fastchess (default: ./fastchess)
#   OPENINGS:  path to opening book (default: openings/UHO_Lichess_4852_v1.epd)
#   OUTPUT:    output directory (default: tuning/spsa)

ITERS="${1:-300}"
TC="${2:-10+0.1}"
CONCURRENCY="${3:-6}"
SEED="${4:-1}"

ENGINE="${ENGINE:-./build/rlngin}"
FASTCHESS="${FASTCHESS:-./fastchess}"
OPENINGS="${OPENINGS:-openings/UHO_Lichess_4852_v1.epd}"
OUTPUT="${OUTPUT:-tuning/spsa}"

if [ ! -x "$ENGINE" ]; then
    echo "Error: engine not found at $ENGINE (run 'make build')"
    exit 1
fi

if [ ! -f "$FASTCHESS" ]; then
    echo "Error: fastchess not found at $FASTCHESS (run 'make fetch-fastchess')"
    exit 1
fi

if [ ! -f "$OPENINGS" ]; then
    echo "Error: opening book not found at $OPENINGS (run 'make fetch-openings')"
    exit 1
fi

mkdir -p "$OUTPUT"

exec python3 tools/spsa/spsa.py \
    --iterations "$ITERS" \
    --concurrency "$CONCURRENCY" \
    --tc "$TC" \
    --seed "$SEED" \
    --engine "$ENGINE" \
    --fastchess "$FASTCHESS" \
    --openings "$OPENINGS" \
    --output-dir "$OUTPUT"
