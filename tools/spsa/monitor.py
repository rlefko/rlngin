#!/usr/bin/env python3
"""Live dashboard for an in-progress SPSA run.

Run in a second terminal while `spsa.py` is tuning. Reads the atomic
`live.json` sidecar plus the tail of `history.csv`, clears the screen
every refresh, and prints a compact progress panel. Stop with Ctrl-C.

Usage:
    python3 tools/spsa/monitor.py [output-dir]

`output-dir` defaults to `tuning/spsa`. Refresh interval defaults to 2
seconds; pass `--interval` to override. Pure stdlib; no curses, no
dependencies.
"""
import argparse
import csv
import json
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional


def read_json(path: Path) -> Optional[Dict[str, Any]]:
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return None


def read_history_tail(path: Path, n: int) -> List[List[str]]:
    if not path.exists():
        return []
    # Small files, tail via full read is fine; history caps at a few thousand rows.
    with path.open(newline="") as f:
        rows = list(csv.reader(f))
    return rows[-n:] if len(rows) > n else rows


def format_duration(sec: float) -> str:
    sec = int(sec)
    h, rem = divmod(sec, 3600)
    m, s = divmod(rem, 60)
    if h > 0:
        return f"{h}h{m:02d}m"
    return f"{m}m{s:02d}s"


def clear() -> None:
    # ANSI: move cursor home + clear screen.
    sys.stdout.write("\x1b[2J\x1b[H")


def render(live: Dict[str, Any], history_tail: List[List[str]], history_header: List[str]) -> str:
    out: List[str] = []
    iter_idx = live.get("iter", -1)
    target = live.get("iterations_target", 0)
    elapsed = live.get("elapsed_sec_cumulative", 0.0)
    eta = live.get("eta_sec", 0.0)
    sessions = live.get("session_count", 1)
    games = live.get("games_total_cumulative", 0)
    running_y = live.get("running_y_last20", 0.0)

    out.append(
        f"SPSA monitor  |  iter {iter_idx + 1} / {target}  "
        f"|  elapsed {format_duration(elapsed)}  "
        f"|  ETA {format_duration(eta)}  |  sessions {sessions}"
    )
    out.append(
        f"games {games}   |  running y (last 20): {running_y:+.3f}"
    )
    out.append("")

    top_movers = live.get("top_movers", [])
    if top_movers:
        out.append("Top movers (|a_k * g| this iter):")
        for mover in top_movers[:5]:
            delta = mover.get("delta", 0.0)
            sign = "+" if delta >= 0 else "-"
            out.append(f"  {mover.get('name', '?'):<18}  {sign}{abs(delta):.3f}")
        out.append("")

    clamp_hits = live.get("clamp_hits", {})
    clamp_nonzero = {k: v for k, v in clamp_hits.items() if v > 0}
    if clamp_nonzero:
        out.append("Clamp hits cumulative:")
        for name, n in sorted(clamp_nonzero.items(), key=lambda kv: -kv[1]):
            out.append(f"  {name:<18}  {n}")
    else:
        out.append("Clamp hits cumulative:  none")
    out.append("")

    theta = live.get("theta", {})
    starting = live.get("starting_theta", {})
    if theta and starting:
        out.append("Theta vs starting defaults:")
        moved = sorted(theta.keys(), key=lambda n: abs(theta[n] - starting.get(n, theta[n])),
                       reverse=True)
        for name in moved[:10]:
            cur = theta[name]
            start = starting.get(name, cur)
            delta = cur - start
            if delta == 0:
                continue
            sign = "+" if delta > 0 else ""
            out.append(f"  {name:<18}  {start} -> {cur}   ({sign}{delta})")
        out.append("")

    if history_tail and history_header:
        out.append("Last iterations (iter / wins / draws / losses / y):")
        try:
            iter_col = history_header.index("iter")
            w_col = history_header.index("wins")
            d_col = history_header.index("draws")
            l_col = history_header.index("losses")
            y_col = history_header.index("y")
        except ValueError:
            iter_col = 0
            w_col = 1
            l_col = 2
            d_col = 3
            y_col = 4
        for row in history_tail[-5:]:
            if len(row) > max(iter_col, w_col, l_col, d_col, y_col):
                out.append(
                    f"  {row[iter_col]:>4}  "
                    f"{row[w_col]}W/{row[d_col]}D/{row[l_col]}L  "
                    f"y={row[y_col]}"
                )

    return "\n".join(out) + "\n"


def render_once(out: Path) -> str:
    live = read_json(out / "live.json")
    if live is None:
        return (
            f"Waiting for {out / 'live.json'} ...\n"
            "Is the SPSA driver running?\n"
        )
    rows = read_history_tail(out / "history.csv", 6)
    header = rows[0] if rows else []
    tail = rows[1:] if rows else []
    return render(live, tail, header)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("output_dir", nargs="?", default="tuning/spsa")
    ap.add_argument("--interval", type=float, default=2.0,
                    help="refresh interval in seconds (ignored with --once)")
    ap.add_argument("--once", action="store_true",
                    help="render the dashboard once and exit; pair with the `watch` utility "
                    "(scripts/spsa-monitor.sh does exactly this) for a clean in-place refresh")
    args = ap.parse_args()

    out = Path(args.output_dir)

    if args.once:
        sys.stdout.write(render_once(out))
        return 0

    try:
        while True:
            clear()
            sys.stdout.write(render_once(out))
            sys.stdout.flush()
            time.sleep(args.interval)
    except KeyboardInterrupt:
        sys.stdout.write("\n")
        return 0


if __name__ == "__main__":
    sys.exit(main())
