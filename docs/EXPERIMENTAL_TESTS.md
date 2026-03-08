# HyperPack v10.2 — Experimental Tests Results

> All tests conducted on the Silesia Corpus with HyperPack v10.2 (128 MB blocks).
> These experiments explored techniques commonly suggested in compression literature
> to verify whether they could improve an already-optimized BWT+LZMA pipeline.

---

## 1. E8/E9 Filter (x86 CALL/JMP Transform)

**Theory:** Convert relative x86 CALL (0xE8) and JMP (0xE9) addresses to absolute,
improving compressibility of executable code. Used by 7-zip's BCJ filter.

**Implementation:** Scanned for E8/E9 opcodes, converted relative offsets to absolute
addresses based on position in stream. Tested three variants:
- E8+E9 (all CALL and JMP)
- E8-only (CALL only, fewer false positives)
- Smart filtering (skip unlikely opcodes)

### Results

| File | Size | Direct HP | E8+E9 + HP | E8-only + HP |
|------|------|-----------|------------|--------------|
| mozilla | 51.2 MB | 14.2 MB (3.60x) | 14.7 MB (3.48x) | 14.4 MB (3.55x) |
| samba | 21.6 MB | 4.5 MB (4.79x) | — | 4.2 MB (5.13x) |
| ooffice | 6.2 MB | 2.0 MB (3.14x) | — | 2.3 MB (2.64x) |

### Analysis

- **mozilla**: −1.5% to −3.5% — Archive (TAR), E8 hits false positives in non-code data
- **samba**: +6.5% — Higher density of ELF executables in the TAR
- **ooffice**: −19% — Real PE32 DLL, but LZMA already handles x86 patterns well

**Verdict: ❌ Not beneficial.** The naive E8/E9 filter hurts more than it helps because:
1. Silesia's "executables" (mozilla, samba) are actually TAR archives containing mixed data
2. LZMA's match finder already captures x86 instruction patterns effectively
3. False positives in non-code sections add noise that degrades compression

A proper BCJ filter would need ELF/PE section parsing to only transform `.text` segments,
adding significant complexity for uncertain gains.

---

## 2. Transpose Transform (Column/Byte Interleave)

**Theory:** For data with fixed-width records, rearranging bytes by column position
groups similar values together, creating longer runs for BWT. Used in libbsc and
scientific data compressors.

**Implementation:** For width W, byte at position i goes to position
`(i % W) * (size/W) + (i / W)`. Tested widths: 4, 8, 16.

### Results

| File | Direct HP | Transpose w=4 | Change |
|------|-----------|---------------|--------|
| sao (star catalog, fixed records) | 1.55x | 1.27x | **−18%** |
| dickens (prose text) | 4.03x | 1.69x | **−58%** |
| xml (structured markup) | 12.11x | 5.54x | **−54%** |

### Analysis

**Catastrophic on all files tested.** The transpose destroys the natural byte-level
context that BWT exploits:
- Text like `<author>Dickens</author>` becomes `<tcs DiDais>ukno/ther`
- BWT can no longer group similar suffixes
- Even `sao` (fixed-width records) loses because LZMA already captures record structure

**Verdict: ❌ Severely harmful.** Transpose is useful in specialized scientific formats
where records are perfectly aligned and compression is column-based (like HDF5),
but it's counterproductive before BWT or LZMA.

---

## 3. Bit-Plane Transform

**Theory:** Separate each byte into 8 bit planes. High-order bits change slowly,
creating long runs. Used in FLAC (audio) and some image codecs.

**Implementation:** For each bit position 0-7, extract that bit from every byte
and pack into a separate stream. Concatenate all 8 streams.

### Results

| File | Direct HP | Bit-plane + HP | Change |
|------|-----------|----------------|--------|
| dickens (prose text) | 4.03x | 1.41x | **−65%** |
| xml (structured markup) | 12.11x | 3.08x | **−75%** |

### Analysis

