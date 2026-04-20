#!/usr/bin/env python3
"""SPSA self-play tuner for the rlngin engine.

Discovers the tunable scalars from the engine's non-standard `tune` UCI
command, runs fastchess match batches with antithetic +/- perturbations,
and updates a single parameter vector using Spall's simultaneous
perturbation stochastic approximation. Each iteration runs
`2 * concurrency` games sharing the same opening slice (via `-repeat`)
and the same theta_plus / theta_minus split, so pairing and color swaps
cancel most opening noise from the gradient estimate.

Output:
    <output-dir>/history.csv  one row per iteration
    <output-dir>/theta.json   final rounded ints + run metadata

The driver is pure stdlib (argparse, subprocess, csv, json, math, random,
time, re, pathlib), so it runs on anything with a Python 3.8+ interpreter.
"""
import argparse
import csv
import json
import math
import random
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    import resource  # POSIX only
except ImportError:  # pragma: no cover
    resource = None


def normalize_fd_limit() -> None:
    """Set RLIMIT_NOFILE to a fastchess-friendly value before spawning it.

    macOS python3 can report RLIMIT_NOFILE as INT64_MAX (~9.2e18), which
    fastchess reads as a 32-bit int and sees as -1 -- triggering a bogus
    'not enough file descriptors' error with 'Limiting concurrency to: -1'.
    The fix is to pin the soft limit at a sane value before forking. 65536
    is far more than any SPSA match needs and matches fastchess's own
    suggested ulimit. On Linux or when the limit is already sane this is
    a no-op.
    """
    if resource is None:
        return
    try:
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        target_hard = hard if 0 < hard <= 65536 else 65536
        target_soft = min(target_hard, 65536)
        if soft != target_soft or hard != target_hard:
            resource.setrlimit(resource.RLIMIT_NOFILE, (target_soft, target_hard))
    except (ValueError, OSError):
        pass


# --- Spec discovery ---------------------------------------------------------


@dataclass
class Spec:
    name: str
    default: int
    minimum: int
    maximum: int
    c_end: float
    r_end: float


def discover_specs(engine_path: str) -> List[Spec]:
    """Start the engine, send `tune`, parse `tune <name> int ...` lines."""
    proc = subprocess.Popen(
        [engine_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
    )
    assert proc.stdin is not None and proc.stdout is not None
    proc.stdin.write("uci\ntune\nquit\n")
    proc.stdin.flush()
    out, _ = proc.communicate(timeout=10)

    specs: List[Spec] = []
    pattern = re.compile(
        r"^tune\s+(\S+)\s+int\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+([-\d.eE+]+)\s+([-\d.eE+]+)\s*$"
    )
    for line in out.splitlines():
        match = pattern.match(line)
        if not match:
            continue
        specs.append(
            Spec(
                name=match.group(1),
                default=int(match.group(2)),
                minimum=int(match.group(3)),
                maximum=int(match.group(4)),
                c_end=float(match.group(5)),
                r_end=float(match.group(6)),
            )
        )
    if not specs:
        raise RuntimeError(
            "Engine did not return any tune specs. "
            "Did you build after landing the tunable registry?"
        )
    return specs


# --- Spall gain sequences ---------------------------------------------------


ALPHA = 0.602
GAMMA = 0.101


def derive_ac(spec: Spec, iterations: int) -> Tuple[float, float, float]:
    """Return (a, c, A) for a parameter's gain schedule.

    Picks A = 10% of iterations per Spall's guidance, then derives c and a
    so that the final c_k equals spec.c_end and the final step magnitude
    `a_k / c_k * 1 = r_end` matches spec.r_end.
    """
    A = 0.1 * max(iterations, 1)
    # c_k = c / (k+1)^gamma; we want c_k at k=iterations-1 to equal c_end
    c = spec.c_end * max(iterations, 1) ** GAMMA
    # a_k / c_k ~= r_end at final iter; a_k = a / (k+1+A)^alpha
    a = spec.r_end * c * ((iterations + A) ** ALPHA)
    return a, c, A


# --- fastchess driver -------------------------------------------------------


def build_fastchess_cmd(
    fastchess: str,
    engine: str,
    openings: str,
    tc: Optional[str],
    nodes: Optional[int],
    concurrency: int,
    plus_values: Dict[str, int],
    minus_values: Dict[str, int],
    opening_start: int,
) -> List[str]:
    plus_opts = [f"option.{name}={value}" for name, value in plus_values.items()]
    minus_opts = [f"option.{name}={value}" for name, value in minus_values.items()]

    limit: List[str] = []
    if tc is not None:
        limit = [f"tc={tc}"]
    elif nodes is not None:
        limit = [f"nodes={nodes}"]
    else:
        raise ValueError("One of --tc or --nodes must be set")

    cmd = [
        fastchess,
        "-engine",
        f"cmd={engine}",
        "name=plus",
        *plus_opts,
        "-engine",
        f"cmd={engine}",
        "name=minus",
        *minus_opts,
        "-each",
        *limit,
        "-rounds",
        str(concurrency),
        "-repeat",
        "-concurrency",
        str(concurrency),
        "-openings",
        f"file={openings}",
        "format=epd",
        "order=random",
        f"start={opening_start}",
        "-pgnout",
        "file=/dev/null",
        "-recover",
    ]
    return cmd


_wins_re = re.compile(r"Wins:\s*(\d+)")
_losses_re = re.compile(r"Losses:\s*(\d+)")
_draws_re = re.compile(r"Draws:\s*(\d+)")


def parse_results(output: str) -> Optional[Tuple[int, int, int]]:
    """Extract wins/losses/draws from fastchess output (plus-perspective).

    fastchess prints periodic "Results of plus vs minus" blocks; we key off
    the last such block so partial games do not inflate the intermediate
    numbers. Returns None if the block cannot be parsed.
    """
    block_starts = [m.start() for m in re.finditer(r"Results of plus vs minus", output)]
    if not block_starts:
        return None
    block = output[block_starts[-1]:]
    w = _wins_re.search(block)
    l = _losses_re.search(block)
    d = _draws_re.search(block)
    if not (w and l and d):
        return None
    return int(w.group(1)), int(l.group(1)), int(d.group(1))


def run_iteration(
    fastchess: str,
    engine: str,
    openings: str,
    tc: Optional[str],
    nodes: Optional[int],
    concurrency: int,
    plus: Dict[str, int],
    minus: Dict[str, int],
    opening_start: int,
    timeout_sec: float,
) -> Optional[Tuple[int, int, int]]:
    cmd = build_fastchess_cmd(
        fastchess,
        engine,
        openings,
        tc,
        nodes,
        concurrency,
        plus,
        minus,
        opening_start,
    )
    try:
        completed = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout_sec,
            text=True,
        )
    except subprocess.TimeoutExpired as exc:
        sys.stderr.write(f"fastchess timed out after {timeout_sec}s: {exc}\n")
        return None
    parsed = parse_results(completed.stdout)
    if parsed is None:
        tail = completed.stdout[-800:] if completed.stdout else "(empty)"
        sys.stderr.write(
            f"fastchess rc={completed.returncode} produced unparseable output:\n{tail}\n"
        )
    return parsed


