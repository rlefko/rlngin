#!/usr/bin/env python3
"""Aggregate `info string stats` lines from stdin into a firing-rate table.

Pair with `scripts/instrument.sh`:

    ./scripts/instrument.sh 100 10 | python3 tools/spsa/aggregate_stats.py

Each input line has the shape

    info string stats nodes=<N> razor=<A> rfp=<B> nmp=<C> ...

We sum every key across all lines, then print a rate table keyed on the
totals. The firing rate is reported as `100 * fires / total_nodes` rounded
to three decimals; the raw fires and the total node count are shown too so
the operator can sanity-check the aggregation.

A small filter below the table flags every heuristic whose rate is below
0.05 percent, which is the threshold the SPSA driver uses when deciding
whether a parameter has enough gradient signal to be worth tuning.
"""
import re
import sys
from typing import Dict

_PAIR = re.compile(r"(\w+)=(-?\d+)")
_LINE = re.compile(r"info string stats\s+(.*)$")

# Fixed print order: node count first, then heuristics in the order that
# the engine emits them. Keeping this stable keeps the output diff-friendly
# across runs.
ORDER = [
    "nodes",
    "razor",
    "rfp",
    "nmp",
    "nmp_verify",
    "probcut",
    "probcut_cut",
    "fp",
    "lmp",
    "see_cap",
    "see_quiet",
    "lmr",
    "lmr_research",
]

THRESHOLD_PCT = 0.05


def main() -> int:
    totals: Dict[str, int] = {k: 0 for k in ORDER}
    rows = 0

    for line in sys.stdin:
        match = _LINE.search(line)
        if not match:
            continue
        body = match.group(1)
        for key, value in _PAIR.findall(body):
            if key in totals:
                totals[key] += int(value)
        rows += 1

    if rows == 0:
        print("No `info string stats` lines found on stdin", file=sys.stderr)
        return 1

    total_nodes = totals["nodes"]
    if total_nodes == 0:
        print("Total node count is zero; did search produce output?", file=sys.stderr)
        return 1

    heuristics = [k for k in ORDER if k != "nodes"]
    name_width = max(len(h) for h in heuristics)

    print(f"Aggregated over {rows} positions, {total_nodes:,} total nodes")
    print()
    print(f"{'heuristic':<{name_width}}  {'fires':>12}  {'% of nodes':>12}  flag")
    print("-" * (name_width + 2 + 12 + 2 + 12 + 2 + 4))
    for h in heuristics:
        fires = totals[h]
        rate = 100.0 * fires / total_nodes
        flag = "  low" if rate < THRESHOLD_PCT else ""
        print(f"{h:<{name_width}}  {fires:>12,}  {rate:>11.3f}%  {flag}")

    print()
    low_heuristics = [h for h in heuristics if 100.0 * totals[h] / total_nodes < THRESHOLD_PCT]
    if low_heuristics:
        print(
            "Heuristics firing below "
            f"{THRESHOLD_PCT}% of nodes (candidates to drop from the SPSA tune scope):"
        )
        for h in low_heuristics:
            print(f"  {h}")
    else:
        print(f"All heuristics fire above the {THRESHOLD_PCT}% threshold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
