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
DRAW_RATIO=$(parse 's/.*DrawRatio: \([0-9.]*\).*/\1/p')
PTNML=$(parse 's/.*Ptnml(0-2): \(\[[0-9, ]*\]\).*/\1/p')

# URL-encode for shields.io badges
urlencode() { python3 -c "import urllib.parse; print(urllib.parse.quote('$1', safe=''))"; }

ELO_DISPLAY="$ELO +/- $ELO_ERR"
WLD_DISPLAY="$WINS / $DRAWS / $LOSSES"
SCORE_DISPLAY="$POINTS / $GAMES ($SCORE_PCT%)"

ELO_ENCODED=$(urlencode "$ELO_DISPLAY")
WLD_ENCODED=$(urlencode "$WLD_DISPLAY")
SCORE_ENCODED=$(urlencode "$SCORE_DISPLAY")

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

cat <<MARKDOWN
## :chess_pawn: Self-Play Screen

![Elo](https://img.shields.io/badge/Elo-${ELO_ENCODED}-${ELO_COLOR}) ![Score](https://img.shields.io/badge/Score-${SCORE_ENCODED}-blue) ![W/D/L](https://img.shields.io/badge/W%2FD%2FL-${WLD_ENCODED}-lightgray) ![Draw%](https://img.shields.io/badge/Draw%25-${DRAW_RATIO}%25-lightgray)

| | |
|:--|:--|
| **Games** | $GAMES_LABEL |
| **Time Control** | $TC |
| **Elo** | $ELO_DISPLAY |
| **Score** | $POINTS / $GAMES ($SCORE_PCT%) |
| **Record (W/D/L)** | $WINS / $DRAWS / $LOSSES |
| **Draw Ratio** | $DRAW_RATIO% |
| **Pentanomial** | $PTNML |

<sub>Opening book: UHO_Lichess_4852_v1.epd</sub>
MARKDOWN
