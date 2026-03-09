# 🏆 HyperPack v11

**Multi-strategy compression algorithm that beats xz, bzip2, and gzip on standard benchmark corpora.**

HyperPack analyzes each data block and automatically selects the best compression strategy among BWT, LZMA, LZ77, LZP, Delta, Audio filters, and Context Mixing. A smart heuristic detects the optimal strategy in ~1ms, avoiding expensive brute-force trials.

## 📊 Results

### Global Benchmark — 3 Corpora (Silesia + Canterbury + Calgary)

| Compressor | Avg Ratio | Wins | Status |
|---|---|---|---|
| 🥇 **HyperPack v11** | **4.413x** | **12/38** | Champion |
| 🥈 xz -9 | 4.119x (-7%) | 9/38 | |
| 🥉 bzip2 -9 | 3.967x (-10%) | 15/38 | |
| 4 | gzip -9 | 3.063x (-31%) | 1/38 | |

### Silesia Corpus (212 MB, 12 files)

| Compressor | Compressed | Ratio | Speed |
|---|---|---|---|
| **HyperPack v11** | **~46.5 MB** | **~4.56x** | ~1.2 MB/s |
| xz -9 | 48.52 MB | 4.368x | 3.8 MB/s |
| bzip2 -9 | 54.07 MB | 3.921x | 12 MB/s |
| gzip -9 | 68.53 MB | 3.093x | 18 MB/s |

### Per-file comparison vs xz -9 (Silesia)

| File | Type | Size | HyperPack | xz -9 | Winner |
|---|---|---|---|---|---|
| dickens | English text | 9.7 MB | **4.04x** | 3.84x | 🟢 HP |
| mozilla | Binary (ELF) | 48.8 MB | **3.60x** | 3.52x | 🟢 HP |
| mr | Medical image | 9.5 MB | **4.23x** | 3.96x | 🟢 HP |
| nci | Chemical DB | 32.0 MB | **24.53x** | 18.44x | 🟢 HP |
| ooffice | Binary (DLL) | 5.9 MB | **2.42x** | 2.37x | 🟢 HP |
| osdb | Benchmark DB | 9.6 MB | **3.94x** | 3.93x | 🟢 HP |
| reymont | Polish text | 6.3 MB | **5.87x** | 5.42x | 🟢 HP |
| samba | Binary (ELF) | 20.6 MB | **5.12x** | 4.73x | 🟢 HP |
| sao | Star catalog | 6.9 MB | 1.56x | **1.59x** | 🔴 xz |
| webster | English dict | 39.5 MB | **5.74x** | 5.47x | 🟢 HP |
| xml | XML data | 5.1 MB | **12.12x** | 11.02x | 🟢 HP |
| x-ray | Medical image | 8.1 MB | **2.13x** | 2.06x | 🟢 HP |

**Score: HyperPack 11 — xz 1** (loses only on sao, a raw star catalog)

## ✨ What's New in v11 (vs v10.2)

### Phase 1 — Small Block Compression
- **LZMA forced on blocks < 1 MB** instead of being skipped by the heuristic
- **BCJ E8/E9 filter** for x86 executables (ELF/PE detection)
- Canterbury +2.1%, Calgary +1.3%, zero Silesia regressions

### Phase 2 — Parallel Compression
- **`-j N` flag** for parallel block compression
- Bit-identical output with 100% roundtrip verification
- ~1.7x speedup with `-j 4`

### Phase 3 — LZMA Speed Recovery
- **Adaptive LZMA chain depth** (128 → 32 in speculative mode)
- **Progressive early-exit** margin (4.7x cold → 1.14x warm)
- Recovered 80% of Phase 1 speed penalty

### Phase 4 — Text Detection + Hash Tables
- **Pure text detection**: skip LZMA on large ASCII text (>100KB, >95% ASCII, entropy < 5.5)
- **LZMA hash table enlarged** (1M → 4M entries, fewer collisions)
- **CM order-3 hash table enlarged** (1M → 4M entries)

