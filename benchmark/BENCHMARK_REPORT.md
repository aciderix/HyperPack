# HyperPack v11 — Benchmark Report
**Date:** 2026-03-09
**Platform:** Linux x86_64
**Compilers/versions:** gcc -O2, gzip 1.12, bzip2 1.0.8, xz 5.4.5

---

## Corpus

22 real-world files (9.5 MB total) from a Debian Linux system, covering a representative
diversity of data types encountered in practice:

| Category | Files |
|----------|-------|
| Documentation (plain text) | eval_doc.txt, syntax_doc.txt, options_doc.txt, builtin_doc.txt |
| XML structured data | mime_types.xml (2.4 MB), supplementalData.xml, xkb_rules.xml |
| JavaScript source | jquery.js |
| C source / headers | hyperpack_src.c, Xlib.h, Xproto.h, glxproto.h |
| Python data tables | langbulgarianmodel.py, langrussianmodel.py, langthaimodel.py |
| Shared libraries (binary) | BIG5HKSCS.so, IBM1390.so, IBM1399.so, libCNS.so, libgp-collector.so |
| Executable binary | xz_binary |
| Unicode data | unicode_allkeys.txt (1.9 MB) |

---

## Global Summary

| Rank | Compressor | Avg Ratio | Compressed | Total Time | Speed | Wins | vs HP |
|------|-----------|-----------|------------|------------|-------|------|-------|
| 1 | **xz -9** | **6.433x** | 1 518 KB | 4.0s | 2.38 MB/s | 8 | −8.3% |
| 2 | **HyperPack v11** | **5.900x** | 1 655 KB | 14.9s | 0.64 MB/s | **5** | — |
| 3 | bzip2 -9 | 5.410x | 1 805 KB | 1.1s | 8.55 MB/s | 9 | −9.0% |
| 4 | gzip -9 | 4.759x | 2 052 KB | 1.4s | 6.78 MB/s | 0 | −24.0% |

> **HyperPack beats gzip by +24.0% and bzip2 by +9.0%.**
> **xz -9 beats HyperPack by 8.3% on this corpus, while being 6× faster.**

---

## Per-File Results

| File | Size | HyperPack | gzip -9 | bzip2 -9 | xz -9 | Winner |
|------|------|-----------|---------|----------|-------|--------|
| BIG5HKSCS.so | 238 KB | 3.241x | 2.743x | 2.632x | 3.385x | xz |
| IBM1390.so | 230 KB | 3.057x | 2.504x | 2.252x | 3.796x | xz |
| IBM1399.so | 230 KB | 3.016x | 2.503x | 2.252x | 3.794x | xz |
| Xlib.h | 97 KB | 5.921x | 5.663x | **6.177x** | 6.122x | bzip2 |
| Xproto.h | 51 KB | 5.358x | 4.970x | **5.592x** | 5.344x | bzip2 |
| builtin_doc.txt | 421 KB | 4.180x | 3.372x | **4.291x** | 4.116x | bzip2 |
| eval_doc.txt | 169 KB | 3.506x | 3.164x | **3.593x** | 3.514x | bzip2 |
| glxproto.h | 76 KB | 7.281x | 6.371x | 7.239x | **7.416x** | xz |
| hyperpack_src.c | 209 KB | 5.123x | 4.738x | 5.352x | **5.405x** | xz |
| jquery.js | 282 KB | 4.046x | 3.414x | **4.197x** | 3.944x | bzip2 |
| langbulgarianmodel.py | 102 KB | **27.487x** | 15.811x | 20.866x | 24.614x | **HP** 🏆 |
| langrussianmodel.py | 125 KB | **23.613x** | 14.180x | 18.454x | 22.896x | **HP** 🏆 |
| langthaimodel.py | 100 KB | **30.042x** | 17.258x | 23.973x | 26.515x | **HP** 🏆 |
| libCNS.so | 461 KB | 2.092x | 1.761x | 1.710x | **2.398x** | xz |
| libgp-collector.so | 1 310 KB | 8.832x | 6.698x | 6.629x | **9.033x** | xz |
| mime_types.xml | 2 412 KB | **11.212x** | 7.236x | 10.839x | 10.574x | **HP** 🏆 |
| options_doc.txt | 409 KB | 3.892x | 3.170x | **4.002x** | 3.824x | bzip2 |
| supplementalData.xml | 377 KB | 7.851x | 6.461x | **8.207x** | 8.097x | bzip2 |
| syntax_doc.txt | 232 KB | 3.438x | 3.019x | **3.507x** | 3.408x | bzip2 |
| unicode_allkeys.txt | 1 893 KB | 7.012x | 6.179x | 7.511x | **10.417x** | xz |
| xkb_rules.xml | 247 KB | 14.881x | 13.660x | **15.941x** | 14.999x | bzip2 |
| xz_binary | 86 KB | **2.917x** | 2.469x | 2.505x | 2.849x | **HP** 🏆 |

