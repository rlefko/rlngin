#!/usr/bin/env bash
set -euo pipefail

# Usage: texel_selfplay.sh [rounds] [limit] [concurrency]
#   rounds:      number of game pairs (total games = 2 * rounds, default 32000)
#   limit:       per-move budget for both sides. Either a Cute-Chess time
#                control like "1+0.08" or a literal "nodes=N" (default
#                "1+0.08").
#   concurrency: parallel games (default 6)
#
# Thin wrapper around scripts/screen.sh that runs both sides on the same
# binary and writes the corpus PGN under tuning/texel/.
#
# Environment variables:
#   ENGINE:    path to the engine (default: ./build/rlngin)
#   FASTCHESS: path to fastchess (default: ./fastchess)
#   OPENINGS:  path to opening book (default: openings/UHO_Lichess_4852_v1.epd)
#   OUTPUT:    output directory (default: tuning/texel)

ROUNDS="${1:-32000}"
LIMIT="${2:-1+0.08}"
CONCURRENCY="${3:-6}"

ENGINE="${ENGINE:-./build/rlngin}"
OUTPUT="${OUTPUT:-tuning/texel}"

ENGINE_DEV="$ENGINE" \
ENGINE_BASE="$ENGINE" \
OUTPUT="$OUTPUT" \
PGN_NAME="games.pgn" \
LOG_NAME="selfplay.log" \
NAME_DEV="rlngin-a" \
NAME_BASE="rlngin-b" \
exec "$(dirname "$0")/screen.sh" "$ROUNDS" "$LIMIT" "$CONCURRENCY"
