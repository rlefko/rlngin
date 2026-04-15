# rlngin

<!-- BENCHMARK:START -->
## Latest Benchmark

![Elo](https://img.shields.io/static/v1?label=Elo&message=119.11%20%2B%2F-%2063.58&color=brightgreen) ![LOS](https://img.shields.io/static/v1?label=LOS&message=100.00%25&color=brightgreen) ![LLR](https://img.shields.io/static/v1?label=LLR&message=N%2FA&color=gray) ![W/D/L](https://img.shields.io/static/v1?label=W/D/L&message=58%20%2F%2017%20%2F%2025&color=lightgray) ![Score](https://img.shields.io/static/v1?label=Score&message=66.5%20%2F%20100%20%2866.50%25%29&color=blue) ![Draws](https://img.shields.io/static/v1?label=Draws&message=30.00%25&color=lightgray)

Ptnml(0-2): `[3, 4, 15, 13, 15]`
100 games (50 pairs) | tc=10+0.1 | UHO_Lichess_4852_v1.epd
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
