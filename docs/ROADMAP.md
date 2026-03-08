# HyperPack — Roadmap & Optimization Status

## ✅ Implemented (v10.2)

| Technique | Impact | Version |
|-----------|--------|---------|
| LZMA 64MB dictionary | **+7.2%** ratio | v10.1 |
| MTF-2 variant | **+0.39%** ratio | v10.1 |
| Smart LZMA heuristic | **2.1× faster** | v10.1 |
| Sample-based strategy selection | ~10% faster | v10.2 |

## ❌ Tested & Rejected

These techniques were tested empirically and found to be harmful or redundant:

| Technique | Predicted | Actual | Why |
|-----------|-----------|--------|-----|
| Context Mixing after BWT | "Better" | −19% to −66% | CM can't beat BWT+MTF+RC |
| Order-4 Range Coder | "Better" | −12% to −35% | Context dilution |
| E8/E9 x86 filter | +5–15% | −1.5% to −19% | False positives in non-code data |
| Transpose transform | +3–7% | **−54% to −58%** | Destroys BWT contexts |
| Bit-plane transform | +10–20% | **−65% to −75%** | Destroys all byte-level structure |
| LZP longer context | "Better" | 0% | BWT alone always wins |
| Adaptive block size | +2–10% | 0% | All files fit in 128 MB |
| Alphabet ranking | +1–3% | Redundant | Already done by MTF |
| 6-bit packing | "Gain" | Redundant | Already done by Range Coder |
| Byte remapping | +1–2% | Redundant | Already done by MTF |

See [EXPERIMENTAL_TESTS.md](EXPERIMENTAL_TESTS.md) for detailed results.

## 🔮 Remaining Possibilities

### Realistic gains

| Technique | Expected Impact | Complexity |
|-----------|----------------|------------|
| Parallel compression (`-j4`) | 3–4× speed (no ratio change) | Medium |
| libdivsufsort for BWT | 17% faster on large text | Adds dependency |
| ANS entropy coder | 2× decompression speed | High (rewrite RC) |

### Uncertain / diminishing returns

| Technique | Notes |
|-----------|-------|
| Grammar/BPE preprocessing | BWT+LZMA already capture repeated patterns |
| Columnar transform | Only useful on fixed-record data (rare in general compression) |
| BCJ with ELF/PE parsing | Complex, uncertain gains over LZMA on real archives |

## 🧠 Key Insight

After extensive testing, the conclusion is clear:

> **HyperPack v10.2's BWT + LZMA pipeline is already near-optimal for general-purpose
> compression.** The remaining gains are in speed (parallelism, faster suffix sort)
> rather than ratio. Most preprocessing transforms that work in isolation become
> harmful when applied before algorithms that already capture those same patterns.

The three improvements that worked (LZMA 64MB, MTF-2, Smart Heuristic) were all
about **tuning existing components**, not adding new transforms.

---

*Updated after 13 empirical tests on the Silesia Compression Corpus.*
