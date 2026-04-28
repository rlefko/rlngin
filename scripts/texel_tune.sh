#!/usr/bin/env bash
set -euo pipefail

# Usage: texel_tune.sh [corpus] [threads] [max-passes]
#   corpus:     labeled EPD corpus (default: $OUTPUT/positions.epd)
#   threads:    parallel worker count (default: 6)
#   max-passes: coordinate-descent passes (default: 30)
#
# Drives build/tune over the Texel corpus and archives the final
# checkpoint into the per-run output directory. The tuner itself
# hard-codes its in-flight checkpoint path to tuning/checkpoint.txt;
# we copy it to $OUTPUT/checkpoint.txt at the end so this run's
# artifacts do not collide with other tuning trees.
#
# Run under caffeinate so the laptop stays awake for the full tune:
#   caffeinate -i ./scripts/texel_tune.sh
#
# Environment variables:
#   OUTPUT:                output directory (default: tuning/texel)
#   TUNE:                  path to the tuner binary (default: ./build/tune)
#   FROM_CHECKPOINT:       resume from this checkpoint (default: unset)
#   REFIT_K_EVERY:         pass cadence for K refits (default: 5)
#   REFRESH_LEAVES_EVERY:  pass cadence for leaf recomputes (default: unset)

OUTPUT="${OUTPUT:-tuning/texel}"
CORPUS="${1:-$OUTPUT/positions.epd}"
THREADS="${2:-6}"
MAX_PASSES="${3:-30}"
TUNE="${TUNE:-./build/tune}"
LOG="$OUTPUT/tune.log"
LIVE_CKPT="tuning/checkpoint.txt"
ARCHIVE_CKPT="$OUTPUT/checkpoint.txt"

if [ ! -x "$TUNE" ]; then
    echo "Error: tuner not found at $TUNE (run 'make tune')"
    exit 1
fi

if [ ! -f "$CORPUS" ]; then
    echo "Error: corpus not found at $CORPUS (run scripts/texel_extract.sh first)"
    exit 1
fi

mkdir -p "$OUTPUT" tuning

ARGS=()
if [ -n "${FROM_CHECKPOINT:-}" ]; then
    if [ ! -f "$FROM_CHECKPOINT" ]; then
        echo "Error: --from checkpoint not found at $FROM_CHECKPOINT"
        exit 1
    fi
    ARGS+=(--from "$FROM_CHECKPOINT")
fi
ARGS+=(--refit-k-every "${REFIT_K_EVERY:-5}")
if [ -n "${REFRESH_LEAVES_EVERY:-}" ]; then
    ARGS+=(--refresh-leaves-every "$REFRESH_LEAVES_EVERY")
fi

echo "Texel tune: $CORPUS"
echo "  threads:    $THREADS"
echo "  max-passes: $MAX_PASSES"
echo "  refit-K:    every ${REFIT_K_EVERY:-5} pass(es)"
if [ -n "${FROM_CHECKPOINT:-}" ]; then
    echo "  resume:     $FROM_CHECKPOINT"
fi
echo "  log:        $LOG"
echo "  archive:    $ARCHIVE_CKPT"

"$TUNE" "${ARGS[@]}" "$CORPUS" "$THREADS" "$MAX_PASSES" 2>&1 | tee "$LOG"

if [ -f "$LIVE_CKPT" ]; then
    cp "$LIVE_CKPT" "$ARCHIVE_CKPT"
    echo ""
    echo "Done. Final checkpoint archived to $ARCHIVE_CKPT"
    echo "Dump tuned values with: $TUNE --dump $ARCHIVE_CKPT"
else
    echo ""
    echo "Warning: no checkpoint at $LIVE_CKPT after tune; nothing archived"
    exit 1
fi