# --- Theta bookkeeping ------------------------------------------------------


def clamp_round(value: float, spec: Spec) -> int:
    v = int(round(value))
    if v < spec.minimum:
        v = spec.minimum
    if v > spec.maximum:
        v = spec.maximum
    return v


def write_history_header(writer: csv.writer, specs: List[Spec]) -> None:
    header = ["iter", "wins", "losses", "draws", "y", "elapsed_sec"]
    header.extend(f"theta_{s.name}" for s in specs)
    writer.writerow(header)


def write_history_row(
    writer: csv.writer,
    k: int,
    specs: List[Spec],
    theta: Dict[str, float],
    wins: int,
    losses: int,
    draws: int,
    y: float,
    elapsed: float,
) -> None:
    row = [k, wins, losses, draws, f"{y:.4f}", f"{elapsed:.2f}"]
    row.extend(clamp_round(theta[s.name], s) for s in specs)
    writer.writerow(row)


def write_theta_json(path: Path, specs: List[Spec], theta: Dict[str, float], meta: dict) -> None:
    out = {
        "params": {s.name: clamp_round(theta[s.name], s) for s in specs},
        "meta": meta,
    }
    path.write_text(json.dumps(out, indent=2, sort_keys=True) + "\n")


# --- Main loop --------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iterations", type=int, default=300)
    ap.add_argument("--concurrency", type=int, default=6)
    ap.add_argument("--tc", type=str, default="10+0.1",
                    help="fastchess time control (mutually exclusive with --nodes)")
    ap.add_argument("--nodes", type=int, default=None,
                    help="nodes per move (mutually exclusive with --tc)")
    ap.add_argument("--output-dir", type=str, default="tuning/spsa")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--engine", type=str, default="./build/rlngin")
    ap.add_argument("--fastchess", type=str, default="./fastchess")
    ap.add_argument("--openings", type=str,
                    default="openings/UHO_Lichess_4852_v1.epd")
    ap.add_argument("--resume", type=str, default=None,
                    help="optional theta.json to resume from")
    ap.add_argument("--dry-run", action="store_true",
                    help="exercise the loop without spawning fastchess")
    ap.add_argument("--list-tunables", action="store_true",
                    help="print discovered specs and exit")
    args = ap.parse_args()

    if args.tc and args.nodes is not None:
        ap.error("--tc and --nodes are mutually exclusive; pass only one")
    tc = args.tc if args.nodes is None else None
    nodes = args.nodes

    normalize_fd_limit()

    specs = discover_specs(args.engine)
    if args.list_tunables:
        for s in specs:
            print(f"{s.name}\tdefault={s.default}\tmin={s.minimum}\tmax={s.maximum}"
                  f"\tc_end={s.c_end}\tr_end={s.r_end}")
        return 0

    theta: Dict[str, float] = {s.name: float(s.default) for s in specs}
    if args.resume:
        resumed = json.loads(Path(args.resume).read_text())
        for name, value in resumed.get("params", {}).items():
            if name in theta:
                theta[name] = float(value)

    gains: Dict[str, Tuple[float, float, float]] = {
        s.name: derive_ac(s, args.iterations) for s in specs
    }

    # Report the iter-0 and iter-final a_k / c_k for sanity.
    sys.stderr.write(
        f"SPSA: {len(specs)} scalars, {args.iterations} iterations, "
        f"concurrency {args.concurrency}\n"
    )
    for s in specs:
        a, c, A = gains[s.name]
        c0 = c / (1 ** GAMMA)
        cN = c / (args.iterations ** GAMMA)
        a0 = a / ((1 + A) ** ALPHA)
        aN = a / ((args.iterations + A) ** ALPHA)
        sys.stderr.write(
            f"  {s.name}: c0={c0:.2f} cN={cN:.2f} a0={a0:.4f} aN={aN:.4f} "
            f"default={s.default} bounds=[{s.minimum},{s.maximum}]\n"
        )

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    history_path = out_dir / "history.csv"
    theta_path = out_dir / "theta.json"

    rng = random.Random(args.seed)
    games_per_iter = 2 * args.concurrency
    # 15 seconds per game is generous at 10+0.1; scale slightly with concurrency
    # so the CI timeout still catches a hung match.
    iter_timeout_sec = max(60.0, 20.0 * games_per_iter / max(args.concurrency, 1)) * 3

    start_wall = time.time()
    games_total = 0

    with history_path.open("w", newline="") as f_hist:
        writer = csv.writer(f_hist)
        write_history_header(writer, specs)

        for k in range(args.iterations):
            iter_start = time.time()

            delta = {s.name: (1 if rng.random() < 0.5 else -1) for s in specs}

            plus: Dict[str, int] = {}
            minus: Dict[str, int] = {}
            for s in specs:
                a, c, A = gains[s.name]
                c_k = c / ((k + 1) ** GAMMA)
                plus[s.name] = clamp_round(theta[s.name] + c_k * delta[s.name], s)
                minus[s.name] = clamp_round(theta[s.name] - c_k * delta[s.name], s)

            opening_start = rng.randint(1, 4000)

            if args.dry_run:
                # Fake a slight preference for +delta on a handful of params so
                # the loop exercises the update direction.
                reward = sum(delta[s.name] for s in specs[:3])
                y = max(-1.0, min(1.0, reward / max(len(specs[:3]), 1) * 0.1))
                wins = int((y + 1.0) / 2.0 * games_per_iter)
                losses = games_per_iter - wins
                draws = 0
            else:
                result = run_iteration(
                    args.fastchess, args.engine, args.openings, tc, nodes,
                    args.concurrency, plus, minus, opening_start,
                    iter_timeout_sec,
                )
                if result is None:
                    sys.stderr.write(
                        f"  iter {k}: could not parse results, skipping update\n"
                    )
                    elapsed = time.time() - iter_start
                    write_history_row(
                        writer, k, specs, theta, 0, 0, 0, 0.0, elapsed
                    )
                    f_hist.flush()
                    continue
                wins, losses, draws = result
                games_observed = wins + losses + draws
                if games_observed == 0:
                    sys.stderr.write(f"  iter {k}: zero games observed, skipping\n")
                    elapsed = time.time() - iter_start
                    write_history_row(writer, k, specs, theta, 0, 0, 0, 0.0, elapsed)
                    f_hist.flush()
                    continue
                y = (wins - losses) / games_observed
                games_total += games_observed

            for s in specs:
                a, c, A = gains[s.name]
                c_k = c / ((k + 1) ** GAMMA)
                a_k = a / ((k + 1 + A) ** ALPHA)
                g = y / (2.0 * c_k * delta[s.name])
                theta[s.name] = float(
                    clamp_round(theta[s.name] + a_k * g, s)
                )

            elapsed = time.time() - iter_start
            write_history_row(
                writer, k, specs, theta, wins, losses, draws, y, elapsed,
            )
            f_hist.flush()

            if (k + 1) % 10 == 0 or k == args.iterations - 1:
                total_elapsed = time.time() - start_wall
                sys.stderr.write(
                    f"  iter {k + 1}/{args.iterations}: y={y:+.3f} "
                    f"({wins}W/{draws}D/{losses}L), games={games_total}, "
                    f"elapsed={total_elapsed:.0f}s\n"
                )

    meta = {
        "iterations": args.iterations,
        "concurrency": args.concurrency,
        "tc": tc,
        "nodes": nodes,
        "seed": args.seed,
        "engine": args.engine,
        "openings": args.openings,
        "games_total": games_total,
        "elapsed_sec": time.time() - start_wall,
    }
    write_theta_json(theta_path, specs, theta, meta)
    sys.stderr.write(f"Wrote {theta_path} and {history_path}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
