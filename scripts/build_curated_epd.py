#!/usr/bin/env python3
"""Build a curated EPD pack labelled by Stockfish for the Texel tuner.

The base self-play corpus (UHO_Lichess opening book) starts every game
past move 6, so early-opening positions where game-result + cp-label
Texel systematically misprices development credit never appear in
training. The most visible symptom is the Scandinavian
Blackburne-Kloosterboer 2...c6 the engine kept choosing at depth 8+
even after the cp-relabeled tune; SKIP_PLIES=0 did not help because
SKIP_PLIES counts from the book's starting FEN, not from game move 1.

This script runs Stockfish at a configurable depth over a hand-picked
seed list of early-opening positions covering:

- Dubious gambits the engine has been picking up (Scandinavian 2...c6,
  Englund, Latvian, Damiano, From's, Wing-accept, etc.).
- The continuations along those PVs (post-recapture leaves, queen-on-
  poisoned-pawn shapes) where the linear eval is most off.
- Sound gambits the eval should NOT discount (King's Gambit Accepted,
  Smith-Morra Sicilian, Benko, Marshall, etc.) so the curated weight
  does not over-correct.
- Quiet, well-known opening positions (Ruy Lopez, QGD, French
  Winawer) for calibration.

The Stockfish score is in side-to-move-relative centipawns; we convert
to White's perspective and squash through the same logistic the tuner
already expects (`cp_to_label` in `extract_tuning_positions.py`).
Output format is the same `FEN | label | gameIds` the C++ loader
parses; gameIds is left empty because curated rows are loaded into
training as standalone weight-N positions, not aggregated by game.

Usage:
    python3 scripts/build_curated_epd.py [--out tuning/curated/openings.epd]
                                          [--depth 20]
                                          [--cp-scale 150]
                                          [--threads 14]
                                          [--hash-mb 2048]
                                          [--syzygy-path ~/projects/chess/3-4-5/]

Environment:
    STOCKFISH: path to the Stockfish binary
              (default: ~/projects/chess/Stockfish/src/stockfish)
"""
import argparse
import math
import os
import sys
from pathlib import Path

import chess
import chess.engine


