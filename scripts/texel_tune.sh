#!/usr/bin/env bash
set -euo pipefail

# Usage: texel_tune.sh [corpus] [threads] [max-passes]
#   corpus:     labeled EPD corpus (default: $OUTPUT/positions.epd)
#   threads:    parallel worker count (default: 6)
#   max-passes: coordinate-descent passes (default: 30)
#
# Drives build/tune over the Texel corpus and archives the tuner's
# in-flight checkpoint (tuning/checkpoint.txt) into $OUTPUT at the end
# so per-run artifacts do not collide.
#
# Environment variables:
#   OUTPUT:                output directory (default: tuning/texel)
#   TUNE:                  path to the tuner binary (default: ./build/tune)
#   FROM_CHECKPOINT:       resume from this checkpoint (default: unset)
#   REFIT_K_EVERY:         pass cadence for K refits (default: 4)
#   REFRESH_LEAVES_EVERY:  pass cadence for leaf recomputes (default: 8)
#   NEWTON_PASSES:         Newton-style initial passes before CD
#                          (default: 10; set 0 for legacy CD-only behavior)
#   USE_GAUSS_NEWTON:      1 to run Gauss-Newton via cached features,
#                          0 to fall back to diagonal Newton via finite
#                          differences on the loss (default: 1)

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
ARGS+=(--refit-k-every "${REFIT_K_EVERY:-4}")
ARGS+=(--refresh-leaves-every "${REFRESH_LEAVES_EVERY:-8}")
ARGS+=(--newton-passes "${NEWTON_PASSES:-10}")
ARGS+=(--gauss-newton "${USE_GAUSS_NEWTON:-1}")

echo "Texel tune: $CORPUS"
echo "  threads:        $THREADS"
echo "  max-passes:     $MAX_PASSES"
echo "  newton-passes:  ${NEWTON_PASSES:-10}"
echo "  gauss-newton:   ${USE_GAUSS_NEWTON:-1}"
echo "  refit-K:        every ${REFIT_K_EVERY:-4} pass(es)"
echo "  refresh-leaves: every ${REFRESH_LEAVES_EVERY:-8} pass(es)"
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
