#!/usr/bin/env python3
"""Re-label the full Texel corpus with fresh Stockfish evaluations.

The base corpus at `tuning/texel/positions.epd` carries cp labels
from the self-play `fastchess` comments. Those labels are the
engine's eval at the time it chose its move, at the per-move node
budget the self-play used (typically depth 10-12). The signal has
two known issues:

1. **Selection bias.** The eval was computed while the engine was
   picking THIS move; the search tree was shaped around the chosen
   move, not the position. A fresh search from the position itself
   tends to disagree with the comment-time eval by a few cp on
   tactical positions.

2. **Variable depth.** Self-play moves get whatever depth fits in
   the per-move node budget; complex positions get less.

This script re-evaluates every position with a fresh fixed-budget
Stockfish search, in parallel across N worker processes. Each
worker spawns its own Stockfish, slices the input file
non-overlapping with the other workers, and uses **raw UCI** in
binary mode rather than going through `chess.engine.analyse()` to
strip the 5-10ms Python-side overhead that capped throughput in an
earlier draft (the queue-based approach hit ~140% CPU on 14 cores;
this approach hits ~900% on small smoke and should saturate at the
full ~1400% on a long run).

Worker invariant per position:
    write   position fen <FEN>\\n
    write   go nodes <N>\\n
    read    info ... lines, capture the last score cp/mate
    read    bestmove ... line
    write   <FEN> | label | <gameids>\\n   to its shard

No `isready` between positions; the `bestmove` line is a sufficient
sync barrier for SF.

**Resume**: on startup each worker inspects its existing shard
(if any) and skips forward in the input by the number of complete
rows it already has. A mid-write crash that left a partial last
line gets trimmed to the previous newline and the truncated row is
re-evaluated. A meta header at the top of each shard records the
`nodes` argument the shard was built with, and a resume attempt
with a different `nodes` value aborts rather than silently mixing
labels. Re-run with the exact same flags to resume; pass
`--shard-dir <newdir>` to start fresh under a different node
budget.

Tablebase probing stays on by default. Syzygy 3-4-5 lives in RAM via
mmap after the first probe, so it adds <1us to endgame positions and
zero to middlegame ones; on balance it improves endgame label quality
materially.

Output preserves the original `gameIds` field so the tuner's
inverse-game-size weighting and train/val split still work.

Usage:
    python3 scripts/relabel_corpus_with_sf.py
        [--in tuning/texel/positions.epd]
        [--out tuning/texel/positions_sf.epd]
        [--nodes 100000]
        [--cp-scale 200.0]
        [--workers 14]
        [--hash-mb 16]
        [--syzygy-path ~/projects/chess/3-4-5/  | --no-syzygy]
        [--stockfish ~/projects/chess/Stockfish/src/stockfish]
        [--shard-dir tuning/texel/relabel_shards]
"""
import argparse
import math
import multiprocessing as mp
import os
import signal
import subprocess
import sys
import time
from pathlib import Path


def cp_to_label(cp: int, scale: float) -> float:
    """Logistic squash matching extract_tuning_positions.py."""
    z = max(-50.0, min(50.0, cp / scale))
    return 1.0 / (1.0 + math.exp(-z))


def parse_score_token(tokens: list, fen_stm_is_black: bool) -> int | None:
    """Find ' score cp X' or ' score mate X' in a tokenized info line.

    Returns White-relative cp clamped to +/- 1500, or None if no score
    token is present.
    """
    try:
        idx = tokens.index(b"score")
    except ValueError:
        return None
    if idx + 2 >= len(tokens):
        return None
    kind = tokens[idx + 1]
    try:
        raw = int(tokens[idx + 2])
    except ValueError:
        return None
    if kind == b"mate":
        cp = 1500 if raw > 0 else -1500
    elif kind == b"cp":
        cp = raw
    else:
        return None
    # SF reports side-to-move-relative. Flip for Black-to-move so we
    # always store White-perspective.
    if fen_stm_is_black:
        cp = -cp
    if cp > 1500:
        cp = 1500
    elif cp < -1500:
        cp = -1500
    return cp


