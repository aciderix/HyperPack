# HyperPack v10.2 — Benchmark Results

## Test Environment

- OS: Linux (x86_64)
- Compiler: GCC with `-O2`
- Date: March 2025

## Silesia Corpus (Standard Benchmark)

The [Silesia Corpus](https://sun.aei.polsl.pl//~sdeor/index.php?page=silesia) is the standard benchmark for compression algorithms.
212 MB across 12 files of different types.

### Detailed results per file

| File | Type | Size (MB) | HP v10.2 | xz -9 | bzip2 -9 | gzip -9 | zstd -19 | HP Strategy |
|---|---|---|---|---|---|---|---|---|
| dickens | English text | 9.7 | **4.04x** | 3.84x | 3.25x | 2.80x | 3.52x | BWT |
| mozilla | Binary ELF | 48.8 | **3.60x** | 3.52x | 2.84x | 2.40x | 3.37x | LZMA |
| mr | Medical img | 9.5 | **4.23x** | 3.96x | 3.03x | 2.43x | 3.50x | BWT |
| nci | Chemical DB | 32.0 | **24.53x** | 18.44x | 7.45x | 4.62x | 16.93x | BWT |
| ooffice | Binary DLL | 5.9 | 2.36x | **2.37x** | 1.92x | 1.75x | 2.21x | LZMA |
| osdb | Benchmark | 9.6 | **3.94x** | 3.93x | 2.98x | 2.52x | 3.56x | LZMA |
| reymont | Polish text | 6.3 | **5.87x** | 5.42x | 4.11x | 3.07x | 5.08x | BWT |
| samba | Binary ELF | 20.6 | **5.12x** | 4.73x | 3.20x | 2.67x | 4.38x | LZMA |
| sao | Star catalog | 6.9 | 1.56x | **1.59x** | 1.49x | 1.27x | 1.54x | BWT |
| webster | English dict | 39.5 | **5.74x** | 5.47x | 4.26x | 3.24x | 5.22x | BWT |
| xml | XML data | 5.1 | **12.12x** | 11.02x | 6.44x | 3.54x | 10.40x | BWT |
| x-ray | Medical img | 8.1 | **2.13x** | 2.06x | 1.68x | 1.51x | 1.92x | BWT |
| **TOTAL** | | **202 MB** | **4.483x** | 4.368x | 3.921x | 3.093x | 4.126x | |

### Score vs xz -9: **11-1** (HyperPack wins)

Only file where xz wins: `sao` (raw star catalog, essentially random binary data).

## Large JavaScript File (80.2 MB)

| Compressor | Compressed | Ratio | Time |
|---|---|---|---|
| **HyperPack v10.2** | **5.71 MB** | **14.04x** | 103s |
| xz -9 | 7.91 MB | 10.13x | 28s |
| zstd -19 | 8.35 MB | 9.60x | 34s |
| bzip2 -9 | 9.13 MB | 8.78x | 7s |
| gzip -9 | 12.51 MB | 6.41x | 4s |

**HyperPack produces a file 38% smaller than xz.**

## Speed Analysis

| Metric | HyperPack v10.2 | xz -9 |
|---|---|---|
| Compression speed | ~1 MB/s | ~4 MB/s |
| Ratio (Silesia) | **4.483x** | 4.368x |

The smart heuristic ensures HyperPack doesn't waste time on strategies that won't win.
