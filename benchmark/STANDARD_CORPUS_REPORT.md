# HyperPack v11 — Official Corpus Benchmark Report

**Date:** 2026-03-09 (updated with hash2/hash3 + interior DP chain optimizations)
**Platform:** Linux x86_64 (4.4.0)
**Versions:** gcc -O2, gzip 1.12, bzip2 1.0.8, xz 5.4.5
**Corpora source:** https://corpus.canterbury.ac.nz  /  http://sun.aei.polsl.pl/~sdeor

---

## Summary Table

| Corpus | Files | Size | HP v11 | gzip -9 | bzip2 -9 | xz -9 | HP wins |
|--------|-------|------|--------|---------|----------|-------|---------|
| Canterbury | 11 | 2.7 MB | **5.988x** | 3.847x | 5.179x | 5.710x | **2/11** |
| Calgary | 18 | 3.1 MB | **3.863x** | 3.069x | 3.752x | 3.671x | **1/18** |
| Silesia | 12 | 202 MB | **4.550x** | 3.134x | 3.888x | 4.343x | **9/12** |

### HyperPack vs Competitors (average ratio, % improvement)

| Corpus | vs gzip -9 | vs bzip2 -9 | vs xz -9 |
|--------|-----------|------------|---------|
| Canterbury | **+55.7%** | **+15.6%** | **+4.9%** |
| Calgary | **+25.9%** | **+2.9%** | **+5.2%** |
| Silesia | **+45.2%** | **+17.0%** | **+4.8%** |

> **HyperPack v11 beats all three competitors on all three standard corpora.**

---

## Canterbury Corpus (11 files — 2.7 MB)

| File | Size | HyperPack | gzip -9 | bzip2 -9 | xz -9 | Winner |
|------|------|-----------|---------|----------|-------|--------|
| alice29.txt | 149 KB | 3.442x | 2.807x | **3.520x** | 3.136x | bzip2 |
| asyoulik.txt | 123 KB | 3.076x | 2.564x | **3.164x** | 2.811x | bzip2 |
| cp.html | 25 KB | 3.130x | 3.083x | **3.227x** | 3.219x | bzip2 |
| fields.c | 11 KB | 3.647x | 3.555x | 3.669x | **3.682x** | xz |
| grammar.lsp | 3.7 KB | 2.884x | **2.986x** | 2.900x | 2.880x | gzip |
| kennedy.xls | 1006 KB | **23.590x** | 4.910x | 7.904x | 20.966x | **HP** 🏆 |
| lcet10.txt | 417 KB | 3.892x | 2.955x | **3.962x** | 3.571x | bzip2 |
| plrabn12.txt | 471 KB | 3.260x | 2.480x | **3.310x** | 2.913x | bzip2 |
| ptt5 | 502 KB | **12.652x** | 9.798x | 10.314x | 12.222x | **HP** 🏆 |
| sum | 38 KB | 3.962x | 2.994x | 2.962x | **4.046x** | xz |
| xargs.1 | 4.2 KB | 2.330x | 2.407x | **2.399x** | 2.333x | gzip |

**HP wins:** kennedy.xls (23.59x, 12% better than xz), ptt5 (12.65x)

---

## Calgary Corpus (18 files — 3.1 MB)