**Even worse than transpose.** Bit-plane separation destroys ALL byte-level structure:
- Text becomes 8 seemingly random bit streams
- Neither BWT nor LZMA can find meaningful patterns in individual bit planes of text
- Works in audio/image codecs because those have strong bit-level correlations
  (PCM samples where high bits are almost constant) — text has no such property

**Verdict: ❌ Devastating.** HyperPack already has specialized audio filters
(mid/side + delta + LPC) that capture bit-level correlations far more effectively.

---

## 4. Previously Tested Techniques (v10.1 Optimization Phase)

| Test | Technique | Predicted | Result |
|------|-----------|-----------|--------|
| 1.2 | Context Mixing after BWT | "Better" | ❌ −19% to −66% |
| 1.3 | Order-4 Range Coder | "Better" | ❌ −12% to −35% |
| 2.2 | LZP with longer context | "Better" | ❌ No improvement |
| 2.3 | Adaptive block size | "+2-10%" | ❌ 0% gain |

See `results/` directory for detailed reports on tests 1.2 and 1.3.

---

## 5. Techniques Not Tested (Redundant)

Several suggested techniques were already implemented in HyperPack under different names:

| Suggestion | Why It's Redundant |
|---|---|
| Alphabet ranking / Symbol remapping | **Already done by MTF** (Move-to-Front) after BWT |
| 6-bit packing | **Already done by Range Coder** (entropy-optimal bit allocation) |
| Byte remapping by frequency | **Already done by MTF** (frequent symbols → small indices) |
| Grammar/BPE preprocessing | **BWT + LZMA already capture** repeated patterns and long matches |
| Block segmentation | **Already tested** (test 2.3): 0% gain, all files fit in 128 MB block |

---

## Summary: Theory vs Practice

| # | Technique | Predicted Gain | Actual Result | Status |
|---|-----------|---------------|---------------|--------|
| 1 | LZMA 64MB dictionary | Uncertain | **+7.2%** | ✅ **Kept** |
| 2 | MTF-2 variant | Small | **+0.39%** | ✅ **Kept** |
| 3 | Smart LZMA heuristic | Speed only | **2.1x faster** | ✅ **Kept** |
| 4 | Context Mixing after BWT | "Better" | −19% to −66% | ❌ |
| 5 | Order-4 Range Coder | "Better" | −12% to −35% | ❌ |
| 6 | E8/E9 x86 filter | "+5-15%" | −1.5% to −19% | ❌ |
| 7 | Transpose transform | "+3-7%" | **−54% to −58%** | ❌ |
| 8 | Bit-plane transform | "+10-20%" | **−65% to −75%** | ❌ |
| 9 | Alphabet ranking | "+1-3%" | Redundant (MTF) | ❌ |
| 10 | 6-bit packing | "Gain" | Redundant (Range Coder) | ❌ |
| 11 | Byte remapping | "+1-2%" | Redundant (MTF) | ❌ |
| 12 | Block segmentation | "+2-10%" | 0% (test 2.3) | ❌ |
| 13 | Grammar/BPE | "+10-25%" | Skipped (redundant with BWT+LZMA) | ⏭️ |

**Only 3 out of 13 techniques provided improvement**, and all three were discovered
through systematic empirical testing.

---

## Key Lessons

1. **BWT + LZMA is already extremely powerful.** Most "preprocessing" transforms destroy
   the patterns these algorithms exploit rather than enhancing them.

2. **Theoretical gains rarely survive contact with an optimized pipeline.** Techniques that
   work in isolation (bit-plane for audio, transpose for columnar data) become harmful
   when applied before algorithms that already capture those patterns differently.

3. **Test everything empirically.** Compression theory is valuable for understanding *why*
   things work, but only benchmarks reveal *whether* they work in a specific pipeline.

4. **Know what your compressor already does.** Several suggestions were already implemented
   under different names (MTF = alphabet ranking, Range Coder = optimal bit packing).

5. **The biggest gains came from tuning existing components** (LZMA dictionary size,
   MTF variant, heuristic scheduling) rather than adding new transforms.

---

*All tests conducted with HyperPack v10.2 on the Silesia Compression Corpus.*
*Hardware: Cloud sandbox (single core).*
