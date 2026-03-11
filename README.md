<p align="center">
  <img src="hyperpack-web/public/icon-512.png" alt="HyperPack Logo" width="128" height="128">
</p>

<h1 align="center">HyperPack</h1>

<p align="center">
  <strong>Multi-strategy lossless data compression — beats xz on standard benchmarks.</strong>
</p>

<p align="center">
  <a href="https://github.com/aciderix/HyperPack/actions/workflows/build.yml"><img src="https://github.com/aciderix/HyperPack/actions/workflows/build.yml/badge.svg" alt="Build"></a>
  <a href="https://github.com/aciderix/HyperPack/releases/latest"><img src="https://img.shields.io/github/v/release/aciderix/HyperPack?include_prereleases&label=release" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="License: MIT"></a>
  <a href="https://aciderix.github.io/HyperPack/"><img src="https://img.shields.io/badge/Try_it-Online-brightgreen" alt="Try Online"></a>
</p>

<p align="center">
  <a href="#-quick-start">Quick Start</a> ·
  <a href="#-benchmarks">Benchmarks</a> ·
  <a href="#-how-it-works">How it Works</a> ·
  <a href="#-web-app--desktop">Web & Desktop</a> ·
  <a href="docs/">Documentation</a>
</p>

---

HyperPack is a lossless data compressor written in C that analyzes each data block and selects the best compression strategy from 31 specialized pipelines. Instead of applying one fixed algorithm, it picks the optimal chain of transforms (BWT, LZMA, PPM, LZP, Delta, Audio PCM…) per block — achieving **best-in-class compression ratios** on standard benchmarks.

## ✨ Highlights

- 🏆 **Beats xz -9** on all 3 standard corpora (Calgary, Canterbury, Silesia) in aggregate
- 🧠 **31 compression strategies** — automatic per-block selection
- ⚡ **Parallel compression** — multi-threaded with `-j N`
- 🎯 **Manual strategy control** — force, include, or exclude strategies
- 📁 **Archive mode** — compress directories with metadata preservation
- 🌐 **Web app** — runs in the browser via WebAssembly
- 🖥️ **Desktop app** — native Tauri builds for Windows, macOS, Linux
- 📦 **Single C file** — ~7,000 lines, zero dependencies beyond zlib

## 🚀 Quick Start

### Download

