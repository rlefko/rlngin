#!/usr/bin/env bash
set -euo pipefail

# Usage: format-screen-comment.sh <screen-log-path> [games-label] [tc]
# Parses the final fastchess results block and outputs formatted markdown.

LOG="${1:?Usage: format-screen-comment.sh <screen-log-path> [games-label] [tc]}"
GAMES_LABEL="${2:-100 games (50 pairs)}"
TC="${3:-10+0.1}"

# Grab only the LAST "Results of" block (final totals, not intermediates)
RESULTS=$(awk '/Results of/{buf=""} /Results of/,/Ptnml/{buf=buf $0 "\n"} END{printf "%s", buf}' "$LOG")

if [ -z "$RESULTS" ]; then
    echo "Could not parse results from $LOG" >&2
    exit 1
fi

# Parse individual fields from the results block
parse() { echo "$RESULTS" | sed -n "$1"; }
GAMES=$(parse 's/.*Games: \([0-9]*\).*/\1/p')
WINS=$(parse 's/.*Wins: \([0-9]*\).*/\1/p')
LOSSES=$(parse 's/.*Losses: \([0-9]*\).*/\1/p')
DRAWS=$(parse 's/.*Draws: \([0-9]*\).*/\1/p')
POINTS=$(parse 's/.*Points: \([0-9.]*\).*/\1/p')
SCORE_PCT=$(parse 's/.*(\([0-9.]*\) %).*/\1/p')
ELO=$(echo "$RESULTS" | grep "^Elo:" | sed 's/Elo: \([^ ,]*\).*/\1/')
ELO_ERR=$(echo "$RESULTS" | grep "^Elo:" | sed 's/.*Elo: [^,]*+\/- \([^,]*\),.*/\1/')
LOS=$(echo "$RESULTS" | grep "^LOS:" | sed 's/LOS: \([^ ]*\).*/\1/')
DRAW_RATIO=$(parse 's/.*DrawRatio: \([0-9.]*\).*/\1/p')
PTNML=$(parse 's/.*Ptnml(0-2): \(\[[0-9, ]*\]\).*/\1/p')

# Check for LLR (only present with SPRT)
LLR=$(echo "$RESULTS" | sed -n 's/.*LLR: \([^ ]*\).*/\1/p')
LLR="${LLR:-N/A}"

# URL-encode for shields.io badges
enc() { python3 -c "import urllib.parse; print(urllib.parse.quote('$1', safe=''))"; }

ELO_MSG=$(enc "$ELO +/- $ELO_ERR")
LOS_MSG=$(enc "$LOS%")
LLR_MSG=$(enc "$LLR")
WLD_MSG=$(enc "$WINS / $DRAWS / $LOSSES")
SCORE_MSG=$(enc "$POINTS / $GAMES ($SCORE_PCT%)")

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

cat <<MARKDOWN
## :chess_pawn: Self-Play Screen

![Elo]($B?label=Elo&message=$ELO_MSG&color=$ELO_COLOR) ![LOS]($B?label=LOS&message=$LOS_MSG&color=$LOS_COLOR) ![LLR]($B?label=LLR&message=$LLR_MSG&color=$LLR_COLOR) ![W/D/L]($B?label=W/D/L&message=$WLD_MSG&color=lightgray) ![Score]($B?label=Score&message=$SCORE_MSG&color=blue) ![Draws]($B?label=Draws&message=${DRAW_RATIO}%25&color=lightgray)

Ptnml(0-2): \`$PTNML\`
$GAMES_LABEL | tc=$TC | UHO_Lichess_4852_v1.epd
MARKDOWN
