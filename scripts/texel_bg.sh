#!/usr/bin/env bash
set -euo pipefail

# Launch the Texel pipeline detached so it survives a closed shell.
# Wraps texel_pipeline.sh with nohup and a PID file at
# tuning/texel/pipeline.pid for stop / status / dump targets.
#
# Usage: texel_bg.sh [rounds] [limit] [concurrency] [tune-threads] [tune-passes]

ROUNDS="${1:-32000}"
LIMIT="${2:-nodes=100000}"
CONCURRENCY="${3:-12}"
TUNE_THREADS="${4:-14}"
TUNE_PASSES="${5:-30}"

OUTPUT="${OUTPUT:-tuning/texel}"
PID_FILE="$OUTPUT/pipeline.pid"
LOG_FILE="$OUTPUT/pipeline.log"

mkdir -p "$OUTPUT"

if [ -f "$PID_FILE" ]; then
    OLD_PID=$(cat "$PID_FILE")
    if kill -0 "$OLD_PID" 2>/dev/null; then
        echo "Error: pipeline already running with PID $OLD_PID"
        echo "  stop it first with: make texel-stop"
        exit 1
    fi
    rm -f "$PID_FILE"
fi

nohup ./scripts/texel_pipeline.sh "$ROUNDS" "$LIMIT" "$CONCURRENCY" "$TUNE_THREADS" "$TUNE_PASSES" \
    > "$LOG_FILE" 2>&1 < /dev/null &
PID=$!
disown $PID 2>/dev/null || true
echo "$PID" > "$PID_FILE"

echo "Pipeline launched with PID $PID"
echo "  log:     $LOG_FILE"
echo "  pidfile: $PID_FILE"
echo ""
echo "Monitor: tail -f $LOG_FILE"
echo "Status:  make texel-status"
echo "Stop:    make texel-stop"
echo "Dump:    make texel-dump"
echo "Apply:   make texel-apply"
