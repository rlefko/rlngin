#!/usr/bin/env bash
set -euo pipefail

# Usage: spsa-monitor.sh [output-dir]
#
# Live dashboard for an in-progress SPSA run. Reads `live.json` and the
# tail of `history.csv` from the output directory and renders a compact
# panel. Stop with Ctrl-C.

exec python3 tools/spsa/monitor.py "${1:-${OUTPUT:-tuning/spsa}}"
