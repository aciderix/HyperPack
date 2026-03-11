# Roadmap

## Completed

### v11 — Multi-threaded Compression & LZMA Improvements

| Phase | Feature | Impact |
|-------|---------|--------|
| Phase 1 | LZMA forced on small blocks + BCJ E8/E9 filter | +2.1% ratio (Canterbury), +1.3% (Calgary) |
| Phase 2 | Parallel compression (`-j N`) | 1.69x speedup with `-j 4` |
| Phase 3 | Adaptive LZMA chain depth + progressive early-exit | 1.62x faster than Phase 1, <0.5% ratio loss |
| Phase 4 | Pure text detection + LZMA/CM hash table enlargement | +0.3% ratio, +9% speed on text |

### v12 — Web & Desktop Platform

| Feature | Details |
|---------|---------|
| WASM build | Emscripten compilation, runs in browser |
| Web app | React + TypeScript + Tailwind, drag-and-drop UI |
| Tauri desktop | Native builds for Windows, macOS, Linux |
| CI/CD | GitHub Actions: build, test, deploy to Pages, release binaries |
| PWA | Offline-capable web app with service worker |

### v12.1 — Manual Strategy Control

| Feature | Details |
|---------|---------|
| Force strategy (`-s N`) | Bypass auto-selection, compress directly with strategy N |
| Include filter (`-S N,N,...`) | Auto-select only among listed strategies |
| Exclude filter (`-X N,N,...`) | Auto-select among all except listed strategies |
| List strategies (`--list-strategies`) | Display all 31 strategies with IDs |
| LZMA heuristic override | `-S` bypasses internal skip heuristics |
| Group-level skipping | Skip entire BWT/Delta/LZ77 groups when unused |
| Library API | New filtered/forced compression functions |

## Future Opportunities

### Neural Context Mixer + Word Model

**Status:** Research  
**Expected impact:** +5–8% on literary text

HyperPack's current CM uses static-weight mixing of 7 byte-level models (order 0–6). A neural mixer (logistic regression, per-bit weight updates via gradient descent) combined with a word-level predictor would capture grammatical structure that byte-level models miss.

This would close the remaining gap with bzip2 on small text files like `alice29.txt` and `book1`. Estimated cost: ~2x slower on CM blocks, ~400 lines of C.

### ANS Entropy Coder

**Status:** Planned  
**Expected impact:** ~2x faster decompression, identical ratios

Replace the Range Coder with Asymmetric Numeral Systems (ANS/rANS) for the main entropy coding path. The rANS coder already exists for some strategies — extending it to all BWT pipelines would significantly improve decompression throughput.

### Streaming Mode

**Status:** Considered  
**Expected impact:** Enables piped workflows

Add stdin/stdout streaming support for integration with Unix pipelines (`tar cf - dir/ | hyperpack c -`). Requires changes to the block buffering logic.

### WASM Multi-threading

**Status:** Blocked on browser support  
**Expected impact:** 2–3x faster web compression

SharedArrayBuffer + Web Workers for parallel block compression in the browser. Currently blocked by cross-origin isolation requirements and inconsistent browser support.

## Rejected Optimizations

These were tested and found unhelpful or harmful:

| Technique | Result | Why |
|-----------|--------|-----|
| LZMA dictionary > 64 MB | No improvement | Already at the sweet spot |
| CM order-3 hash enlargement | ~0% gain | Not limited by hash collisions at this scale |
| E8/E9 on BWT output | −1.5% to −19% | Destroys BWT patterns (but works great before LZMA) |
| Transpose / bit-plane transforms | −54% to −75% | Catastrophic — destroys BWT byte correlations |
| MTF on LZP output | −2% to −5% | LZP output is already decorrelated |
| O2 arithmetic on all strategies | Mixed | Only helps on high-entropy data; hurts on structured |
