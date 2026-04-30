#!/usr/bin/env bash
set -euo pipefail

# Kill the running Texel pipeline and every descendant. Walks the
# process tree under the PID stored in $OUTPUT/pipeline.pid, then sweeps
# any orphan tune / fastchess / extract processes that are pinned to the
# Texel output directory.
#
# Usage: texel_stop.sh

OUTPUT="${OUTPUT:-tuning/texel}"
PID_FILE="$OUTPUT/pipeline.pid"

kill_tree() {
    local pid=$1
    local sig="${2:-TERM}"
    local children
    children=$(pgrep -P "$pid" 2>/dev/null || true)
    for child in $children; do
        kill_tree "$child" "$sig"
    done
    kill "-$sig" "$pid" 2>/dev/null || true
}

if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    if kill -0 "$PID" 2>/dev/null; then
        echo "Stopping pipeline tree under PID $PID (SIGTERM)"
        kill_tree "$PID" TERM
        # Give children up to 3s to clean up before SIGKILL.
        for _ in 1 2 3; do
            sleep 1
            kill -0 "$PID" 2>/dev/null || break
        done
        if kill -0 "$PID" 2>/dev/null; then
            echo "Pipeline still alive; sending SIGKILL"
            kill_tree "$PID" KILL
        fi
        echo "Pipeline stopped"
    else
        echo "PID $PID not running (stale pidfile)"
    fi
    rm -f "$PID_FILE"
else
    echo "No pipeline PID file at $PID_FILE"
fi

# Sweep any orphan workers pinned to the Texel output. These can exist
# if a previous pipeline was killed at the wrapper layer (the wrapper
# died but its child got reparented to PID 1).
sweep() {
    local pattern="$1"
    local label="$2"
    local found
    found=$(pgrep -f "$pattern" 2>/dev/null || true)
    if [ -n "$found" ]; then
        echo "Killing orphan $label processes: $found"
        echo "$found" | xargs kill 2>/dev/null || true
        sleep 1
        local survivors
        survivors=$(pgrep -f "$pattern" 2>/dev/null || true)
        if [ -n "$survivors" ]; then
            echo "$survivors" | xargs kill -9 2>/dev/null || true
        fi
    fi
}

sweep "build/tune .*$OUTPUT" "tuner"
sweep "fastchess.*pgnout file=$OUTPUT" "fastchess"
sweep "extract_tuning_positions.py.*$OUTPUT" "extractor"

echo "Done"
