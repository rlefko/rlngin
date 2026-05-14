#!/usr/bin/env bash
set -euo pipefail

# Stop the relabel run started by relabel_bg.sh. Kills:
#   1. The python relabel driver (read from the PID file)
#   2. Its caffeinate wrapper, if any (also under the same process group)
#   3. Every orphan Stockfish worker still pinned to this run
#
# Survival rule: workers writing shards flush every 100 rows, so a
# SIGTERM/SIGKILL here loses at most that many rows per worker. The
# relabel can be resumed by re-running `make relabel-bg`.

IN_EPD="${IN_EPD:-tuning/texel/positions.epd}"
OUT_EPD="${OUT_EPD:-tuning/texel/positions_sf.epd}"
SHARD_DIR="${SHARD_DIR:-tuning/texel/relabel_shards}"
OUTPUT_DIR="$(dirname "$OUT_EPD")"
PID_FILE="$OUTPUT_DIR/relabel.pid"

PIDS_TO_KILL=()

# Driver from the pid file.
if [ -f "$PID_FILE" ]; then
    DRIVER_PID=$(cat "$PID_FILE")
    if kill -0 "$DRIVER_PID" 2>/dev/null; then
        echo "Stopping relabel driver PID $DRIVER_PID"
        PIDS_TO_KILL+=("$DRIVER_PID")
    else
        echo "Stale PID file: PID $DRIVER_PID is not running"
    fi
    rm -f "$PID_FILE"
fi

# Any python relabel driver (covers the case where the pid file was lost).
DRIVERS=$(pgrep -f "relabel_corpus_with_sf.py" 2>/dev/null || true)
for p in $DRIVERS; do
    PIDS_TO_KILL+=("$p")
done

# caffeinate processes started for this run.
CAFFEINATES=$(pgrep -f "caffeinate.*relabel_corpus_with_sf" 2>/dev/null || true)
for p in $CAFFEINATES; do
    PIDS_TO_KILL+=("$p")
done

# Stockfish workers spawned by the relabel. We assume any Stockfish
# instance currently running on this box was spawned by us; tighten
# this if your workflow ever has unrelated SF processes running
# concurrently.
SF_WORKERS=$(pgrep -f "stockfish" 2>/dev/null || true)
for p in $SF_WORKERS; do
    PIDS_TO_KILL+=("$p")
done

# Dedupe and send SIGTERM first so workers flush their last shard
# write cleanly. Five-second grace, then SIGKILL anything still alive.
if [ ${#PIDS_TO_KILL[@]} -gt 0 ]; then
    UNIQUE_PIDS=$(printf "%s\n" "${PIDS_TO_KILL[@]}" | sort -u)
    echo "Sending SIGTERM to: $UNIQUE_PIDS"
    for p in $UNIQUE_PIDS; do
        kill -TERM "$p" 2>/dev/null || true
    done
    sleep 5
    STILL_ALIVE=""
    for p in $UNIQUE_PIDS; do
        if kill -0 "$p" 2>/dev/null; then
            STILL_ALIVE="$STILL_ALIVE $p"
        fi
    done
    if [ -n "$STILL_ALIVE" ]; then
        echo "Sending SIGKILL to:$STILL_ALIVE"
        for p in $STILL_ALIVE; do
            kill -KILL "$p" 2>/dev/null || true
        done
    fi
    echo "Done."
else
    echo "No relabel processes were running."
fi

if [ -d "$SHARD_DIR" ]; then
    SHARD_COUNT=$(find "$SHARD_DIR" -maxdepth 1 -name 'shard_*.epd' -type f 2>/dev/null | wc -l | tr -d ' ')
    if [ "$SHARD_COUNT" != "0" ]; then
        ROWS=$(find "$SHARD_DIR" -maxdepth 1 -name 'shard_*.epd' -type f -exec grep -hv '^#' {} + 2>/dev/null | wc -l | tr -d ' ')
        echo ""
        echo "$SHARD_COUNT shard(s) preserved with $ROWS rows total."
        echo "Resume with: make relabel-bg"
    fi
fi
