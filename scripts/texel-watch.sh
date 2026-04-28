#!/usr/bin/env bash
set -euo pipefail

# Usage: texel-watch.sh [pgn-path] [interval-sec]
#
# Live viewer for an in-progress Texel self-play match. Two flavors:
#
#   - If `watch` is on PATH and an explicit interval is given as $2,
#     repaint a stats-and-recent-games dashboard every N seconds via
#     `tools/texel/watch_pgn.py --once`. Cleanest for a dedicated
#     terminal tile.
#   - Otherwise tail the PGN continuously and print each completed game
#     as it lands, with a periodic stats banner. Lighter on the eye if
#     you just want a stream of game results.
#
# Both flavors only read the PGN file, so they have zero impact on the
# engines running the match.

PGN="${1:-tuning/texel/games.pgn}"
INTERVAL="${2:-}"

if [ -n "$INTERVAL" ] && command -v watch >/dev/null 2>&1; then
    exec watch -t -n "$INTERVAL" python3 tools/texel/watch_pgn.py --once "$PGN"
fi

exec python3 tools/texel/watch_pgn.py "$PGN"
