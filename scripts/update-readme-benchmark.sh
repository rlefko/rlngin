#!/usr/bin/env bash
set -euo pipefail

# Usage: update-readme-benchmark.sh <screen-log-path> [games] [tc]
LOG="${1:?Usage: update-readme-benchmark.sh <screen-log-path> [games] [tc]}"
GAMES="${2:-1000 (500 pairs)}"
TC="${3:-60+0.6}"
README="README.md"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Grab the last results block
RESULTS=$(awk '/Results of/{buf=""} /Results of/,/Ptnml/{buf=buf $0 "\n"} END{printf "%s", buf}' "$LOG")

# Parse fields
parse() { echo "$RESULTS" | sed -n "$1"; }
WINS=$(parse 's/.*Wins: \([0-9]*\).*/\1/p')
LOSSES=$(parse 's/.*Losses: \([0-9]*\).*/\1/p')
DRAWS=$(parse 's/.*Draws: \([0-9]*\).*/\1/p')
POINTS=$(parse 's/.*Points: \([0-9.]*\).*/\1/p')
SCORE_PCT=$(parse 's/.*(\([0-9.]*\) %).*/\1/p')
TOTAL_GAMES=$(parse 's/.*Games: \([0-9]*\).*/\1/p')
ELO=$(echo "$RESULTS" | grep "^Elo:" | sed 's/Elo: \([^ ,]*\).*/\1/')
ELO_ERR=$(echo "$RESULTS" | grep "^Elo:" | sed 's/.*Elo: [^,]*+\/- \([^,]*\),.*/\1/')
DRAW_RATIO=$(parse 's/.*DrawRatio: \([0-9.]*\).*/\1/p')
PTNML=$(parse 's/.*Ptnml(0-2): \(\[[0-9, ]*\]\).*/\1/p')

DATE=$(date -u +"%Y-%m-%d")

# Write the benchmark block to a temp file
TMPBLOCK=$(mktemp)
cat > "$TMPBLOCK" <<BLOCKEOF
<!-- BENCHMARK:START -->
## Latest Benchmark

| | |
|:--|:--|
| **Date** | $DATE |
| **Games** | $GAMES |
| **Time Control** | $TC |
| **Elo** | $ELO +/- $ELO_ERR |
| **Score** | $POINTS / $TOTAL_GAMES ($SCORE_PCT%) |
| **Record (W/D/L)** | $WINS / $DRAWS / $LOSSES |
| **Draw Ratio** | $DRAW_RATIO% |
| **Pentanomial** | $PTNML |

<sub>Opening book: UHO_Lichess_4852_v1.epd</sub>
<!-- BENCHMARK:END -->
BLOCKEOF

if grep -q "<!-- BENCHMARK:START -->" "$README"; then
    awk '
        /<!-- BENCHMARK:START -->/ { while ((getline line < "'"$TMPBLOCK"'") > 0) print line; skip=1; next }
        /<!-- BENCHMARK:END -->/ { skip=0; next }
        !skip { print }
    ' "$README" > README.tmp && mv README.tmp "$README"
else
    awk '
        /^# rlngin$/ { print; print ""; while ((getline line < "'"$TMPBLOCK"'") > 0) print line; print ""; next }
        { print }
    ' "$README" > README.tmp && mv README.tmp "$README"
fi

rm -f "$TMPBLOCK"
echo "README.md updated with benchmark results."
