#!/usr/bin/env bash
set -uo pipefail

# Fetches a master games corpus and extracts labeled positions for use
# as the Texel tuner's curated TRAINING pack (loaded via CURATED_EPD).
# Output lives at `tuning/train/master_positions.epd` by default.
#
# This is the training-side counterpart to scripts/fetch_val_corpus.sh.
# The two scripts intentionally pull DIFFERENT TWIC issues so the
# train and val corpora do not overlap. Default issue offsets:
#   val_corpus:    issue 1530 (latest)
#   train_master:  issue 1525 (5 earlier)
# Override the TWIC issue with TRAIN_TWIC_NUM.
#
# Usage:
#     ./scripts/fetch_train_master.sh
#
# Environment:
#     TRAIN_OUTPUT_DIR     output directory (default: tuning/train)
#     TRAIN_CORPUS_URL     primary source URL override (optional)
#     TRAIN_TWIC_NUM       TWIC issue number to fetch (default: 1525)
#     SKIP_PLIES           passed to extract_tuning_positions.py (default: 8)
#     TAIL_PLIES           passed to extract_tuning_positions.py (default: 2)
#
# Exit codes:
#     0   success (corpus already present, or fetched + extracted)
#     1   no source URL succeeded; downstream tune skips the curated
#         pack and trains on self-play alone

OUTPUT_DIR="${TRAIN_OUTPUT_DIR:-tuning/train}"
OUTPUT_EPD="$OUTPUT_DIR/master_positions.epd"
TWIC_NUM="${TRAIN_TWIC_NUM:-1525}"

mkdir -p "$OUTPUT_DIR"

if [ -f "$OUTPUT_EPD" ] && [ -s "$OUTPUT_EPD" ]; then
    echo "Master train corpus already present at $OUTPUT_EPD"
    echo "  $(wc -l < "$OUTPUT_EPD") positions"
    echo "  remove the file to force a re-fetch"
    exit 0
fi

TMP_DIR=$(mktemp -d -t train-master.XXXXXX)
trap 'rm -rf "$TMP_DIR"' EXIT

# Same TWIC fallback ladder as fetch_val_corpus.sh, walking earlier
# issues if the requested one is not available. Each candidate must
# point at a TWIC zip that is at least one issue offset away from the
# val script's default to avoid corpus overlap.
SOURCES=()
[ -n "${TRAIN_CORPUS_URL:-}" ] && SOURCES+=("${TRAIN_CORPUS_URL}")
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
            -A "rlngin-fetch-train-master" \
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
    echo "No source URL succeeded. The tuner will skip the curated"
    echo "training pack and train on self-play alone. Override"
    echo "TRAIN_CORPUS_URL with a direct PGN/PGN.zst/PGN.zip URL to"
    echo "retry, or place an EPD directly at $OUTPUT_EPD."
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
echo "Master train corpus ready at $OUTPUT_EPD"
echo "  $(wc -l < "$OUTPUT_EPD") positions"
echo "  the tuner picks it up via CURATED_EPD on the next run"
