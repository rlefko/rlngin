#!/usr/bin/env python3
"""Extract quiet labeled positions from a PGN for Texel tuning.

Reads a PGN with game results, walks every game, and for each position
skips the opening, skips positions that are in check, and skips the last
two plies. Emits one labeled position per surviving ply in the form
`FEN | result` where `result` is 1.0 / 0.5 / 0.0 from White's
perspective.
"""
import argparse
import sys

import chess
import chess.pgn


RESULT_MAP = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}


def extract(pgn_path: str, out_path: str, skip_plies: int, tail_plies: int):
    positions = 0
    games = 0
    skipped_games = 0
    with open(pgn_path) as f_in, open(out_path, "w") as f_out:
        while True:
            game = chess.pgn.read_game(f_in)
            if game is None:
                break
            games += 1
            result = game.headers.get("Result", "*")
            if result not in RESULT_MAP:
                skipped_games += 1
                continue
            label = RESULT_MAP[result]
            moves = list(game.mainline_moves())
            n = len(moves)
            board = game.board()
            for ply, move in enumerate(moves):
                if ply >= skip_plies and ply < n - tail_plies:
                    if not board.is_check() and not board.is_game_over():
                        fen = board.fen()
                        f_out.write(f"{fen} | {label}\n")
                        positions += 1
                board.push(move)
            if games % 500 == 0:
                print(f"  processed {games} games, {positions} positions",
                      file=sys.stderr)

    print(f"done: {games} games, {positions} positions, "
          f"{skipped_games} games skipped (no result)", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pgn", help="input PGN file")
    ap.add_argument("out", help="output labeled EPD file")
    ap.add_argument("--skip-plies", type=int, default=8,
                    help="skip this many opening plies (default 8)")
    ap.add_argument("--tail-plies", type=int, default=2,
                    help="skip this many plies at the end (default 2)")
    args = ap.parse_args()
    extract(args.pgn, args.out, args.skip_plies, args.tail_plies)


if __name__ == "__main__":
    main()
