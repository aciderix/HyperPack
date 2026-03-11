# Changelog

All notable changes to HyperPack are documented in this file.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [v12.1] — 2026-03-11

### Added — Manual Strategy Selection

- **`-s N` / `--strategy N`**: Force a single compression strategy, bypassing all auto-selection (no sampling, no trial compression). N = 0..30.
- **`-S N,N,...`**: Include filter — restrict auto-selection to only the listed strategies. The auto-selector still picks the best among them.
- **`-X N,N,...`**: Exclude filter — auto-select among all strategies except those listed.
- **`--list-strategies`**: Print all 31 strategies with their IDs and names.
- **LZMA heuristic override**: When LZMA is explicitly included via `-S`, internal heuristics (skip on large text, entropy checks) are bypassed.
- **Group-level skip optimization**: When no strategies from a group (BWT, Delta, LZ77) are in the allowed set, the entire group's preprocessing is skipped.
- **Library API**: New `hp_lib_compress_with_strategy()`, `hp_lib_compress_filtered()`, `hp_lib_archive_compress_with_strategy()`, `hp_lib_archive_compress_filtered()`, `hp_lib_num_strategies()`, `hp_lib_strategy_name()`.
- WASM builds unchanged (always auto, all strategies).

### Implementation Details

- `force_strategy` (`int`, -1 = auto) propagated through `compress_block()`, `file_compress()`, `archive_compress()`, and parallel workers.
- `allowed_mask` (`uint32_t`, bitmask) propagated through the same path. Default `0xFFFFFFFF` = all strategies.
- New `compress_block_forced()` function for single-strategy compression.
- Validation: out-of-range values produce clear error messages with `--list-strategies` hint.
- `-s`, `-S`, and `-X` are mutually compatible only as: `-s` alone, `-S` alone, `-X` alone. Combinations produce errors.

### Regressions

None. All existing behavior unchanged when no new options are used.

---

## [v12] — 2026-03-10

### Added — Parallel Decompression

- **Parallel Base64 V1 substreams**: S_BASE64 decompression now launches 2 concurrent threads — skeleton + decoded — then joins before reconstruction
- **Parallel Base64 V2 substreams**: S_BASE64_V2 launches 3 concurrent threads (pos_tbl, skeleton, decoded simultaneously). ~2–3× speedup on Base64-heavy files
- **Parallel block decompression**: Blocks processed in batches of 4 threads. Non-dup blocks within a batch are decompressed simultaneously. Measured **~2.9× speedup** on 22-block files, **~3× on 11-block files**
- **Optimized Base64 encoding**: `b64_encode_chunk` uses static lookup array instead of pointer indirection

### Added — Web & Desktop Platform

- WASM build via Emscripten — runs entirely in the browser
- React + TypeScript web app with drag-and-drop UI
- Tauri desktop app for Windows, macOS (ARM & Intel), Linux
- GitHub Actions CI/CD: build, test, deploy to GitHub Pages, multi-platform releases
- PWA support with service worker for offline use

### Regressions

None. All corpora ratios unchanged, all roundtrip tests pass.

---

## [v11] — 2026-03-09

### Added — Small Block Compression (Phase 1)

- **LZMA forced on blocks < 1 MB**: Previously skipped by heuristic, now tested on all small blocks
- **BCJ E8/E9 x86 filter**: Detects ELF and PE executables, applies call/jump address translation before LZMA
- Canterbury: +2.1% ratio | Calgary: +1.3% | Zero Silesia regressions

### Added — Parallel Compression (Phase 2)

- **`-j N` flag**: Compresses independent blocks in parallel using pthreads
- Bit-identical output regardless of thread count (deterministic)
- Measured: 1.69× speedup with `-j 4`

### Improved — LZMA Speed Recovery (Phase 3)

- **Adaptive LZMA chain depth**: 128 → 32 in speculative mode (~2× faster search)
- **Progressive early-exit**: Tolerance starts at 4.7× (cold model), decreases to 1.14× as model warms
- Phase 1 slowdown reduced from 2.03× to 1.26× vs v10.2, with <0.5% ratio loss

### Improved — Text Detection (Phase 4)

- **Pure text detection**: Blocks >100 KB with >95% ASCII and entropy < 5.5 skip LZMA entirely
- **Enlarged hash tables**: LZMA 20→22 bits (1M→4M entries), CM O3 20→22 bits

### Summary

- **+1.4% compression ratio** globally
- **Only 1.16× slower** than v10.2
- **Zero regressions** on any file

---

## [v10.2] — 2026-03-08

### Added

- **Sample-based strategy selection**: Tests strategies on 1 MB sample, runs only the winner on full block. ~10% speedup on BWT-heavy files.

---

## [v10.1] — 2026-03-07

### Added

- **LZMA 64 MB threshold** (was 8 MB): mozilla 2.88×→3.60× (+25%), samba 4.44×→5.12× (+15%)
- **MTF-2 variant**: Moves repeated symbols to position 1 instead of 0. +0.39% on all BWT files
- **Smart LZMA heuristic**: Predicts in ~1ms whether LZMA will beat BWT. Correctly skips text files with 0% ratio loss

---

## [v3.0] — Baseline

### Features

- Multi-strategy compressor: BWT, LZ77, LZMA, LZP, Delta, Audio, PPM, CM
- Range Coder (Order 0/1/2) + Context Mixing
- Silesia: 4.142× — beats xz on 7/12 files