Pre-built binaries are available on the [Releases page](https://github.com/aciderix/HyperPack/releases).

| Platform | File |
|----------|------|
| Linux x64 | `hyperpack-linux-x64` |
| macOS ARM | `hyperpack-macos-arm64` |
| macOS x64 | `hyperpack-macos-x64` |
| Windows | `hyperpack-windows-x64.exe` |

### Build from Source

```bash
# Requirements: gcc (or clang), make, zlib
git clone https://github.com/aciderix/HyperPack.git
cd HyperPack
make
```

### Usage

```bash
# Compress a file
./hyperpack compress file.dat

# Compress with parallel threads
./hyperpack compress -j 4 largefile.bin

# Force a specific compression strategy
./hyperpack c -s 24 file.dat out.hpk        # Force LZMA

# Auto-select among a subset of strategies
./hyperpack c -S 1,2,5,6 file.dat out.hpk   # Only try BWT variants

# Auto-select excluding specific strategies
./hyperpack c -X 24,25 file.dat out.hpk      # Skip LZMA (too slow)

# List all available strategies
./hyperpack --list-strategies

# Decompress
./hyperpack decompress file.dat.hpk

# Compress a directory (archive mode)
./hyperpack archive my_folder/
# or: ./hyperpack a my_folder/ my_folder.hpk

# Extract an archive
./hyperpack extract my_folder.hpk output_dir/
# or: ./hyperpack x my_folder.hpk output_dir/

# List archive contents
./hyperpack list my_folder.hpk
# or: ./hyperpack l my_folder.hpk

# Show file info (coming soon)
# ./hyperpack info file.dat.hpk

# Benchmark mode (coming soon)
# ./hyperpack benchmark file.dat
```

See [docs/TECHNICAL.md](docs/TECHNICAL.md) for the full CLI reference and advanced options.

## 📊 Benchmarks

Tested against gzip -9, bzip2 -9, xz -9, and zstd -19 on the three standard compression corpora.

**Compressed size as % of original — lower is better:**

| Corpus | Files | Size | HyperPack | gzip -9 | bzip2 -9 | xz -9 | zstd -19 |
|--------|------:|-----:|:---------:|:-------:|:--------:|:-----:|:--------:|
| Calgary | 18 | 3.3 MB | **26.0%** | 32.6% | 26.7% | 27.2% | 28.3% |
| Canterbury | 11 | 2.8 MB | **16.0%** | 26.0% | 19.3% | 17.5% | 18.4% |
| Silesia | 12 | 212 MB | **21.9%** | 31.9% | 25.7% | 23.0% | 25.0% |
| **Overall** | **41** | **218 MB** | **21.9%** | **31.8%** | **25.7%** | **23.0%** | **24.9%** |

> HyperPack achieves the **best aggregate compression on all three corpora**, reducing data ~5% more than xz -9 on Silesia — the most demanding modern benchmark (202 MB of mixed real-world data).

<details>
<summary><strong>Individual file wins (41 files)</strong></summary>

| Tool | Wins | Share |
|------|-----:|------:|
| bzip2 -9 | 16 | 39% |
| **HyperPack** | **12** | **29%** |
| xz -9 | 10 | 24% |
| zstd -19 | 3 | 7% |

bzip2 wins on many small text files where BWT shines at minimal overhead.
HyperPack dominates on larger, heterogeneous files where multi-strategy selection pays off.

</details>

Full results with per-file breakdowns: [docs/BENCHMARKS.md](docs/BENCHMARKS.md)

## 🧠 How it Works

HyperPack's core innovation is **per-block strategy selection**:

```
Input → Block Splitter (64–128 MB)
            ↓
    ┌── Content Analysis ──┐
    │  Detect type per block│
    │  (text, binary, audio,│
    │   base64, float32…)   │
    └───────┬───────────────┘
            ↓
    ┌── Strategy Trial ────┐
    │  Sample-based testing │
    │  of candidate chains  │
    └───────┬───────────────┘
            ↓
    Best Strategy Applied
    (1 of 31 pipelines)
            ↓
    Entropy Coder
    (Arithmetic / rANS)
            ↓
    .hpk output
```

Each strategy is a **pipeline of transforms**. For example:

| Strategy | Pipeline | Best For |
|----------|----------|----------|
| S_BWT_ARITH_O1 | BWT → MTF → ZRLE → Arithmetic O1 | General text |
| S_LZMA | LZMA (64 MB dict) | Executables, structured data |
| S_PPM_O6 | PPM order-6 | High-entropy text |
| S_DELTA_BWT | Delta → BWT → MTF → ZRLE → Arith | Time-series, sensor data |
| S_AUDIO_PCM | PCM detect → channel split → delta → entropy | Raw audio |
| S_BASE64V2 | Base64 decode → compress → re-encode | Base64 payloads |

The selector uses **sample-based heuristics** (compresses a small sample with each candidate) to pick the winner without full trial compression — making it fast even on large files.

→ Deep dive: [docs/TECHNICAL.md](docs/TECHNICAL.md) — all 31 strategies, algorithm details, format specification.

## 🌐 Web App & Desktop

### Try it Online

**[▶ Launch HyperPack Web](https://aciderix.github.io/HyperPack/)**

The web app runs entirely in your browser via WebAssembly. No upload, no server — your files stay local.

### Desktop App (Tauri)

Native builds with full performance. Download from [Releases](https://github.com/aciderix/HyperPack/releases):
- **Windows** — `.msi` installer
- **macOS** — `.dmg` (ARM & Intel)
- **Linux** — `.deb` / `.AppImage`

### Platform Comparison

| Feature | CLI | Desktop (Tauri) | Web (WASM) |
|---------|:---:|:---------------:|:----------:|
| Multi-threading | ✅ `-j N` | ✅ native | ❌ single thread |
| Max block size | 128 MB | 128 MB | 64 MB |
| Archive mode | ✅ | ✅ | ❌ |
| Drag & drop | — | ✅ | ✅ |
| Strategy control | ✅ `-s`/`-S`/`-X` | ✅ | ❌ (auto only) |
| PNG optimization | ✅ | ❌ | ❌ |
| Offline | ✅ | ✅ | ✅ (PWA) |

Full comparison: [docs/WEB_VS_NATIVE.md](docs/WEB_VS_NATIVE.md)

## 📁 Project Structure

```
HyperPack/
├── src/
│   ├── hyperpack.c          # Main CLI — all algorithms (~7,000 lines)
│   ├── hyperpack_lib.c      # Library wrapper (compress/decompress API)
│   └── hyperpack_wasm.c     # WASM bindings (Emscripten)
├── hyperpack-web/           # React + TypeScript web app
│   ├── src-tauri/           # Tauri desktop wrapper
│   └── public/worker.js     # WASM Web Worker
├── docs/                    # Technical documentation
├── benchmark/               # Benchmark scripts & results
├── .github/workflows/       # CI/CD (build + release)
├── Makefile                 # Build system
└── build-wasm.sh            # WASM build script
```

## 📖 Documentation

| Document | Description |
|----------|-------------|
| [Technical Deep Dive](docs/TECHNICAL.md) | Algorithms, strategies, format spec, CLI reference |
| [Benchmarks](docs/BENCHMARKS.md) | Full benchmark results on standard corpora |
| [Architecture](docs/ARCHITECTURE.md) | Build system, platforms, CI/CD |
| [Web vs Native](docs/WEB_VS_NATIVE.md) | Platform differences and limitations |
| [Roadmap](docs/ROADMAP.md) | Completed phases and future plans |
| [Optimization History](docs/OPTIMIZATION_HISTORY.md) | What worked, what didn't |
| [Changelog](CHANGELOG.md) | Version history |

## 🤝 Contributing

Contributions are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) before submitting a pull request.

## 📄 License

This project is licensed under the [MIT License](LICENSE).

---

<p align="center">
  <sub>Made with obsessive attention to compression ratios.</sub>
</p>
