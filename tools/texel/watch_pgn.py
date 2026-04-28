#!/usr/bin/env python3
"""Live viewer for an in-progress fastchess PGN.

Tails a PGN file that fastchess is appending to, parses each game as it
lands, and prints a one-line summary plus a periodic running-stats line.
The viewer is read-only on a sequentially appended file, so it adds no
contention on the engine threads driving the match.

Two modes:

* Follow mode (default): block on the file, print every completed game
  as it appears, and emit a stats banner every ``--banner-every`` games.
  Stop with Ctrl-C.
* One-shot mode (``--once``): print the most recent ``--tail`` games and
  the stats banner, then exit. Suitable as the inner command for
  ``watch -t -n <interval>``.
"""
from __future__ import annotations

import argparse
import io
import os
import re
import sys
import time
from collections import deque
from datetime import datetime
from typing import Optional

import chess
import chess.pgn


# Each fastchess game block ends with one of the four PGN result tokens
# followed by a blank line. We use that to decide when the buffer holds
# at least one complete game ready to feed to ``chess.pgn.read_game``.
GAME_BOUNDARY_RE = re.compile(r"(?:1-0|0-1|1/2-1/2|\*)\s*\n\s*\n")

RESULT_LABEL = {
    "1-0": "1-0",
    "0-1": "0-1",
    "1/2-1/2": "1/2",
    "*":     " * ",
}


class GameSummary:
    __slots__ = ("seen_at", "white", "black", "result", "plies", "tail_san", "term")

    def __init__(self, white: str, black: str, result: str, plies: int,
                 tail_san: str, term: str):
        self.seen_at = datetime.now()
        self.white = white
        self.black = black
        self.result = result
        self.plies = plies
        self.tail_san = tail_san
        self.term = term

    def line(self, idx: int) -> str:
        ts = self.seen_at.strftime("%H:%M:%S")
        res = RESULT_LABEL.get(self.result, self.result)
        term = f"  [{self.term}]" if self.term else ""
        return (f"[{ts}] #{idx:06d}  {self.white} (W) {res} {self.black}"
                f"  plies={self.plies}{term}  ...{self.tail_san}")


class Stats:
    def __init__(self, target: int):
        self.target = target
        self.start = time.monotonic()
        self.total = 0
        self.w_wins = 0
        self.draws = 0
        self.b_wins = 0
        self.unknown = 0
        # Sliding window of (monotonic_time) of recent game completions
        # so we can report a short-window rate that reflects current
        # conditions rather than a full-match average.
        self.recent = deque(maxlen=200)

    def record(self, result: str) -> None:
        self.total += 1
        self.recent.append(time.monotonic())
        if result == "1-0":
            self.w_wins += 1
        elif result == "0-1":
            self.b_wins += 1
        elif result == "1/2-1/2":
            self.draws += 1
        else:
            self.unknown += 1

    def banner(self) -> str:
        if self.total == 0:
            return "--- waiting for first game ---"
        score = (self.w_wins + 0.5 * self.draws) / self.total
        if len(self.recent) >= 2:
            window = self.recent[-1] - self.recent[0]
            window_n = len(self.recent) - 1
            rate_per_min = (window_n / window) * 60 if window > 0 else 0.0
        else:
            rate_per_min = 0.0
        remaining = max(self.target - self.total, 0)
        if rate_per_min > 0 and remaining > 0:
            eta_h = remaining / rate_per_min / 60
            eta_str = f"ETA {eta_h:.1f}h"
        elif remaining == 0:
            eta_str = "ETA done"
        else:
            eta_str = "ETA --"
        return (f"--- {self.total}/{self.target} games"
                f"  W:{self.w_wins} D:{self.draws} L:{self.b_wins}"
                f"  ({score*100:.1f}%)"
                f"  rate {rate_per_min:.1f}/min"
                f"  {eta_str} ---")


def summarize_game(game: chess.pgn.Game, tail_moves: int) -> Optional[GameSummary]:
    headers = game.headers
    result = headers.get("Result", "*")
    white = headers.get("White", "?")
    black = headers.get("Black", "?")
    term = headers.get("Termination", "")
    board = game.board()
    san_history: deque[str] = deque(maxlen=tail_moves)
    plies = 0
    node = game
    while node.variations:
        nxt = node.variation(0)
        try:
            san = board.san(nxt.move)
        except (ValueError, AssertionError):
            san = nxt.move.uci() if nxt.move else "?"
        san_history.append(san)
        board.push(nxt.move)
        plies += 1
        node = nxt
    tail_san = " ".join(san_history) if san_history else ""
    return GameSummary(white, black, result, plies, tail_san, term)


