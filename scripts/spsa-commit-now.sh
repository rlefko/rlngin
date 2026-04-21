#!/usr/bin/env bash
set -euo pipefail

# Usage: spsa-commit-now.sh [output-dir]
#
# Reads the current SPSA checkpoint, applies the current theta to
# src/search_params.cpp, runs make test to validate, commits, and
# pushes. Safe to run between the background sidecar's 50-iter
# milestones: the sidecar's in-memory `last_applied` counter only
# advances when it does its own commit, so a manual push here just
# fires an extra CI screen without disturbing its schedule.
#
# Environment variables:
#   OUTPUT: output directory (default: tuning/spsa)

OUTPUT="${1:-${OUTPUT:-tuning/spsa}}"

exec python3 tools/spsa/auto_apply.py \
    --force \
    --output-dir "$OUTPUT"
