#!/usr/bin/env bash
set -euo pipefail

# Usage: texel-watch.sh [pgn-path] [interval-sec]
#
# Live dashboard for an in-progress Texel self-play match. Re-renders
# every `interval-sec` seconds via `watch -t -n`, falling back to the
# Python tool's own follow loop when `watch` is unavailable.

PGN="${1:-tuning/texel/games.pgn}"
INTERVAL="${2:-2}"

if command -v watch >/dev/null 2>&1; then
    exec watch -t -n "$INTERVAL" python3 tools/texel/watch_pgn.py --once "$PGN"
fi

exec python3 tools/texel/watch_pgn.py --poll-interval "$INTERVAL" "$PGN"