# Seed list: each entry is (FEN, comment). Comments are stripped before
# write; they exist so this list reads as chess, not hex.
#
# We deliberately mix three classes:
#   GAMBIT_REFUTED: SF prefers a clear material edge for the accepting
#     side. The HCE eval's failure here was the original Scandinavian
#     bug.
#   GAMBIT_SOUND: SF prices compensation in the standard pawn-or-half-
#     pawn range. Important counterweight so the tuner does not learn
#     "all gambit positions are bad for the sacrificer."
#   QUIET_CALIBRATION: well-known equal-ish opening positions, fitted
#     so the tuner has anchors for "this is a normal opening shape".
SEED_POSITIONS = [
    # --- Scandinavian Blackburne-Kloosterboer (the original symptom) ---
    ("rnbqkbnr/ppp1pppp/8/3P4/8/8/PPPP1PPP/RNBQKBNR b KQkq - 0 2",
     "1.e4 d5 2.exd5 - Black branch point (Qxd5/Nf6/c6)"),
    ("rnbqkbnr/pp2pppp/2p5/3P4/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 3",
     "1.e4 d5 2.exd5 c6 - White to move, should play dxc6"),
    ("rnbqkbnr/pp2pppp/2P5/8/8/8/PPPP1PPP/RNBQKBNR b KQkq - 0 3",
     "After 3.dxc6 - Black to recapture (Nxc6 main)"),
    ("r1bqkbnr/pp2pppp/2n5/8/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 4",
     "After 3.dxc6 Nxc6 - White +1 pawn, principled accept"),
    ("rnbqkbnr/pp2pppp/2p5/8/3P4/8/PPP2PPP/RNBQKBNR b KQkq - 0 3",
     "After 3.d4 (declining) - Black recovers cxd5"),
    ("rnbqkbnr/pp2pppp/8/3p4/3P4/8/PPP2PPP/RNBQKBNR w KQkq - 0 4",
     "After 3.d4 cxd5 - Caro-Kann-like, slightly White"),
    ("rnb1kbnr/ppp1pppp/8/3q4/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 3",
     "After 2...Qxd5 - main line, White to play Nc3"),
    ("rnb1kbnr/ppp1pppp/8/3q4/8/2N5/PPPP1PPP/R1BQKBNR b KQkq - 1 3",
     "After 2...Qxd5 3.Nc3 - queen kicked"),
    ("rnbqkbnr/ppp1pppp/5n2/3P4/8/8/PPPP1PPP/RNBQKBNR w KQkq - 1 3",
     "After 2...Nf6 - Marshall, White can play c4 or Bb5+ or d4"),
    ("rnbqkbnr/ppp1pppp/5n2/3P4/2P5/8/PP1P1PPP/RNBQKBNR b KQkq - 0 3",
     "After 2...Nf6 3.c4 - Marshall, Black plays c6"),
    # The c6 PV endpoint our engine reaches at depth 12-14 (Qxb2
    # poisoned pawn shape). SF should price this as clearly bad for
    # Black despite the material recovery.
    ("r3kbnr/pp2pppp/2n5/3p4/3P2b1/3BBN2/PqP2PPP/RN1Q1RK1 w kq - 0 8",
     "c6-PV endpoint: Black queen on b2 after our depth-12 PV"),

    # --- Other dubious early-game gambits (HCE-pattern hazards) ---
    ("rnbqkbnr/pppp1ppp/8/4P3/8/8/PPP1PPPP/RNBQKBNR b KQkq - 0 2",
     "Englund Gambit: 1.d4 e5 2.dxe5 - Black accepts gambit refutation"),
    ("r1bqkbnr/pppp1ppp/2n5/4P3/8/8/PPP1PPPP/RNBQKBNR w KQkq - 1 3",
     "Englund 2...Nc6 - White just keeps the pawn"),
    ("rnbqkbnr/pppp2pp/8/4pp2/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 3",
     "Latvian Gambit: 1.e4 e5 2.Nf3 f5 - White to move (SF: +1.0)"),
    ("rnbqkbnr/pppp1p1p/5p2/8/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 0 3",
     "Damiano Defense: 1.e4 e5 2.Nf3 f6 - 3.Nxe5 wins (SF: +2.0)"),
    ("rnbqkbnr/pppp1ppp/8/8/4pP2/8/PPPP2PP/RNBQKBNR w KQkq - 0 3",
     "From's Gambit accepted: 1.f4 e5 2.fxe5 d6 line"),
    ("rnbqkbnr/pp1ppppp/8/8/1pP5/8/PP1PPPPP/RNBQKBNR w KQkq - 0 3",
     "Sicilian Wing Gambit accepted: 1.e4 c5 2.b4 cxb4 - White's mistake"),

    # --- Sound gambits: compensation IS real per SF (counter weight) ---
    ("rnbqkbnr/pppp1ppp/8/4p3/4PP2/8/PPPP2PP/RNBQKBNR b KQkq - 0 2",
     "King's Gambit: 1.e4 e5 2.f4 - Black to play exf4 or decline"),
    ("rnbqkbnr/pppp1ppp/8/8/4Pp2/8/PPPP2PP/RNBQKBNR w KQkq - 0 3",
     "King's Gambit Accepted: 2...exf4 - White has real compensation"),
    ("rnbqkb1r/pp1ppppp/5n2/2pP4/8/8/PPP1PPPP/RNBQKBNR w KQkq - 0 3",
     "Benko Gambit setup: 1.d4 Nf6 2.c4 c5 3.d5 - pre b5"),
    ("rnbqkb1r/p2ppppp/5n2/1ppP4/8/8/PPP1PPPP/RNBQKBNR w KQkq - 0 4",
     "Benko Gambit: 3...b5 - sound positional sac"),
    ("r1bqk2r/pppp1ppp/2n2n2/4p3/1bB1P3/2N2N2/PPPP1PPP/R1BQK2R w KQkq - 6 6",
     "Two Knights Defense - balanced classical opening"),
    ("rnbqkb1r/pp2pppp/3p1n2/8/3NP3/8/PPP2PPP/RNBQKB1R w KQkq - 0 5",
     "Smith-Morra Sicilian declined - White has small edge"),

    # --- Quiet calibration anchors (well-known equal positions) ---
    ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
     "Startpos - 0.0 calibration anchor"),
    ("rnbqkb1r/pppp1ppp/5n2/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
     "Petroff Defense - balanced opening"),
    ("r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4",
     "Italian Game classical - balanced"),
    ("r1bqkb1r/pppp1ppp/2n2n2/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b KQkq - 4 4",
     "Ruy Lopez Berlin Defense - balanced"),
    ("rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR w KQkq - 0 4",
     "Queen's Gambit Declined - balanced"),
    ("rnbqkb1r/pp3ppp/2p1pn2/3p4/2PP4/2N1PN2/PP3PPP/R1BQKB1R b KQkq - 0 5",
     "Semi-Slav - balanced"),
    ("rnbqk2r/ppp1bppp/4pn2/3p4/2PP4/2N2N2/PP2PPPP/R1BQKB1R w KQkq - 4 5",
     "QGD Orthodox - balanced"),
    ("rnbqkbnr/pp3ppp/8/2pp4/3Pp3/2N1P3/PPP1NPPP/R1BQKB1R b KQkq - 1 5",
     "French Winawer-ish - small edge"),
    ("r1bqkbnr/pppp1ppp/2n5/4p3/3PP3/8/PPP2PPP/RNBQKBNR b KQkq - 0 3",
     "Center Game - slight White edge"),

    # --- Symmetric anchors so the tuner has 0-cp ground truth ---
    ("rnbqkb1r/pppp1ppp/5n2/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
     "Symmetric kingside development (repeat OK; canonical anchor)"),
    ("r1bqkb1r/pppp1ppp/2n2n2/4p3/4P3/2N2N2/PPPP1PPP/R1BQKB1R w KQkq - 6 5",
     "Four Knights symmetric - canonical 0cp anchor"),
]


