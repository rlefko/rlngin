#!/usr/bin/env python3
"""Extract quiet labeled positions from a PGN for Texel tuning.

Reads a PGN with game results, walks every game, and for each position
skips the opening, skips positions that are in check, skips the last
two plies, and (by default) skips positions where the engine's reported
score during the game was a mate score. Emits labeled positions in the
form ``FEN | result`` where ``result`` is in [0, 1] from White's
perspective.

By default identical positions are folded into a single row whose
label is the average of every observed game outcome from that
position; the tuner's MSE loss handles fractional labels just as well
as 1.0 / 0.5 / 0.0 and a transposition that lost twice and drew once
gets the label 1/6 instead of three rows pulling against the model
with conflicting gradients. Equality is the four-field position state
(board / side to move / castling / en passant), matching the standard
chess-rules notion of position; halfmove and fullmove counters do not
participate in the dedup key. The emitted FEN is the first occurrence
seen, so the eval still reads a real halfmove counter from the game.
Pass ``--no-dedup`` to keep one row per ply (legacy mode).
"""
import argparse
import re
import sys

import chess
import chess.pgn


RESULT_MAP = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}

# fastchess writes engine scores in the comment after each move:
#   ``{+0.32/14 0.07s}`` for centipawn, ``{+M5/7 0.008s}`` /
#   ``{-M4/4 ...}`` for mate, occasionally ``{#5}`` from other engines.
# python-chess strips the curly braces, so the comment string starts at
# the score token directly. Match only against that leading token to
# avoid misfiring on later metadata.
MATE_RE = re.compile(r"^[+\-]?M\d+|^#\d+", re.IGNORECASE)


def comment_is_mate(comment: str) -> bool:
    if not comment:
        return False
    token = comment.split(None, 1)[0] if comment.split() else ""
    return bool(MATE_RE.match(token))


def extract(pgn_path: str, out_path: str, skip_plies: int, tail_plies: int,
            mate_filter: bool, dedup: bool):
    raw_positions = 0
    games = 0
    skipped_games = 0
    mate_filtered = 0
    fen_stats: dict = {} if dedup else {}
    out_handle = None if dedup else open(out_path, "w")
    try:
        with open(pgn_path) as f_in:
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

                # Walk the mainline as a node sequence so each step
                # exposes both the move and the engine comment that
                # followed it. The comment after move N is the engine's
                # evaluation of the position it was *searching from*,
                # which is the position before move N -- exactly the
                # position we are about to label. So a mate comment
                # after move N means we drop the FEN at that ply.
                move_records = []
                node = game
                while node.variations:
                    nxt = node.variation(0)
                    move_records.append((nxt.move, nxt.comment))
                    node = nxt
                n = len(move_records)

                board = game.board()
                for ply, (move, comment) in enumerate(move_records):
                    if ply >= skip_plies and ply < n - tail_plies:
                        if board.is_check() or board.is_game_over():
                            pass
                        elif mate_filter and comment_is_mate(comment):
                            mate_filtered += 1
                        else:
                            fen = board.fen()
                            if dedup:
                                key = fen.rsplit(" ", 2)[0]
                                slot = fen_stats.get(key)
                                if slot is None:
                                    fen_stats[key] = [1, label, fen]
                                else:
                                    slot[0] += 1
                                    slot[1] += label
                            else:
                                out_handle.write(f"{fen} | {label}\n")
                            raw_positions += 1
                    board.push(move)

                if games % 500 == 0:
                    extra = (f"{len(fen_stats)} unique" if dedup
                             else f"{raw_positions} positions")
                    print(f"  processed {games} games, {extra}, "
                          f"{mate_filtered} mate-filtered",
                          file=sys.stderr)
    finally:
        if out_handle is not None:
            out_handle.close()

    if dedup:
        unique = len(fen_stats)
        with open(out_path, "w") as f_out:
            for _key, (count, total, repr_fen) in fen_stats.items():
                f_out.write(f"{repr_fen} | {total / count}\n")
        dup_pct = (100.0 * (raw_positions - unique) / raw_positions
                   if raw_positions else 0.0)
        print(f"done: {games} games, {raw_positions} raw positions, "
              f"{unique} unique ({dup_pct:.1f}% folded), "
              f"{mate_filtered} mate-filtered, "
              f"{skipped_games} games skipped (no result)",
              file=sys.stderr)
    else:
        print(f"done: {games} games, {raw_positions} positions, "
              f"{mate_filtered} mate-filtered, "
              f"{skipped_games} games skipped (no result)",
              file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pgn", help="input PGN file")
    ap.add_argument("out", help="output labeled EPD file")
    ap.add_argument("--skip-plies", type=int, default=8,
                    help="skip this many opening plies (default 8)")
    ap.add_argument("--tail-plies", type=int, default=2,
                    help="skip this many plies at the end (default 2)")
    ap.add_argument("--no-mate-filter", dest="mate_filter",
                    action="store_false",
                    help="keep positions whose engine score was a mate "
                         "(default: drop them)")
    ap.set_defaults(mate_filter=True)
    ap.add_argument("--no-dedup", dest="dedup", action="store_false",
                    help="emit one row per ply instead of folding "
                         "identical FENs into a single weighted-average "
                         "row (default: dedup on)")
    ap.set_defaults(dedup=True)
    args = ap.parse_args()
    extract(args.pgn, args.out, args.skip_plies, args.tail_plies,
            args.mate_filter, args.dedup)


if __name__ == "__main__":
    main()
