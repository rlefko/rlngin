#!/usr/bin/env python3
"""Build a curated EPD pack labelled by Stockfish for the Texel tuner.

The base self-play corpus (UHO_Lichess opening book) starts every game
past move 6, so early-opening positions where game-result + cp-label
Texel systematically misprices development credit never appear in
training. The most visible symptom is the Scandinavian
Blackburne-Kloosterboer 2...c6 the engine kept choosing at depth 8+
even after the cp-relabeled tune.

The curated EPD pack combines three sources:

1. **Hand-curated canaries** (`SEED_POSITIONS`): ~56 positions
   targeting known eval weaknesses (Scandinavian gambit, Englund,
   Latvian, Damiano, sound-gambit anchors, single-developed-piece
   calibrators, endgame textbook positions).

2. **Stockfish `Defaults` bench** (`SF_BENCH_QUIET`): hand-picked
   subset of `Stockfish/src/benchmark.cpp:Defaults`, filtered to
   eval-calibration-useful positions only. The mate / stalemate
   puzzle subsections are dropped because their cp labels saturate
   at +/- mate-value, which is uninformative for a static eval
   gradient. SF devs chose this set as their benchmark testbed, so
   it covers unusual motifs (color-complex weakness, fortress
   patterns) a random master sample would miss.

3. **Master corpus samples** (`tuning/val/master_positions.epd`):
   random samples bucketed by phase. The val corpus is master-game
   derived, so these ARE top-level theory at volume. Phase
   classification:
     - opening:    fullmove <= 10
     - middlegame: fullmove 11..25 and >= 3 non-pawn pieces per side
     - endgame:    fullmove >= 30 OR <= 4 non-pawn pieces total

Every position is labelled by running Stockfish on it (depth set by
``--depth``, syzygy probing on for endgame ground-truth) and squashed
through `1/(1+exp(-cp/SCALE))` to a [0, 1] label.

Output format is the same `FEN | label | gameIds` the C++ loader
parses (`tools/tune.cpp:loadDataset`); gameIds is left empty because
curated rows are loaded into training as standalone weight-N
positions, not aggregated by game.

Usage:
    python3 scripts/build_curated_epd.py
        [--out tuning/curated/openings.epd]
        [--depth 20]                 # Stockfish search depth per position
        [--cp-scale 200]             # logistic scale for cp -> label
        [--opening-count 700]
        [--middlegame-count 200]
        [--endgame-count 100]
        [--master-epd tuning/val/master_positions.epd]
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
import random
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

    # --- Single-developed-piece anchors (development-credit calibration).
    # Each of these has exactly one minor moved from its back rank;
    # otherwise the position is symmetric. SF should price the
    # advantage at the small "tempo" range (single-digit to low-30s cp),
    # which directly anchors the tuner on the right *magnitude* for one
    # developed piece - the failure mode the Scandinavian symptom was
    # diagnosing.
    ("rnbqkbnr/pppppppp/8/8/8/5N2/PPPPPPPP/RNBQKB1R b KQkq - 1 1",
     "1.Nf3 - one developed White knight, otherwise neutral"),
    ("rnbqkb1r/pppppppp/5n2/8/8/5N2/PPPPPPPP/RNBQKB1R w KQkq - 2 2",
     "1.Nf3 Nf6 - symmetric one developed knight each, should be ~0cp"),
    ("rnbqkbnr/pppppppp/8/8/8/2N5/PPPPPPPP/R1BQKBNR b KQkq - 1 1",
     "1.Nc3 - one developed knight, no central pawn, small edge"),
    ("rnbqkbnr/pppp1ppp/8/4p3/8/5N2/PPPPPPPP/RNBQKB1R b KQkq - 0 2",
     "1.Nf3 e5 - Black grabs center, one dev each, should be ~0cp"),
    ("rnbqkb1r/pppppppp/5n2/8/8/2N5/PPPPPPPP/R1BQKBNR w KQkq - 2 2",
     "1.Nc3 Nf6 - asymmetric development, should be ~0cp"),
    ("r1bqkbnr/pppppppp/2n5/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 1 2",
     "1.e4 Nc6 - Black single-dev, White has center, small edge"),
    ("rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
     "1.e4 e5 - classical anchor for the most-studied opening"),
    ("rnbqkb1r/pppp1ppp/5n2/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 2 3",
     "1.e4 e5 2.??? Nf6 - Black single-dev under tension"),

    # --- Broader early-opening anchors (move 1-3 lines the UHO book
    # never reaches). Each is a well-studied small-edge position. ---
    ("rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b KQkq - 0 1",
     "1.d4 - small edge, balanced opening"),
    ("rnbqkbnr/pppp1ppp/8/4p3/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2",
     "1.d4 e5 - Englund preview, white can transpose to mainline"),
    ("rnbqkbnr/ppp1pppp/8/3p4/3P4/8/PPP1PPPP/RNBQKBNR w KQkq - 0 2",
     "1.d4 d5 - classical Queens-pawn start"),
    ("rnbqkbnr/pppppppp/8/8/2P5/8/PP1PPPPP/RNBQKBNR b KQkq - 0 1",
     "1.c4 - English opening anchor"),
    ("rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
     "1.e4 c5 - Sicilian, the most-played reply to e4"),
    ("rnbqkbnr/pp1ppppp/2p5/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 0 2",
     "1.e4 c6 - Caro-Kann anchor"),
    ("rnbqkbnr/pp1ppppp/2p5/8/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2",
     "1.e4 c6 2.Nf3 - Caro-Kann two knights, balanced"),
    ("rnbqkbnr/pppppppp/8/8/8/1P6/P1PPPPPP/RNBQKBNR b KQkq - 0 1",
     "1.b3 - Larsen, hypermodern, small edge to side that handles it"),

    # --- Endgame anchors. The cp-relabeled corpus is dominated by
    # middlegame phase (mg=14-24); a few exact-truth endgame rows
    # keep the eg half of the eval honest. ---
    ("8/8/8/4k3/8/4K3/4P3/8 w - - 0 1",
     "KPK with the defender king blocking the pawn (opposition) - drawn"),
    ("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1",
     "KPK with the strong king escorting the e-pawn home - winning"),
    ("8/8/4k3/8/8/4B3/4K3/8 w - - 0 1",
     "KBK - drawn even with the bishop (canonical 0cp anchor)"),
    ("8/8/8/3kp3/3P4/8/4K3/8 w - - 0 1",
     "KPKP blockaded pawns - drawn"),
    ("8/8/8/3pk3/4Pp2/8/3K4/8 w - - 0 1",
     "KPKP opposite kings - drawn"),
    ("4k3/8/4K3/3B4/3N4/8/8/8 w - - 0 1",
     "KBNK - winning for the BN side (canonical eg anchor)"),
]


# Hand-picked subset of Stockfish/src/benchmark.cpp's `Defaults` array.
# We keep the diverse middlegame and endgame positions and explicitly
# drop the "Positions with high numbers of changed threats" speed-test
# block, the 5-/6-/7-man tactical puzzles, and the mate/stalemate test
# positions. Their cp labels would saturate at +/- mate-value and add
# no calibration signal to the static eval gradient.
SF_BENCH_QUIET = [
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 11",
    "4rrk1/pp1n3p/3q2pQ/2p1pb2/2PP4/2P3N1/P2B2PP/4RRK1 b - - 7 19",
    "r3r1k1/2p2ppp/p1p1bn2/8/1q2P3/2NPQN2/PPP3PP/R4RK1 b - - 2 15",
    "r1bbk1nr/pp3p1p/2n5/1N4p1/2Np1B2/8/PPP2PPP/2KR1B1R w kq - 0 13",
    "r1bq1rk1/ppp1nppp/4n3/3p3Q/3P4/1BP1B3/PP1N2PP/R4RK1 w - - 1 16",
    "4r1k1/r1q2ppp/ppp2n2/4P3/5Rb1/1N1BQ3/PPP3PP/R5K1 w - - 1 17",
    "2rqkb1r/ppp2p2/2npb1p1/1N1Nn2p/2P1PP2/8/PP2B1PP/R1BQK2R b KQ - 0 11",
    "r1bq1r1k/b1p1npp1/p2p3p/1p6/3PP3/1B2NN2/PP3PPP/R2Q1RK1 w - - 1 16",
    "3r1rk1/p5pp/bpp1pp2/8/q1PP1P2/b3P3/P2NQRPP/1R2B1K1 b - - 6 22",
    "r1q2rk1/2p1bppp/2Pp4/p6b/Q1PNp3/4B3/PP1R1PPP/2K4R w - - 2 18",
    "4k2r/1pb2ppp/1p2p3/1R1p4/3P4/2r1PN2/P4PPP/1R4K1 b - - 3 22",
    "3q2k1/pb3p1p/4pbp1/2r5/PpN2N2/1P2P2P/5PP1/Q2R2K1 b - - 4 26",
    "6k1/6p1/6Pp/ppp5/3pn2P/1P3K2/1PP2P2/3N4 b - - 0 1",
    "3b4/5kp1/1p1p1p1p/pP1PpP1P/P1P1P3/3KN3/8/8 w - - 0 1",
    "8/6pk/1p6/8/PP3p1p/5P2/4KP1q/3Q4 w - - 0 1",
    "7k/3p2pp/4q3/8/4Q3/5Kp1/P6b/8 w - - 0 1",
    "8/2p5/8/2kPKp1p/2p4P/2P5/3P4/8 w - - 0 1",
    "8/1p3pp1/7p/5P1P/2k3P1/8/2K2P2/8 w - - 0 1",
    "8/pp2r1k1/2p1p3/3pP2p/1P1P1P1P/P5KR/8/8 w - - 0 1",
    "8/3p4/p1bk3p/Pp6/1Kp1PpPp/2P2P1P/2P5/5B2 b - - 0 1",
    "6k1/6p1/P6p/r1N5/5p2/7P/1b3PP1/4R1K1 w - - 0 1",
    "1r3k2/4q3/2Pp3b/3Bp3/2Q2p2/1p1P2P1/1P2KP2/3N4 w - - 0 1",
    "6k1/4pp1p/3p2p1/P1pPb3/R7/1r2P1PP/3B1P2/6K1 w - - 0 1",
    "8/3p3B/5p2/5P2/p7/PP5b/k7/6K1 w - - 0 1",
    "5rk1/q6p/2p3bR/1pPp1rP1/1P1Pp3/P3B1Q1/1K3P2/R7 w - - 93 90",
    "4rrk1/1p1nq3/p7/2p1P1pp/3P2bp/3Q1Bn1/PPPB4/1K2R1NR w - - 40 21",
    "r3k2r/3nnpbp/q2pp1p1/p7/Pp1PPPP1/4BNN1/1P5P/R2Q1RK1 w kq - 0 16",
    "3Qb1k1/1r2ppb1/pN1n2q1/Pp1Pp1Pr/4P2p/4BP2/4B1R1/1R5K b - - 11 40",
    "4k3/3q1r2/1N2r1b1/3ppN2/2nPP3/1B1R2n1/2R1Q3/3K4 w - - 5 1",
    "1r6/1P4bk/3qr1p1/N6p/3pp2P/6R1/3Q1PP1/1R4K1 w - - 1 42",
]


# Stockfish bench `BenchmarkPositions` is 5 sequential master-game
# walks (309 FENs total). We take a stride sample (every 4th position)
# rather than the full set so the entries are less correlated; sampling
# every 4th from a 60-ply game yields ~15 positions per game = ~75
# total. Hand-listed here so the script is self-contained.
SF_BENCH_GAMES_STRIDED = [
    # Game 1 (sample every 4 plies)
    "rnbq1k1r/ppp1bppp/4pn2/8/2B5/2NP1N2/PPP2PPP/R1BQR1K1 b - - 2 8",
    "r1b2k1r/pp2bppp/2n1p3/2p5/2B1PB2/5N2/PPP2PPP/3RR1K1 b - - 0 12",
    "r1b1k2r/4bppp/p1n1p3/1pp5/P3PB2/2P2N2/1P2BPPP/3RR1K1 b - - 0 15",
    "1r2k2r/3bbppp/2n1p3/8/1pPNP3/2P5/3BBPPP/3RR1K1 b - - 3 20",
    "1r5r/3bk1pp/2n1pp2/1N6/2P1PP2/8/3RBKPP/4R3 b - - 2 26",
    "1r1R4/4k1pp/2b2p2/4p3/2P1PP2/6P1/4BK1P/R7 b - - 0 31",
    "8/3r2pp/2bk4/R1P1p3/4P3/6P1/4BK1P/8 b - - 0 35",
    "1k6/3r2pp/2b5/2P1p3/4P3/3BK1P1/7P/R7 b - - 8 39",
]


def cp_to_label(cp: int, scale: float) -> float:
    """Logistic squash matching extract_tuning_positions.py."""
    z = max(-50.0, min(50.0, cp / scale))
    return 1.0 / (1.0 + math.exp(-z))


def score_to_white_cp(score: chess.engine.PovScore) -> int:
    """Convert a python-chess PovScore to a White-perspective cp value.

    Both mate scores and tablebase-decisive cp signals are clamped to
    +/- 1500 so a single TB-win row does not dominate the curated
    gradient. Stockfish with Syzygy probing emits cp values in the
    tens of thousands for in-TB winning positions; left raw, those
    rows would saturate the logistic label to 1.0 and pull the
    tuner toward "everything that looks like a winning endgame is
    worth +1500 cp".
    """
    white = score.white()
    cp = 1500 if white.is_mate() and (white.mate() or 0) > 0 \
        else -1500 if white.is_mate() \
        else white.score(mate_score=1500)
    if cp is None:
        return 0
    if cp > 1500:
        return 1500
    if cp < -1500:
        return -1500
    return cp


def evaluate_position(engine: chess.engine.SimpleEngine, fen: str,
                      depth: int, time_cap_s: float) -> int:
    """Run Stockfish on a position; return White cp.

    Search budget is `depth` plies OR `time_cap_s` seconds, whichever
    hits first. The time cap is the safety valve: pathological
    endgame positions occasionally stall the engine inside Syzygy
    probing or extension-budget exhaustion at high depth, and without
    a cap a single bad position halts the whole curated build.
    """
    board = chess.Board(fen)
    info = engine.analyse(board, chess.engine.Limit(depth=depth, time=time_cap_s))
    return score_to_white_cp(info["score"])


def fen_key(fen: str) -> str:
    """First four FEN fields (board / side / castling / ep). Used as
    the dedup key: halfmove and fullmove counters vary between sources
    for the same position."""
    return " ".join(fen.split()[:4])


def classify_phase(fen: str) -> str:
    """Bucket a position into 'opening' / 'middlegame' / 'endgame'.

    Rules:
      opening:    fullmove <= 10
      endgame:    fullmove >= 30 OR <= 4 total non-pawn non-king pieces
      middlegame: everything else (i.e. fullmove 11..29 with >= 5
                  non-pawn pieces).
    """
    board = chess.Board(fen)
    fullmove = board.fullmove_number
    non_pawn = sum(
        len(board.pieces(pt, color))
        for pt in (chess.KNIGHT, chess.BISHOP, chess.ROOK, chess.QUEEN)
        for color in (chess.WHITE, chess.BLACK)
    )
    if fullmove <= 10:
        return "opening"
    if fullmove >= 30 or non_pawn <= 4:
        return "endgame"
    return "middlegame"


def sample_master_corpus(epd_path: Path, want_opening: int, want_mg: int,
                         want_eg: int, seed: int = 42):
    """Bucket master_positions.epd by phase and random-sample N from each.

    Returns three lists of FENs (opening, middlegame, endgame), in
    that order. If a bucket has fewer than the requested count, returns
    the whole bucket.
    """
    buckets: dict = {"opening": [], "middlegame": [], "endgame": []}
    with open(epd_path) as f:
        for line in f:
            sep = line.find("|")
            fen = (line[:sep] if sep != -1 else line).strip()
            if not fen:
                continue
            try:
                phase = classify_phase(fen)
            except Exception:
                # Skip malformed FENs rather than abort the whole run.
                continue
            buckets[phase].append(fen)
    rng = random.Random(seed)
    return (rng.sample(buckets["opening"], min(want_opening, len(buckets["opening"]))),
            rng.sample(buckets["middlegame"], min(want_mg, len(buckets["middlegame"]))),
            rng.sample(buckets["endgame"], min(want_eg, len(buckets["endgame"]))))


def gather_sources(master_epd: Path, opening_count: int, mg_count: int,
                   eg_count: int, seed: int):
    """Collect (fen, source_tag) tuples from all sources, deduped.

    Priority: hand-curated canaries > SF Defaults > SF strided games >
    master corpus samples. Earlier sources keep their slot when a
    later source produces a duplicate (4-field FEN key).

    Master-corpus sampling fills the REMAINDER after canaries +
    SF positions have been classified, so target counts are not
    inflated by overlap.
    """
    seen: dict = {}  # fen_key -> (fen, source_tag)
    canary_phases = {"opening": 0, "middlegame": 0, "endgame": 0}

    # 1. Hand-curated canaries (target the gambit / single-dev / eg
    #    anchor positions). Source-tag carries the human comment.
    for fen, comment in SEED_POSITIONS:
        key = fen_key(fen)
        if key in seen:
            continue
        seen[key] = (fen, f"canary: {comment}")
        try:
            canary_phases[classify_phase(fen)] += 1
        except Exception:
            pass

    # 2. SF Defaults (quiet, filtered).
    sf_phases = {"opening": 0, "middlegame": 0, "endgame": 0}
    for fen in SF_BENCH_QUIET + SF_BENCH_GAMES_STRIDED:
        key = fen_key(fen)
        if key in seen:
            continue
        seen[key] = (fen, "sf-bench")
        try:
            sf_phases[classify_phase(fen)] += 1
        except Exception:
            pass

    # 3. Master corpus, filling per-phase to the requested totals
    #    minus what canaries/SF already contributed for that phase.
    remaining_open = max(0, opening_count - canary_phases["opening"] - sf_phases["opening"])
    remaining_mg = max(0, mg_count - canary_phases["middlegame"] - sf_phases["middlegame"])
    remaining_eg = max(0, eg_count - canary_phases["endgame"] - sf_phases["endgame"])

    # Oversample 20% to absorb dedup loss.
    open_fens, mg_fens, eg_fens = sample_master_corpus(
        master_epd,
        int(remaining_open * 1.2) + 10,
        int(remaining_mg * 1.2) + 10,
        int(remaining_eg * 1.2) + 10,
        seed=seed,
    )

    def add(fen_list, tag, target):
        added = 0
        for fen in fen_list:
            if added >= target:
                break
            key = fen_key(fen)
            if key in seen:
                continue
            seen[key] = (fen, tag)
            added += 1
        return added

    added_open = add(open_fens, "master-opening", remaining_open)
    added_mg = add(mg_fens, "master-middlegame", remaining_mg)
    added_eg = add(eg_fens, "master-endgame", remaining_eg)

    print(f"sources: {len(SEED_POSITIONS)} canaries, "
          f"{len(SF_BENCH_QUIET) + len(SF_BENCH_GAMES_STRIDED)} SF-bench, "
          f"{added_open} master-open, {added_mg} master-mg, "
          f"{added_eg} master-eg = {len(seen)} unique total",
          file=sys.stderr)
    return list(seen.values())


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="tuning/curated/openings.epd",
                    help="output EPD path (default: tuning/curated/openings.epd)")
    ap.add_argument("--depth", type=int, default=20,
                    help="Stockfish search depth per position (default: 20)")
    ap.add_argument("--time-cap-s", type=float, default=30.0,
                    help="hard wall-clock cap per position in seconds "
                         "(default: 30; safety net against TB-probing stalls)")
    ap.add_argument("--cp-scale", type=float, default=200.0,
                    help="logistic scale for cp -> label (default: 200, "
                         "matches scripts/texel_extract.sh CP_SCALE)")
    ap.add_argument("--opening-count", type=int, default=700,
                    help="target opening positions (default: 700)")
    ap.add_argument("--middlegame-count", type=int, default=200,
                    help="target middlegame positions (default: 200)")
    ap.add_argument("--endgame-count", type=int, default=100,
                    help="target endgame positions (default: 100)")
    ap.add_argument("--master-epd", default="tuning/val/master_positions.epd",
                    help="master corpus path for sampling "
                         "(default: tuning/val/master_positions.epd)")
    ap.add_argument("--seed", type=int, default=42,
                    help="random seed for master sampling (default: 42)")
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
    master_path = Path(args.master_epd)
    if not master_path.exists():
        print(f"error: master corpus not found at {master_path}", file=sys.stderr)
        sys.exit(1)

    print(f"Stockfish: {sf_path}", file=sys.stderr)
    print(f"  Threads={args.threads}, Hash={args.hash_mb}MB, "
          f"SyzygyPath={syzygy}", file=sys.stderr)
    print(f"  depth={args.depth}, cp_scale={args.cp_scale}", file=sys.stderr)
    print(f"  targets: opening={args.opening_count}, "
          f"mg={args.middlegame_count}, eg={args.endgame_count}",
          file=sys.stderr)
    print(f"  master corpus: {master_path}", file=sys.stderr)
    print(f"  out: {out_path}", file=sys.stderr)

    positions = gather_sources(master_path, args.opening_count,
                               args.middlegame_count, args.endgame_count,
                               args.seed)
    total = len(positions)

    engine = chess.engine.SimpleEngine.popen_uci(sf_path)
    try:
        # Configure once; UCI options persist across analyse() calls.
        engine.configure({
            "Threads": args.threads,
            "Hash": args.hash_mb,
            "SyzygyPath": syzygy,
        })

        rows = []
        skipped = 0
        for i, (fen, tag) in enumerate(positions):
            try:
                cp = evaluate_position(engine, fen, args.depth, args.time_cap_s)
            except chess.engine.EngineTerminatedError:
                # Engine crashed (rare; some FENs trigger SF bugs).
                # Reopen and continue.
                print(f"  [{i + 1:>4}/{total}] SF terminated; reopening",
                      file=sys.stderr)
                engine = chess.engine.SimpleEngine.popen_uci(sf_path)
                engine.configure({
                    "Threads": args.threads,
                    "Hash": args.hash_mb,
                    "SyzygyPath": syzygy,
                })
                skipped += 1
                continue
            except Exception as e:
                # Includes EngineError, asyncio cancellations, etc.
                print(f"  [{i + 1:>4}/{total}] skipped ({e}): {fen}",
                      file=sys.stderr)
                skipped += 1
                continue
            label = cp_to_label(cp, args.cp_scale)
            rows.append((fen, label, cp, tag))
            # Sparse progress print so the log stays readable.
            if (i + 1) % 25 == 0 or (i + 1) == total:
                print(f"  [{i + 1:>4}/{total}] "
                      f"cp={cp:+5d} label={label:.4f}  {tag[:60]}",
                      file=sys.stderr)
    finally:
        engine.quit()

    with open(out_path, "w") as f:
        # Header comment so a reader knows what they're looking at.
        # Leading `#` lines are ignored by the C++ loader's `|` split
        # because they do not contain a `|`.
        f.write("# Curated opening / middlegame / endgame EPD pack\n")
        f.write("# Labelled by Stockfish for the Texel tuner.\n")
        f.write("# Generated by scripts/build_curated_epd.py.\n")
        f.write(f"# stockfish-depth={args.depth} cp-scale={args.cp_scale}\n")
        f.write(f"# rows={len(rows)}\n")
        for fen, label, cp, _tag in rows:
            f.write(f"{fen} | {label} | \n")

    print(f"done: wrote {len(rows)} rows to {out_path}", file=sys.stderr)


if __name__ == "__main__":
    main()
