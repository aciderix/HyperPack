# Changelog

All notable changes to HyperPack are documented in this file.

## [v10.2] — 2025-03-08

### Added
- **Sample-based strategy selection**: Tests all strategies on a 1 MB sample, then runs only the winner on the full block. Saves ~10% on large files by skipping LZP and unnecessary Range Coder orders.
- `hint_strat` field in thread context for intra-thread strategy optimization.

### Performance
- Silesia: 4.481x (same as v10.1, speed +10% on BWT-heavy files)
- classes.js 80 MB: 14.04x in 103s (vs 110s in v10.1)

---

## [v10.1] — 2025-03-07

### Added
- **LZMA 64 MB threshold** (was 8 MB): Allows LZMA on large binaries where it excels.
  - mozilla: 2.88x → 3.60x (+25%)
  - samba: 4.44x → 5.12x (+15%)
- **MTF-2 (Move-To-Front variant)**: After BWT, moves repeated symbols to position 1 instead of 0. Small but consistent improvement on all BWT files (+0.39%).
- **Smart LZMA heuristic**: Predicts in ~1ms whether LZMA will outperform BWT using entropy and ASCII analysis.
  - Rule: Skip LZMA if `entropy < 5.0` OR `(ASCII% > 95% AND entropy < 6.0)`
  - Correctly skips 6/12 Silesia files (text files) with 0% ratio loss
  - 2.1x faster than brute-force v10.1

### Performance
- Silesia: 4.483x (vs 4.142x in v3.0, **+8.2%**)
- Beats xz -9 on 11/12 Silesia files (was 7/12)

---

## [v3.0] — Baseline

### Features
- Multi-strategy compressor: BWT, LZ77, LZMA, LZP, Delta, Audio
- Range Coder (Order 0/1/2) after BWT+MTF+ZRLE
- Context Mixing and PPM modes
- LZMA with 8 MB threshold
- Standard MTF (Move-To-Front)

### Performance
- Silesia: 4.142x
- Beats xz on 7/12 Silesia files

---

## Rejected Optimizations

The following approaches were tested and rejected during the v10.1/v10.2 development:

| Test | Result | Why rejected |
|---|---|---|
| Context Mixing after BWT | -19% to -66% | CM loses to ZRLE+RC on BWT output |
| Order-4 Range Coder | -12% to -35% | Context dilution, worse than O1 |
| LZP longer context | No gain | LZP+BWT never beats BWT alone |
| Adaptive block size | No gain | All Silesia files fit in 128 MB |
| XML preprocessing | Skipped | BWT already achieves 12.29x |
| libdivsufsort | +0% on Silesia | Only helps on 80 MB+ text, adds dependency |
| Smaller BWT blocks | -11% ratio | BWT needs large blocks for long-range patterns |
