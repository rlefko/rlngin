#!/usr/bin/env bash
set -euo pipefail

# Usage: texel_extract.sh [pgn] [out]
#   pgn: input PGN (default: $OUTPUT/games.pgn)
#   out: output labeled EPD (default: $OUTPUT/positions.epd)
#
# Wraps scripts/extract_tuning_positions.py with the Texel-tuning paths
# and a tee'd log. Defaults match the CPW Texel recipe: skip the first
# 8 plies as opening, drop the last 2 plies, and skip any position
# whose engine score during the game was a mate score.
#
# Run under caffeinate so the laptop stays awake:
#   caffeinate -i ./scripts/texel_extract.sh
#
# Environment variables:
#   OUTPUT:       output directory (default: tuning/texel)
#   SKIP_PLIES:   plies to skip at the start (default: 8)
#   TAIL_PLIES:   plies to skip at the end (default: 2)
#   KEEP_MATES:   if set to "1", retain mate-scored positions

OUTPUT="${OUTPUT:-tuning/texel}"
PGN="${1:-$OUTPUT/games.pgn}"
OUT="${2:-$OUTPUT/positions.epd}"
SKIP_PLIES="${SKIP_PLIES:-8}"
TAIL_PLIES="${TAIL_PLIES:-2}"
LOG="$OUTPUT/extract.log"

if [ ! -f "$PGN" ]; then
    echo "Error: PGN not found at $PGN (run scripts/texel_selfplay.sh first)"
    exit 1
fi

mkdir -p "$OUTPUT"

EXTRA_ARGS=()
if [ "${KEEP_MATES:-0}" = "1" ]; then
    EXTRA_ARGS+=(--no-mate-filter)
fi

echo "Texel extract: $PGN -> $OUT"
echo "  skip-plies: $SKIP_PLIES"
echo "  tail-plies: $TAIL_PLIES"
echo "  mate filter: $([ "${KEEP_MATES:-0}" = "1" ] && echo off || echo on)"
echo "  log: $LOG"

python3 scripts/extract_tuning_positions.py \
    --skip-plies "$SKIP_PLIES" \
    --tail-plies "$TAIL_PLIES" \
    ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"} \
    "$PGN" "$OUT" \
    2>&1 | tee "$LOG"

echo ""
echo "Done. Corpus at $OUT"
