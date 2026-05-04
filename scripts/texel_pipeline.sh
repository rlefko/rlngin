#!/usr/bin/env bash
set -euo pipefail

# Usage: texel_pipeline.sh [rounds] [limit] [concurrency] [tune-threads] [tune-passes]
#   rounds:         number of self-play game pairs (default 32000)
#   limit:          per-move budget for self-play, either Cute-Chess tc
#                   like "1+0.08" or literal "nodes=N" (default
#                   "nodes=100000")
#   concurrency:    self-play parallel games (default 12)
#   tune-threads:   parallel tuner threads (default 14)
#   tune-passes:    coordinate-descent passes (default 30)
#
# Drives the full Texel pipeline end-to-end:
#   self-play -> extract -> tune
# Each stage is skipped if its output artifact already exists (resumable);
# set FORCE=1 to redo every stage from scratch. Set WAIT_PID=<pid> to
# block until an external self-play job exits before starting (handy
# when piggy-backing on an already-running self-play).
#
# Each stage logs under $OUTPUT/. Designed to be launched detached via
# nohup so the run survives a closed shell:
#
#   nohup ./scripts/texel_pipeline.sh > tuning/texel/pipeline.log 2>&1 &
#   disown
#
# Environment variables:
#   OUTPUT:                output directory (default: tuning/texel)
#   ENGINE:                engine binary (default: ./build/rlngin)
#   TUNE:                  tuner binary (default: ./build/tune)
#   POLL_SECS:             seconds between WAIT_PID checks (default: 30)
#   FORCE:                 set to 1 to redo all stages (default: 0)
#   WAIT_PID:              external PID to wait for before starting
#                          (default: unset)
#   FROM_CHECKPOINT:       passed through to texel_tune.sh
#   REFIT_K_EVERY:         passed through to texel_tune.sh (default 4)
#   REFRESH_LEAVES_EVERY:  passed through to texel_tune.sh (default 8)
#   NEWTON_PASSES:         Newton-style initial passes before CD;
#                          passed through to texel_tune.sh (default 10)
#   USE_GAUSS_NEWTON:      1 for Gauss-Newton via cached features,
#                          0 for diagonal Newton; passed through to
#                          texel_tune.sh (default 1)
#   SKIP_PLIES / TAIL_PLIES: passed through to texel_extract.sh

ROUNDS="${1:-32000}"
LIMIT="${2:-nodes=100000}"
CONCURRENCY="${3:-12}"
TUNE_THREADS="${4:-14}"
TUNE_PASSES="${5:-30}"

OUTPUT="${OUTPUT:-tuning/texel}"
ENGINE="${ENGINE:-./build/rlngin}"
TUNE="${TUNE:-./build/tune}"
POLL_SECS="${POLL_SECS:-30}"
FORCE="${FORCE:-0}"
WAIT_PID="${WAIT_PID:-0}"

PGN="$OUTPUT/games.pgn"
EPD="$OUTPUT/positions.epd"

mkdir -p "$OUTPUT"

stamp() { date +"%Y-%m-%d %H:%M:%S"; }

echo "[$(stamp)] pipeline starting"
echo "  rounds:         $ROUNDS"
echo "  limit:          $LIMIT"
echo "  concurrency:    $CONCURRENCY"
echo "  tune-threads:   $TUNE_THREADS"
echo "  tune-passes:    $TUNE_PASSES"
echo "  newton-passes:  ${NEWTON_PASSES:-10}"
echo "  gauss-newton:   ${USE_GAUSS_NEWTON:-1}"
echo "  refit-K:        every ${REFIT_K_EVERY:-0} pass(es)"
echo "  refresh-leaves: every ${REFRESH_LEAVES_EVERY:-0} pass(es)"
echo "  output:         $OUTPUT"
echo "  force:          $FORCE"
[ "$WAIT_PID" != "0" ] && echo "  wait-pid:       $WAIT_PID"

# --- Optional: wait on an external self-play PID --------------------------

if [ "$WAIT_PID" != "0" ]; then
    echo "[$(stamp)] waiting on PID $WAIT_PID (poll every ${POLL_SECS}s)"
    while kill -0 "$WAIT_PID" 2>/dev/null; do
        sleep "$POLL_SECS"
    done
    echo "[$(stamp)] PID $WAIT_PID exited; resuming pipeline"
fi

# --- Ensure binaries exist ------------------------------------------------

if [ ! -x "$ENGINE" ]; then
    echo "[$(stamp)] $ENGINE missing; running 'make build'"
    make build
fi
if [ ! -x "$TUNE" ]; then
    echo "[$(stamp)] $TUNE missing; running 'make tune'"
    make tune
fi

# --- Stage 1: self-play ---------------------------------------------------

if [ "$FORCE" = "1" ] || [ ! -f "$PGN" ]; then
    echo "[$(stamp)] running self-play"
    OUTPUT="$OUTPUT" ./scripts/texel_selfplay.sh "$ROUNDS" "$LIMIT" "$CONCURRENCY"
    echo "[$(stamp)] self-play finished"
else
    echo "[$(stamp)] $PGN exists; skipping self-play (set FORCE=1 to redo)"
fi

# --- Stage 2: extract -----------------------------------------------------

if [ "$FORCE" = "1" ] || [ ! -f "$EPD" ]; then
    echo "[$(stamp)] running extract"
    OUTPUT="$OUTPUT" ./scripts/texel_extract.sh
    echo "[$(stamp)] extract finished"
else
    echo "[$(stamp)] $EPD exists; skipping extract (set FORCE=1 to redo)"
fi

# --- Stage 2.5: fetch external val corpus ---------------------------------
#
# Pulls a small master-games corpus from a public source (TWIC by
# default; see scripts/fetch_val_corpus.sh) and parks it at
# tuning/val/master_positions.epd. The tuner picks that file up
# automatically and uses it as the entire val partition, replacing
# the in-corpus stratified split. If the fetch fails (offline, 404,
# rate limited, etc.) the tuner silently falls back to the in-corpus
# split, so this stage is best-effort and never blocks the pipeline.

VAL_EPD_DEFAULT="tuning/val/master_positions.epd"
if [ -z "${VAL_EPD:-}" ]; then
    if [ ! -f "$VAL_EPD_DEFAULT" ]; then
        echo "[$(stamp)] fetching external master val corpus"
        if ./scripts/fetch_val_corpus.sh; then
            export VAL_EPD="$VAL_EPD_DEFAULT"
            echo "[$(stamp)] external val corpus ready at $VAL_EPD"
        else
            echo "[$(stamp)] no external val corpus (offline or fetch failed); " \
                 "falling back to in-corpus stratified val split"
        fi
    else
        export VAL_EPD="$VAL_EPD_DEFAULT"
        echo "[$(stamp)] reusing existing val corpus at $VAL_EPD"
    fi
else
    echo "[$(stamp)] VAL_EPD set explicitly to $VAL_EPD"
fi

# --- Stage 3: tune --------------------------------------------------------

echo "[$(stamp)] running tune ($TUNE_THREADS threads, $TUNE_PASSES passes)"
OUTPUT="$OUTPUT" ./scripts/texel_tune.sh "$EPD" "$TUNE_THREADS" "$TUNE_PASSES"
echo "[$(stamp)] tune finished"

echo "[$(stamp)] pipeline complete"
echo ""
echo "Artifacts:"
echo "  pgn:        $PGN"
echo "  corpus:     $EPD"
echo "  checkpoint: $OUTPUT/checkpoint.txt"
echo ""
echo "Dump tuned values with: $TUNE --dump $OUTPUT/checkpoint.txt"
