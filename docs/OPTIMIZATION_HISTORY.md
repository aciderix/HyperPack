# Optimization History

Systematic testing of 10 potential improvements to HyperPack v3.0.
Only 2 out of 8 tested approaches produced improvements. Lesson: **test > theory**.

## ✅ Test 1.1 — LZMA 64 MB Threshold

**Hypothesis:** LZMA's 8 MB limit prevents it from running on large binaries where it excels.

**Result: SUCCESS (+7.2%)**

| File | Before | After | Gain |
|---|---|---|---|
| mozilla (48.8 MB) | 2.88x | **3.60x** | +25% |
| samba (20.6 MB) | 4.44x | **5.12x** | +15% |
| osdb (9.6 MB) | 3.69x | **3.94x** | +7% |

This single change is responsible for 88% of the total improvement.

## ❌ Test 1.2 — Context Mixing / PPM after BWT

**Result: FAILURE (-19% to -66%)**

BWT output after ZRLE is extremely sparse. Context Mixing adds overhead without benefit.

## ❌ Test 1.3 — Order-4 Range Coder

**Result: COUNTER-PRODUCTIVE (-12% to -35%)**

Order-4 contexts cause severe dilution.

## ✅ Test 2.1 — MTF-2 Variant

**Result: SMALL WIN (+0.39%)**

Consistent improvement across all BWT files.

## ❌ Test 2.2 — LZP Longer Context

**Result: NO IMPROVEMENT.** LZP + BWT never beats BWT alone.

## ❌ Test 2.3 — Adaptive Block Size

**Result: NO GAIN.** Reducing block size hurts BWT ratio (-11% at 16 MB).

## ⏭️ Tests 2.4, 3.1 — Skipped

CM optimization skipped (proven weak). XML preprocessing skipped (BWT already 12.29x).

## ⚡ Speed: Smart LZMA Heuristic

Skip LZMA if `entropy < 5.0` OR `(ASCII% > 95% AND entropy < 6.0)` → **2.1x speedup, 0% loss**.

## ⚡ Speed: Sample-based Strategy Selection

Test 1 MB sample → run only winner on full block → **+10% speedup**.
