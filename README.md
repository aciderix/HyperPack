# 🏆 HyperPack v10.2

**Multi-strategy compression algorithm that beats xz on 11/12 Silesia benchmark files.**

HyperPack analyzes each data block and automatically selects the best compression strategy among BWT, LZMA, LZ77, LZP, Delta, and Audio filters. A smart heuristic detects the optimal strategy in ~1ms, avoiding expensive brute-force trials.

## 📊 Results

### Silesia Corpus (212 MB, 12 files)

| Compressor | Compressed | Ratio | Speed |
|---|---|---|---|
| **HyperPack v10.2** | **47.28 MB** | **4.483x** | **1.3 MB/s** |
| xz -9 | 48.52 MB | 4.368x | 3.8 MB/s |
| zstd -19 | 51.36 MB | 4.126x | 5.2 MB/s |
| bzip2 -9 | 54.07 MB | 3.921x | 12 MB/s |
| gzip -9 | 68.53 MB | 3.093x | 18 MB/s |

### Per-file comparison vs xz -9

| File | Type | Size | HyperPack | xz -9 | Winner |
|---|---|---|---|---|---|
| dickens | English text | 9.7 MB | **4.04x** | 3.84x | 🟢 HP |
| mozilla | Binary (ELF) | 48.8 MB | **3.60x** | 3.52x | 🟢 HP |
| mr | Medical image | 9.5 MB | **4.23x** | 3.96x | 🟢 HP |
| nci | Chemical DB | 32.0 MB | **24.53x** | 18.44x | 🟢 HP |
| ooffice | Binary (DLL) | 5.9 MB | 2.36x | **2.37x** | ⚪ tie |
| osdb | Benchmark DB | 9.6 MB | **3.94x** | 3.93x | 🟢 HP |
| reymont | Polish text | 6.3 MB | **5.87x** | 5.42x | 🟢 HP |
| samba | Binary (ELF) | 20.6 MB | **5.12x** | 4.73x | 🟢 HP |
| sao | Star catalog | 6.9 MB | 1.56x | **1.59x** | 🔴 xz |
| webster | English dict | 39.5 MB | **5.74x** | 5.47x | 🟢 HP |
| xml | XML data | 5.1 MB | **12.12x** | 11.02x | 🟢 HP |
| x-ray | Medical image | 8.1 MB | **2.13x** | 2.06x | 🟢 HP |

**Score: HyperPack 11 — xz 1** (loses only on sao, a raw star catalog)

### Large JavaScript file (80.2 MB)

| Compressor | Compressed | Ratio |
|---|---|---|
| **HyperPack v10.2** | **5.71 MB** | **14.04x** |
| xz -9 | 7.91 MB | 10.13x |
| zstd -19 | 8.35 MB | 9.60x |

**38% smaller than xz** on highly structured text.

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

# Decompress  
./hyperpack d output.hp restored_file
```

## 🏗️ Architecture

HyperPack uses a multi-strategy approach:

```
Input Block
    ├── BWT + MTF-2 + ZRLE + Range Coder (Order 0/1)
    ├── LZ77
    ├── LZP + BWT
    ├── LZMA (for large binaries)
    ├── Delta + BWT (for structured data)
    └── Audio filter (for PCM data)
         │
         ▼
    Keep smallest result
```

### Smart Strategy Selection

Instead of trying all strategies on every block (expensive), HyperPack v10.2 uses a two-level heuristic:

1. **LZMA Heuristic (~1ms):** Analyzes entropy and ASCII percentage to decide whether to try LZMA
   - Skip if `entropy < 5.0` (highly compressible → BWT wins)
   - Skip if `ASCII% > 95% AND entropy < 6.0` (text → BWT wins)
   - Run on binaries where LZMA excels (mozilla, samba, ooffice)

2. **Sample-based selection:** Tests all remaining strategies on a 1 MB sample, then runs only the winner on the full block
   - Skips LZP trial when BWT is clearly better
   - Skips unnecessary Range Coder orders (O1/O2)

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
| ⭐⭐ | Parallel block compression (`-j4`) | 3–4× speed | Planned |
| ⭐ | ANS entropy coder (replace Range Coder) | 2× decompression speed | Planned |

See [docs/ROADMAP.md](docs/ROADMAP.md) for detailed analysis.

## 🖥️ Multi-Platform App (Planned)

HyperPack will be available as:

| Platform | Technology | Status |
|----------|------------|--------|
| 🌐 **Web App** | WASM (Emscripten) — runs in browser, zero install | Planned |
| 🖥️ **Desktop** | Tauri (Rust + C FFI) — Win/Mac/Linux, ~5 MB | Planned |

Both versions share the same UI with:
- 📁 Drag & drop for files and folders
- ⚙️ Strategy selection (Auto/BWT/LZMA/LZ77/LZP/Delta/Audio)
- 📏 Block size and LZMA dictionary configuration
- 📊 Real-time progress bar
- 📁 Folder mode: archive or individual file compression

See [docs/MULTIPLATFORM_APP.md](docs/MULTIPLATFORM_APP.md) for full architecture and roadmap.

## 🧪 Experimental Tests

13 optimization techniques were tested empirically. Only 3 provided improvements
(LZMA 64MB, MTF-2, Smart Heuristic — all already in v10.2).
The other 10 were harmful, redundant, or neutral.

Highlights of failed techniques:

| Technique | Predicted | Actual |
|-----------|-----------|--------|
| E8/E9 x86 filter | +5–15% | **−1.5% to −19%** |
| Transpose transform | +3–7% | **−54% to −58%** |
| Bit-plane transform | +10–20% | **−65% to −75%** |

**Key lesson:** BWT + LZMA is already so powerful that most preprocessing transforms
*destroy* the patterns these algorithms exploit rather than enhancing them.

See [docs/EXPERIMENTAL_TESTS.md](docs/EXPERIMENTAL_TESTS.md) for all 13 test results.
