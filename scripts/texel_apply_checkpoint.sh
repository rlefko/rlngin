#!/usr/bin/env bash
set -euo pipefail

# Dump the latest tuned checkpoint and replace the kDefaultEvalParams
# block in src/eval_params.cpp. Does NOT commit; review with `git diff`
# and commit when ready.
#
# Usage: texel_apply_checkpoint.sh
# Env:   CHECKPOINT (default: tuning/checkpoint.txt)
#        TUNE       (default: ./build/tune)

CHECKPOINT="${CHECKPOINT:-tuning/checkpoint.txt}"
TUNE="${TUNE:-./build/tune}"
EVAL_PARAMS="src/eval_params.cpp"

if [ ! -f "$CHECKPOINT" ]; then
    echo "Error: checkpoint not found at $CHECKPOINT"
    exit 1
fi

if [ ! -x "$TUNE" ]; then
    echo "Error: tuner not found at $TUNE (run 'make tune')"
    exit 1
fi

if [ ! -f "$EVAL_PARAMS" ]; then
    echo "Error: $EVAL_PARAMS not found (are you running from repo root?)"
    exit 1
fi

DUMP_FILE=$(mktemp /tmp/texel_dump.XXXXXX)
trap "rm -f $DUMP_FILE" EXIT

echo "Dumping $CHECKPOINT via $TUNE..."
"$TUNE" --dump "$CHECKPOINT" > "$DUMP_FILE"

python3 - "$EVAL_PARAMS" "$DUMP_FILE" << 'PYEOF'
import sys

src_path, dump_path = sys.argv[1], sys.argv[2]

with open(src_path) as f:
    eval_lines = f.readlines()
with open(dump_path) as f:
    dump_lines = f.readlines()

def find_block(lines):
    start = end = None
    for i, line in enumerate(lines):
        if line.startswith("static const EvalParams kDefaultEvalParams = {"):
            start = i
        elif start is not None and end is None and line.strip() == "};":
            end = i
            return start, end
    return None, None

start, end = find_block(eval_lines)
if start is None:
    raise SystemExit(f"could not locate kDefaultEvalParams block in {src_path}")

dump_start, dump_end = find_block(dump_lines)
if dump_start is None:
    raise SystemExit(f"could not locate kDefaultEvalParams block in dump output")

new_block = dump_lines[dump_start:dump_end + 1]
result = eval_lines[:start] + new_block + eval_lines[end + 1:]
with open(src_path, "w") as f:
    f.writelines(result)

print(f"replaced {end - start + 1} lines in {src_path} with {len(new_block)} lines from dump")
PYEOF

echo ""
echo "Updated $EVAL_PARAMS. Review with: git diff $EVAL_PARAMS"
echo "Rebuild with: make build && make tune"
