# Changelog

All notable changes to HyperPack are documented in this file.

## [v12] — 2026-03-10

### Speed — Parallel Decompression

- **Parallel Base64 V1 substreams**: S_BASE64 decompression now launches 2 concurrent pthreads — one for skeleton, one for decoded — then joins before reconstruction. Independent substreams with no data dependencies.
- **Parallel Base64 V2 substreams**: S_BASE64_V2 launches 3 concurrent pthreads (pos_tbl, skeleton, decoded simultaneously). Expected ~2-3× speedup for Base64-heavy files (e.g. classes.js).
- **Parallel block decompression**: `file_decompress` now processes blocks in batches of 4 (HP_PAR_THREADS). Non-dup blocks within a batch are decompressed simultaneously. DUP blocks handled sequentially after thread join. Measured **~2.9× speedup** on 22-block files, **~3× on 11-block files**.
- **b64_encode_chunk**: Changed char pointer lookup to `static const uint8_t[64]` array — avoids pointer indirection, improves icache behavior.

### Regression Results
- Calgary: 3.841× (no change)
- Silesia: 4.574× (no regression)
- Canterbury: 0 failures
- All roundtrip diffs pass

---

## [v11] — 2025-03-09

### Phase 1 — Small Block Compression
- **LZMA forced on blocks < 1 MB**: Previously skipped by the heuristic, now tested on all small blocks where LZMA can improve results
- **BCJ E8/E9 x86 filter**: Detects ELF (0x7F ELF) and PE (MZ) executables, applies E8/E9 call/jump address translation before LZMA compression
- Canterbury corpus: +2.1% ratio improvement
- Calgary corpus: +1.3% ratio improvement
- Zero Silesia regressions

### Phase 2 — Parallel Compression
- **`-j N` flag**: Compresses independent blocks in parallel using pthreads
- Bit-identical output regardless of thread count (deterministic)
- 100% decompression roundtrip verification
- Measured: 1.69x speedup with `-j 4` on large files

### Phase 3 — LZMA Speed Recovery
- **Adaptive LZMA chain depth**: Reduced from 128 to 32 when LZMA is in speculative mode (must beat BWT). Same quality for top matches, ~2x faster search
- **Progressive early-exit margin**: Starts at 4.7x tolerance (cold model), decreases to 1.14x as model warms up. Aborts unpromising LZMA trials early
- Result: Phase 1 slowdown reduced from 2.03x to 1.26x vs v10.2, with <0.5% ratio loss
- 17/18 tested files accelerated, ooffice highlight: 19.5s → 10.6s (1.85x speedup)

### Phase 4 — Text Detection + Hash Tables
- **Pure text detection**: On blocks >100 KB with >95% ASCII and entropy < 5.5, skip LZMA entirely (BWT always wins). Speed boost on literary text (book1: 47% faster)
- **LZMA hash table enlarged**: 20 → 22 bits (1M → 4M entries). Fewer collisions = better match finding on binary files (ooffice: +0.26%)
- **CM order-3 hash table enlarged**: 20 → 22 bits (1M → 4M entries). More context states for order-3 model

### Cumulative v10.2 → v11 Results
- **+1.4% compression ratio** globally across all corpora
- **Only 1.16x slower** than v10.2 (was 2.03x after Phase 1)
- **Zero regressions** on any tested file
- Beats xz -9 by 7%, bzip2 -9 by 10%, gzip -9 by 31% on average

---

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

The following approaches were tested and rejected during development:

| Test | Result | Why rejected |
|---|---|---|
| Context Mixing after BWT | -19% to -66% | CM loses to ZRLE+RC on BWT output |
| Order-4 Range Coder | -12% to -35% | Context dilution, worse than O1 |
| LZP longer context | No gain | LZP+BWT never beats BWT alone |
| Adaptive block size | No gain | All Silesia files fit in 128 MB |
| XML preprocessing | Skipped | BWT already achieves 12.29x |
| libdivsufsort | +0% on Silesia | Only helps on 80 MB+ text, adds dependency |
| Smaller BWT blocks | -11% ratio | BWT needs large blocks for long-range patterns |
| E8/E9 on BWT output | -1.5% to -19% | Harmful — but E8/E9 BEFORE LZMA works great |
| Transpose transform | -54% to -58% | Catastrophic — destroys BWT patterns |
| Bit-plane transform | -65% to -75% | Devastating — incompatible with BWT |
| CM O3 hash 4M entries | ~0% | CM not limited by hash collisions at this scale |
