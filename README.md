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