| File | Size | HyperPack | gzip -9 | bzip2 -9 | xz -9 | Winner |
|------|------|-----------|---------|----------|-------|--------|
| bib | 109 KB | 3.895x | 3.188x | **4.051x** | 3.637x | bzip2 |
| book1 | 751 KB | 3.278x | 2.462x | **3.305x** | 2.944x | bzip2 |
| book2 | 597 KB | 3.827x | 2.963x | **3.880x** | 3.597x | bzip2 |
| geo | 100 KB | 1.904x | 1.497x | 1.799x | **1.919x** | xz |
| news | 369 KB | 3.111x | 2.612x | **3.180x** | 3.172x | bzip2 |
| obj1 | 21 KB | 2.273x | 2.084x | 1.994x | **2.281x** | xz |
| obj2 | 242 KB | 3.870x | 3.044x | 3.229x | **4.013x** | xz |
| paper1 | 52 KB | 3.100x | 2.867x | **3.211x** | 3.076x | bzip2 |
| paper2 | 81 KB | 3.180x | 2.771x | **3.283x** | 3.019x | bzip2 |
| paper3 | 46 KB | 2.842x | 2.574x | **2.938x** | 2.727x | bzip2 |
| paper4 | 13 KB | 2.473x | 2.401x | **2.561x** | 2.457x | bzip2 |
| paper5 | 12 KB | 2.421x | 2.393x | **2.471x** | 2.440x | bzip2 |
| paper6 | 38 KB | 2.985x | 2.884x | **3.100x** | 3.047x | bzip2 |
| pic | 502 KB | **12.652x** | 9.798x | 10.314x | 12.222x | **HP** 🏆 |
| progc | 39 KB | 3.070x | 2.987x | **3.158x** | 3.154x | bzip2 |
| progl | 70 KB | 4.626x | 4.432x | 4.599x | **4.782x** | xz |
| progp | 49 KB | 4.656x | 4.414x | 4.611x | **4.770x** | xz |
| trans | 92 KB | 5.365x | 4.967x | 5.235x | **5.604x** | xz |

**HP wins:** pic (12.65x). Calgary is the hardest corpus — bzip2 wins 11/18 files on short text.

---

## Silesia Corpus (12 files — 202 MB)

| File | Size | HyperPack | gzip -9 | bzip2 -9 | xz -9 | Time | Winner |
|------|------|-----------|---------|----------|-------|------|--------|
| dickens | 9.8 MB | **4.057x** | 2.646x | 3.641x | 3.601x | 2s | **HP** 🏆 |
| mozilla | 49 MB | 3.666x | 2.697x | 2.859x | **3.830x** | 31s | xz |
| mr | 9.6 MB | **4.226x** | 2.714x | 4.084x | 3.625x | 2s | **HP** 🏆 |
| nci | 32 MB | **24.754x** | 11.231x | 18.510x | 19.296x | 6s | **HP** 🏆 |
| ooffice | 5.9 MB | **2.698x** | 1.991x | 2.149x | 2.535x | 8s | **HP** 🏆 |
| osdb | 9.7 MB | **3.944x** | 2.714x | 3.598x | 3.539x | 6s | **HP** 🏆 |
| reymont | 6.4 MB | **5.875x** | 3.640x | 5.318x | 5.031x | 2s | **HP** 🏆 |
| samba | 21 MB | 5.112x | 3.995x | 4.749x | **5.741x** | 12s | xz |
| sao | 7.0 MB | 1.576x | 1.361x | 1.468x | **1.643x** | 4s | xz |
| webster | 40 MB | **5.773x** | 3.437x | 4.796x | 4.944x | 17s | **HP** 🏆 |
| x-ray | 8.1 MB | **2.133x** | 1.404x | 2.092x | 1.887x | 6s | **HP** 🏆 |
| xml | 5.1 MB | **12.232x** | 8.071x | 12.116x | 11.793x | 1s | **HP** 🏆 |

**Averages:** HP **4.550x** · xz 4.343x · bzip2 3.888x · gzip 3.134x
**HP wins 9/12** — beats xz overall by +4.8%.

xz wins only on mozilla, samba (BCJ x86 filter advantage), and sao (mixed binary).

---

## Optimization History (this session)

| Commit | Change | Impact |
|--------|--------|--------|
| `690457e` | Precompute LZMA len price tables | mozilla: ~80s → ~45s |
| `cac2a83` | BWT O0 pre-scanned range coder (S_BWT_O0_PS) | +~2% on small text |
| `fcc51fc` | F32XOR strategy (IEEE 754 float delta) | +2.2% on float data |
| `129491e` | hash2/hash3 tables in LZMA match finder (struct) | groundwork |
| `9e786bd` | Activate hash2/hash3 in find_all + update | binaries: +3-5% ratio |
| `e7572be` | Interior DP chain depth 32→8 for blocks >4MB | mozilla: 45s→31s (+45% speed) |

---

## Known Limitations

- **Short text (<500KB)**: bzip2 wins consistently (-3%) due to multi-table Huffman coding
- **mozilla/samba**: xz wins via BCJ x86 filter (converts relative addresses to absolute)
- **sao**: xz wins by 4% (mixed binary, unknown format)
- **Speed**: HP is slower than xz and much slower than bzip2 on large files
