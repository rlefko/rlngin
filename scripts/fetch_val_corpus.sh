#!/usr/bin/env bash
set -uo pipefail

# Fetches a master games corpus and extracts labeled positions for use
# as the Texel tuner's external val partition. The val EPD lives at
# `tuning/val/master_positions.epd` by default and is loaded by
# `build/tune` automatically when present (or when VAL_EPD points at
# any other path).
#
# This script is *idempotent* and tries several source URLs in order;
# the first one that succeeds wins. Override the source by setting
# VAL_CORPUS_URL to a direct PGN / PGN.zst / PGN.zip URL.
#
# The default source is The Week in Chess (TWIC), which publishes a
# weekly archive of master tournament games (~1-3MB compressed,
# ~5-15k games). One TWIC issue extracts to ~30-100k positions --
# plenty of val signal without bloating the repo.
#
# Usage:
#     ./scripts/fetch_val_corpus.sh
#
# Environment:
#     VAL_OUTPUT_DIR     output directory (default: tuning/val)
#     VAL_CORPUS_URL     primary source URL override (optional)
#     VAL_TWIC_NUM       TWIC issue number to fetch (default: 1530)
#     SKIP_PLIES         passed to extract_tuning_positions.py (default: 8)
#     TAIL_PLIES         passed to extract_tuning_positions.py (default: 2)
#
# Exit codes:
#     0   success (corpus already present, or fetched + extracted)
#     1   no source URL succeeded; downstream tune falls back to
#         in-corpus stratified val split

OUTPUT_DIR="${VAL_OUTPUT_DIR:-tuning/val}"
OUTPUT_EPD="$OUTPUT_DIR/master_positions.epd"
TWIC_NUM="${VAL_TWIC_NUM:-1530}"

mkdir -p "$OUTPUT_DIR"

if [ -f "$OUTPUT_EPD" ] && [ -s "$OUTPUT_EPD" ]; then
    echo "Master val corpus already present at $OUTPUT_EPD"
    echo "  $(wc -l < "$OUTPUT_EPD") positions"
    echo "  remove the file to force a re-fetch"
    exit 0
fi

TMP_DIR=$(mktemp -d -t val-corpus.XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

# Source candidates, tried in order. Each must be a direct URL to a
# PGN, PGN.zst, or PGN.zip artifact. TWIC issues are the most
# reliable; Lichess monthly archives are huge so we don't pull them
# by default, but the user can point VAL_CORPUS_URL at one.
SOURCES=()
[ -n "${VAL_CORPUS_URL:-}" ] && SOURCES+=("${VAL_CORPUS_URL}")
SOURCES+=(
    "https://theweekinchess.com/zips/twic${TWIC_NUM}g.zip"
    "https://theweekinchess.com/zips/twic$((TWIC_NUM - 1))g.zip"
    "https://theweekinchess.com/zips/twic$((TWIC_NUM - 2))g.zip"
)

PGN_FILE=""
for url in "${SOURCES[@]}"; do
    [ -z "$url" ] && continue
    fname=$(basename "$url")
    out_path="$TMP_DIR/$fname"
    echo "  trying $url"
    if curl -fsSL --connect-timeout 30 --max-time 1800 \
            -A "rlngin-fetch-val-corpus" \
            "$url" -o "$out_path" 2>/dev/null; then
        size=$(wc -c < "$out_path" | tr -d ' ')
        if [ "$size" -lt 1024 ]; then
            echo "    fetched $size bytes (suspiciously small); skipping"
            continue
        fi
        echo "    fetched $size bytes"
        case "$fname" in
            *.zst)
                if command -v zstd >/dev/null 2>&1; then
                    zstd -d "$out_path" -o "$TMP_DIR/master.pgn" 2>/dev/null \
                        && PGN_FILE="$TMP_DIR/master.pgn"
                else
                    echo "    zstd not installed; skipping"
                    continue
                fi
                ;;
            *.zip)
                if command -v unzip >/dev/null 2>&1; then
                    unzip -o -q "$out_path" -d "$TMP_DIR/unzipped"
                    pgn=$(find "$TMP_DIR/unzipped" -type f \( -iname '*.pgn' \) | head -1)
                    if [ -n "$pgn" ] && [ -s "$pgn" ]; then
                        PGN_FILE="$pgn"
                    fi
                else
                    echo "    unzip not installed; skipping"
                    continue
                fi
                ;;
            *.pgn|*.PGN)
                PGN_FILE="$out_path"
                ;;
            *)
                echo "    unknown file extension on $fname; skipping"
                continue
                ;;
        esac
        if [ -n "$PGN_FILE" ] && [ -s "$PGN_FILE" ]; then
            echo "    extracted PGN: $PGN_FILE ($(wc -c < "$PGN_FILE" | tr -d ' ') bytes)"
            break
        fi
    else
        echo "    fetch failed (offline, 404, or rate-limited)"
    fi
    PGN_FILE=""
done

if [ -z "$PGN_FILE" ] || [ ! -s "$PGN_FILE" ]; then
    echo ""
    echo "No source URL succeeded. The tuner will fall back to an"
    echo "in-corpus stratified val split. Override VAL_CORPUS_URL with"
    echo "a direct PGN/PGN.zst/PGN.zip URL to retry, or place an EPD"
    echo "directly at $OUTPUT_EPD."
    exit 1
fi

echo "Extracting positions from $PGN_FILE..."
SKIP_PLIES_ARG="${SKIP_PLIES:-8}"
TAIL_PLIES_ARG="${TAIL_PLIES:-2}"
python3 scripts/extract_tuning_positions.py \
    --skip-plies "$SKIP_PLIES_ARG" \
    --tail-plies "$TAIL_PLIES_ARG" \
    "$PGN_FILE" "$OUTPUT_EPD"

if [ ! -s "$OUTPUT_EPD" ]; then
    echo "Extract produced an empty EPD; aborting"
    rm -f "$OUTPUT_EPD"
    exit 1
fi

echo ""
echo "Master val corpus ready at $OUTPUT_EPD"
echo "  $(wc -l < "$OUTPUT_EPD") positions"
echo "  the tuner will pick it up automatically on the next run"