def parse_input_line(line: bytes):
    """Returns (fen_bytes, fen_stm_is_black, gameids_bytes) or None."""
    if not line or line.startswith(b"#"):
        return None
    parts = line.rstrip(b"\n").split(b"|", 2)
    if len(parts) < 2:
        return None
    fen = parts[0].strip()
    gids = parts[2].strip() if len(parts) >= 3 else b""
    # Side-to-move is the second whitespace-separated field of the FEN.
    fen_fields = fen.split(None, 2)
    if len(fen_fields) < 2:
        return None
    fen_stm_is_black = (fen_fields[1] == b"b")
    return fen, fen_stm_is_black, gids


# Shard meta header. Lines starting with `#` are skipped by the C++
# loader and by parse_input_line(); the header carries the nodes
# value the shard was built with so a resume run can detect if it's
# being asked to mix labels at different search budgets.
SHARD_META_PREFIX = b"# nodes="


def shard_resume_state(shard_path: str, expected_nodes: int) -> int:
    """Inspect an existing shard for resume.

    Returns the number of DATA rows already written, after trimming any
    trailing partial line (a worker killed mid-write may have left an
    unterminated last line). Raises RuntimeError if the shard's meta
    header disagrees with the current `nodes` argument — refusing to
    silently mix labels at different search budgets.

    If the shard does not exist, returns 0.
    """
    path = Path(shard_path)
    if not path.exists():
        return 0

    # First, read the whole file (shards are <100 MB even on the full
    # corpus, comfortably fits in RAM).
    with open(path, "rb") as f:
        buf = f.read()

    # If the last byte isn't a newline, the worker died mid-row. Find
    # the last newline and trim everything after it: that row will be
    # re-evaluated.
    if buf and not buf.endswith(b"\n"):
        last_nl = buf.rfind(b"\n")
        if last_nl < 0:
            # No complete row at all; reset the shard.
            with open(path, "wb"):
                pass
            return 0
        buf = buf[:last_nl + 1]
        with open(path, "wb") as f:
            f.write(buf)

    # Validate meta header. If a header line exists and disagrees with
    # the current nodes value, refuse to continue.
    saw_nodes = None
    rows = 0
    for line in buf.split(b"\n"):
        if not line:
            continue
        if line.startswith(SHARD_META_PREFIX):
            # `# nodes=NNNN ...`
            tail = line[len(SHARD_META_PREFIX):]
            try:
                saw_nodes = int(tail.split(b" ", 1)[0])
            except ValueError:
                saw_nodes = None
            continue
        if line.startswith(b"#"):
            continue
        rows += 1

    if saw_nodes is not None and saw_nodes != expected_nodes:
        raise RuntimeError(
            f"shard {shard_path} was built with nodes={saw_nodes}, "
            f"refusing to mix with nodes={expected_nodes}. "
            f"Delete the shard or use --shard-dir to point elsewhere."
        )
    return rows


