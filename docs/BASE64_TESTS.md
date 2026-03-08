# Base64 Compression Tests

## Context

A common question: can HyperPack compress Base64-encoded data effectively?
Base64 encoding inflates data by ~33%, so naively one would expect worse compression.

**TL;DR: Yes, and on text data it actually compresses _better_ than the binary original.**

## Test Protocol

- Take Silesia corpus files (binary originals)
- Encode them to Base64 (`base64` command)
- Compress both versions with HyperPack v10.2
- Compare compressed sizes and ratios

## Results

| File | Type | Original Size | Binary → HP | Base64 → HP | Overhead |
|------|------|--------------|-------------|-------------|----------|
| **xml** | XML | 5.1 MB | 441 KB (12.12×) | **433 KB** (16.67×) | **−2%** 🟢 |
| **dickens** | Text | 9.7 MB | 2,524 KB (4.04×) | **2,505 KB** (5.50×) | **−1%** 🟢 |
| **Audio PCM** | Sine wave | 512 KB | 4.5 KB (115.9×) | 4.7 KB (151.1×) | **+4%** 🟡 |

## Analysis

### Why Base64 compresses better on text

This is counter-intuitive but makes sense:

1. **Base64 reduces the alphabet to 64 characters** (A-Z, a-z, 0-9, +, /)
2. **BWT thrives on small alphabets** — it finds repetition patterns more easily
3. The +33% size inflation from Base64 is **more than compensated** by better BWT compression

### Audio in Base64

- HyperPack's audio filter cannot activate on Base64 (it looks like text)
- BWT still compensates well → only **+4% overhead**
- For audio, decoding Base64 first is recommended to leverage the audio filter

## Practical Recommendations

| Situation | Recommendation |
|-----------|---------------|
| You have the binary original | **Compress the binary** (always optimal for non-text) |
| You only have Base64 | **Compress it directly** — works great! |
| Should you decode Base64 first? | Not needed for text; recommended for audio/binary |

## Key Takeaway

HyperPack's BWT-based architecture handles Base64 remarkably well. The reduced alphabet
(64 chars vs 256 bytes) creates more exploitable patterns for the Burrows-Wheeler Transform.
On text data, the compressed output is actually **smaller** than compressing the binary original.
