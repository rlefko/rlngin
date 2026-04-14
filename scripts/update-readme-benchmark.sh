#!/usr/bin/env bash
set -euo pipefail

# Usage: update-readme-benchmark.sh <screen-log-path> [games] [tc]
LOG="${1:?Usage: update-readme-benchmark.sh <screen-log-path> [games] [tc]}"
GAMES="${2:-1000 (500 pairs)}"
TC="${3:-60+0.6}"
README="README.md"

# Extract the results block from fastchess output
SUMMARY=$(grep -A4 "Results of" "$LOG" || tail -10 "$LOG")

DATE=$(date -u +"%Y-%m-%d")

# Write the benchmark block to a temp file
TMPBLOCK=$(mktemp)
cat > "$TMPBLOCK" <<BLOCKEOF
<!-- BENCHMARK:START -->
## Latest Benchmark

| Key | Value |
|-----|-------|
| Date | $DATE |
| Games | $GAMES |
| Time Control | $TC |
| Opening Book | UHO_Lichess_4852_v1.epd |

\`\`\`
$SUMMARY
\`\`\`
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
