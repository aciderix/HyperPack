# HyperPack Roadmap — Future Improvements

## Status: v10.2 Released

All systematic optimizations from v3.0 to v10.2 are complete.
Below are the most promising directions for future work, ranked by expected impact.

---

## ⭐⭐⭐ High Impact

### 1. E8/E9 Filter for x86 Executables

**Problem:** Binary executables (mozilla, samba) contain x86 CALL/JMP instructions
with **relative addresses**. These appear as semi-random data to the compressor.

**Solution:** An E8/E9 filter converts relative addresses to absolute ones before
compression. This groups similar addresses together, making them highly compressible.

**Expected gain:** +5–15% on x86 binaries (mozilla, samba, ooffice)
**Complexity:** ~50 lines of C code
**Used by:** 7-zip, BCJ filter in LZMA SDK

```
Before E8:  E8 3A 02 00 00  →  CALL [PC+0x23A]  (relative, varies by position)
After E8:   E8 7A 12 00 00  →  CALL [0x127A]     (absolute, repeats across file)
```

**Impact on Silesia benchmark:**

| File | Current | Expected with E8/E9 |
|------|---------|-------------------|
| mozilla (48.8 MB) | 3.60× | ~3.9–4.1× |
| samba (20.6 MB) | 4.79× | ~5.1–5.4× |
| ooffice (5.9 MB) | 3.14× | ~3.3–3.5× |

This is HyperPack's **single biggest remaining opportunity** — it targets exactly
the files where we're weakest compared to 7-zip.

---

## ⭐⭐ Medium Impact

### 2. Parallel Block Compression

**Problem:** An 80 MB file takes ~100s because BWT processes the entire block sequentially.

**Solution:** Split large files into N independent blocks (e.g., 4 × 20 MB) and compress
them in parallel on multi-core CPUs.

**Expected gain:** 3–4× speed improvement on large files
**Cost:** ~1–2% ratio loss (BWT benefits from large context)
**Complexity:** Medium — requires block index in file format

| File | Current (1 block) | 4 blocks parallel |
|------|-------------------|-------------------|
| classes.js (80 MB) | 103s | ~30s |
| webster (41 MB) | 10s | ~4s |

**Trade-off:** Best offered as a command-line flag (`-j4`) so users can choose
speed vs. ratio.

### 3. Columnar Detection for Structured Data

**Problem:** Structured/tabular data (CSV, databases, fixed-width records) stores
values row-by-row, mixing different data types. This is suboptimal for compression.

**Solution:** Detect record-based structure and transpose to columnar layout before
compression. Each "column" then contains homogeneous data that compresses much better.

**Expected gain:** +10–20% on structured/tabular data
**Complexity:** Medium — requires structure detection heuristic
**Used by:** BSC, Blosc

---

## ⭐ Lower Priority

### 4. ANS (Asymmetric Numeral Systems) Entropy Coder

**Problem:** The current Range Coder is effective but slower than modern alternatives
for decompression.

**Solution:** Replace Range Coder with FSE/tANS (used by zstd, LZFSE). ANS achieves
similar compression ratios but decodes significantly faster.

**Expected gain:** 2× decompression speed
**Cost:** No ratio change
**Complexity:** High — requires rewriting the entropy coding layer
**Trade-off:** Would break backward compatibility with v10.2 archives

### 5. Better LZMA Tuning

**Problem:** LZMA uses default parameters that may not be optimal for all file types.

**Solution:** Auto-tune match finder, word size, and dictionary size based on file
characteristics (similar to the existing entropy/ASCII heuristic).

**Expected gain:** 1–3% on LZMA-selected files
**Complexity:** Low — parameter search on sample

---

## Completed Optimizations (v3.0 → v10.2)

| Version | Optimization | Impact |
|---------|-------------|--------|
| v10.1 | LZMA 64 MB threshold | **+7.2%** ratio |
| v10.1 | MTF-2 (Move-to-Front variant) | **+0.4%** ratio |
| v10.1 | Smart LZMA heuristic | **2.1×** faster |
| v10.2 | Sample-based strategy selection | **+10%** faster |

### Tested and Rejected

| Test | Result | Reason |
|------|--------|--------|
| Context Mixing after BWT | ❌ −19 to −66% | CM loses to specialized BWT pipeline |
| Order-4 Range Coder | ❌ −12 to −35% | Context dilution, memory explosion |
| LZP longer context | ❌ No gain | LZP+BWT never beats BWT alone |
| Adaptive block size | ❌ No gain | All Silesia files fit in 128 MB |
| libdivsufsort | ❌ ~0% avg | Marginal gain, adds external dependency |
| Smaller BWT blocks | ❌ −11% ratio | BWT needs large context |

---

## Architecture Notes

HyperPack's multi-strategy approach (BWT, LZ77, LZMA, LZP, Delta, Audio)
automatically selects the best algorithm per block. The v10.2 sample-based
heuristic reduces the cost of this exploration from O(N × strategies) to
O(sample × strategies + N × 1).

The main performance bottleneck is the **BWT suffix array construction**
on large blocks (O(N) but with high constant factor). This is fundamental
to the algorithm and cannot be improved without switching to a different
sorting implementation or reducing block size (at the cost of ratio).
