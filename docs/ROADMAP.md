# HyperPack Roadmap

## ✅ Completed (v11)

| Phase | Feature | Impact |
|-------|---------|--------|
| **Phase 1** | LZMA forced on small blocks + BCJ E8/E9 filter | +2.1% ratio (Canterbury), +1.3% (Calgary) |
| **Phase 2** | Parallel compression (`-j N`) | 1.69x speedup with `-j 4` |
| **Phase 3** | Adaptive LZMA chain depth + progressive early-exit | 1.62x faster than Phase 1, <0.5% ratio loss |
| **Phase 4** | Pure text detection + LZMA/CM hash tables enlarged | +0.3% ratio, +9% speed on text |

## 🔮 Future Opportunities

### Neural Context Mixer + Word Model (High Impact)

**Current limitation:** HyperPack's CM uses static-weight mixing of 7 byte-level models (order 0-6). This is good but not optimal for natural language text, where bzip2 still wins by ~3%.

**Proposed solution:**
1. **Neural mixer**: Replace static weights with a logistic regression mixer that learns per-bit which model is most reliable. Updates weights after each bit via gradient descent.
2. **Word model**: Add a word-level predictor that uses the last N words as context (instead of N bytes). Captures grammatical structure that byte models miss.

**Expected gains:**
- +5-8% on literary text (alice29, book1, plrabn12)
- Would close the gap with bzip2 on small text files
- Cost: ~2x slower on CM blocks, ~400 lines of C

### ANS Entropy Coder

Replace the Range Coder with ANS (Asymmetric Numeral Systems) for ~2x faster decompression. Compression ratio unchanged.

### Rejected Optimizations

| Technique | Result | Why |
|-----------|--------|-----|
| LZMA dictionary enlargement | Already 64 MB | No room to improve |
| CM O3 hash enlargement | ~0% gain | CM not limited by hash collisions at this scale |
| E8/E9 on BWT output | -1.5% to -19% | Harmful — but E8/E9 before LZMA works great |
| Transpose/bit-plane transforms | -54% to -75% | Catastrophic — destroys BWT patterns |
