#!/usr/bin/env bash
# Per-position breakdown of the engine's static eval, used to surface
# eval terms that are firing too hot or too cold relative to ground
# truth in opening / middlegame positions where the engine is known to
# misjudge "material vs activity" trade-offs.
#
# Usage:
#   ./scripts/eval_probe.sh                # run the curated set
#   ./scripts/eval_probe.sh "FEN STRING"   # run a single ad-hoc FEN
#
# Requires `build/rlngin`. Builds it if missing.

set -euo pipefail

cd "$(dirname "$0")/.."

if [[ ! -x build/rlngin ]]; then
    echo "Building engine..." >&2
    make build >&2
fi

# A curated set of "material vs activity" trade-off positions where the
# engine has been observed to over-rate side-to-move activity at the
# expense of material. Each line is `LABEL|FEN`. Add positions here as
# they surface during analysis.
read -r -d '' POSITIONS <<'EOF' || true
Scandinavian: 2...c6 3.dxc6 Nxc6 (white +1 pawn, gambit accepted)|r1bqkbnr/pp2pppp/2n5/8/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 4
Scandinavian: 2...Nf6 3.Nf3 Nxd5 (Marshall, material even, knight on d5)|rnbqkb1r/ppp1pppp/8/3n4/8/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 4
Scandinavian: 2...Qxd5 3.Nc3 Qd8 (queen retreats, material even)|rnbqkbnr/ppp1pppp/8/8/8/2N5/PPPP1PPP/R1BQKBNR w KQkq - 2 4
Scandinavian: just after 2.exd5 (black to choose recapture)|rnbqkbnr/ppp1pppp/8/3P4/8/8/PPPP1PPP/RNBQKBNR b KQkq - 0 2
Caro-Kann: 1.e4 c6 2.d4 d5 3.exd5 cxd5|rnbqkbnr/pp2pppp/8/3p4/3P4/8/PPP2PPP/RNBQKBNR w KQkq - 0 4
Slav: 1.d4 d5 2.c4 c6 3.cxd5 cxd5|rnbqkbnr/pp2pppp/8/3p4/3P4/8/PP2PPPP/RNBQKBNR w KQkq - 0 4
Center pawn parity (post-1.e4 e5)|rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2
Endgame K+P vs K (white winning)|8/8/3k4/8/3P4/3K4/8/8 w - - 0 1
EOF

# Allow a single ad-hoc FEN via the first argument.
if (( $# >= 1 )); then
    POSITIONS="adhoc|$1"
fi

probe_one() {
    local label="$1"
    local fen="$2"
    printf '\n=== %s ===\n' "$label"
    printf 'FEN: %s\n' "$fen"
    printf 'uci\nposition fen %s\neval\nquit\n' "$fen" \
        | ./build/rlngin \
        | sed -n '/rlngin eval breakdown/,/^$/p'
}

while IFS='|' read -r label fen; do
    [[ -z "$label" || -z "$fen" ]] && continue
    probe_one "$label" "$fen"
done <<<"$POSITIONS"