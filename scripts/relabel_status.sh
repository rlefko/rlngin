#!/usr/bin/env bash
set -euo pipefail

# Quick status check on the parallel Stockfish corpus relabel. Reports:
#   - Whether the run is currently active (PID alive + caffeinate)
#   - Per-shard row counts and meta header
#   - Aggregate progress toward the corpus total
#   - Recent log tail and ETA from the rate worker reports
#
# Usage: relabel_status.sh

IN_EPD="${IN_EPD:-tuning/texel/positions.epd}"
OUT_EPD="${OUT_EPD:-tuning/texel/positions_sf.epd}"
SHARD_DIR="${SHARD_DIR:-tuning/texel/relabel_shards}"
OUTPUT_DIR="$(dirname "$OUT_EPD")"
PID_FILE="$OUTPUT_DIR/relabel.pid"
LOG_FILE="$OUTPUT_DIR/relabel.log"

# Activity check.
if [ -f "$PID_FILE" ]; then
    PID=$(cat "$PID_FILE")
    if kill -0 "$PID" 2>/dev/null; then
        ETIME=$(ps -p "$PID" -o etime= 2>/dev/null | tr -d ' ' || echo '?')
        echo "Status: RUNNING (driver PID $PID, elapsed $ETIME)"
        SF_COUNT=$(pgrep -f stockfish 2>/dev/null | wc -l | tr -d ' ')
        echo "  Stockfish workers alive: $SF_COUNT"
    else
        echo "Status: NOT RUNNING (stale PID file; PID $PID exited)"
    fi
else
    echo "Status: NOT RUNNING (no PID file)"
fi
echo ""

# Corpus + shard accounting.
if [ -f "$IN_EPD" ]; then
    TOTAL=$(wc -l < "$IN_EPD" | tr -d ' ')
    echo "Input corpus: $IN_EPD ($TOTAL rows)"
else
    TOTAL=0
    echo "Input corpus: $IN_EPD (NOT FOUND)"
fi

if [ -d "$SHARD_DIR" ]; then
    SHARDS=$(find "$SHARD_DIR" -maxdepth 1 -name 'shard_*.epd' -type f 2>/dev/null | sort)
    if [ -z "$SHARDS" ]; then
        echo "Shards:       0 (none yet)"
    else
        SHARD_COUNT=$(echo "$SHARDS" | wc -l | tr -d ' ')
        ROWS=$(echo "$SHARDS" | xargs grep -hv '^#' 2>/dev/null | wc -l | tr -d ' ')
        if [ "$TOTAL" != "0" ]; then
            PCT=$(awk -v r="$ROWS" -v t="$TOTAL" 'BEGIN{printf "%.1f", 100.0*r/t}')
        else
            PCT="?"
        fi
        echo "Shards:       $SHARD_COUNT under $SHARD_DIR"
        echo "Progress:     $ROWS / $TOTAL rows (${PCT}%)"

        # Meta header is repeated per shard; show the first one's value.
        FIRST_SHARD=$(echo "$SHARDS" | head -1)
        META=$(grep -m1 '^# nodes=' "$FIRST_SHARD" 2>/dev/null || true)
        if [ -n "$META" ]; then
            echo "Shard meta:   $META"
        fi

        # Per-shard line counts for an at-a-glance balance check. If
        # one shard is far behind the others, that worker probably
        # crashed and silently exited.
        echo ""
        echo "Per-shard row counts:"
        for f in $SHARDS; do
            n=$(grep -cv '^#' "$f" || true)
            printf "  %-50s %s\n" "$f" "$n"
        done
    fi
fi
echo ""

# Recent log tail. Workers print every 30s; show the most recent
# rate-line per worker if available, then the very last few lines.
if [ -f "$LOG_FILE" ]; then
    echo "Latest per-worker rate (one line each):"
    # grep returns 1 when no matches; force success so pipefail does
    # not abort the script before the log tail prints.
    LATEST=$( (grep -E '^\s*w[0-9]+:' "$LOG_FILE" 2>/dev/null || true) \
                  | awk '{ key=$1; a[key]=$0 } END { for (k in a) print "  " a[k] }' \
                  | sort )
    if [ -z "$LATEST" ]; then
        echo "  (workers have not reported yet; they print every 30s)"
    else
        echo "$LATEST"
    fi
    echo ""
    echo "Log tail ($LOG_FILE):"
    tail -5 "$LOG_FILE" | sed 's/^/  /'
fi
