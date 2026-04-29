#!/usr/bin/env bash
set -euo pipefail

# Usage: texel_extract.sh [pgn] [out] [-- <extra extractor flags>]
#   pgn: input PGN (default: $OUTPUT/games.pgn)
#   out: output labeled EPD (default: $OUTPUT/positions.epd)
#
# Thin wrapper around scripts/extract_tuning_positions.py with the
# Texel-tuning paths and a tee'd log. Anything after `--` is forwarded
# verbatim to the Python driver.
#
# Environment variables:
#   OUTPUT:     output directory (default: tuning/texel)
#   SKIP_PLIES: plies to skip at the start (default: 8)
#   TAIL_PLIES: plies to skip at the end (default: 2)

OUTPUT="${OUTPUT:-tuning/texel}"
PGN="${1:-$OUTPUT/games.pgn}"
OUT="${2:-$OUTPUT/positions.epd}"
shift $(( $# < 2 ? $# : 2 ))
if [ "${1:-}" = "--" ]; then
    shift
fi
EXTRA_ARGS=("$@")

SKIP_PLIES="${SKIP_PLIES:-8}"
TAIL_PLIES="${TAIL_PLIES:-2}"
LOG="$OUTPUT/extract.log"

if [ ! -f "$PGN" ]; then
    echo "Error: PGN not found at $PGN (run scripts/texel_selfplay.sh first)"
    exit 1
fi

mkdir -p "$OUTPUT"

echo "Texel extract: $PGN -> $OUT"
echo "  skip-plies: $SKIP_PLIES"
echo "  tail-plies: $TAIL_PLIES"
echo "  log: $LOG"

python3 scripts/extract_tuning_positions.py \
    --skip-plies "$SKIP_PLIES" \
    --tail-plies "$TAIL_PLIES" \
    ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} \
    "$PGN" "$OUT" \
    2>&1 | tee "$LOG"

echo ""
echo "Done. Corpus at $OUT"
