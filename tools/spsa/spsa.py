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
    <output-dir>/theta.json   atomic checkpoint; written every iteration
    <output-dir>/live.json    live-dashboard sidecar updated every iteration

Pause and resume:
    * SIGINT or SIGTERM flips a stop flag. The driver lets the in-flight
      fastchess match finish (up to ~80 seconds at 10+0.1), writes a final
      checkpoint, and exits 0.
    * A second signal within 5 seconds escalates: the running fastchess
      is SIGKILL'd, the in-flight iteration is discarded, the pre-iteration
      checkpoint is preserved, and the driver exits immediately.
    * `--resume <theta.json>` restores theta, RNG state, and iteration
      counter, so resumed runs are byte-identical to uninterrupted ones.

The driver is pure stdlib (argparse, subprocess, csv, json, math, os,
random, signal, time, re, pathlib), so it runs on anything with a
Python 3.8+ interpreter.
"""
import argparse
import csv
import json
import math
import os
import random
import re
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

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


def filter_specs(specs: List[Spec], names: Optional[List[str]]) -> List[Spec]:
    """Filter `specs` to a caller-requested subset, preserving original order.

    Unknown names cause a hard error with an explicit available list so a
    typo in `--params` never silently drops a parameter from the tune.
    """
    if not names:
        return specs
    by_name = {s.name: s for s in specs}
    missing = [n for n in names if n not in by_name]
    if missing:
        available = ", ".join(s.name for s in specs)
        raise SystemExit(
            f"--params included unknown scalars: {', '.join(missing)}\n"
            f"Known tunables: {available}"
        )
    wanted = set(names)
    return [s for s in specs if s.name in wanted]


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


# --- Signal handling --------------------------------------------------------


class StopController:
    """Tracks graceful vs forceful stop requests across SIGINT/SIGTERM.

    A single signal flips `stop_requested`; the main loop flushes a
    checkpoint between iterations and exits cleanly. A second signal
    within `FORCEFUL_SIGNAL_WINDOW_SEC` escalates to `force_stop`, kills
    the currently tracked subprocess (if any), and unblocks the main
    loop's `subprocess.communicate` call so the driver exits promptly.
    """

    FORCEFUL_SIGNAL_WINDOW_SEC = 5.0

    def __init__(self) -> None:
        self.stop_requested = False
        self.force_stop = False
        self._last_signal_time = 0.0
        self.current_proc: Optional[subprocess.Popen] = None

    def install(self) -> None:
        signal.signal(signal.SIGINT, self._handle)
        signal.signal(signal.SIGTERM, self._handle)

    def _handle(self, signum: int, frame: Any) -> None:
        now = time.time()
        if self.stop_requested and now - self._last_signal_time <= self.FORCEFUL_SIGNAL_WINDOW_SEC:
            self.force_stop = True
            if self.current_proc is not None:
                try:
                    self.current_proc.kill()
                except ProcessLookupError:
                    pass
            sys.stderr.write(
                "Second signal received inside 5s window; force-stopping.\n"
            )
        else:
            self.stop_requested = True
            self._last_signal_time = now
            sys.stderr.write(
                "Signal received; finishing the in-flight iteration, then exiting.\n"
            )


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
    stop: StopController,
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
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    stop.current_proc = proc
    try:
        stdout, _ = proc.communicate(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        sys.stderr.write(f"fastchess timed out after {timeout_sec}s\n")
        return None
    finally:
        stop.current_proc = None

    if stop.force_stop:
        # The subprocess was killed by the forceful-stop signal handler.
        # Treat this iteration as discarded and propagate None upward.
        return None

    parsed = parse_results(stdout)
    if parsed is None:
        tail = stdout[-800:] if stdout else "(empty)"
        sys.stderr.write(
            f"fastchess rc={proc.returncode} produced unparseable output:\n{tail}\n"
        )
    return parsed


# --- Theta + checkpoint bookkeeping -----------------------------------------


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


def atomic_write_json(path: Path, obj: Dict[str, Any]) -> None:
    """Write `obj` as JSON to `path` atomically via tempfile + os.replace.

    Readers of `path` always see either the previous contents or the new
    contents, never a half-written file. Critical for checkpoints so a
    SIGKILL mid-write leaves the previous checkpoint intact.
    """
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(obj, indent=2, sort_keys=True) + "\n")
    os.replace(tmp, path)


def rng_state_to_json(state: Tuple[Any, ...]) -> List[Any]:
    # random.Random.getstate() returns (version, internal_tuple, gauss_next).
    # JSON has no tuple type; recursively convert the one nested tuple.
    return [state[0], list(state[1]), state[2]]


def rng_state_from_json(data: List[Any]) -> Tuple[Any, ...]:
    return (data[0], tuple(data[1]), data[2])


def build_checkpoint_meta(
    *,
    iterations_target: int,
    next_iter: int,
    seed: int,
    tc: Optional[str],
    nodes: Optional[int],
    concurrency: int,
    engine: str,
    openings: str,
    params_scope: Optional[List[str]],
    elapsed_cumulative: float,
    games_cumulative: int,
    session_count: int,
    rng: random.Random,
) -> Dict[str, Any]:
    return {
        "iterations_target": iterations_target,
        "next_iter": next_iter,
        "seed": seed,
        "tc": tc,
        "nodes": nodes,
        "concurrency": concurrency,
        "engine": engine,
        "openings": openings,
        "params_scope": params_scope,
        "elapsed_sec_cumulative": elapsed_cumulative,
        "games_total_cumulative": games_cumulative,
        "session_count": session_count,
        "rng_state": rng_state_to_json(rng.getstate()),
        "alpha": ALPHA,
        "gamma": GAMMA,
    }


def write_checkpoint(
    path: Path, specs: List[Spec], theta: Dict[str, float], meta: Dict[str, Any]
) -> None:
    obj = {
        "params": {s.name: clamp_round(theta[s.name], s) for s in specs},
        "meta": meta,
    }
    atomic_write_json(path, obj)


# --- Live sidecar -----------------------------------------------------------


def write_live(
    path: Path,
    *,
    iter_index: int,
    iterations_target: int,
    elapsed_cumulative: float,
    games_cumulative: int,
    session_count: int,
    recent_ys: List[float],
    theta: Dict[str, float],
    starting_theta: Dict[str, float],
    specs: List[Spec],
    per_iter_updates: List[Tuple[str, float]],
    clamp_hits: Dict[str, int],
) -> None:
    iters_left = max(iterations_target - iter_index - 1, 0)
    rate_sec_per_iter = (
        elapsed_cumulative / max(iter_index + 1, 1) if iter_index >= 0 else 0.0
    )
    eta_sec = rate_sec_per_iter * iters_left
    running_y = sum(recent_ys[-20:]) / max(len(recent_ys[-20:]), 1)

    top_movers = sorted(per_iter_updates, key=lambda kv: abs(kv[1]), reverse=True)[:5]
    obj = {
        "iter": iter_index,
        "iterations_target": iterations_target,
        "elapsed_sec_cumulative": elapsed_cumulative,
        "games_total_cumulative": games_cumulative,
        "session_count": session_count,
        "eta_sec": eta_sec,
        "running_y_last20": running_y,
        "theta": {s.name: clamp_round(theta[s.name], s) for s in specs},
        "starting_theta": {s.name: int(starting_theta[s.name]) for s in specs},
        "top_movers": [
            {"name": name, "delta": round(delta, 3)} for name, delta in top_movers
        ],
        "clamp_hits": dict(clamp_hits),
    }
    atomic_write_json(path, obj)


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
                    help="theta.json to resume from (restores theta, RNG, iter counter)")
    ap.add_argument("--dry-run", action="store_true",
                    help="exercise the loop without spawning fastchess")
    ap.add_argument("--list-tunables", action="store_true",
                    help="print discovered specs and exit")
    ap.add_argument("--params", type=str, default=None,
                    help="comma-separated subset of tunables to perturb; others stay at engine defaults")
    ap.add_argument("--stop-after", type=int, default=None,
                    help="exit gracefully after completing N iterations this session; useful "
                    "for testing resume or capping per-session wall time on a laptop")
    args = ap.parse_args()

    if args.tc and args.nodes is not None:
        ap.error("--tc and --nodes are mutually exclusive; pass only one")
    tc = args.tc if args.nodes is None else None
    nodes = args.nodes

    normalize_fd_limit()

    all_specs = discover_specs(args.engine)
    if args.list_tunables:
        for s in all_specs:
            print(f"{s.name}\tdefault={s.default}\tmin={s.minimum}\tmax={s.maximum}"
                  f"\tc_end={s.c_end}\tr_end={s.r_end}")
        return 0

    params_scope: Optional[List[str]] = None
    if args.params:
        params_scope = [p.strip() for p in args.params.split(",") if p.strip()]
    specs = filter_specs(all_specs, params_scope)

    # Fresh-run initial state.
    theta: Dict[str, float] = {s.name: float(s.default) for s in specs}
    starting_theta: Dict[str, float] = dict(theta)
    rng = random.Random(args.seed)
    start_iter = 0
    elapsed_cumulative = 0.0
    games_cumulative = 0
    session_count = 1
    iterations_target = args.iterations

    if args.resume:
        resumed = json.loads(Path(args.resume).read_text())
        for name, value in resumed.get("params", {}).items():
            if name in theta:
                theta[name] = float(value)
        meta = resumed.get("meta", {})
        start_iter = int(meta.get("next_iter", 0))
        if "rng_state" in meta:
            rng.setstate(rng_state_from_json(meta["rng_state"]))
        elapsed_cumulative = float(meta.get("elapsed_sec_cumulative", 0.0))
        games_cumulative = int(meta.get("games_total_cumulative", 0))
        session_count = int(meta.get("session_count", 0)) + 1
        iterations_target = int(meta.get("iterations_target", iterations_target))

        if start_iter >= iterations_target:
            sys.stderr.write(
                f"Checkpoint already at iter {start_iter}/{iterations_target}; "
                f"nothing to do.\n"
            )
            return 0

    gains: Dict[str, Tuple[float, float, float]] = {
        s.name: derive_ac(s, iterations_target) for s in specs
    }

    # Report the iter-0 and iter-final a_k / c_k for sanity.
    sys.stderr.write(
        f"SPSA session #{session_count}: {len(specs)} scalars, iters "
        f"{start_iter} -> {iterations_target}, concurrency {args.concurrency}\n"
    )
    for s in specs:
        a, c, A = gains[s.name]
        c0 = c / (1 ** GAMMA)
        cN = c / (iterations_target ** GAMMA)
        a0 = a / ((1 + A) ** ALPHA)
        aN = a / ((iterations_target + A) ** ALPHA)
        sys.stderr.write(
            f"  {s.name}: c0={c0:.2f} cN={cN:.2f} a0={a0:.4f} aN={aN:.4f} "
            f"theta={int(theta[s.name])} bounds=[{s.minimum},{s.maximum}]\n"
        )

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    history_path = out_dir / "history.csv"
    theta_path = out_dir / "theta.json"
    live_path = out_dir / "live.json"

    games_per_iter = 2 * args.concurrency
    # 20 seconds per game is generous at 10+0.1; scale slightly with concurrency
    # so the CI timeout still catches a hung match.
    iter_timeout_sec = max(60.0, 20.0 * games_per_iter / max(args.concurrency, 1)) * 3

    stop = StopController()
    stop.install()

    # Track cumulative clamp hits and a rolling y window for live.json.
    clamp_hits: Dict[str, int] = {s.name: 0 for s in specs}
    recent_ys: List[float] = []

    # History CSV: append if resuming, write header if starting fresh.
    hist_mode = "a" if args.resume and history_path.exists() else "w"
    with history_path.open(hist_mode, newline="") as f_hist:
        writer = csv.writer(f_hist)
        if hist_mode == "w":
            write_history_header(writer, specs)

        session_start_wall = time.time()
        iters_this_session = 0
        next_iter = start_iter  # advanced only after an iteration's theta update completes

        for k in range(start_iter, iterations_target):
            if stop.stop_requested:
                # Graceful stop between iterations: save and exit.
                break
            if args.stop_after is not None and iters_this_session >= args.stop_after:
                # Session-cap reached: drop through to final checkpoint write
                # and exit cleanly. Identical to a SIGTERM between iterations.
                stop.stop_requested = True
                break

            iter_start = time.time()

            delta = {s.name: (1 if rng.random() < 0.5 else -1) for s in specs}

            plus: Dict[str, int] = {}
            minus: Dict[str, int] = {}
            per_iter_updates: List[Tuple[str, float]] = []
            for s in specs:
                a, c, A = gains[s.name]
                c_k = c / ((k + 1) ** GAMMA)
                plus_raw = theta[s.name] + c_k * delta[s.name]
                minus_raw = theta[s.name] - c_k * delta[s.name]
                plus[s.name] = clamp_round(plus_raw, s)
                minus[s.name] = clamp_round(minus_raw, s)
                if plus[s.name] != int(round(plus_raw)) or minus[s.name] != int(
                    round(minus_raw)
                ):
                    clamp_hits[s.name] += 1

            opening_start = rng.randint(1, 4000)

            wins = 0
            losses = 0
            draws = 0
            y = 0.0

            if args.dry_run:
                reward = sum(delta[s.name] for s in specs[: min(3, len(specs))])
                y = max(-1.0, min(1.0, reward / max(len(specs[:3]), 1) * 0.1))
                wins = int((y + 1.0) / 2.0 * games_per_iter)
                losses = games_per_iter - wins
                draws = 0
            else:
                result = run_iteration(
                    args.fastchess, args.engine, args.openings, tc, nodes,
                    args.concurrency, plus, minus, opening_start,
                    iter_timeout_sec, stop,
                )
                if stop.force_stop:
                    # Current iteration was aborted by signal; break without updating theta.
                    break
                if result is None:
                    sys.stderr.write(
                        f"  iter {k}: could not parse results, skipping update\n"
                    )
                    elapsed_iter = time.time() - iter_start
                    write_history_row(writer, k, specs, theta, 0, 0, 0, 0.0, elapsed_iter)
                    f_hist.flush()
                    elapsed_cumulative += elapsed_iter
                    iters_this_session += 1
                    next_iter = k + 1
                    continue
                wins, losses, draws = result
                games_observed = wins + losses + draws
                if games_observed == 0:
                    sys.stderr.write(f"  iter {k}: zero games observed, skipping\n")
                    elapsed_iter = time.time() - iter_start
                    write_history_row(writer, k, specs, theta, 0, 0, 0, 0.0, elapsed_iter)
                    f_hist.flush()
                    elapsed_cumulative += elapsed_iter
                    iters_this_session += 1
                    next_iter = k + 1
                    continue
                y = (wins - losses) / games_observed
                games_cumulative += games_observed

            # SPSA update.
            for s in specs:
                a, c, A = gains[s.name]
                c_k = c / ((k + 1) ** GAMMA)
                a_k = a / ((k + 1 + A) ** ALPHA)
                g = y / (2.0 * c_k * delta[s.name])
                update = a_k * g
                theta[s.name] = float(clamp_round(theta[s.name] + update, s))
                per_iter_updates.append((s.name, update))

            recent_ys.append(y)
            iters_this_session += 1
            next_iter = k + 1
            elapsed_iter = time.time() - iter_start
            elapsed_cumulative += elapsed_iter

            write_history_row(writer, k, specs, theta, wins, losses, draws, y, elapsed_iter)
            f_hist.flush()

            # Atomic per-iter checkpoint so any stop / crash loses at most this iter.
            meta = build_checkpoint_meta(
                iterations_target=iterations_target,
                next_iter=k + 1,
                seed=args.seed,
                tc=tc,
                nodes=nodes,
                concurrency=args.concurrency,
                engine=args.engine,
                openings=args.openings,
                params_scope=params_scope,
                elapsed_cumulative=elapsed_cumulative,
                games_cumulative=games_cumulative,
                session_count=session_count,
                rng=rng,
            )
            write_checkpoint(theta_path, specs, theta, meta)

            write_live(
                live_path,
                iter_index=k,
                iterations_target=iterations_target,
                elapsed_cumulative=elapsed_cumulative,
                games_cumulative=games_cumulative,
                session_count=session_count,
                recent_ys=recent_ys,
                theta=theta,
                starting_theta=starting_theta,
                specs=specs,
                per_iter_updates=per_iter_updates,
                clamp_hits=clamp_hits,
            )

            if (k + 1) % 10 == 0 or k + 1 == iterations_target:
                sys.stderr.write(
                    f"  iter {k + 1}/{iterations_target}: y={y:+.3f} "
                    f"({wins}W/{draws}D/{losses}L), games={games_cumulative}, "
                    f"elapsed={elapsed_cumulative:.0f}s\n"
                )

    # Final checkpoint (covers clean completion AND graceful stop paths).
    # `next_iter` is tracked inside the loop: it advances only after an
    # iteration's theta update completes, so an interrupted iter leaves
    # next_iter pointing at the iter that needs to be redone on resume.
    meta = build_checkpoint_meta(
        iterations_target=iterations_target,
        next_iter=next_iter,
        seed=args.seed,
        tc=tc,
        nodes=nodes,
        concurrency=args.concurrency,
        engine=args.engine,
        openings=args.openings,
        params_scope=params_scope,
        elapsed_cumulative=elapsed_cumulative,
        games_cumulative=games_cumulative,
        session_count=session_count,
        rng=rng,
    )
    write_checkpoint(theta_path, specs, theta, meta)

    if stop.force_stop:
        sys.stderr.write(
            f"Force-stopped. Checkpoint at iter {next_iter} saved to {theta_path}\n"
        )
    elif stop.stop_requested:
        sys.stderr.write(
            f"Paused cleanly at iter {next_iter}. Resume with --resume {theta_path}\n"
        )
    elif next_iter >= iterations_target:
        sys.stderr.write(
            f"Run complete. Final theta in {theta_path}, history in {history_path}\n"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