def consume_games(buffer: str):
    """Parse every complete game from ``buffer`` and return the list of
    games together with the unparsed tail. ``buffer`` is the raw PGN text
    seen so far; the unparsed tail is whatever remains after the last
    complete game and should be carried over to the next read."""
    if not GAME_BOUNDARY_RE.search(buffer):
        return [], buffer
    games = []
    stream = io.StringIO(buffer)
    consumed = 0
    while True:
        # Only attempt another read if the unconsumed remainder still
        # contains a result-token boundary; otherwise we'd block parsing
        # an incomplete trailing game and lose state mid-stream.
        if not GAME_BOUNDARY_RE.search(buffer, pos=consumed):
            break
        pos_before = stream.tell()
        try:
            game = chess.pgn.read_game(stream)
        except Exception:
            # Corrupt or partial entry: drop everything up to the next
            # safe boundary so the tail keeps flowing rather than wedge.
            m = GAME_BOUNDARY_RE.search(buffer, pos=consumed)
            consumed = m.end() if m else len(buffer)
            stream.seek(consumed)
            continue
        if game is None:
            break
        games.append(game)
        consumed = stream.tell()
        if consumed == pos_before:
            break
    return games, buffer[consumed:]


def follow(path: str, target: int, tail_moves: int, banner_every: int,
           poll_interval: float, from_start: bool) -> None:
    stats = Stats(target)
    print(f"watching {path} (target={target})", file=sys.stderr)
    # Wait for the file to exist; fastchess opens it lazily after the
    # opening book is loaded so a freshly started match may briefly have
    # no PGN file on disk.
    while not os.path.exists(path):
        time.sleep(poll_interval)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        if not from_start:
            f.seek(0, os.SEEK_END)
        buffer = ""
        try:
            while True:
                chunk = f.read()
                if chunk:
                    buffer += chunk
                    games, buffer = consume_games(buffer)
                    for game in games:
                        summary = summarize_game(game, tail_moves)
                        if summary is None:
                            continue
                        stats.record(summary.result)
                        print(summary.line(stats.total))
                        if banner_every > 0 and stats.total % banner_every == 0:
                            print(stats.banner())
                            sys.stdout.flush()
                        else:
                            sys.stdout.flush()
                else:
                    time.sleep(poll_interval)
        except KeyboardInterrupt:
            print("", file=sys.stderr)
            print(stats.banner(), file=sys.stderr)


def once(path: str, target: int, tail_moves: int, tail_games: int) -> None:
    if not os.path.exists(path):
        print(f"(no PGN yet at {path})")
        return
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        buffer = f.read()
    games, _ = consume_games(buffer)
    stats = Stats(target)
    summaries = []
    for game in games:
        summary = summarize_game(game, tail_moves)
        if summary is None:
            continue
        stats.record(summary.result)
        summaries.append(summary)
    if not summaries:
        print(f"(no games yet in {path})")
        return
    start = max(0, len(summaries) - tail_games)
    print(stats.banner())
    print()
    for offset, summary in enumerate(summaries[start:], start=start + 1):
        print(summary.line(offset))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path", nargs="?", default="tuning/texel/games.pgn",
                    help="PGN file to follow (default: tuning/texel/games.pgn)")
    ap.add_argument("--target", type=int, default=64000,
                    help="expected total games for ETA (default 64000)")
    ap.add_argument("--tail-moves", type=int, default=8,
                    help="how many SAN moves to print at end of each game"
                         " line (default 8)")
    ap.add_argument("--banner-every", type=int, default=25,
                    help="emit stats banner every N games in follow"
                         " mode (default 25, set 0 to disable)")
    ap.add_argument("--poll-interval", type=float, default=1.0,
                    help="follow-mode poll interval seconds (default 1.0)")
    ap.add_argument("--from-start", action="store_true",
                    help="in follow mode, start from the beginning of"
                         " the file rather than the current end")
    ap.add_argument("--once", action="store_true",
                    help="print recent games + stats once and exit"
                         " (suitable for 'watch -t -n')")
    ap.add_argument("--tail-games", type=int, default=15,
                    help="in --once mode, how many recent games to show"
                         " (default 15)")
    args = ap.parse_args()

    if args.once:
        once(args.path, args.target, args.tail_moves, args.tail_games)
    else:
        follow(args.path, args.target, args.tail_moves, args.banner_every,
               args.poll_interval, args.from_start)
    return 0


if __name__ == "__main__":
    sys.exit(main())
