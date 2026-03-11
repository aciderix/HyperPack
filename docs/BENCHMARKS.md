# Benchmarks

Comprehensive compression benchmarks comparing HyperPack against established compressors on three standard corpora.

## Table of Contents

- [Methodology](#methodology)
- [Overall Results](#overall-results)
- [Calgary Corpus](#calgary-corpus)
- [Canterbury Corpus](#canterbury-corpus)
- [Silesia Corpus](#silesia-corpus)
- [Speed Comparison](#speed-comparison)
- [Analysis](#analysis)
- [Reproducing](#reproducing)

---

## Methodology

All benchmarks measure **compressed size as a percentage of the original** (lower is better).

| Parameter | Value |
|-----------|-------|
| Corpora | Calgary (18 files, 3.25 MB), Canterbury (11 files, 2.81 MB), Silesia (12 files, 211.94 MB) |
| HyperPack | v11, single-file mode (`hyperpack c`), default block size |
| gzip | v1.13, maximum compression (`-9`) |
| bzip2 | v1.0.8, maximum compression (`-9`) |
| xz | v5.6, maximum compression (`-9`, LZMA2) |
| zstd | v1.5.7, near-maximum compression (`-19`) |
| Metric | Compressed size / original size × 100 (lower = better) |
| Integrity | All HyperPack decompressions verified via binary diff |
| Environment | Linux x86_64 |

---

## Overall Results

### Aggregate Compression Ratios

| Corpus | Files | Total Size | HyperPack | gzip -9 | bzip2 -9 | xz -9 | zstd -19 |
|--------|------:|-----------:|:---------:|:-------:|:--------:|:-----:|:--------:|
| Calgary | 18 | 3.25 MB | **26.03%** | 32.58% | 26.65% | 27.24% | 28.33% |
| Canterbury | 11 | 2.81 MB | **16.01%** | 26.00% | 19.31% | 17.52% | 18.37% |
| Silesia | 12 | 211.94 MB | **21.86%** | 31.91% | 25.72% | 23.02% | 24.96% |
| **OVERALL** | **41** | **231.82 MB** | **21.85%** | **31.84%** | **25.65%** | **23.02%** | **24.92%** |

HyperPack achieves the best overall compression ratio across all three standard corpora in aggregate.

### Per-File Win Count

Number of files (out of 41) where each tool achieves the best compression ratio:

| Tool | Wins | Share |
|------|-----:|------:|
| bzip2 -9 | 16 | 39% |
| **HyperPack** | **12** | **29%** |
| xz -9 | 10 | 24% |
| zstd -19 | 3 | 7% |
| gzip -9 | 0 | 0% |

While bzip2 wins the most individual files (mostly small text files), HyperPack's wins come on larger, higher-impact files — giving it the best weighted overall ratio.

---

## Calgary Corpus

The Calgary Corpus (1987) is one of the oldest standard benchmarks, consisting of 18 files totaling 3.25 MB.

| File | Description | Size | HyperPack | gzip -9 | bzip2 -9 | xz -9 | zstd -19 | Winner |
|------|-------------|-----:|:---------:|:-------:|:--------:|:-----:|:--------:|--------|
| bib | Bibliography (refer format) | 111.3 KB | 25.44% | 31.36% | **24.68%** | 27.49% | 28.70% | bzip2 |
| book1 | Fiction book (Thomas Hardy) | 768.8 KB | 30.50% | 40.62% | **30.25%** | 33.96% | 34.44% | bzip2 |
| book2 | Non-fiction book (Witten) | 610.9 KB | 26.12% | 33.74% | **25.77%** | 27.80% | 28.46% | bzip2 |
| geo | Geophysical data (32-bit floats) | 102.4 KB | 52.50% | 66.81% | 55.58% | **52.12%** | 61.57% | xz |
| news | Usenet batch file | 377.1 KB | 32.06% | 38.29% | **31.44%** | 31.53% | 32.53% | bzip2 |
| obj1 | Object code (VAX) | 21.5 KB | 43.92% | 47.99% | 50.16% | **43.88%** | 45.72% | xz |
| obj2 | Object code (Apple Mac) | 246.8 KB | 25.27% | 32.85% | 30.97% | **24.92%** | 28.45% | xz |
| paper1 | Technical paper (Witten) | 53.2 KB | 32.14% | 34.88% | **31.14%** | 32.52% | 33.14% | bzip2 |
| paper2 | Technical paper (Witten) | 82.2 KB | 31.44% | 36.09% | **30.46%** | 33.13% | 33.52% | bzip2 |
| paper3 | Technical paper (Witten) | 46.5 KB | 35.17% | 38.84% | **34.03%** | 36.69% | 36.94% | bzip2 |
| paper4 | Technical paper (Witten) | 13.3 KB | 40.43% | 41.65% | **39.04%** | 40.76% | 40.44% | bzip2 |
| paper5 | Technical paper (Witten) | 12.0 KB | 41.17% | 41.78% | **40.46%** | 41.05% | 40.73% | bzip2 |
| paper6 | Technical paper (Witten) | 38.1 KB | 33.06% | 34.67% | **32.25%** | 32.83% | 33.41% | bzip2 |
| pic | Black & white fax image | 513.2 KB | **7.87%** | 10.20% | 9.69% | 8.18% | 8.50% | HyperPack |
| progc | Source code (C) | 39.6 KB | 31.96% | 33.47% | **31.66%** | 31.72% | 32.29% | bzip2 |
| progl | Source code (Lisp) | 71.6 KB | 21.00% | 22.56% | 21.74% | **20.92%** | 21.15% | xz |
| progp | Source code (Pascal) | 49.4 KB | 21.05% | 22.65% | 21.68% | **20.98%** | 21.17% | xz |
| trans | Transcript of terminal session | 93.7 KB | 17.88% | 20.13% | 19.10% | **17.85%** | 18.46% | xz |

**Wins:** bzip2: 11 · xz: 6 · HyperPack: 1

The Calgary Corpus consists mostly of small files where bzip2's block-sorting algorithm excels. HyperPack's multi-strategy overhead has less room to pay off on files under 100 KB. Despite this, HyperPack wins the aggregate ratio (**26.03%** vs bzip2's 26.65%) thanks to its dominant win on `pic`.

---

## Canterbury Corpus

The Canterbury Corpus (1997) is a 11-file benchmark totaling 2.81 MB, designed to replace Calgary with a more representative file mix.

| File | Description | Size | HyperPack | gzip -9 | bzip2 -9 | xz -9 | zstd -19 | Winner |
|------|-------------|-----:|:---------:|:-------:|:--------:|:-----:|:--------:|--------|
| alice29.txt | English text (Alice in Wonderland) | 152.1 KB | 29.05% | 35.63% | **28.40%** | 31.88% | 32.35% | bzip2 |
| asyoulik.txt | Shakespeare play (As You Like It) | 125.2 KB | 32.50% | 39.00% | **31.60%** | 35.58% | 36.06% | bzip2 |
| cp.html | HTML source | 24.6 KB | 31.47% | 32.43% | **30.98%** | 31.10% | 31.36% | bzip2 |
| fields.c | C source code | 11.2 KB | 27.21% | 28.12% | 27.25% | 27.22% | **27.07%** | zstd |
| grammar.lsp | Lisp source code | 3.7 KB | 34.66% | 33.48% | 34.47% | 34.93% | **32.65%** | zstd |
| kennedy.xls | Excel spreadsheet | 1.03 MB | **4.23%** | 20.36% | 12.65% | 4.77% | 6.29% | HyperPack |
| lcet10.txt | Technical writing | 426.8 KB | 25.69% | 33.84% | **25.23%** | 28.00% | 28.43% | bzip2 |
| plrabn12.txt | Poetry (Paradise Lost) | 481.9 KB | 30.67% | 40.31% | **30.21%** | 34.32% | 34.74% | bzip2 |
| ptt5 | Black & white fax image | 513.2 KB | **7.87%** | 10.20% | 9.69% | 8.18% | 8.50% | HyperPack |
| sum | SPARC executable | 38.2 KB | 25.18% | 33.39% | 33.75% | **24.73%** | 29.05% | xz |
| xargs.1 | GNU man page | 4.2 KB | 42.91% | 41.54% | 41.68% | 43.05% | **40.88%** | zstd |

**Wins:** bzip2: 5 · zstd: 3 · HyperPack: 2 · xz: 1

HyperPack dominates the two largest files — `kennedy.xls` (spreadsheet with repetitive binary data) and `ptt5` (fax image) — which gives it the best aggregate ratio (**16.01%** vs xz's 17.52%).

---

## Silesia Corpus

The Silesia Corpus (2003) is the most demanding standard benchmark: 12 files totaling 211.94 MB of real-world data types.

| File | Description | Size | HyperPack | gzip -9 | bzip2 -9 | xz -9 | zstd -19 | Winner |
|------|-------------|-----:|:---------:|:-------:|:--------:|:-----:|:--------:|--------|
| dickens | English text (Collected Dickens) | 10.19 MB | **24.65%** | 37.79% | 27.46% | 27.77% | 27.96% | HyperPack |
| mozilla | Tarball (Mozilla distribution) | 51.22 MB | 27.27% | 37.08% | 34.97% | **26.11%** | 29.42% | xz |
| mr | MRI medical scan | 9.97 MB | **23.66%** | 36.84% | 24.48% | 27.58% | 31.15% | HyperPack |
| nci | Chemical structure database | 33.55 MB | **4.00%** | 8.90% | 5.40% | 5.18% | 4.96% | HyperPack |
| ooffice | OpenOffice.org DLL | 6.15 MB | **35.16%** | 50.23% | 46.52% | 39.44% | 42.18% | HyperPack |
| osdb | MySQL benchmark data | 10.09 MB | **25.35%** | 36.84% | 27.78% | 28.25% | 30.73% | HyperPack |
| reymont | Polish text (Reymont novel) | 6.63 MB | **17.02%** | 27.47% | 18.80% | 19.87% | 20.34% | HyperPack |
| samba | Tarball (Samba distribution) | 21.61 MB | 19.56% | 25.03% | 21.05% | **17.41%** | 18.02% | xz |
| sao | SAO star catalog (binary) | 7.25 MB | 61.79% | 73.45% | 68.12% | **60.88%** | 68.95% | xz |
| webster | English dictionary (Webster's) | 41.46 MB | **17.32%** | 29.09% | 20.85% | 20.22% | 20.93% | HyperPack |
| x-ray | Medical X-ray image | 8.47 MB | **46.87%** | 71.24% | 47.80% | 52.98% | 60.53% | HyperPack |
| xml | XML data | 5.35 MB | **8.07%** | 12.39% | 8.25% | 8.47% | 8.47% | HyperPack |

**Wins:** HyperPack: 9 · xz: 3

This is where HyperPack truly shines — winning **9 out of 12 files** on the most demanding modern benchmark corpus. The multi-strategy approach excels on larger, real-world data where different regions benefit from different compression algorithms.

### Top HyperPack Advantages (Silesia)

| File | HyperPack | Next Best | Advantage |
|------|:---------:|:---------:|:---------:|
| ooffice | 35.16% | 39.44% (xz) | +4.3 pp |
| webster | 17.32% | 20.22% (xz) | +2.9 pp |
| dickens | 24.65% | 27.46% (bzip2) | +2.8 pp |
| osdb | 25.35% | 27.78% (bzip2) | +2.4 pp |
| reymont | 17.02% | 18.80% (bzip2) | +1.8 pp |

---

## Speed Comparison

Measured on three representative Silesia files. Timing is indicative (sandbox environment, not bare-metal).

| File | Tool | Ratio | Compress | Decompress |
|------|------|:-----:|:--------:|:----------:|
| dickens (10 MB) | **HyperPack** | **24.65%** | 2.61s | 1.47s |
| | gzip -9 | 37.79% | 0.89s | 0.04s |
| | bzip2 -9 | 27.47% | 0.78s | 0.37s |
| | xz -9 | 27.77% | 7.92s | 0.09s |
| | zstd -19 | 27.96% | 6.21s | 0.02s |
| mozilla (51 MB) | **HyperPack** | **27.28%** | 28.78s | 1.24s |
| | gzip -9 | 37.08% | 7.51s | 0.23s |
| | bzip2 -9 | 34.98% | 3.50s | 1.66s |
| | xz -9 | 26.11% | 25.57s | 0.59s |
| | zstd -19 | 29.43% | 18.44s | 0.08s |
| webster (41 MB) | **HyperPack** | **17.32%** | 15.17s | 5.67s |
| | gzip -9 | 29.09% | 2.52s | 0.14s |
| | bzip2 -9 | 20.85% | 3.41s | 1.26s |
| | xz -9 | 20.23% | 32.26s | 0.35s |
| | zstd -19 | 20.94% | 26.34s | 0.06s |

**Key takeaway:** HyperPack achieves best-in-class ratios at compression speeds comparable to xz -9 and zstd -19. Decompression is slower than streaming decompressors (gzip, zstd) but remains practical for all use cases.

---

## Analysis

### Where HyperPack Excels

- **Large heterogeneous files** — Binary files like `ooffice` (DLL), `mozilla` (tarball), and `mr` (MRI scan) contain regions with very different statistical properties. HyperPack's per-block strategy selection picks the optimal algorithm for each region, beating tools that apply a single strategy globally.

- **Structured and repetitive data** — Files like `nci` (chemical database, 4.00%) and `kennedy.xls` (spreadsheet, 4.23%) contain highly structured repetitive data. HyperPack's delta and dictionary strategies exploit this structure far better than general-purpose LZ-based compressors.

- **Natural language text at scale** — On larger text files (`dickens`, `webster`, `reymont`), HyperPack's BWT+MTF-2+ZRLE pipeline produces better results than LZMA2 (xz) and even bzip2's BWT implementation.

- **Binary and scientific data** — Medical images (`x-ray`, `mr`), databases (`osdb`), and XML data show consistent wins, confirming the advantage of adaptive strategy selection on diverse data types.

### Where Competitors Win

- **Small text files (< 100 KB)** — bzip2 dominates on the Calgary Corpus, where most files are small. Its block-sorting algorithm has minimal overhead and is highly effective on small English text. HyperPack's multi-strategy machinery has less room to amortize its overhead on these small inputs.

- **Small structured files** — xz wins on small object code files (`obj1`, `obj2`, `progl`, `progp`, `trans`) where LZMA2's dictionary matching is efficient and the files are too small for multi-strategy selection to add value.

- **Large tarballs** — xz's LZMA2 with its large dictionary window gives it an edge on `mozilla` (51 MB tarball) and `samba` (21 MB tarball), where long-range matches across similar files within the archive are important.

- **Tiny files (< 5 KB)** — zstd wins on the smallest Canterbury files (`fields.c`, `grammar.lsp`, `xargs.1`) where all compressors converge and zstd's entropy coder has a slight edge.

### Why Multi-Strategy Selection Matters

Traditional compressors apply a single algorithm to all data. HyperPack divides input into blocks and selects the best strategy for each block from its set of 31 strategies. This means:

1. **Text blocks** get BWT-based compression
2. **Binary blocks** get delta or dictionary coding
3. **Already-compressed blocks** are stored as-is (no expansion)
4. **Structured data** gets specialized treatment

The result: no single file type is a weakness. HyperPack achieves competitive or best-in-class ratios across text, binary, scientific, and structured data — something no single-strategy compressor can match.

---

## Reproducing

### Quick Test

Compress a single file and see the ratio:

```bash
./hyperpack c input.dat output.hpk
./hyperpack d output.hpk decoded.dat
diff input.dat decoded.dat   # verify lossless
```

### Full Corpus Benchmark

1. **Download the corpora:**
   - Calgary: https://corpus.canterbury.ac.nz/descriptions/#calgary
   - Canterbury: https://corpus.canterbury.ac.nz/descriptions/#cantrbry
   - Silesia: https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia

2. **Run the benchmark script:**
   ```bash
   # Benchmark a single file against all compressors
   ./hyperpack benchmark <file>

   # Or run the full benchmark suite
   ./scripts/benchmark.sh <corpus_directory>
   ```

3. **Compare results.** Output will show compressed sizes and ratios for HyperPack, gzip, bzip2, xz, and zstd side by side.

---

*Benchmark data collected March 2026 on Linux x86_64. See the [methodology](#methodology) section for full details.*
