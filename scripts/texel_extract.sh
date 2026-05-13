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
#   OUTPUT:        output directory (default: tuning/texel)
#   SKIP_PLIES:    plies to skip at the start (default: 8)
#   TAIL_PLIES:    plies to skip at the end (default: 2)
#   LABEL_FROM_CP: 1 to label each position from its engine cp
#                  comment via a logistic (default: 1; the Stockfish
#                  self-play corpus already carries depth-bounded cp
#                  per move and per-position cp is strictly more
#                  informative than aggregate W/D/L). Set to 0 to
#                  fall back to the legacy game-outcome label.
#   CP_SCALE:      cp scale for the logistic squash when
#                  LABEL_FROM_CP=1 (default: 200)

OUTPUT="${OUTPUT:-tuning/texel}"
PGN="${1:-$OUTPUT/games.pgn}"
OUT="${2:-$OUTPUT/positions.epd}"
shift $(( $# < 2 ? $# : 2 ))
if [ "${1:-}" = "--" ]; then
    shift
fi
EXTRA_ARGS=("$@")

SKIP_PLIES="${SKIP_PLIES:-0}"
TAIL_PLIES="${TAIL_PLIES:-2}"
LABEL_FROM_CP="${LABEL_FROM_CP:-1}"
CP_SCALE="${CP_SCALE:-150}"
LOG="$OUTPUT/extract.log"

if [ ! -f "$PGN" ]; then
    echo "Error: PGN not found at $PGN (run scripts/texel_selfplay.sh first)"
    exit 1
fi

mkdir -p "$OUTPUT"

ARGS=()
ARGS+=(--skip-plies "$SKIP_PLIES")
ARGS+=(--tail-plies "$TAIL_PLIES")
if [ "$LABEL_FROM_CP" != "0" ]; then
    ARGS+=(--label-from-cp --cp-scale "$CP_SCALE")
fi
ARGS+=(${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})

echo "Texel extract: $PGN -> $OUT"
echo "  skip-plies:     $SKIP_PLIES"
echo "  tail-plies:     $TAIL_PLIES"
if [ "$LABEL_FROM_CP" != "0" ]; then
    echo "  label-from-cp:  on (cp-scale=$CP_SCALE)"
else
    echo "  label-from-cp:  off (legacy W/D/L outcome label)"
fi
echo "  log:            $LOG"

python3 scripts/extract_tuning_positions.py \
    "${ARGS[@]}" \
    "$PGN" "$OUT" \
    2>&1 | tee "$LOG"

echo ""
echo "Done. Corpus at $OUT"
