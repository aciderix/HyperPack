# HyperPack — Technical Documentation

Comprehensive technical reference for HyperPack v12, the lossless multi-strategy data compressor.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Compression Pipeline](#2-compression-pipeline)
3. [Strategies](#3-strategies)
4. [Algorithm Components](#4-algorithm-components)
5. [Strategy Selection (Heuristic)](#5-strategy-selection-heuristic)
6. [File Formats](#6-file-formats)
7. [Parallel Compression](#7-parallel-compression)
8. [CLI Reference](#8-cli-reference)
9. [Building](#9-building)

---

## 1. Overview

HyperPack is a lossless data compressor written in pure C (~7,000 lines in a single file, `src/hyperpack.c`). Its core innovation is **automatic per-block strategy selection**: each block of input data is tested against up to 31 different compression strategies, and the one producing the smallest output is retained.

This "intelligent brute-force" approach allows HyperPack to outperform general-purpose compressors (gzip, bzip2, xz, zstd) on heterogeneous data by adapting its algorithm to the content of each block independently.

### Key Characteristics

- **Zero external dependencies** (except zlib for PNG pre-transform)
- **31 compression strategies** selected automatically per block
- **2 file formats**: HPK5 (single file) and HPK6 (multi-file archive)
- **Intelligent pre-transforms**: PNG (inflate IDAT), Base64, Audio PCM, Float32 IEEE 754
- **Block-level deduplication** (FNV-1a hash) and file-level deduplication (CRC32)
- **Parallel compression** via pthreads with configurable thread count
- **CRC32 integrity** verified per block on decompression
- **3 build targets**: CLI (Linux/macOS/Windows), Web (WASM), Desktop (Tauri)

### Source Architecture

```
src/
├── hyperpack.c          # ~7,000 lines — complete compression engine
│                        # (BWT, MTF, ZRLE, Arith O0-O2, PPM, LZMA, LZ77,
│                        #  rANS, CM, Audio, Base64, PNG, Archive, Main)
├── hyperpack_wasm.c     # ~90 lines — WASM wrapper (Emscripten + MEMFS)
└── hyperpack_lib.c      # ~65 lines — Library wrapper (Tauri FFI)
```

Conditional compilation macros control the build target:

| Macro | Target | Differences |
|-------|--------|-------------|
| *(none)* | **CLI** | `main()` function, pthreads, direct filesystem access |
| `HYPERPACK_WASM` | **Web** | API via `EMSCRIPTEN_KEEPALIVE`, no intra-block threads, MEMFS |
| `HYPERPACK_LIB` | **Tauri** | `hp_lib_*()` API, pthreads, native file access |

---

## 2. Compression Pipeline

### Full Pipeline Overview

```
Input File
    │
    ▼
┌─────────────────────────┐
│ Pre-Transform            │  PNG → inflate IDAT (replaces deflate with HP)
│ (auto-detection)         │  If result > original → fallback without PT
└─────────┬───────────────┘
          │
          ▼
┌─────────────────────────┐
│ Block Splitting          │  Configurable size: 1–128 MB (default: 128 MB)
│ (block_size)             │
└─────────┬───────────────┘
          │
    ┌─────┴─────┐
    ▼           ▼
┌────────┐ ┌────────┐
│ Block 1│ │ Block 2│ ...  (parallelizable with -j N)
└───┬────┘ └───┬────┘
    │           │
    ▼           ▼
┌─────────────────────────┐
│ Strategy Selection       │  For each block:
│ compress_block()         │
│                          │  1. Entropy check → skip if > 7.95 bits/byte
│                          │  2. If block > 2 MB: test 1 MB sample → hint
│                          │  3. Two parallel strategy groups:
│                          │     Group AC  : BWT, LZP, PPM, rANS, CTX2
│                          │     Group BDE : Delta, LZ77, Float, LZMA
│                          │  4. Select smallest result
│                          │  5. If no strategy < original → STORE
└─────────┬───────────────┘
          │
          ▼
┌─────────────────────────┐
│ Deduplication            │  FNV-1a hash per block → reference if identical
└─────────┬───────────────┘
          │
          ▼
┌─────────────────────────┐
│ Write .hpk File          │  Header + compressed blocks + CRC32 per block
└─────────────────────────┘
```

### Pipeline Stages

1. **Pre-Transform** — Content-specific preprocessing applied before compression. PNG files have their IDAT chunks inflated so HyperPack's algorithms can replace deflate. If the pre-transform increases file size, it falls back to raw compression automatically.

2. **Block Splitting** — Input data is divided into fixed-size blocks. Each block is compressed independently, enabling parallelism and per-block strategy selection.

3. **Strategy Selection** — The core of the engine. Each block goes through entropy checking, optional sample-based hinting, and parallel trial compression across all candidate strategies. The smallest output wins.

4. **Deduplication** — Identical blocks are detected via FNV-1a hashing and stored as references, avoiding redundant compression.

5. **Output** — Blocks are written sequentially with their strategy ID, compressed size, original size, CRC32, and compressed data.

---

## 3. Strategies

HyperPack implements 31 compression strategies (IDs 0–30), organized into families based on their primary algorithm.

### Complete Strategy Table

#### BWT-Based Strategies

| ID | Name | Pipeline | Optimal Data Type |
|----|------|----------|-------------------|
| 0 | `STORE` | Raw copy (no compression) | Incompressible data (entropy > 7.95) |
| 1 | `BWT+O0` | BWT → MTF → ZRLE → Arithmetic O0 | Simple text, uniform data |
| 2 | `BWT+O1` | BWT → MTF → ZRLE → Arithmetic O1 | Natural text, prose, code |
| 12 | `BWT+O2` | BWT → MTF → ZRLE → Arithmetic O2 | Structured/tabular data (CSV, XML) |
| 27 | `BWT+O0PS` | BWT → MTF → ZRLE → Arithmetic O0 (pre-scanned) | Cold-start fix for short blocks |
| 30 | `BWT+O1PS` | BWT → MTF → ZRLE → Arithmetic O1 (warm-start) | Source code, repetitive keywords |
| 28 | `BWT+rANS` | BWT → MTF → ZRLE → rANS O0 | Fast-decode scenarios, similar ratio to O0PS |
| 29 | `BWT+CTX2` | BWT → MTF → ZRLE → Range Coder 2-context | Data with distinct run vs literal patterns |

#### LZP-Based Strategies

| ID | Name | Pipeline | Optimal Data Type |
|----|------|----------|-------------------|
| 5 | `LZP+BWT+O0` | LZP → BWT → MTF → ZRLE → Arithmetic O0 | Data with long-range repeats |
| 6 | `LZP+BWT+O1` | LZP → BWT → MTF → ZRLE → Arithmetic O1 | Source code with repetitive patterns |
| 14 | `LZP+BWT+O2` | LZP → BWT → MTF → ZRLE → Arithmetic O2 | Structured data with repeats |

#### Delta-Based Strategies

| ID | Name | Pipeline | Optimal Data Type |
|----|------|----------|-------------------|
| 3 | `D+BWT+O0` | Delta → BWT → MTF → ZRLE → Arithmetic O0 | Slowly varying numeric data |
| 4 | `D+BWT+O1` | Delta → BWT → MTF → ZRLE → Arithmetic O1 | Sensor data, numeric columns |
| 13 | `D+BWT+O2` | Delta → BWT → MTF → ZRLE → Arithmetic O2 | Highly structured columnar data |
| 7 | `D+LZP+BWT+O0` | Delta → LZP → BWT → MTF → ZRLE → Arithmetic O0 | Delta + long-range repeats |
| 8 | `D+LZP+BWT+O1` | Delta → LZP → BWT → MTF → ZRLE → Arithmetic O1 | Delta + repetitive structured data |
| 15 | `D+LZP+BWT+O2` | Delta → LZP → BWT → MTF → ZRLE → Arithmetic O2 | Best for structured numeric data |

#### LZMA-Based Strategies

| ID | Name | Pipeline | Optimal Data Type |
|----|------|----------|-------------------|
| 24 | `LZMA` | LZMA with optimal parsing | General binary, executables |
| 25 | `BCJ+LZMA` | x86 BCJ filter → LZMA | x86 executables (ELF, PE) |

#### LZ77-Based Strategies

| ID | Name | Pipeline | Optimal Data Type |
|----|------|----------|-------------------|
| 22 | `LZ77+BWT+O0` | LZ77 → BWT → MTF → ZRLE → Arithmetic O0 | Data with many local repeats |
| 23 | `LZ77+BWT+O1` | LZ77 → BWT → MTF → ZRLE → Arithmetic O1 | Local repeats + text-like data |

#### PPM-Based Strategies

| ID | Name | Pipeline | Optimal Data Type |
|----|------|----------|-------------------|
| 16 | `PPM` | PPMC orders 0–3 direct | Moderate-context text |
| 17 | `BWT+PPM` | BWT → PPM | BWT-transformed data with context |
| 18 | `BWT+MTF+PPM` | BWT → MTF → PPM | BWT + MTF-transformed data |

#### Context Mixing Strategies

| ID | Name | Pipeline | Optimal Data Type |
|----|------|----------|-------------------|
| 9 | `CM` | Context Mixing direct | General (rarely selected) |
| 10 | `BWT+CM` | BWT → Context Mixing | BWT output with complex patterns |
| 11 | `BWT+MTF+CM` | BWT → MTF → Context Mixing | BWT + MTF with complex patterns |

#### Specialized Strategies

| ID | Name | Pipeline | Optimal Data Type |
|----|------|----------|-------------------|
| 19 | `Audio` | PCM detection → channel deinterleave → delta | Uncompressed audio (WAV PCM) |
| 20 | `Base64` | Base64 detection → decode → split compress v1 | Data containing base64 segments |
| 21 | `Base64v2` | Base64 detection → 3-stream parallel split | Data with base64 (parallel decode) |
| 26 | `F32XOR+BWT` | Float32 XOR-delta → BWT + Range Coder | IEEE 754 float32 arrays |

### Strategy Selection by Data Type

| Data Type | Typically Winning Strategies |
|-----------|------------------------------|
| Natural text (prose, documentation) | BWT+O1, BWT+O1PS, BWT+O2 |
| Source code | BWT+O1PS, LZP+BWT+O1 |
| Tabular data (CSV, XML, JSON) | BWT+O2, LZP+BWT+O2 |
| x86 executables | BCJ+LZMA, LZMA |
| PNG images | PNG pre-transform → BWT+O1 |
| Uncompressed audio (WAV) | Audio (deinterleave + delta) |
| Files with embedded base64 | Base64v2 (3-stream split) |
| Floating-point arrays | F32XOR+BWT |
| Structured numeric columns | D+BWT+O1, D+LZP+BWT+O2 |
| Random / incompressible data | STORE (raw copy) |

---

## 4. Algorithm Components

### 4.1 BWT (Burrows-Wheeler Transform)

The BWT rearranges input bytes so that characters occurring in similar contexts are grouped together, dramatically increasing compressibility for subsequent stages.

- **Implementation**: Pure C with suffix array-based sorting
- **Output**: Permuted data + primary index (`pidx`)
- **Workspace**: Pre-allocated `BWTWorkspace` struct reused across blocks (no per-block malloc/free)
- **Role**: Foundation of 20+ strategies; groups similar contexts for MTF/entropy coding

### 4.2 MTF (Move-to-Front Transform)

Converts BWT output into a sequence where recently seen symbols receive low numeric values, concentrating the distribution toward zero.

- **Variants**: Standard MTF, MTF-1, and MTF-2
- **Effect**: After BWT, the output is dominated by low values (especially zeros), which ZRLE and arithmetic coding exploit efficiently

### 4.3 ZRLE (Zero Run-Length Encoding)

Encodes runs of zeros (extremely frequent after MTF) using special 16-bit symbols, significantly reducing data size before entropy coding.

- **Alphabet**: 258 symbols — RUNA (0), RUNB (1), and byte+2 for literals
- **Encoding**: Binary run-length for zero runs using RUNA/RUNB pair coding
- **Effect**: Collapses long zero runs into compact representations

### 4.4 Arithmetic Coding (Order 0, 1, 2)

Entropy coding with adaptive probability models at three context depths:

| Variant | Context | Models | Description |
|---------|---------|--------|-------------|
| **O0** | None | 1 global model | Frequency-based, fastest |
| **O0_PS** | None (pre-scanned) | 1 model with pre-computed frequencies | Fixes cold-start problem; 258-byte overhead |
| **O1** | 1 previous byte | 256 models | One model per preceding symbol |
| **O1_PS** | 1 byte (warm-start) | 256 models with global freq init | Better prior than uniform initialization |
| **O2** | 2 previous bytes | 65,536 models | Highest context, best for structured data |

Implementation: 32-bit range coder with normalization and adaptive rescaling.

### 4.5 rANS (Range Asymmetric Numeral Systems)

An alternative entropy codec to arithmetic coding, used as strategy `S_BWT_RANS` (ID 28).

- **Advantage**: O(1) per-symbol decoding via lookup tables
- **Ratio**: Comparable to O0_PS
- **Use case**: When fast decompression is beneficial without sacrificing ratio

### 4.6 CTX2 (2-Context Range Coder)

Separates symbols into two independent contexts — **run context** (within a zero run) and **literal context** (outside runs) — each with its own probability model. This improves prediction accuracy over a single unified model.

- Strategy ID: 29 (`BWT+CTX2`)
- Exploits the bimodal distribution typical of BWT+MTF+ZRLE output

### 4.7 LZP (Lempel-Ziv Prediction)

A preprocessing stage that eliminates long matches before BWT, using a hash table to predict the next byte based on a context window.

- **Hash table**: 2^20 entries (1M slots)
- **Context**: 8 bytes
- **Minimum match**: 12 bytes
- **Role**: Removes long-distance redundancy that BWT captures poorly
- **Strategies**: Used in IDs 5, 6, 7, 8, 14, 15

### 4.8 PPM (Prediction by Partial Matching)

PPMC implementation with exclusion, operating at multiple orders:

| Order | Contexts | Storage |
|-------|----------|---------|
| O0 | 1 global | Exact |
| O1 | 256 | Exact |
| O2 | 65,536 | Exact |
| O3 | 131,072 | Hashed |

- **Rescaling**: When total count exceeds 4,000
- **Strategies**: PPM direct (ID 16), BWT+PPM (17), BWT+MTF+PPM (18)

### 4.9 LZMA (Lempel-Ziv-Markov Chain Algorithm)

Complete from-scratch LZMA implementation including:

- **Match finder**: Hash chain with hash2 (64K exact for len=2), hash3 (64K hashed), hash4
- **Optimal parsing**: Bit-price calculation to find optimal match/literal path
- **Coding**: Range coder with adaptive models for literals, lengths, distances, and slots
- **BCJ filter** (strategy 25): x86 pre-filter that converts relative call/jump addresses to absolute, improving LZMA compression of executables (E8/E9 filter)

### 4.10 LZ77

Classic LZ77 compressor followed by BWT processing. Uses a 64K sliding window for match finding.

- **Strategies**: LZ77+BWT+O0 (ID 22), LZ77+BWT+O1 (ID 23)
- **Effective on**: Data with many local repetitions

### 4.11 Word Preprocessing (WP)

Pre-transform that replaces **frequent words** (identifiers, keywords) with single-byte tokens `0x80..0xFE`:

1. **Phase 1**: Word frequency counting via 256K-slot hash table
2. **Phase 2**: Sort top 127 words by frequency → assign token IDs
3. **Phase 3**: Encode (replace words → tokens, escape `0xFF` for existing bytes ≥ 0x80)

- **Constraints**: Minimum block size 2,048 bytes; words 3–31 chars (letters, digits, `_`, `$`)
- **Token capacity**: Up to 127 words per block (`WP_MAX_TOKENS`)
- **Applied automatically** to text-like blocks

### 4.12 Delta Coding

Byte-level differencing that stores the difference between consecutive bytes. Excellent for slowly varying data such as sensor readings, numeric columns, and time-series data.

- Used as a pre-filter in strategies 3, 4, 7, 8, 13, 15

### 4.13 Float32 XOR-Delta

Specialized transform for IEEE 754 single-precision floating-point data. Applies XOR between consecutive 32-bit floats, exploiting the fact that neighboring mantissas often share high-order bits.

- Strategy ID: 26 (`F32XOR+BWT`)
- Followed by BWT + Range Coder

### 4.14 Audio PCM

Automatic detection and specialized compression of uncompressed PCM audio data:

1. **Analysis**: Detects channel configuration (2 or 4 channels, 16-bit samples)
2. **Deinterleave**: Separates interleaved channels into independent streams
3. **Delta encoding**: Per-channel delta for temporal prediction
4. **Compression**: BWT-based compression of the transformed output

- Strategy ID: 19

### 4.15 Base64 Split

Detects embedded Base64-encoded segments within data and processes them separately:

**Version 1** (ID 20): Basic detection and split compression.

**Version 2** (ID 21): Advanced 3-stream parallel approach:
1. Identifies Base64 runs ≥ 76 characters (`B64_MIN_RUN`)
2. Splits into 3 sub-streams: **positions** (delta-varint encoded), **skeleton** (non-Base64 text), **decoded binary data**
3. Each sub-stream is compressed independently with its best strategy
4. Decompression of the 3 sub-streams is **parallelized** (3 threads)

---

## 5. Strategy Selection (Heuristic)

The function `compress_block()` is the core of the engine. It employs a multi-level heuristic to find the best strategy for each block without exhaustively testing all 31 strategies on full data every time.

### Level 1: Entropy Check

```c
double ent = block_entropy(data, n);
if (ent > 7.95) return best_size;  // random data → STORE
```

If the block entropy exceeds 7.95 bits/byte, the data is considered incompressible and stored raw (strategy 0). This avoids wasting time on already-compressed or random data.

### Level 2: Sample-Based Fast Selection (v10.2)

For blocks larger than `SAMPLE_THRESHOLD` (2 MB), a 1 MB sub-sample is compressed with all candidate strategies. The result produces a `hint_strat` that constrains the strategies tested on the full block:

- If hint is an **AC strategy** (BWT-based) → skip the Delta/LZ77/LZMA group entirely
- If hint is a **BDE strategy** (Delta/LZ77/LZMA) → skip the BWT group entirely
- If hint is **LZMA** → test **only** LZMA on the full block

This optimization reduces compression time by **30–50%** on large blocks with no measurable loss in compression ratio.

### Level 3: Parallel Group Trial

Two groups of strategies run in parallel (2 threads per block):

| Group AC (thread_groups_AC) | Group BDE (thread_groups_BDE) |
|-----------------------------|-------------------------------|
| BWT + O0 | Delta + BWT + O0/O1/O2 |
| BWT + O1 | Delta + LZP + BWT + O0/O1/O2 |
| BWT + O1_PS (warm-start) | Float32 XOR + BWT |
| BWT + O2 | LZ77 + BWT + O0/O1 |
| BWT + O0_PS (pre-scanned) | LZMA (optimal parsing) |
| BWT + rANS O0 | BCJ + LZMA |
| BWT + CTX2 (2-context) | |
| LZP + BWT + O0/O1/O2 | |
| PPM direct | |
| BWT + CM | |
| Audio detect + encode | |
| Base64 detect + split | |

### Final Selection

The smallest compressed output across both groups wins. Its strategy ID (1 byte) is stored in the block header. If no strategy produces output smaller than the original data, the block is stored uncompressed (STORE).

---

## 6. File Formats

### HPK5 (Single File)

HPK5 is the format for compressing individual files.

#### Header

```
Offset  Size    Field
------  ------  -----
0       4       MAGIC = 0x48504B35 ("HPK5")
4       1       VERSION (current: 11)
5       1       PRETRANSFORM (0=none, 1=PNG)
6       4       META_SIZE (if PT != 0)
10      var     METADATA (if PT != 0, e.g., PNG chunks)
var     4       BLOCK_SIZE (in bytes)
var+4   8       ORIG_SIZE (original file size, 64-bit)
var+12  4       NBLOCKS
```

#### Block Structure

Each block is encoded as:

```
Offset  Size    Field
------  ------  -----
0       1       FLAGS (bit 0: is_dup)

If is_dup = 1:
1       4       DUP_REF (index of original block)

If is_dup = 0:
1       1       STRATEGY (0–30)
2       4       COMP_SIZE (compressed size in bytes)
6       4       ORIG_BLOCK_SIZE (original block size)
10      4       CRC32 (of original data)
14      var     COMPRESSED_DATA
```

#### Pre-Transform: PNG

When a PNG file is detected:
1. All chunks are read; non-IDAT/IEND chunks are stored as metadata
2. IDAT chunks are concatenated and inflated (zlib decompress) → raw filtered scanlines
3. Scanlines are compressed with HyperPack's strategies (BWT outperforms deflate on PNG filter output)
4. On decompression: decompress → re-deflate → reconstruct valid PNG

**Fallback**: If the pre-transformed HPK is larger than the original, HyperPack retries automatically without pre-transform.

#### Compatibility

- Current format can read legacy V5–V10 archives
- V5–V10 readers cannot decode V11+ files that use `PRETRANSFORM != 0`

### HPK6 (Archive)

HPK6 is the multi-file archive format with directory support and file-level deduplication.

#### Header

```
Offset  Size    Field
------  ------  -----
0       4       MAGIC = 0x48504B36 ("HPK6")
4       1       VERSION
5       4       BLOCK_SIZE
9       4       ENTRY_COUNT
13      4       TOTAL_BLOCKS
17      8       TOTAL_SIZE (sum of all original file sizes)
```

#### Directory Catalog (per entry)

```
Field           Size    Description
-----           ------  -----------
type            1       0=file, 1=directory
path_len        2       Path length (big-endian)
path            var     Relative path (UTF-8, / separated)
size            8       Original file size
perms           4       Unix permissions (07777 mask)
mtime           8       Modification timestamp
crc             4       CRC32 of complete file
first_block     4       Index of first block
nblocks         4       Number of blocks
is_dedup        1       1 if file is a duplicate
dedup_ref       4       Index of original entry (if is_dedup=1)
```

#### Block Data

Same format as HPK5 blocks (FLAGS + STRATEGY + COMP_SIZE + ORIG_SIZE + CRC32 + DATA).

#### Index and Footer

```
[Block data...]
[Block offset table: uint64 x total_blocks]
[INDEX_OFFSET: uint64]
[FOOTER_MAGIC: uint32 = 0x48504B36]
```

#### File-Level Deduplication

Before compression, HyperPack computes CRC32 for each file. Files with identical size and CRC32 are marked `is_dedup=1` and reference the original entry, requiring zero additional blocks.

---

## 7. Parallel Compression

HyperPack employs four levels of parallelism:

```
Level 1: Inter-block       (CLI -j N, Tauri nthreads)
    └── N blocks compressed simultaneously, each on its own thread

Level 2: Intra-block       (CLI & Tauri only, NOT WASM)
    └── 2 threads per block: Group AC || Group BDE

Level 3: Decompression     (CLI & Tauri only, NOT WASM)
    └── 4 threads (HP_PAR_THREADS) for batch block decompression

Level 4: Base64v2          (CLI & Tauri only, NOT WASM)
    └── 3 threads to decompress the 3 sub-streams in parallel
```

### Thread Pool Architecture

- **Inter-block parallelism**: Controlled by the `-j N` option (1–16 threads). Each thread picks the next unprocessed block and runs the full strategy selection pipeline.
- **Intra-block parallelism**: Within each block, strategy groups AC and BDE run on separate threads simultaneously, cutting per-block strategy selection time roughly in half.
- **Decompression parallelism**: Blocks are decompressed in batches of 4 using a thread pool (`HP_PAR_THREADS = 4`).

### Performance Impact

| Configuration | Relative Speed |
|---------------|----------------|
| CLI `-j 1` | 1.0x (baseline) |
| CLI `-j 4` | ~3.5x (near-linear scaling) |
| CLI `-j 8` | ~5–6x (diminishing returns) |
| WASM (single thread) | ~0.4–0.6x of CLI `-j 1` |
| Tauri (`nthreads=auto`) | ~3–4x (similar to CLI `-j 4`) |

### WASM Limitations

In the WASM build (`HYPERPACK_WASM` defined), `pthread_create` is stubbed to run the function synchronously on the calling thread, and `pthread_join` is a no-op. This means:
- No intra-block parallelism (strategies run sequentially)
- No inter-block parallelism (single Web Worker)
- No parallel decompression
- No parallel Base64v2 decode

---

## 8. CLI Reference

### Commands

```bash
# Compress a single file (HPK5)
hyperpack c [-b SIZE_MB] [-j THREADS] [-s STRATEGY] [-S LIST] [-X LIST] input output.hpk

# Compress into an archive (HPK6)
hyperpack a [-b SIZE_MB] [-j THREADS] [-s STRATEGY] [-S LIST] [-X LIST] input1 input2 dir/ output.hpk

# Decompress (auto-detects HPK5/HPK6)
hyperpack d input.hpk [output]

# List archive contents
hyperpack l archive.hpk

# Selective extraction from archive
hyperpack x archive.hpk output_dir [-e pattern]

# List all available strategies with their IDs
hyperpack --list-strategies
```

### Options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `-b` | 1–128 | 128 | Block size in MB |
| `-j` | 1–16 | 1 | Number of parallel compression threads |
| `-s N` / `--strategy N` | 0–30 | -1 (auto) | Force a single compression strategy (bypasses auto-selection entirely) |
| `-S N,N,...` | 0–30 (comma-sep) | all | **Include filter** — auto-selects only among listed strategies |
| `-X N,N,...` | 0–30 (comma-sep) | none | **Exclude filter** — auto-selects among all strategies except those listed |
| `--list-strategies` | — | — | Print all 31 strategies with their IDs and exit |
| `-e` | pattern | — | Selective extraction pattern (with `x` command) |

> **Note:** `-s` (force) cannot be combined with `-S` or `-X`. `-S` and `-X` cannot be combined with each other.

### Examples

```bash
# Compress a large file with 16 MB blocks and 4 threads
hyperpack c -b 16 -j 4 database.sql database.hpk

# Force LZMA strategy on a file
hyperpack c -s 24 binary.dat binary.hpk

# Force BCJ+LZMA on an executable
hyperpack c -s 25 program.elf program.hpk

# Auto-select among BWT variants only (fast, skip LZMA)
hyperpack c -S 1,2,5,6,12,14 data.csv data.hpk

# Auto-select but exclude slow strategies (LZMA, BCJ+LZMA)
hyperpack c -X 24,25 largefile.bin largefile.hpk

# Compare just two strategies — let auto pick the best
hyperpack c -S 1,24 file.dat file.hpk

# Archive a directory with only LZP+BWT strategies
hyperpack a -S 5,6,14 /home/user/project project.hpk

# List all strategies to see IDs and names
hyperpack --list-strategies

# Decompress a single file
hyperpack d database.hpk database_restored.sql

# Decompress an archive into a directory
hyperpack d project.hpk ./output/

# Extract a single file from an archive
hyperpack x project.hpk ./out/ -e "src/main.c"
```

### Strategy Control

HyperPack offers three levels of strategy control for compression:

#### Level 1: Fully Automatic (default)

With no options, HyperPack tests all 31 strategies on each block using sample-based heuristics and selects the best one automatically. This is the recommended mode for most use cases.

#### Level 2: Filtered Auto-Selection (`-S` / `-X`)

Restricts which strategies the auto-selector considers:

```bash
# Only test BWT+O0, BWT+O1, and LZP+BWT+O0
hyperpack c -S 1,2,5 file.dat file.hpk

# Test everything except LZMA and BCJ+LZMA (faster)
hyperpack c -X 24,25 file.dat file.hpk
```

When `-S` is used, strategies normally skipped by internal heuristics (e.g., LZMA on large text) will be **forced to run** — the user explicitly opted into them. Group-level optimizations still apply: if no strategies from a group (e.g., Delta, LZ77) are in the allowed set, that entire group's preprocessing is skipped, saving time.

The filter is printed at startup:
```
[HP5] Strategy filter: 1(BWT+O0), 2(BWT+O1), 5(LZP+BWT+O0)
```

#### Level 3: Forced Single Strategy (`-s`)

Bypasses all auto-selection logic entirely — no sampling, no trial compression:

```bash
# Force LZMA
hyperpack c -s 24 file.dat file.hpk

# Force BWT+O1
hyperpack c -s 2 file.dat file.hpk
```

Useful for benchmarking individual strategies or when you know which algorithm works best for your data.

### Diagnostic Output (stderr)

```
[HP5] Compressing data.bin (150.00 MB, 19 blocks of 8 MB)
  Block 1/19: 8388608 -> 2145832 (3.91x) [BWT+O1PS]
  Block 2/19: 8388608 -> 1987654 (4.22x) [LZMA]
  Block 3/19: DUP of 1
  ...
[HP5] Done: 157286400 -> 38456789 bytes (4.09x) in 45.2s
```

---

## 9. Building

### CLI (Linux/macOS)

```bash
# Standard build
make

# Or directly with gcc
gcc -O3 -o hyperpack src/hyperpack.c -lm -lz -lpthread
```

The binary has minimal dependencies:
- **libc** — standard C library
- **libm** — math functions (`log2` for entropy calculation)
- **zlib** — PNG pre-transform (inflate/deflate of IDAT chunks)
- **pthreads** — parallel compression

### Build Options

| Target | Command | Notes |
|--------|---------|-------|
| CLI release | `make` or `gcc -O3 ...` | Optimized native binary |
| Debug | `gcc -g -O0 ...` | With debug symbols |
| Windows | `gcc -O3 ... -D_WIN32` | Uses `_mkdir`, `tmpfile` polyfill for `fmemopen` |

### WASM Build (Emscripten)

```bash
# Requires Emscripten SDK installed
./build-wasm.sh
```

The WASM build script uses Emscripten with the following configuration:

| Parameter | Value | Purpose |
|-----------|-------|---------|
| `INITIAL_MEMORY` | 256 MB | Starting WASM heap |
| `MAXIMUM_MEMORY` | 2 GB | Hard memory ceiling |
| `STACK_SIZE` | 8 MB | Thread stack size |
| `USE_ZLIB` | 1 | Enable zlib for PNG pre-transform |
| `ALLOW_MEMORY_GROWTH` | 1 | Dynamic heap expansion |

Key WASM constraints:
- Maximum block size limited to **64 MB** (vs 128 MB native)
- Maximum practical file size **~500 MB** (2 GB heap minus WASM overhead)
- All file I/O through **MEMFS** (in-memory virtual filesystem)

---

## Appendix: Key Constants

Reference values from the source code:

| Constant | Value | Description |
|----------|-------|-------------|
| `MAGIC` | `0x48504B35` | HPK5 magic number |
| `MAGIC6` | `0x48504B36` | HPK6 magic number |
| `VERSION` | `11` | Current format version |
| `DEFAULT_BS` | `128 << 20` | Default block size (128 MB) |
| `SAMPLE_THRESHOLD` | `2 << 20` | Sample heuristic threshold (2 MB) |
| `SAMPLE_SIZE` | `1 << 20` | Sample size for hinting (1 MB) |
| `WP_MAX_TOKENS` | `127` | Maximum word preprocessing tokens per block |
| `WP_MAX_WLEN` | `31` | Maximum word length for WP |
| `PPM_MAXORD` | `3` | Maximum PPM order |
| `PPM_RESCALE` | `4000` | PPM rescaling threshold |
| `HP_PAR_THREADS` | `4` | Parallel decompression thread count |
| `B64_MIN_RUN` | `76` | Minimum Base64 run length for detection |
| `ZRLE_ALPHA` | `258` | ZRLE alphabet size (RUNA + RUNB + 256 literals) |
| `RC_TOP` | `1 << 24` | Range coder top value |
| `RC_BOT` | `1 << 16` | Range coder bottom value |
| `MAX_TOTAL` | `16383` | Maximum frequency total |
| `LZP_HTAB_BITS` | `20` | LZP hash table size (2^20 = 1M entries) |
| `LZP_CONTEXT` | `8` | LZP context length in bytes |
| `LZP_MIN_MATCH` | `12` | LZP minimum match length |
