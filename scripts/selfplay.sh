#!/usr/bin/env bash
set -euo pipefail

# Usage: selfplay.sh [rounds]
#
# Thin wrapper around screen.sh for quick local regression checks.
# Compares ENGINE_DEV (default: ./build/rlngin) against ENGINE_BASE.
#
# Environment variables (same as screen.sh):
#   ENGINE_DEV:  path to the dev engine (default: ./build/rlngin)
#   ENGINE_BASE: path to the base engine (required)
#   FASTCHESS:   path to fastchess binary (default: ./fastchess)
#   OPENINGS:    path to opening book (default: openings/UHO_Lichess_4852_v1.epd)

ROUNDS="${1:-10}"

if [ -z "${ENGINE_BASE:-}" ]; then
    echo "Error: ENGINE_BASE must be set to the baseline engine binary."
    echo "Usage: ENGINE_BASE=path/to/base ./scripts/selfplay.sh [rounds]"
    exit 1
fi

exec "$(dirname "$0")/screen.sh" "$ROUNDS" "10+0.1" 1
