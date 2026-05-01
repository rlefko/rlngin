#!/usr/bin/env python3
"""Extract quiet labeled positions from a PGN for Texel tuning.

Reads a PGN with game results, walks every game, skips opening and
trailing plies, drops positions in check, and (by default) drops
positions whose engine score during the game was a mate score.
Identical positions (four-field state: board / side to move /
castling / en passant) are folded into a single row whose label is
the average of every observed game outcome from that position. The
emitted FEN is the first occurrence seen, so the eval still reads a
real halfmove counter from the game.

Output is one ``FEN | result | gameIds`` line per unique position,
with ``result`` in [0, 1] from White's perspective and ``gameIds`` a
comma-separated list of source game indices (one per occurrence so a
position seen N times in 5 different games carries 5 ids). The third
field is parsed by the tuner for per-game inverse weighting and the
train/val split; older corpora without it still load (the C++
loader treats the field as optional).
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
            mate_filter: bool):
    raw_positions = 0
    games = 0
    next_game_id = 0
    skipped_games = 0
    mate_filtered = 0
    # Slot layout: [count, label_total, repr_fen, game_id_list].
    # game_id_list collects every source game id that produced this
    # FEN (with multiplicity, so a position seen twice in one game
    # appears twice in the list). The tuner uses the list both for
    # per-game inverse weighting and for the by-game train / val
    # split.
    fen_stats: dict = {}
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
            game_id = next_game_id
            next_game_id += 1

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
                        key = fen.rsplit(" ", 2)[0]
                        slot = fen_stats.get(key)
                        if slot is None:
                            fen_stats[key] = [1, label, fen, [game_id]]
                        else:
                            slot[0] += 1
                            slot[1] += label
                            slot[3].append(game_id)
                        raw_positions += 1
                board.push(move)

            if games % 500 == 0:
                print(f"  processed {games} games, {len(fen_stats)} unique, "
                      f"{mate_filtered} mate-filtered",
                      file=sys.stderr)

    unique = len(fen_stats)
    with open(out_path, "w") as f_out:
        for _key, (count, total, repr_fen, game_ids) in fen_stats.items():
            ids_field = ",".join(str(g) for g in game_ids)
            f_out.write(f"{repr_fen} | {total / count} | {ids_field}\n")
    dup_pct = (100.0 * (raw_positions - unique) / raw_positions
               if raw_positions else 0.0)
    print(f"done: {games} games, {raw_positions} raw positions, "
          f"{unique} unique ({dup_pct:.1f}% folded), "
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
    args = ap.parse_args()
    extract(args.pgn, args.out, args.skip_plies, args.tail_plies,
            args.mate_filter)


if __name__ == "__main__":
    main()
