# rlngin

<!-- BENCHMARK:START -->
## Latest Benchmark

![Elo](https://img.shields.io/static/v1?label=Elo&message=45.42%20%2B%2F-%2051.11&color=brightgreen) ![LOS](https://img.shields.io/static/v1?label=LOS&message=96.21%25&color=brightgreen) ![LLR](https://img.shields.io/static/v1?label=LLR&message=N%2FA&color=gray) ![W/D/L](https://img.shields.io/static/v1?label=W/D/L&message=39%20%2F%2035%20%2F%2026&color=lightgray) ![Score](https://img.shields.io/static/v1?label=Score&message=56.5%20%2F%20100%20%2856.50%25%29&color=blue) ![Draws](https://img.shields.io/static/v1?label=Draws&message=34.00%25&color=lightgray)

Ptnml(0-2): `[2, 10, 17, 15, 6]`
100 games (50 pairs) | tc=10+0.1 | UHO_Lichess_4852_v1.epd
<!-- BENCHMARK:END -->

A chess engine, made with love.

## Building

```bash
make build
```

This compiles the engine to `build/rlngin` with `-O3 -flto -DNDEBUG` and, by default, `-march=native -mtune=native` so the host CPU's extensions (BMI2/PEXT, POPCNT, AVX2, NEON, and friends) are all turned on.

### Architecture tiers

For reproducible distributable binaries, pick an explicit tier with `ARCH=`:

| Tier | Target |
|---|---|
| `native` (default) | Whatever the build host supports |
| `x86-64-v3` | Modern x86-64 baseline (Haswell/Zen2+): AVX2, BMI2, POPCNT |
| `x86-64` | Portable x86-64 with POPCNT and SSSE3 |
| `armv8` | Generic ARMv8-A (NEON is baseline) |
| `apple-silicon` | Tuned for Apple M-series cores |

```bash
make build ARCH=x86-64-v3
```

When the active tier supplies BMI2, the sliding-attack lookups compile to `_pext_u64` instead of the scalar magic multiply. If you are running on a Zen1 or Zen2 CPU, where PEXT is microcoded and slower than the fallback, force the scalar path with `NO_PEXT=1`.

Other knobs: `LTO=off` to disable link-time optimization, `DEBUG=on` to swap in `-O0 -g` with asserts enabled.

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

eval
 +---+---+---+---+---+---+---+---+
 ...
 Term          |    White     |    Black     |    Total
 ...
 Final:         0 internal (0 cp) from side to move

quit
```

Alongside the standard UCI commands, `eval` prints an ASCII board and a per-term static-evaluation breakdown for the current position.

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
