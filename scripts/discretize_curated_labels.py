#!/usr/bin/env python3
"""Discretize the curated EPD's continuous sigmoid-of-cp labels into
W/D/L-shaped labels so the curated pack pulls the tuner in the same
shape as the W/D/L base corpus instead of injecting cp-shape gradient
on the rows it covers.

Each data row of the input EPD looks like:

    FEN | label | optional_gameIds_or_empty

where label is a float in [0, 1] from sigmoid(cp / cp_scale). After
discretization the label is mapped to one of {0.0, 0.5, 1.0}:

    label > 0.5 + margin   ->  1.0   (decisive win for White)
    label < 0.5 - margin   ->  0.0   (decisive loss for White)
    otherwise              ->  0.5   (drawish)

Default margin is 0.1, corresponding to roughly +/- 81 cp at the
cp_scale=200 the curated builder writes its labels at. That is a
reasonable "clear advantage vs balanced" boundary for an anti-gambit
anchor pack: positions chosen because chess truth says one side is
better get a decisive 0 or 1 label, calibration anchors close to
equal get 0.5.

Header lines starting with '#' are preserved verbatim except for the
trailing 'rows=' header which gets a discretized-from cp comment for
provenance.
"""
import argparse


def discretize(label: float, margin: float) -> float:
    if label > 0.5 + margin:
        return 1.0
    if label < 0.5 - margin:
        return 0.0
    return 0.5


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="in_path", required=True,
                    help="input EPD (sigmoid-of-cp labels)")
    ap.add_argument("--out", dest="out_path", required=True,
                    help="output EPD (W/D/L-discretized labels)")
    ap.add_argument("--margin", type=float, default=0.1,
                    help="distance from 0.5 for the decisive cutoff "
                         "(default 0.1, ~+/-81 cp at cp_scale=200)")
    args = ap.parse_args()

    win, draw, loss = 0, 0, 0
    header_written = False
    with open(args.in_path) as f_in, open(args.out_path, "w") as f_out:
        for line in f_in:
            line = line.rstrip("\n")
            if not line:
                f_out.write("\n")
                continue
            if line.startswith("#"):
                f_out.write(line + "\n")
                if not header_written:
                    f_out.write(f"# discretized to W/D/L at margin={args.margin}\n")
                    header_written = True
                continue
            parts = line.split(" | ")
            if len(parts) < 2:
                f_out.write(line + "\n")
                continue
            fen, label_str = parts[0], parts[1]
            rest = parts[2:] if len(parts) > 2 else []
            label = float(label_str)
            new_label = discretize(label, args.margin)
            if new_label > 0.5:
                win += 1
            elif new_label < 0.5:
                loss += 1
            else:
                draw += 1
            out_fields = [fen, f"{new_label}"] + rest
            f_out.write(" | ".join(out_fields) + "\n")

    print(f"discretized: {win} win, {draw} draw, {loss} loss")


if __name__ == "__main__":
    main()
