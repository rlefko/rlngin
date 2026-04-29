#!/usr/bin/env bash
set -euo pipefail

# Usage: screen.sh <rounds> <limit> [concurrency]
#   rounds:      number of game pairs (total games = 2 * rounds)
#   limit:       per-move budget. Either a Cute-Chess time control like
#                "10+0.1" (passed through as -each tc=...) or a literal
#                "nodes=N" (passed through as -each nodes=N).
#   concurrency: parallel games (default: 2)
#
# Environment variables:
#   ENGINE_DEV:  path to the dev engine (default: ./build/rlngin)
#   ENGINE_BASE: path to the base engine (default: same as ENGINE_DEV)
#   FASTCHESS:   path to fastchess binary (default: ./fastchess)
#   OPENINGS:    path to opening book (default: openings/UHO_Lichess_4852_v1.epd)
#   OUTPUT:      output directory (default: results)
#   PGN_NAME:    pgn filename inside OUTPUT (default: screen.pgn)
#   LOG_NAME:    log filename inside OUTPUT (default: screen.log)
#   NAME_DEV:    fastchess engine label for ENGINE_DEV (default: rlngin-dev)
#   NAME_BASE:   fastchess engine label for ENGINE_BASE (default: rlngin-base)

ROUNDS="${1:?Usage: screen.sh <rounds> <limit> [concurrency]}"
LIMIT="${2:?Usage: screen.sh <rounds> <limit> [concurrency]}"
CONCURRENCY="${3:-2}"

FASTCHESS="${FASTCHESS:-./fastchess}"
ENGINE_DEV="${ENGINE_DEV:-./build/rlngin}"
ENGINE_BASE="${ENGINE_BASE:-$ENGINE_DEV}"
OPENINGS="${OPENINGS:-openings/UHO_Lichess_4852_v1.epd}"
OUTPUT="${OUTPUT:-results}"
PGN_NAME="${PGN_NAME:-screen.pgn}"
LOG_NAME="${LOG_NAME:-screen.log}"
NAME_DEV="${NAME_DEV:-rlngin-dev}"
NAME_BASE="${NAME_BASE:-rlngin-base}"

if [[ "$LIMIT" == nodes=* ]]; then
    LIMIT_OPT=("$LIMIT")
    LIMIT_LABEL="$LIMIT"
else
    LIMIT_OPT=(tc="$LIMIT")
    LIMIT_LABEL="tc=$LIMIT"
fi

if [ ! -f "$FASTCHESS" ]; then
    echo "Error: fastchess not found at $FASTCHESS"
    echo "Run 'make fetch-fastchess' to download it."
    exit 1
fi

if [ ! -f "$ENGINE_DEV" ]; then
    echo "Error: dev engine not found at $ENGINE_DEV"
    echo "Run 'make build' first."
    exit 1
fi

if [ ! -f "$ENGINE_BASE" ]; then
    echo "Error: base engine not found at $ENGINE_BASE"
    exit 1
fi

if [ ! -f "$OPENINGS" ]; then
    echo "Error: opening book not found at $OPENINGS"
    echo "Run 'make fetch-openings' to download it."
    exit 1
fi

mkdir -p "$OUTPUT"

# Fastchess refuses to start when its file-descriptor budget falls
# below the per-engine reservation. macOS reports "unlimited" for the
# default soft limit but fastchess still rejects that, so raise the
# soft limit to a comfortable concrete value before invoking.
ulimit -n 65536 2>/dev/null || true

PGN="$OUTPUT/$PGN_NAME"
LOG="$OUTPUT/$LOG_NAME"

echo "Running $ROUNDS pairs ($((ROUNDS * 2)) games) at $LIMIT_LABEL, concurrency=$CONCURRENCY"
echo "  dev:  $ENGINE_DEV ($NAME_DEV)"
echo "  base: $ENGINE_BASE ($NAME_BASE)"
echo "  pgn:  $PGN"
echo "  log:  $LOG"

"$FASTCHESS" \
    -engine cmd="$ENGINE_DEV" name="$NAME_DEV" \
    -engine cmd="$ENGINE_BASE" name="$NAME_BASE" \
    -each "${LIMIT_OPT[@]}" \
    -resign movecount=3 score=600 \
    -draw movenumber=34 movecount=8 score=20 \
    -rounds "$ROUNDS" \
    -repeat \
    -concurrency "$CONCURRENCY" \
    -openings file="$OPENINGS" format=epd order=random \
    -pgnout file="$PGN" \
    -recover \
    2>&1 | tee "$LOG"

echo ""
echo "Done. Results saved to $OUTPUT/"
