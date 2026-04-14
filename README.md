# rlngin

<!-- BENCHMARK:START -->
## Latest Benchmark

| | |
|:--|:--|
| **Date** | 2026-04-14 |
| **Games** | 100 games (50 pairs) |
| **Time Control** | 10+0.1 |
| **Elo** | 552.08 +/- 134.87 |
| **Score** | 96.0 / 100 (96.00%) |
| **Record (W/D/L)** | 92 / 8 / 0 |
| **Draw Ratio** | 0.00% |
| **Pentanomial** | [0, 0, 0, 8, 42] |

<sub>Opening book: UHO_Lichess_4852_v1.epd</sub>
<!-- BENCHMARK:END -->

A chess engine, made with love.

## Building

```bash
make build
```

This compiles the engine to `build/rlngin`.

## Running

```bash
make run
```

Or run the binary directly:

```bash
./build/rlngin
```

The engine communicates over stdin/stdout using the [UCI protocol](https://www.chessprogramming.org/UCI). Example session:

```
uci
id name rlngin
id author Ryan Lefkowitz
uciok

isready
readyok

position startpos
go
bestmove a2a3

quit
```

## Testing

Run the full test suite:

```bash
make test
```

Tests use [Catch2](https://github.com/catchorg/Catch2) and include perft validation against known positions to verify move generation correctness.

## Formatting

Check formatting:

```bash
make format-check
```

Auto-format all source files:

```bash
make format
```

## Self-Play

Download [fastchess](https://github.com/Disservin/fastchess) and run self-play games:

```bash
make fetch-fastchess
make selfplay
```

Or run the script directly with a custom round count:

```bash
./scripts/selfplay.sh 50
```

Results are saved to `results/`.

## Cleaning

```bash
make clean
```

## License

[GPLv3](LICENSE)
