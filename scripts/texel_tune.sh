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
#   ADAM_EPOCHS:           Adam optimiser epochs between GN and CD
#                          (default: 100). Set to 0 to skip the Adam
#                          phase and run GN -> CD directly. The Adam
#                          phase reuses the GN feature cache and runs
#                          ~100x faster per iteration than CD; on a
#                          well-converged GN starting point it
#                          typically eliminates most of the CD ladder
#                          residual.
#   ADAM_LR:               Adam learning rate in cp-ish units
#                          (default: 1.0). Standard Texel-style
#                          gradient optimisation lr range is 0.5 to
#                          5.0; lower values are slower but more
#                          stable, higher values may overshoot.
#   LEAF_DEPTH:            PV walk depth for the leaf precompute
#                          (default: 0 = qsearch leaves only; 4-6 walks
#                          a low-depth alpha-beta PV before falling
#                          into qsearch, Andrew-Grant style)
#   VAL_FRACTION:          held-out validation slice as a fraction of
#                          the corpus (default: 0.10; 0 disables the
#                          split and the val gate)
#   VAL_GATE_WARMUP:       passes the val gate is in report-only mode
#                          before its patience counter starts ticking
#                          on non-improving passes (default: 5)
#   VAL_GATE_PATIENCE:     consecutive post-warmup passes without val
#                          improvement before the loop breaks
#                          (default: 8). Best-val params are restored
#                          on exit regardless of where the loop ends.
#   NO_VAL_GATE:           1 to disable the val-loss accept gate
#                          entirely while still emitting the bucketed
#                          val report each pass (default: 0)
#   VAL_EPD:               path to an external val corpus EPD that
#                          overrides the in-corpus split. The tuner
#                          uses the entire loaded file as the val
#                          partition (in-corpus self-play stays in
#                          train). Default: auto-detect
#                          tuning/val/master_positions.epd if it
#                          exists; populate it via
#                          scripts/fetch_val_corpus.sh. Falls back to
#                          the in-corpus stratified split when no
#                          file is available.
#   CURATED_EPD:           path to a small curated EPD pack to mix
#                          into the training set (default: unset).
#                          Read by the tuner directly; this script
#                          forwards the env to the child process.
#   CURATED_WEIGHT:        per-position weight for curated rows
#                          (default: 5.0; only meaningful with
#                          CURATED_EPD set)
#   PST_SMOOTH_LAMBDA:     PST smoothness regulariser strength
#                          (default: 1e-9). Read by the tuner.
#   PAWN_MIRROR_LAMBDA:    pawn PST mirror prior strength
#                          (default: 1e-8). Read by the tuner.

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
ARGS+=(--refit-k-every "${REFIT_K_EVERY:-0}")
ARGS+=(--refresh-leaves-every "${REFRESH_LEAVES_EVERY:-0}")
ARGS+=(--newton-passes "${NEWTON_PASSES:-10}")
ARGS+=(--gauss-newton "${USE_GAUSS_NEWTON:-1}")
ARGS+=(--adam-epochs "${ADAM_EPOCHS:-100}")
ARGS+=(--adam-lr "${ADAM_LR:-1.0}")
ARGS+=(--leaf-depth "${LEAF_DEPTH:-0}")
ARGS+=(--val-fraction "${VAL_FRACTION:-0.10}")
ARGS+=(--val-gate-warmup "${VAL_GATE_WARMUP:-5}")
ARGS+=(--val-gate-patience "${VAL_GATE_PATIENCE:-8}")
if [ "${NO_VAL_GATE:-0}" != "0" ]; then
    ARGS+=(--no-val-gate)
fi

echo "Texel tune: $CORPUS"
echo "  threads:           $THREADS"
echo "  max-passes:        $MAX_PASSES"
echo "  newton-passes:     ${NEWTON_PASSES:-10}"
echo "  gauss-newton:      ${USE_GAUSS_NEWTON:-1}"
echo "  adam-epochs:       ${ADAM_EPOCHS:-100}"
echo "  adam-lr:           ${ADAM_LR:-1.0}"
echo "  refit-K:           every ${REFIT_K_EVERY:-0} pass(es)"
echo "  refresh-leaves:    every ${REFRESH_LEAVES_EVERY:-0} pass(es)"
echo "  leaf-depth:        ${LEAF_DEPTH:-0}"
echo "  val-fraction:      ${VAL_FRACTION:-0.10}"
if [ "${NO_VAL_GATE:-0}" != "0" ]; then
    echo "  val-gate:          off (diagnostics only)"
else
    echo "  val-gate-warmup:   ${VAL_GATE_WARMUP:-5} pass(es)"
    echo "  val-gate-patience: ${VAL_GATE_PATIENCE:-8} pass(es)"
fi
if [ -n "${VAL_EPD:-}" ]; then
    echo "  val-epd (external): $VAL_EPD"
elif [ -f "tuning/val/master_positions.epd" ]; then
    echo "  val-epd (external): tuning/val/master_positions.epd (auto-detected)"
fi
if [ -n "${CURATED_EPD:-}" ]; then
    echo "  curated-epd:       $CURATED_EPD (weight=${CURATED_WEIGHT:-5.0})"
fi
if [ -n "${PST_SMOOTH_LAMBDA:-}" ]; then
    echo "  pst-smooth-lambda: $PST_SMOOTH_LAMBDA"
fi
if [ -n "${PAWN_MIRROR_LAMBDA:-}" ]; then
    echo "  pawn-mirror-lambda: $PAWN_MIRROR_LAMBDA"
fi
if [ -n "${FROM_CHECKPOINT:-}" ]; then
    echo "  resume:            $FROM_CHECKPOINT"
fi
echo "  log:               $LOG"
echo "  archive:           $ARCHIVE_CKPT"

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