### Cumulative Result (v10.2 → v11)
- **+1.4% compression ratio** globally
- **Only 1.16x slower** (was 2x in Phase 1)
- **Zero regressions** on any file

## 🔧 Building

### Requirements

- GCC (any recent version)
- POSIX threads (pthread)
- Math library (libm)

### Compile

```bash
gcc -O2 -o hyperpack src/hyperpack.c -lm -lpthread
```

That's it — single file, zero external dependencies.

## 🚀 Usage

```bash
# Compress
./hyperpack c input_file output.hp

# Compress with parallel threads
./hyperpack c -j 4 input_file output.hp

# Decompress  
./hyperpack d output.hp restored_file
```

## 🏗️ Architecture

HyperPack uses a multi-strategy approach:

```
Input Block
    ├── BCJ E8/E9 filter (x86 executables)
    ├── BWT + MTF-2 + ZRLE + Range Coder (Order 0/1)
    ├── LZ77
    ├── LZP + BWT
    ├── LZMA (adaptive chain depth + early-exit)
    ├── Delta + BWT (for structured data)
    └── Audio filter (for PCM data)
         │
         ▼
    Keep smallest result
```

### Smart Strategy Selection

Instead of trying all strategies on every block (expensive), HyperPack v11 uses a multi-level heuristic:

1. **Text detection (~0.1ms):** On blocks >100 KB, detects pure ASCII text (>95% ASCII, entropy < 5.5) → skip LZMA entirely, BWT always wins
2. **LZMA Heuristic (~1ms):** Analyzes entropy and ASCII percentage to decide whether to try LZMA
3. **Adaptive LZMA:** When LZMA must compete with BWT, uses shallow hash chains (32 vs 128) and progressive early-exit
4. **BCJ filter:** Detects ELF/PE executables and applies E8/E9 x86 filter before LZMA
5. **Sample-based selection:** Tests all remaining strategies on a 1 MB sample, then runs only the winner on the full block

## 📋 Version History

See [CHANGELOG.md](CHANGELOG.md) for the complete optimization history.

## 📄 License

See source file for license information.

## 📋 Base64 Compression

HyperPack handles Base64-encoded data surprisingly well. On text data, Base64 actually
compresses **better** than the binary original thanks to the reduced alphabet (64 chars)
that BWT exploits more efficiently.

| File | Binary → HP | Base64 → HP | Overhead |
|------|-------------|-------------|----------|
| xml (5.1 MB) | 441 KB (12.12×) | 433 KB (16.67×) | **−2%** 🟢 |
| dickens (9.7 MB) | 2,524 KB (4.04×) | 2,505 KB (5.50×) | **−1%** 🟢 |

See [docs/BASE64_TESTS.md](docs/BASE64_TESTS.md) for full analysis.

## 🗺️ Roadmap

| Priority | Improvement | Expected Gain | Status |
|----------|------------|---------------|--------|
| ⭐⭐⭐ | Neural context mixer + word model | +5-8% on text | Planned |
| ⭐⭐ | ANS entropy coder (replace Range Coder) | 2× decompression speed | Planned |

See [docs/ROADMAP.md](docs/ROADMAP.md) for detailed analysis.

## 🖥️ Multi-Platform App (Planned)

HyperPack will be available as:

| Platform | Technology | Status |
|----------|------------|--------|
| 🌐 **Web App** | WASM (Emscripten) — runs in browser, zero install | Planned |
| 🖥️ **Desktop** | Tauri (Rust + C FFI) — Win/Mac/Linux, ~5 MB | Planned |

See [docs/MULTIPLATFORM_APP.md](docs/MULTIPLATFORM_APP.md) for full architecture and roadmap.

## 🧪 Experimental Tests

13 optimization techniques were tested empirically. Only 3 provided improvements
(LZMA 64MB, MTF-2, Smart Heuristic — all already in v10.2).
The other 10 were harmful, redundant, or neutral.

See [docs/EXPERIMENTAL_TESTS.md](docs/EXPERIMENTAL_TESTS.md) for all 13 test results.