def cp_to_label(cp: int, scale: float) -> float:
    """Logistic squash matching extract_tuning_positions.py."""
    z = max(-50.0, min(50.0, cp / scale))
    return 1.0 / (1.0 + math.exp(-z))


def score_to_white_cp(score: chess.engine.PovScore) -> int:
    """Convert a python-chess PovScore to a White-perspective cp value.

    Mate scores are clamped at +/- 1500 cp so the curated rows still
    carry a usable gradient signal (the tuner already drops literal
    mate-comment rows from self-play; here we choose to keep them at a
    saturated logistic value rather than skip).
    """
    white = score.white()
    if white.is_mate():
        mate = white.mate()
        return 1500 if (mate is not None and mate > 0) else -1500
    return white.score(mate_score=1500)


def evaluate_position(engine: chess.engine.SimpleEngine, fen: str,
                      depth: int) -> int:
    """Run Stockfish to a fixed depth on a position; return White cp."""
    board = chess.Board(fen)
    info = engine.analyse(board, chess.engine.Limit(depth=depth))
    return score_to_white_cp(info["score"])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="tuning/curated/openings.epd",
                    help="output EPD path (default: tuning/curated/openings.epd)")
    ap.add_argument("--depth", type=int, default=20,
                    help="Stockfish search depth per position (default: 20)")
    ap.add_argument("--cp-scale", type=float, default=150.0,
                    help="logistic scale for cp -> label (default: 150)")
    ap.add_argument("--threads", type=int, default=14,
                    help="Stockfish Threads option (default: 14)")
    ap.add_argument("--hash-mb", type=int, default=2048,
                    help="Stockfish Hash option, MB (default: 2048)")
    ap.add_argument("--syzygy-path", default=None,
                    help="Stockfish SyzygyPath option (default: $HOME/projects/chess/3-4-5)")
    ap.add_argument("--stockfish", default=None,
                    help="Stockfish binary path "
                         "(default: $STOCKFISH or $HOME/projects/chess/Stockfish/src/stockfish)")
    args = ap.parse_args()

    sf_path = (args.stockfish
               or os.environ.get("STOCKFISH")
               or str(Path.home() / "projects/chess/Stockfish/src/stockfish"))
    syzygy = (args.syzygy_path
              or str(Path.home() / "projects/chess/3-4-5"))

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    print(f"Stockfish: {sf_path}", file=sys.stderr)
    print(f"  Threads={args.threads}, Hash={args.hash_mb}MB, "
          f"SyzygyPath={syzygy}", file=sys.stderr)
    print(f"  depth={args.depth}, cp_scale={args.cp_scale}", file=sys.stderr)
    print(f"  out={out_path}", file=sys.stderr)
    print(f"  seed positions: {len(SEED_POSITIONS)}", file=sys.stderr)

    engine = chess.engine.SimpleEngine.popen_uci(sf_path)
    try:
        # Configure once; UCI options persist across analyse() calls.
        engine.configure({
            "Threads": args.threads,
            "Hash": args.hash_mb,
            "SyzygyPath": syzygy,
        })

        rows = []
        for i, (fen, comment) in enumerate(SEED_POSITIONS):
            cp = evaluate_position(engine, fen, args.depth)
            label = cp_to_label(cp, args.cp_scale)
            # gameIds column left empty (no comma) -- the C++ loader
            # accepts a trailing empty field as "no game ids".
            rows.append((fen, label, cp, comment))
            print(f"  [{i + 1:>2}/{len(SEED_POSITIONS)}] "
                  f"cp={cp:+5d} label={label:.4f}  {comment}",
                  file=sys.stderr)
    finally:
        engine.quit()

    with open(out_path, "w") as f:
        # Header comment so a reader knows what they're looking at.
        # Leading `#` lines are ignored by the C++ loader's `|` split
        # because they do not contain a `|`.
        f.write("# Curated opening EPD pack labelled by Stockfish for\n")
        f.write("# the Texel tuner. Generated by scripts/build_curated_epd.py.\n")
        f.write(f"# stockfish-depth={args.depth} cp-scale={args.cp_scale}\n")
        f.write(f"# rows={len(rows)}\n")
        for fen, label, cp, comment in rows:
            f.write(f"{fen} | {label} | \n")

    print(f"done: wrote {len(rows)} rows to {out_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
