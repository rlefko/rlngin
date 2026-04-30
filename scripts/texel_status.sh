#!/usr/bin/env bash
set -euo pipefail

# Show what's currently running in the Texel pipeline and where each
# stage stands.
#
# Usage: texel_status.sh

OUTPUT="${OUTPUT:-tuning/texel}"
PID_FILE="$OUTPUT/pipeline.pid"

echo "=== Pipeline orchestrator ==="
if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    if kill -0 "$PID" 2>/dev/null; then
        ps -p "$PID" -o pid,etime,stat,command 2>/dev/null | tail -n +1
    else
        echo "PID $PID not running (stale pidfile at $PID_FILE)"
    fi
else
    echo "No pidfile at $PID_FILE (not launched via 'make texel-bg')"
fi

echo ""
echo "=== Active workers ==="
WORKER_PIDS=$(pgrep -f "fastchess|build/tune|extract_tuning_positions" 2>/dev/null | grep -v "^$$\$" || true)
if [ -n "$WORKER_PIDS" ]; then
    # shellcheck disable=SC2086
    ps -p $WORKER_PIDS -o pid,etime,stat,command 2>/dev/null | tail -n +1
else
    echo "(none)"
fi

echo ""
echo "=== Self-play progress ==="
if [ -f "$OUTPUT/selfplay.log" ]; then
    LAST_STARTED=$(grep "^Started game" "$OUTPUT/selfplay.log" 2>/dev/null | tail -1 || true)
    LAST_FINISHED=$(grep "^Finished game" "$OUTPUT/selfplay.log" 2>/dev/null | tail -1 || true)
    [ -n "$LAST_STARTED" ] && echo "$LAST_STARTED"
    [ -n "$LAST_FINISHED" ] && echo "$LAST_FINISHED"
    [ -z "$LAST_STARTED$LAST_FINISHED" ] && echo "(no games yet)"
else
    echo "(no selfplay.log)"
fi

echo ""
echo "=== Extract progress ==="
if [ -f "$OUTPUT/extract.log" ]; then
    tail -2 "$OUTPUT/extract.log"
else
    echo "(no extract.log)"
fi

echo ""
echo "=== Tune progress ==="
if [ -f "$OUTPUT/tune.log" ]; then
    LAST=$(grep -E "^pass [0-9]+|loss=" "$OUTPUT/tune.log" 2>/dev/null | tail -1 || true)
    if [ -n "$LAST" ]; then
        echo "$LAST"
    else
        tail -3 "$OUTPUT/tune.log"
    fi
else
    echo "(no tune.log)"
fi

echo ""
echo "=== Artifacts ==="
for f in games.pgn positions.epd checkpoint.txt; do
    p="$OUTPUT/$f"
    if [ -f "$p" ]; then
        size=$(ls -lh "$p" | awk '{print $5}')
        mtime=$(stat -f "%Sm" -t "%Y-%m-%d %H:%M" "$p" 2>/dev/null || stat -c "%y" "$p" 2>/dev/null || echo "?")
        printf "  %-20s %8s  %s\n" "$f" "$size" "$mtime"
    fi
done
LIVE_CKPT="tuning/checkpoint.txt"
if [ -f "$LIVE_CKPT" ]; then
    size=$(ls -lh "$LIVE_CKPT" | awk '{print $5}')
    mtime=$(stat -f "%Sm" -t "%Y-%m-%d %H:%M" "$LIVE_CKPT" 2>/dev/null || stat -c "%y" "$LIVE_CKPT" 2>/dev/null || echo "?")
    printf "  %-20s %8s  %s  (live)\n" "checkpoint.txt" "$size" "$mtime"
fi
