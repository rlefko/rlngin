#!/usr/bin/env bash
set -euo pipefail

# Usage: spsa-resume.sh
#
# Resumes the SPSA run from its most recent checkpoint. All run parameters
# (iterations target, concurrency, time control, params scope, seed, RNG
# state) are read out of `$OUTPUT/theta.json` so no CLI flags are needed.
#
# Environment variables:
#   ENGINE:    path to the engine (default: ./build/rlngin)
#   FASTCHESS: path to fastchess (default: ./fastchess)
#   OPENINGS:  path to opening book (default: openings/UHO_Lichess_4852_v1.epd)
#   OUTPUT:    output directory (default: tuning/spsa)

OUTPUT="${OUTPUT:-tuning/spsa}"
CHECKPOINT="$OUTPUT/theta.json"

if [ ! -f "$CHECKPOINT" ]; then
    echo "Error: no checkpoint found at $CHECKPOINT" >&2
    echo "Start a fresh run with ./scripts/spsa.sh instead." >&2
    exit 1
fi

ENGINE="${ENGINE:-./build/rlngin}"
FASTCHESS="${FASTCHESS:-./fastchess}"
OPENINGS="${OPENINGS:-openings/UHO_Lichess_4852_v1.epd}"

if [ ! -x "$ENGINE" ]; then
    echo "Error: engine not found at $ENGINE (run 'make build')" >&2
    exit 1
fi

if [ ! -f "$FASTCHESS" ]; then
    echo "Error: fastchess not found at $FASTCHESS (run 'make fetch-fastchess')" >&2
    exit 1
fi

exec python3 tools/spsa/spsa.py \
    --resume "$CHECKPOINT" \
    --engine "$ENGINE" \
    --fastchess "$FASTCHESS" \
    --openings "$OPENINGS" \
    --output-dir "$OUTPUT"