def worker_main(worker_id: int, in_path: str, shard_out: str,
                start_line: int, end_line: int, *,
                sf_path: str, hash_mb: int, syzygy: str | None,
                nodes: int, cp_scale: float, flush_every: int = 100):
    """Each worker spawns its own SF, slices the input file, and writes
    its own shard. No queue, no python-chess wrapper.

    If the shard file already exists with N valid data rows, the worker
    skips ahead N positions in its input slice and resumes from there.
    A mid-write crash that left the shard with a partial last line is
    repaired by truncating to the last newline (the truncated row gets
    re-evaluated, which is correct).
    """
    signal.signal(signal.SIGINT, signal.SIG_IGN)

    # Resume check: how many rows are already in our shard?
    already_done = shard_resume_state(shard_out, nodes)
    resume_start = start_line + already_done
    if already_done > 0:
        print(f"  w{worker_id:02d}: resuming, {already_done} rows already in "
              f"{shard_out}, picking up at line {resume_start}",
              file=sys.stderr, flush=True)
    if resume_start >= end_line:
        print(f"  w{worker_id:02d}: shard already complete, skipping",
              file=sys.stderr, flush=True)
        return

    proc = subprocess.Popen([sf_path],
                            stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL,
                            bufsize=0)
    stdin = proc.stdin
    stdout = proc.stdout
    readline = stdout.readline

    def write(cmd: bytes):
        stdin.write(cmd)
        stdin.flush()

    def wait_for(prefix: bytes):
        while True:
            line = readline()
            if not line:
                raise RuntimeError("SF closed stdout unexpectedly")
            if line.startswith(prefix):
                return

    # UCI handshake. We deliberately skip `ucinewgame` between
    # positions because the SF transposition table is small (16 MB)
    # and clearing it adds a few ms per call without changing the
    # search outcome on independent positions.
    write(b"uci\n")
    wait_for(b"uciok")
    write(b"setoption name Threads value 1\n")
    write(f"setoption name Hash value {hash_mb}\n".encode())
    if syzygy:
        write(f"setoption name SyzygyPath value {syzygy}\n".encode())
    write(b"isready\n")
    wait_for(b"readyok")

    nodes_cmd = f"go nodes {nodes}\n".encode()
    bestmove_prefix = b"bestmove"

    # Open shard in APPEND mode so resume keeps existing rows.
    # On a fresh shard, write the meta header first so future resume
    # runs can verify the nodes value matches.
    fresh_shard = (already_done == 0)
    out_f = open(shard_out, "ab")
    if fresh_shard:
        meta = f"# nodes={nodes} cp-scale={cp_scale} worker={worker_id}\n"
        out_f.write(meta.encode())
        out_f.flush()
    processed = 0
    skipped = 0
    start_t = time.monotonic()
    last_report_t = start_t

    try:
        with open(in_path, "rb") as in_f:
            # Skip lines before resume_start. readline() on a binary
            # file is the fastest scan available without indexing.
            for _ in range(resume_start):
                if not in_f.readline():
                    break
            line_no = resume_start
            for raw_line in in_f:
                if line_no >= end_line:
                    break
                line_no += 1
                parsed = parse_input_line(raw_line)
                if parsed is None:
                    continue
                fen, stm_black, gids = parsed

                # Two writes fused into one flush: position then go.
                write(b"position fen " + fen + b"\n" + nodes_cmd)

                last_cp = None
                while True:
                    line = readline()
                    if not line:
                        last_cp = None
                        break
                    if line.startswith(bestmove_prefix):
                        break
                    if b" score " in line:
                        cp = parse_score_token(line.split(), stm_black)
                        if cp is not None:
                            last_cp = cp

                if last_cp is None:
                    skipped += 1
                    out_f.write(fen + b" | 0.5 | " + gids + b"\n")
                else:
                    label = cp_to_label(last_cp, cp_scale)
                    out_f.write(fen + b" | " + f"{label}".encode() + b" | " + gids + b"\n")
                processed += 1

                # Flush the shard every `flush_every` rows so a hard
                # crash (OOM kill, SIGKILL, power loss) loses at most
                # that many rows. The OS may still hold the bytes in
                # its page cache for a few more seconds; that is OK
                # for resume because we trim a partial last line on
                # restart and re-evaluate one row.
                if processed % flush_every == 0:
                    out_f.flush()

                now = time.monotonic()
                if now - last_report_t >= 30.0:
                    elapsed = now - start_t
                    rate = processed / elapsed if elapsed > 0 else 0.0
                    print(f"  w{worker_id:02d}: {processed} done, "
                          f"rate={rate:.0f} pos/s, "
                          f"elapsed={elapsed:.0f}s, "
                          f"skipped={skipped}",
                          file=sys.stderr, flush=True)
                    last_report_t = now
    finally:
        try:
            write(b"quit\n")
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        out_f.close()

    elapsed = time.monotonic() - start_t
    print(f"  w{worker_id:02d}: FINISHED {processed} positions in "
          f"{elapsed:.0f}s, skipped={skipped}",
          file=sys.stderr, flush=True)


