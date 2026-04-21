#!/usr/bin/env bash
set -euo pipefail

# Usage: spsa-monitor.sh [output-dir] [interval-sec]
#
# Live dashboard for an in-progress SPSA run. Uses `watch` to re-render
# the panel every `interval-sec` seconds with proper in-place refresh
# (watch diffs frames and only repaints changed cells). Stop with Ctrl-C.
#
# Falls back to the Python driver's own ANSI-clear loop when `watch` is
# unavailable.

OUT="${1:-${OUTPUT:-tuning/spsa}}"
INTERVAL="${2:-2}"

if command -v watch >/dev/null 2>&1; then
    exec watch -t -n "$INTERVAL" python3 tools/spsa/monitor.py --once "$OUT"
fi

exec python3 tools/spsa/monitor.py --interval "$INTERVAL" "$OUT"
