#!/usr/bin/env bash
set -euo pipefail

# Usage: update-readme-benchmark.sh <screen-log-path> [games] [tc]
LOG="${1:?Usage: update-readme-benchmark.sh <screen-log-path> [games] [tc]}"
GAMES="${2:-1000 (500 pairs)}"
TC="${3:-60+0.6}"
README="README.md"

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

# URL-encode for shields.io badges
enc() { python3 -c "import urllib.parse; print(urllib.parse.quote('$1', safe=''))"; }

ELO_MSG=$(enc "$ELO +/- $ELO_ERR")
LOS=$(echo "$RESULTS" | grep "^LOS:" | sed 's/LOS: \([^ ]*\).*/\1/')
LOS_MSG=$(enc "$LOS%")
LLR=$(echo "$RESULTS" | sed -n 's/.*LLR: \([^ ]*\).*/\1/p')
LLR="${LLR:-N/A}"
LLR_MSG=$(enc "$LLR")
WLD_MSG=$(enc "$WINS / $DRAWS / $LOSSES")
SCORE_MSG=$(enc "$POINTS / $TOTAL_GAMES ($SCORE_PCT%)")

# Pick Elo badge color
if echo "$ELO" | grep -q "nan"; then
    ELO_COLOR="gray"
elif [ "$(echo "$ELO > 5" | bc -l 2>/dev/null)" = "1" ]; then
    ELO_COLOR="brightgreen"
elif [ "$(echo "$ELO < -5" | bc -l 2>/dev/null)" = "1" ]; then
    ELO_COLOR="red"
else
    ELO_COLOR="gray"
fi

# Pick LOS badge color
if echo "$LOS" | grep -q "nan"; then
    LOS_COLOR="gray"
elif [ "$(echo "$LOS > 95" | bc -l 2>/dev/null)" = "1" ]; then
    LOS_COLOR="brightgreen"
elif [ "$(echo "$LOS < 5" | bc -l 2>/dev/null)" = "1" ]; then
    LOS_COLOR="red"
else
    LOS_COLOR="yellow"
fi

# Pick LLR badge color
if [ "$LLR" = "N/A" ]; then
    LLR_COLOR="gray"
else
    LLR_COLOR="blue"
fi

B="https://img.shields.io/static/v1"

# Write the benchmark block to a temp file
TMPBLOCK=$(mktemp)
cat > "$TMPBLOCK" <<BLOCKEOF
<!-- BENCHMARK:START -->
## Latest Benchmark

![Elo]($B?label=Elo&message=$ELO_MSG&color=$ELO_COLOR) ![LOS]($B?label=LOS&message=$LOS_MSG&color=$LOS_COLOR) ![LLR]($B?label=LLR&message=$LLR_MSG&color=$LLR_COLOR) ![W/D/L]($B?label=W/D/L&message=$WLD_MSG&color=lightgray) ![Score]($B?label=Score&message=$SCORE_MSG&color=blue) ![Draws]($B?label=Draws&message=${DRAW_RATIO}%25&color=lightgray)

Ptnml(0-2): \`$PTNML\`
$GAMES | tc=$TC | UHO_Lichess_4852_v1.epd
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
