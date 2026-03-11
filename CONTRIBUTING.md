# Contributing to HyperPack

Thank you for your interest in contributing to HyperPack! Here's how to get started.

## Ways to Contribute

- **Bug reports** — Open an issue with steps to reproduce
- **Benchmark results** — Share results on new data sets or platforms
- **Documentation** — Fix typos, improve clarity, add examples
- **Feature suggestions** — Open an issue describing the use case
- **Code** — Submit a pull request (see below)

## Development Setup

```bash
# Clone
git clone https://github.com/aciderix/HyperPack.git
cd HyperPack

# Build native
make

# Run tests
./hyperpack benchmark testfile.dat

# Build web app
cd hyperpack-web
npm install
npm run dev
```

### Requirements

- **C compiler**: gcc or clang
- **Libraries**: zlib, pthreads
- **Web**: Node.js 18+, npm
- **WASM**: Emscripten SDK (for `build-wasm.sh`)

## Pull Request Guidelines

1. **One feature per PR** — Keep changes focused
2. **Test compression ratios** — Run benchmarks on Silesia corpus before/after to catch regressions
3. **Don't break the format** — HPK5/HPK6 backward compatibility is critical
4. **Keep it in one file** — The C engine is intentionally monolithic; don't split it
5. **Comment non-obvious code** — Especially algorithm tweaks and magic constants

## Code Style

- C99 with POSIX threads
- 4-space indentation
- `snake_case` for functions and variables
- `UPPER_CASE` for constants and macros
- Descriptive function names: `compress_block_bwt_arith_o1()`

## Reporting Bugs

Please include:
- HyperPack version (`./hyperpack` with no args)
- Operating system and architecture
- Input file details (size, type)
- Expected vs actual behavior
- Full command line used

## Compression Experiments

If you want to experiment with new strategies or algorithms:

1. Run the **full Silesia benchmark** before your change
2. Make your change
3. Run the benchmark again
4. Include before/after numbers in your PR description

A strategy that improves some files but hurts others needs careful analysis.

## License

By contributing, you agree that your contributions will be licensed under the [MIT License](LICENSE).