---

## Analysis by Data Type

### Highly repetitive Python data tables (3 files)
HyperPack dominates with ratios of **23×–30×** versus xz's 22×–26×.
These are language model frequency tables (highly structured, very repetitive).

### XML structured data (3 files)
Mixed results. HyperPack wins on `mime_types.xml` (+6.0% over xz), but bzip2 edges ahead
on smaller XML files like `supplementalData.xml`.

### Shared libraries / binaries (5 files)
xz dominates. Its LZMA implementation with BCJ x86 filter appears better tuned for
ELF binary data than HyperPack's strategy selector.

### Plain text documentation (4 files)
bzip2 leads, followed closely by xz and HyperPack (within 5–10% of each other).
HyperPack consistently beats gzip by ~25%.

### Source code (C, JS, headers) (4 files)
xz and bzip2 lead. HyperPack is competitive but trails by ~3–8%.

---

## Speed vs Ratio Trade-off

```
Ratio (higher = better)
 6.5 |                              xz ●
 6.0 |
 5.9 |              HP ●
 5.5 |
 5.4 |          bzip2 ●
 5.0 |
 4.8 |     gzip ●
     +----+--------+--------+--------+
        0.6      2.4      6.8      8.6   Speed (MB/s)
```

- **gzip**: fastest writer (6.8 MB/s), worst ratio
- **bzip2**: excellent speed/ratio balance (8.5 MB/s, 5.4×)
- **xz**: best ratio (6.4×) at moderate speed (2.4 MB/s)
- **HyperPack**: second-best ratio (5.9×) but slowest (0.64 MB/s)

---

## Key Observations

1. **HyperPack excels on highly repetitive/structured data** (Python frequency tables,
   large XML) where its multi-strategy approach (BWT, Context Mixing) outshines single-
   strategy compressors.

2. **xz's LZMA wins on binary ELF files** — its x86 BCJ filter is highly effective for
   shared libraries and executables.

3. **HyperPack's main weakness is speed**: at 0.64 MB/s compression throughput versus
   xz's 2.38 MB/s, it is ~3.7× slower than xz and ~13× slower than bzip2.

4. **All 22 roundtrips passed** — perfect decompression fidelity verified for all methods.

---

## About Standard Compression Benchmarks

The compression field uses several canonical corpora:

| Corpus | Size | Description |
|--------|------|-------------|
| **Silesia** (2002) | 211 MB / 12 files | Current gold standard (mahoney.mattmahoney.net) |
| **Canterbury** (1997) | 2.8 MB / 11 files | Classic: alice29.txt, source code, executables |
| **Calgary** (1987) | 3.1 MB / 18 files | Historical standard (book1, obj1, pic…) |
| **enwik8/enwik9** | 100 MB / 1 GB | Wikipedia dump — Hutter Prize benchmark |
| **Squash** | varied | Web leaderboard (squash.github.io) |

This report uses a **real-world system files corpus** which is not standardised but more
representative of everyday compression workloads. The `docs/BENCHMARKS.md` file in this
repository contains results on the canonical Canterbury, Calgary, and Silesia corpora.

---

*Generated by `benchmark/run_benchmark.sh` — HyperPack v11 / 2026-03-09*
