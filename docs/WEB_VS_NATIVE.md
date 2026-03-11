# Web vs Native — Platform Comparison

HyperPack is available on three platforms, each targeting a different use case. The compression algorithm and output are identical across all three — only the runtime environment and available features differ.

## Table of Contents

- [Overview](#overview)
- [Feature Matrix](#feature-matrix)
- [Web App (WASM)](#web-app-wasm)
- [Desktop App (Tauri)](#desktop-app-tauri)
- [CLI (Native)](#cli-native)
- [Performance Comparison](#performance-comparison)
- [When to Use What](#when-to-use-what)

---

## Overview

| Platform | Description |
|----------|-------------|
| **CLI (Native)** | Full-featured command-line binary. The reference implementation. |
| **Desktop (Tauri)** | Cross-platform GUI app — React frontend with a native Rust/C backend. |
| **Web (WASM)** | Browser-based app compiled to WebAssembly. No installation required. |

All three platforms share the same core compression engine (`hyperpack.c`) and produce bit-identical output for the same input and settings. The differences come down to threading, memory constraints, and OS-level features.

---

## Feature Matrix

| Feature | CLI (Native) | Desktop (Tauri) | Web (WASM) |
|---------|:---:|:---:|:---:|
| **HPK5 compression** | ✅ | ✅ | ✅ |
| **HPK6 archive compression** | ✅ | ✅ | ✅ |
| **Decompression** | ✅ | ✅ | ✅ |
| **Archive listing** | ✅ | ✅ | ✅ |
| **Selective extraction** | ✅ | ✅ | ❌ |
| **Multi-threading (compression)** | ✅ `-j 1..16` | ✅ native threads | ❌ single thread |
| **Multi-threading (decompression)** | ✅ 4 threads | ✅ 4 threads | ❌ sequential |
| **Intra-block parallelism** | ✅ 2 groups | ✅ 2 groups | ❌ sequential |
| **Max block size** | 128 MB | 128 MB | 64 MB |
| **Max file size** | Unlimited (streaming) | Unlimited (RAM) | ~500 MB (browser memory) |
| **Max memory** | System RAM | System RAM | 2 GB (WASM heap) |
| **PNG optimization** | ✅ | ✅ | ✅ |
| **Word preprocessing** | ✅ | ✅ | ✅ |
| **Block deduplication** | ✅ | ✅ | ✅ |
| **File deduplication** | ✅ | ✅ | ✅ |
| **Base64v2 parallel decomp** | ✅ 3 threads | ✅ 3 threads | ❌ sequential |
| **Unix permissions preserved** | ✅ | ✅ | ❌ (MEMFS) |
| **Timestamps preserved** | ✅ | ✅ | ❌ (MEMFS) |
| **Benchmark mode** | ✅ | ❌ | ❌ |
| **Scripting / automation** | ✅ | ❌ (GUI only) | ❌ |
| **Drag & drop** | — | ✅ | ✅ |
| **Native file dialogs** | — | ✅ | ❌ |
| **Progress indicator** | Terminal | GUI | GUI |
| **File system access** | Full | Full (Tauri APIs) | Virtual (MEMFS) |
| **Offline capable** | ✅ | ✅ | ✅ (PWA) |
| **Installation required** | Yes (binary / `make`) | Yes (.msi / .dmg / .deb) | None (browser) |
| **GUI** | ❌ | ✅ | ✅ |

---

## Web App (WASM)

The web version compiles `hyperpack.c` to WebAssembly via Emscripten, running entirely in the browser with no server-side processing.

### Architecture

```
Browser
    │
    ├── Main Thread (React UI)
    │   ├── App.tsx — interface and state
    │   ├── useHyperPack.ts — state management + worker messages
    │   └── Real-time progress bar (parsed from WASM stderr)
    │
    └── Web Worker (worker.js)
        ├── WASM module (createHyperPack)
        ├── Emscripten MEMFS (in-memory virtual filesystem)
        └── C API via cwrap/ccall
            ├── hp_compress(block_mb)
            ├── hp_decompress()
            ├── hp_archive_compress(dir, out, block_mb)
            ├── hp_archive_decompress(in, outdir, pattern)
            └── hp_archive_list(in)
```

### Data Flow

1. **Upload:** `File` → `ArrayBuffer` → Worker (Transferable, zero-copy) → `FS.writeFile()` into MEMFS
2. **Compress/Decompress:** WASM engine reads/writes within MEMFS
3. **Download:** `FS.readFile()` → `ArrayBuffer` → Main thread → `Blob` → browser download

### Key Characteristics

- **Single-threaded:** All 31 strategies run sequentially in one Web Worker. Strategy groups A/C and B/D/E that normally run in parallel on native are serialized.
- **64 MB max block size** — hardcoded in the WASM wrapper for memory safety.
- **2 GB WASM heap** (`MAXIMUM_MEMORY=2147483648`), 256 MB initial allocation, 8 MB stack.
- **~500 MB practical file limit** — a 100 MB file needs ~300 MB of heap (original + MEMFS copy + working buffers).
- **Performance: ~40–60% of native single-thread speed** due to WASM overhead and lack of SIMD.
- **PWA capable** — service worker enables full offline use after first load.
- **Keyboard shortcuts:** Enter to start, Esc to cancel.
- **ZIP export** for extracted archive contents (native implementation, no library).

### Limitations vs Native

| Aspect | Impact |
|--------|--------|
| No threads | ~2.5–4× slower than multi-threaded CLI |
| No real filesystem | Cannot preserve Unix permissions or timestamps |
| Memory ceiling | Large files may exhaust the 2 GB WASM heap |
| No selective extraction | Cannot extract individual files by pattern |

---

## Desktop App (Tauri)

The desktop app wraps the same React frontend in a native window using Tauri, with a Rust backend that calls into `hyperpack_lib.c` via FFI.

### Architecture

```
Tauri App
    │
    ├── Frontend (React — same codebase as the web app)
    │   ├── Detection: isNative() → true if __TAURI_INTERNALS__
    │   ├── Calls via native.ts → invoke('hp_compress', ...)
    │   └── Native file dialogs (open, save, reveal in Finder/Explorer)
    │
    └── Backend (Rust — src-tauri/)
        ├── Tauri commands → FFI to hyperpack_lib.c
        ├── hp_lib_compress(in, out, block_mb, nthreads)
        ├── hp_lib_decompress(in, out)
        ├── hp_lib_archive_compress(paths, out, block_mb, nthreads)
        └── hp_lib_archive_decompress(in, outdir, pattern)
```

### Key Characteristics

- **Native performance:** Uses `hyperpack_lib.c` compiled as a shared library — same speed as CLI.
- **Full multi-threading:** Automatic thread count based on CPU cores, with both inter-block and intra-block parallelism.
- **128 MB block size** and unlimited file size (direct filesystem access).
- **Real filesystem access** via Tauri APIs — preserves permissions, timestamps, and metadata.
- **Native file dialogs:** Open file picker, folder picker, "Save As" dialog, reveal in Finder/Explorer.
- **CPU detection:** Automatically determines optimal thread count.
- **Auto-updates** via the Tauri updater system.
- **Cross-platform:** Windows (.msi), macOS (.dmg), Linux (.deb).

### Tauri-Specific Features vs Web

| Feature | Web | Desktop (Tauri) |
|---------|:---:|:---:|
| Native file picker | ❌ (HTML input) | ✅ |
| Folder picker | ❌ | ✅ |
| "Save As" dialog | ❌ | ✅ |
| Reveal in Finder/Explorer | ❌ | ✅ |
| CPU core detection | ❌ | ✅ |
| Multi-threaded compression | ❌ | ✅ |
| Native filesystem access | ❌ (MEMFS) | ✅ |
| Unlimited file size | ~500 MB | ✅ |
| Unlimited memory | 2 GB max | ✅ |

### Limitations vs CLI

| Aspect | Impact |
|--------|--------|
| No benchmark mode | Diagnostic/benchmarking features are CLI-only |
| No scripting | GUI-only — cannot be used in shell scripts or pipelines |

---

## CLI (Native)

The CLI binary is the **reference implementation** with the complete feature set.

### Key Characteristics

- **All 31 strategies**, all compression modes, all pre-transforms.
- **Parallel compression** with `-j N` (1–16 threads for inter-block, plus 2 intra-block threads per block).
- **4-thread parallel decompression** and 3-thread Base64v2 decompression.
- **128 MB maximum block size**, streaming I/O for unlimited file sizes.
- **PNG optimization mode** — inflates IDAT chunks for better compression, restores on decompression.
- **Archive mode** (`-r`) with full metadata preservation (permissions, timestamps).
- **Selective extraction** (`-e pattern`) from HPK6 archives.
- **Benchmark/diagnostic mode** for testing and profiling.
- **Block and file deduplication** (FNV-1a and CRC32).
- **CRC32 integrity verification** per block on decompression.

### Typical Usage

```bash
# Compress a single file (8 MB blocks, 4 threads)
hyperpack -b 8 -j 4 input.bin

# Compress a directory into an archive
hyperpack -r -b 16 -j 8 my_folder/

# Decompress
hyperpack -d archive.hpk

# List archive contents
hyperpack -l archive.hpk

# Extract specific files
hyperpack -d -e "*.txt" archive.hpk
```

---

## Performance Comparison

Compression ratios are **identical** across all three platforms — the same algorithm produces the same output. Only speed differs.

| Configuration | Relative Speed | Notes |
|---------------|:--------------:|-------|
| CLI (`-j 4`) | **1.0×** | Baseline (4 inter-block threads + intra-block parallelism) |
| CLI (`-j 8`) | ~1.5–1.7× | Diminishing returns beyond core count |
| CLI (`-j 1`) | ~0.3× | Single inter-block thread, still has 2 intra-block threads |
| Tauri (native, auto threads) | ~1.0× | Same as CLI — identical native binary |
| Web (WASM) | ~0.1–0.2× | Single-threaded + WASM overhead (~40–60% of single-thread native) |

### Speed Scaling (from technical benchmarks)

| Configuration | Speed vs CLI `-j 1` |
|---------------|:-------------------:|
| CLI `-j 1` | 1.0× |
| CLI `-j 4` | ~3.5× |
| CLI `-j 8` | ~5–6× |
| Tauri (auto) | ~3–4× |
| WASM | ~0.4–0.6× |

> **Key takeaway:** The web version is roughly 3–4× slower than a 4-thread CLI run, but produces byte-identical compressed output.

---

## When to Use What

| Use Case | Recommended Platform | Why |
|----------|---------------------|-----|
| Maximum performance | **CLI** | Full threading, streaming I/O, no overhead |
| Batch processing / scripting | **CLI** | Shell integration, pipes, automation |
| Server-side compression | **CLI** | Headless, scriptable, scalable |
| User-friendly GUI | **Desktop (Tauri)** | Native dialogs, drag & drop, auto-updates |
| Sharing with non-technical users | **Desktop (Tauri)** | Familiar app experience |
| Large files (> 500 MB) | **CLI** or **Desktop** | No browser memory limits |
| Quick try / demo | **Web (WASM)** | Zero installation, instant access |
| Small-to-medium files | **Web (WASM)** | Convenient for files under ~200 MB |
| No installation possible | **Web (WASM)** | Runs in any modern browser |
| Offline, portable use | **Web (WASM)** | PWA with service worker |

---

*All platforms produce identical compressed output. Choose based on your performance needs and workflow.*
