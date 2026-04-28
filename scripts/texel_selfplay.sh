#!/usr/bin/env bash
set -euo pipefail

# Usage: texel_selfplay.sh [rounds] [limit] [concurrency]
#   rounds:      number of game pairs (total games = 2 * rounds, default 32000)
#   limit:       per-move budget (default "1+0.08"). Two flavors are
#                accepted: a Cute-Chess-style time control like
#                "1+0.08" gets passed through as -each tc=..., and a
#                literal "nodes=N" gets passed through as -each
#                nodes=N. Nodes mode is deterministic and roughly 6x
#                faster wall-clock per game on this engine, at the
#                cost of label quality vs. real-clock search.
#   concurrency: parallel games (default 6)
#
# Drives a long fastchess self-play match for Texel corpus generation.
# Both sides run the same engine binary; openings are drawn at random
# from the UHO_Lichess_4852_v1 EPD book. Every completed game is
# appended to "$OUTPUT/games.pgn" so a parallel "scripts/texel-watch.sh"
# can render games as they finish without disturbing the engines.
#
# Run under caffeinate so the laptop stays awake for the full match:
#   caffeinate -i ./scripts/texel_selfplay.sh
#
# Environment variables:
#   ENGINE:    path to the engine (default: ./build/rlngin)
#   FASTCHESS: path to fastchess (default: ./fastchess)
#   OPENINGS:  path to opening book (default: openings/UHO_Lichess_4852_v1.epd)
#   OUTPUT:    output directory (default: tuning/texel)

ROUNDS="${1:-32000}"
LIMIT="${2:-1+0.08}"
CONCURRENCY="${3:-6}"

if [[ "$LIMIT" == nodes=* ]]; then
    LIMIT_OPT=("$LIMIT")
    LIMIT_LABEL="$LIMIT"
else
    LIMIT_OPT=(tc="$LIMIT")
    LIMIT_LABEL="tc=$LIMIT"
fi

ENGINE="${ENGINE:-./build/rlngin}"
FASTCHESS="${FASTCHESS:-./fastchess}"
OPENINGS="${OPENINGS:-openings/UHO_Lichess_4852_v1.epd}"
OUTPUT="${OUTPUT:-tuning/texel}"

if [ ! -x "$ENGINE" ]; then
    echo "Error: engine not found at $ENGINE (run 'make build')"
    exit 1
fi

if [ ! -f "$FASTCHESS" ]; then
    echo "Error: fastchess not found at $FASTCHESS (run 'make fetch-fastchess')"
    exit 1
fi

if [ ! -f "$OPENINGS" ]; then
    echo "Error: opening book not found at $OPENINGS (run 'make fetch-openings')"
    exit 1
fi

mkdir -p "$OUTPUT"

# Fastchess refuses to start when its file-descriptor budget falls
# below the per-engine reservation. macOS reports "unlimited" for the
# default soft limit but fastchess still rejects that, so raise the
# soft limit to a comfortable concrete value before invoking.
ulimit -n 65536 2>/dev/null || true

PGN="$OUTPUT/games.pgn"
LOG="$OUTPUT/selfplay.log"

echo "Texel self-play: $ROUNDS pairs ($((ROUNDS * 2)) games) at $LIMIT_LABEL, concurrency=$CONCURRENCY"
echo "  engine:   $ENGINE"
echo "  openings: $OPENINGS"
echo "  pgn:      $PGN"
echo "  log:      $LOG"

"$FASTCHESS" \
    -engine cmd="$ENGINE" name=rlngin-a \
    -engine cmd="$ENGINE" name=rlngin-b \
    -each "${LIMIT_OPT[@]}" \
    -resign movecount=3 score=600 \
    -draw movenumber=34 movecount=8 score=20 \
    -rounds "$ROUNDS" \
    -repeat \
    -concurrency "$CONCURRENCY" \
    -openings file="$OPENINGS" format=epd order=random \
    -pgnout file="$PGN" \
    -recover \
    2>&1 | tee "$LOG"

echo ""
echo "Done. PGN at $PGN"
