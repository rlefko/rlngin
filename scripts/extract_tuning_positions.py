#!/usr/bin/env python3
"""Extract quiet labeled positions from a PGN for Texel tuning.

Reads a PGN with game results, walks every game, skips opening and
trailing plies, drops positions in check, and (by default) drops
positions whose engine score during the game was a mate score.
Identical positions (four-field state: board / side to move /
castling / en passant) are folded into a single row whose label is
the average of every observed score from that position. The emitted
FEN is the first occurrence seen, so the eval still reads a real
halfmove counter from the game.

Output is one ``FEN | label | gameIds`` line per unique position,
with ``label`` in [0, 1] from White's perspective and ``gameIds`` a
comma-separated list of source game indices (one per occurrence so a
position seen N times in 5 different games carries 5 ids). The third
field is parsed by the tuner for per-game inverse weighting and the
train/val split; older corpora without it still load (the C++
loader treats the field as optional).

By default the label is the game's W/D/L outcome (1.0 / 0.5 / 0.0).
With ``--label-from-cp`` the label comes from each move's engine
comment instead: the cp score Stockfish wrote next to the move is
converted to a White-perspective centipawn value and squashed through
a logistic ``1 / (1 + exp(-cp / SCALE))``. This trades aggregate
game-outcome supervision (every position in a winning game looks
"good") for per-position supervision (the engine that played the
move already evaluated this exact position) and is the right
teacher signal when the corpus comes from a strong reference engine.
"""
import argparse
import math
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
# Pawn-units cp token, e.g. "+0.32", "-1.50", "0", "+12.34". May be
# followed by "/depth" (and arbitrary other metadata after a space).
# Anchored at the start of the comment so a stray number deeper in
# the metadata can't be mistaken for the score.
CP_RE = re.compile(r"^([+\-]?\d+(?:\.\d+)?)")


def comment_is_mate(comment: str) -> bool:
    if not comment:
        return False
    token = comment.split(None, 1)[0] if comment.split() else ""
    return bool(MATE_RE.match(token))


def comment_cp(comment: str):
    """Parse the engine cp score from a fastchess move comment.

    Returns the cp value (positive = side-to-move winning) as an int,
    or ``None`` when the comment is missing, a mate score, or otherwise
    unparseable. Callers must convert side-to-move-relative cp to
    White-perspective themselves.
    """
    if not comment:
        return None
    token = comment.split(None, 1)[0] if comment.split() else ""
    if MATE_RE.match(token):
        return None
    m = CP_RE.match(token)
    if not m:
        return None
    try:
        pawns = float(m.group(1))
    except ValueError:
        return None
    return int(round(pawns * 100))


def cp_to_label(cp: int, scale: float) -> float:
    """Logistic squash of a White-perspective centipawn score into [0, 1]."""
    # Clamp the exponent so a runaway cp from a buggy comment doesn't
    # blow up math.exp; +/- 20 pawns saturates the sigmoid to within
    # 1e-9 of the rail at SCALE = 200 anyway.
    z = max(-50.0, min(50.0, cp / scale))
    return 1.0 / (1.0 + math.exp(-z))


def extract(pgn_path: str, out_path: str, skip_plies: int, tail_plies: int,
            mate_filter: bool, label_from_cp: bool, cp_scale: float):
    raw_positions = 0
    games = 0
    next_game_id = 0
    skipped_games = 0
    mate_filtered = 0
    # Positions dropped in cp-label mode because their comment is
    # missing or unparseable as a cp token. In W/D/L mode this stays
    # zero (the game result substitutes for any missing per-move
    # signal).
    cp_missing = 0
    # Slot layout: [count, label_total, repr_fen, game_id_list].
    # game_id_list collects every source game id that produced this
    # FEN (with multiplicity, so a position seen twice in one game
    # appears twice in the list). The tuner uses the list both for
    # per-game inverse weighting and for the by-game train / val
    # split.
    fen_stats: dict = {}
    # PGN files in the wild are a mix of UTF-8 (modern engine self-play
    # output) and latin-1 / windows-1252 (TWIC archives, historical
    # databases). Open with `errors="replace"` so a stray 0xa0 in a
    # comment or player name does not abort the whole extract; the
    # python-chess parser only cares about the ASCII syntax, so the
    # replacement codepoints in non-ASCII tags are harmless.
    with open(pgn_path, encoding="utf-8", errors="replace") as f_in:
        while True:
            game = chess.pgn.read_game(f_in)
            if game is None:
                break
            games += 1
            result = game.headers.get("Result", "*")
            # In W/D/L mode every position in a game inherits the game
            # outcome, so a game without a parseable result has no
            # signal at all and we drop it. In cp-label mode each
            # position carries its own per-move comment score, so a
            # missing game result is no longer a blocker.
            if not label_from_cp and result not in RESULT_MAP:
                skipped_games += 1
                continue
            game_result_label = RESULT_MAP.get(result)
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
                        # Pick the per-position label. cp-mode reads
                        # the engine comment for this move (which is
                        # the eval of the position before the move,
                        # i.e. the position we are labeling now), and
                        # drops the row if the comment is unusable.
                        # W/D/L mode just inherits the game outcome.
                        if label_from_cp:
                            cp = comment_cp(comment)
                            if cp is None:
                                cp_missing += 1
                                board.push(move)
                                continue
                            if board.turn == chess.BLACK:
                                cp = -cp
                            label = cp_to_label(cp, cp_scale)
                        else:
                            label = game_result_label
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
                      f"{mate_filtered} mate-filtered, "
                      f"{cp_missing} cp-missing",
                      file=sys.stderr)

    unique = len(fen_stats)
    with open(out_path, "w") as f_out:
        for _key, (count, total, repr_fen, game_ids) in fen_stats.items():
            ids_field = ",".join(str(g) for g in game_ids)
            f_out.write(f"{repr_fen} | {total / count} | {ids_field}\n")
    dup_pct = (100.0 * (raw_positions - unique) / raw_positions
               if raw_positions else 0.0)
    label_mode = "cp" if label_from_cp else "wdl"
    print(f"done: {games} games, {raw_positions} raw positions, "
          f"{unique} unique ({dup_pct:.1f}% folded), "
          f"{mate_filtered} mate-filtered, "
          f"{cp_missing} cp-missing, "
          f"{skipped_games} games skipped (no result), "
          f"label-mode={label_mode}",
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
    ap.add_argument("--label-from-cp", dest="label_from_cp",
                    action="store_true",
                    help="label each position by squashing the engine's "
                         "per-move cp score through a logistic instead of "
                         "inheriting the game W/D/L outcome. Positions whose "
                         "comment is missing or unparseable are dropped.")
    ap.add_argument("--cp-scale", dest="cp_scale", type=float, default=200.0,
                    help="cp value at which the logistic crosses 0.731 / "
                         "0.269 (default 200; matches the texel-tune K range)")
    ap.set_defaults(mate_filter=True, label_from_cp=False)
    args = ap.parse_args()
    extract(args.pgn, args.out, args.skip_plies, args.tail_plies,
            args.mate_filter, args.label_from_cp, args.cp_scale)


if __name__ == "__main__":
    main()
