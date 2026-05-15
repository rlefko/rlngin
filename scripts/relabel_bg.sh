#!/usr/bin/env bash
set -euo pipefail

# Launch the parallel Stockfish corpus relabel detached so it
# survives a closed shell. Wraps relabel_corpus_with_sf.py under
# `caffeinate -is` so the Mac stays awake for the full ~17h run, and
# writes a PID file at tuning/texel/relabel.pid for status / stop
# targets.
#
# Usage: relabel_bg.sh [nodes] [workers] [hash-mb]
#
# Env:
#   FORCE=1            wipe existing shards before starting (else resume)
#   IN_EPD             input corpus (default tuning/texel/positions.epd)
#   OUT_EPD            merged output (default tuning/texel/positions_sf.epd)
#   SHARD_DIR          per-worker shard dir (default tuning/texel/relabel_shards)
#   CP_SCALE           label logistic scale (default 200)
#   SYZYGY_PATH        Stockfish SyzygyPath (default $HOME/projects/chess/3-4-5)
#   STOCKFISH          SF binary path

NODES="${1:-100000}"
WORKERS="${2:-14}"
HASH_MB="${3:-16}"

IN_EPD="${IN_EPD:-tuning/texel/positions.epd}"
OUT_EPD="${OUT_EPD:-tuning/texel/positions_sf.epd}"
SHARD_DIR="${SHARD_DIR:-tuning/texel/relabel_shards}"
CP_SCALE="${CP_SCALE:-200}"
FORCE="${FORCE:-0}"

OUTPUT_DIR="$(dirname "$OUT_EPD")"
PID_FILE="$OUTPUT_DIR/relabel.pid"
LOG_FILE="$OUTPUT_DIR/relabel.log"

mkdir -p "$OUTPUT_DIR" "$SHARD_DIR"

# Refuse to start a second copy.
if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE")
    if kill -0 "$OLD_PID" 2>/dev/null; then
        echo "Error: relabel already running with PID $OLD_PID"
        echo "  stop it first with: make relabel-stop"
        echo "  or check progress:  make relabel-status"
        exit 1
    fi
    rm -f "$PID_FILE"
fi

if [ "$FORCE" = "1" ]; then
    echo "FORCE=1: wiping existing shards under $SHARD_DIR"
    find "$SHARD_DIR" -maxdepth 1 -name 'shard_*.epd' -type f -delete 2>/dev/null || true
    rm -f "$OUT_EPD" 2>/dev/null || true
fi

# Count existing shards so the operator can tell at a glance whether
# this is a fresh run or a resume. `find` returns success on an empty
# match so it plays nicely with `set -euo pipefail`.
EXISTING_SHARDS=$(find "$SHARD_DIR" -maxdepth 1 -name 'shard_*.epd' -type f 2>/dev/null | wc -l | tr -d ' ')
if [ "$EXISTING_SHARDS" != "0" ]; then
    EXISTING_ROWS=$(find "$SHARD_DIR" -maxdepth 1 -name 'shard_*.epd' -type f -exec grep -hv '^#' {} + 2>/dev/null | wc -l | tr -d ' ')
    echo "Resuming: $EXISTING_SHARDS shards already present with $EXISTING_ROWS rows"
fi

ARGS=(
    --in "$IN_EPD"
    --out "$OUT_EPD"
    --shard-dir "$SHARD_DIR"
    --nodes "$NODES"
    --workers "$WORKERS"
    --hash-mb "$HASH_MB"
    --cp-scale "$CP_SCALE"
)
if [ -n "${SYZYGY_PATH:-}" ]; then
    ARGS+=(--syzygy-path "$SYZYGY_PATH")
fi
if [ -n "${STOCKFISH:-}" ]; then
    ARGS+=(--stockfish "$STOCKFISH")
fi

# Caffeinate so a 17h overnight run is not interrupted by idle sleep
# or system sleep. -i = prevent idle sleep, -s = prevent system sleep
# (the -d display flag is intentionally omitted; closed lid is fine).
nohup caffeinate -is python3 ./scripts/relabel_corpus_with_sf.py "${ARGS[@]}" \
    > "$LOG_FILE" 2>&1 < /dev/null &
PID=$!
disown $PID 2>/dev/null || true
echo "$PID" > "$PID_FILE"

echo "Relabel launched with PID $PID (caffeinated)"
echo "  nodes:    $NODES"
echo "  workers:  $WORKERS"
echo "  hash:     ${HASH_MB} MB"
echo "  in:       $IN_EPD"
echo "  shards:   $SHARD_DIR"
echo "  out:      $OUT_EPD"
echo "  log:      $LOG_FILE"
echo "  pidfile:  $PID_FILE"
echo ""
echo "Monitor: tail -f $LOG_FILE"
echo "Status:  make relabel-status"
echo "Stop:    make relabel-stop"