def count_lines(path: Path) -> int:
    n = 0
    with open(path, "rb") as f:
        for _ in f:
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="in_path",
                    default="tuning/texel/positions.epd",
                    help="input EPD (default: tuning/texel/positions.epd)")
    ap.add_argument("--out", default="tuning/texel/positions_sf.epd",
                    help="merged output EPD")
    ap.add_argument("--shard-dir", default="tuning/texel/relabel_shards",
                    help="per-worker shard output directory")
    ap.add_argument("--nodes", type=int, default=100000,
                    help="Stockfish node budget per position (default: 100000)")
    ap.add_argument("--cp-scale", type=float, default=200.0,
                    help="logistic scale for cp -> label (default: 200)")
    ap.add_argument("--workers", type=int, default=14,
                    help="parallel SF worker processes (default: 14)")
    ap.add_argument("--hash-mb", type=int, default=16,
                    help="per-worker Stockfish Hash MB (default: 16)")
    ap.add_argument("--syzygy-path", default=None,
                    help="Stockfish SyzygyPath (default: $HOME/projects/chess/3-4-5)")
    ap.add_argument("--no-syzygy", action="store_true",
                    help="disable Syzygy tablebase probing entirely")
    ap.add_argument("--stockfish", default=None,
                    help="Stockfish binary "
                         "(default: $STOCKFISH or $HOME/projects/chess/Stockfish/src/stockfish)")
    ap.add_argument("--max-rows", type=int, default=0,
                    help="if > 0, only relabel the first N rows (smoke-test)")
    args = ap.parse_args()

    sf_path = (args.stockfish
               or os.environ.get("STOCKFISH")
               or str(Path.home() / "projects/chess/Stockfish/src/stockfish"))
    if args.no_syzygy:
        syzygy = None
    else:
        syzygy = (args.syzygy_path
                  or str(Path.home() / "projects/chess/3-4-5"))
        if not Path(syzygy).exists():
            print(f"  syzygy path {syzygy} does not exist; "
                  "disabling TB probing", file=sys.stderr)
            syzygy = None

    in_path = Path(args.in_path)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    shard_dir = Path(args.shard_dir)
    shard_dir.mkdir(parents=True, exist_ok=True)

    print(f"Stockfish: {sf_path}", file=sys.stderr)
    print(f"  workers={args.workers}, threads=1, hash={args.hash_mb}MB, "
          f"syzygy={syzygy or 'off'}", file=sys.stderr)
    print(f"  nodes={args.nodes}, cp_scale={args.cp_scale}", file=sys.stderr)
    print(f"  in={in_path}", file=sys.stderr)
    print(f"  out={out_path}", file=sys.stderr)
    print(f"  shards={shard_dir}", file=sys.stderr)

    print(f"counting lines in {in_path}...", file=sys.stderr)
    total_lines = count_lines(in_path)
    if args.max_rows and args.max_rows < total_lines:
        total_lines = args.max_rows
    print(f"total rows to process: {total_lines}", file=sys.stderr)

    chunk = (total_lines + args.workers - 1) // args.workers
    slices = []
    for w in range(args.workers):
        a = w * chunk
        b = min(a + chunk, total_lines)
        slices.append((a, b))

    start_t = time.monotonic()

    procs = []
    for w, (a, b) in enumerate(slices):
        shard_path = shard_dir / f"shard_{w:02d}.epd"
        p = mp.Process(target=worker_main, args=(w, str(in_path), str(shard_path), a, b),
                       kwargs=dict(
                           sf_path=sf_path,
                           hash_mb=args.hash_mb,
                           syzygy=syzygy,
                           nodes=args.nodes,
                           cp_scale=args.cp_scale,
                       ),
                       daemon=False)
        p.start()
        procs.append(p)
        print(f"  spawned worker {w:02d}: lines [{a}, {b}) -> {shard_path}",
              file=sys.stderr)

    try:
        for p in procs:
            p.join()
    except KeyboardInterrupt:
        print("interrupt: terminating workers...", file=sys.stderr)
        for p in procs:
            p.terminate()
        for p in procs:
            p.join(timeout=5)
        sys.exit(1)

    elapsed = time.monotonic() - start_t
    print(f"all workers finished in {elapsed:.0f}s ({elapsed/60:.1f} min)",
          file=sys.stderr)

    print(f"merging {args.workers} shards -> {out_path}...", file=sys.stderr)
    with open(out_path, "wb") as out_f:
        out_f.write(b"# Texel corpus relabelled with Stockfish.\n")
        out_f.write(b"# Generated by scripts/relabel_corpus_with_sf.py.\n")
        meta = (f"# nodes={args.nodes} cp-scale={args.cp_scale} "
                f"workers={args.workers} "
                f"syzygy={('on' if syzygy else 'off')} "
                f"hash={args.hash_mb}MB\n")
        out_f.write(meta.encode())
        for w in range(args.workers):
            shard_path = shard_dir / f"shard_{w:02d}.epd"
            if not shard_path.exists():
                print(f"  WARN: shard {shard_path} missing", file=sys.stderr)
                continue
            with open(shard_path, "rb") as in_f:
                # Stream-copy in 1MB chunks; faster than line iteration
                # for 5M-row shards.
                while True:
                    buf = in_f.read(1 << 20)
                    if not buf:
                        break
                    out_f.write(buf)

    print(f"done: wall clock {elapsed:.0f}s ({elapsed/60:.1f} min)",
          file=sys.stderr)


if __name__ == "__main__":
    main()
