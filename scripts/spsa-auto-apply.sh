#!/usr/bin/env bash
set -euo pipefail

# Usage: spsa-auto-apply.sh [every=50] [output=tuning/spsa]
#
# Runs the auto-apply sidecar. Every `every` iterations of an in-progress
# SPSA run (reading tuning/spsa/theta.json), applies the current theta to
# src/search_params.cpp, runs make test, commits, and pushes. Pipe
# stderr/stdout somewhere durable if running detached.
#
# Environment variables:
#   POLL_SEC: how often to re-check the checkpoint (default 30s)

EVERY="${1:-50}"
OUTPUT="${2:-tuning/spsa}"
POLL="${POLL_SEC:-30}"

exec python3 tools/spsa/auto_apply.py \
    --every "$EVERY" \
    --output-dir "$OUTPUT" \
    --poll-sec "$POLL"
