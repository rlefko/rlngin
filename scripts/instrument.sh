#!/usr/bin/env bash
set -euo pipefail

# Usage: instrument.sh [limit=100] [depth=10]
#
# Feeds the first `limit` FENs from `$POSITIONS` into the engine one at a
# time, runs a fixed-depth search per position, and prints the trailing
# `info string stats` line for each. Pipe into tools/spsa/aggregate_stats.py
# to get per-heuristic firing rates over the corpus.
#
# Environment variables:
#   ENGINE:    path to the engine binary (default: ./build/rlngin)
#   POSITIONS: path to the position corpus (default: tuning/sample.epd, a
#              5k-position slice harvested from the Texel dataset). EPD-style
#              lines with `|` separators are fine; only the FEN portion is
#              used.
#   LIMIT:     how many positions to sample (default: 100)
#   DEPTH:     fixed search depth per position (default: 10)

LIMIT="${1:-${LIMIT:-100}}"
DEPTH="${2:-${DEPTH:-10}}"

ENGINE="${ENGINE:-./build/rlngin}"
POSITIONS="${POSITIONS:-tuning/sample.epd}"

if [ ! -x "$ENGINE" ]; then
    echo "Error: engine not found at $ENGINE (run 'make build')" >&2
    exit 1
fi

if [ ! -f "$POSITIONS" ]; then
    echo "Error: positions not found at $POSITIONS" >&2
    exit 1
fi

head -n "$LIMIT" "$POSITIONS" | awk -F '|' '{print $1}' | while read -r fen; do
    [ -n "$fen" ] || continue
    printf 'position fen %s\ngo depth %s\n' "$fen" "$DEPTH"
done | {
    cat
    printf 'quit\n'
} | "$ENGINE" 2>/dev/null | grep '^info string stats'
