# HyperPack — Multi-Platform App Architecture

> Goal: Turn HyperPack into a user-friendly app available as a **web app (WASM)**
> and a **native desktop app (Tauri)** for Windows, macOS, and Linux.

---

## Overview

```
┌───────────────────────────────────────────┐
│          Shared Frontend (HTML/CSS/JS)     │
│                                           │
│  ┌───────────────────────────────────┐    │
│  │  📁 Drag & drop: file or folder   │    │
│  │  ⚙️ Strategy / block / level      │    │
│  │  📊 Progress bar + stats          │    │
│  │  📥 Download result               │    │
│  └───────────────────────────────────┘    │
│                                           │
├───────────────────┬───────────────────────┤
│   🌐 WASM Mode    │   🖥️ Tauri Mode      │
│   (Emscripten)    │   (Rust FFI → C)      │
│   Runs in browser │   Native executable   │
│   GitHub Pages    │   Win / Mac / Linux   │
├───────────────────┴───────────────────────┤
│          hyperpack.c (core, unchanged)     │
└───────────────────────────────────────────┘
```

Both modes share the same frontend code. The backend differs:
- **WASM**: hyperpack.c compiled to WebAssembly via Emscripten, runs in a Web Worker
- **Tauri**: hyperpack.c called via Rust FFI, native performance

---

## User Interface

### Main Screen

```
┌─────────────────────────────────────────────────┐
│  🏆 HyperPack v10.2              [⚙️ Settings] │
├─────────────────────────────────────────────────┤
│                                                 │
│     ┌───────────────────────────────────┐       │
│     │                                   │       │
│     │   📁 Drop file or folder here     │       │
│     │      or click to browse           │       │
│     │                                   │       │
│     └───────────────────────────────────┘       │
│                                                 │
│  ○ Compress    ○ Decompress                     │
│                                                 │
│  [▶ Start]                                      │
│                                                 │
├─────────────────────────────────────────────────┤
│  Status: Ready                                  │
│  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ 0%             │
└─────────────────────────────────────────────────┘
```

### Settings Panel

```
┌─────────────────────────────────────────────────┐
│  ⚙️ Compression Settings                       │
├─────────────────────────────────────────────────┤
│                                                 │
│  Strategy:    [Auto ▾]                          │
│               Auto (recommended)                │
│               BWT + MTF + ZRLE + RC             │
│               LZMA                              │
│               LZ77                              │
│               LZP + BWT                         │
│               Delta + BWT                       │
│               Audio (PCM)                       │
│                                                 │
│  Block size:  [128 MB ▾]                        │
│               8 MB / 32 MB / 64 MB / 128 MB    │
│                                                 │
│  LZMA dict:   [64 MB ▾]                        │
│               16 MB / 32 MB / 64 MB             │
│                                                 │
│  Level:       [Normal ▾]                        │
│               Fast (sample-based, ~2× speed)    │
│               Normal (smart heuristic)          │
│               Maximum (try all strategies)      │
│                                                 │
│  Folder mode: [Archive ▾]                       │
│               Archive (.tar → .hp)              │
│               Individual (per-file)             │
│                                                 │
└─────────────────────────────────────────────────┘
```

---

## Parameters

### Strategy Selection

| Value | Description | Best For |
|-------|-------------|----------|
| **Auto** (default) | Smart heuristic selects best strategy | All files |
| BWT | Burrows-Wheeler + MTF-2 + ZRLE + Range Coder | Text, XML, code |
| LZMA | Lempel-Ziv-Markov chain | Binaries, executables |
| LZ77 | Classic sliding window | Fast compression |
| LZP | Longest Previous Prefix + BWT | Repetitive data |
| Delta | Delta filter + BWT | Structured/numeric data |
| Audio | Mid/side + Delta + LPC | PCM audio (WAV) |

### Block Size

| Value | RAM Usage (approx.) | Trade-off |
|-------|---------------------|-----------|
| 8 MB | ~50 MB | Fast, lower ratio |
| 32 MB | ~200 MB | Balanced |
| 64 MB | ~400 MB | Good ratio |
| **128 MB** (default) | ~800 MB | Best ratio |

### LZMA Dictionary Size

| Value | RAM Usage | Effect |
|-------|-----------|--------|
| 16 MB | ~100 MB | Baseline |
| 32 MB | ~200 MB | Better on large files |
| **64 MB** (default) | ~400 MB | Best ratio (+7.2% vs 16 MB) |

### Compression Level

| Level | Behavior | Speed |
|-------|----------|-------|
| **Fast** | Sample 1 MB → run winner only | ~2× faster |
| **Normal** (default) | Smart LZMA heuristic + sample | 1.3 MB/s |
| **Maximum** | Try ALL strategies, keep best | ~0.6 MB/s |

### Folder Handling

| Mode | Behavior | Output |
|------|----------|--------|
| **Archive** (default) | Bundle folder → single .hp file | `folder.hp` |
| **Individual** | Compress each file, preserve tree | `folder_hp/` |

---

## Phase 1: WASM Web App

### Technical Stack

| Component | Technology |
|-----------|------------|
| Compiler | Emscripten (C → WASM) |
| Worker | Web Worker (non-blocking UI) |
| Frontend | Vanilla HTML/CSS/JS + modern CSS |
| File handling | File System Access API / download blob |
| Hosting | GitHub Pages (free) |

### Architecture

```
Browser
├── index.html          ← UI
├── app.js              ← Controls, settings, progress
├── worker.js           ← Web Worker thread
│   └── hyperpack.wasm  ← Compiled HyperPack
└── style.css           ← Styling
```

### Flow

```
User drops file
    → app.js reads file into ArrayBuffer
    → sends to worker.js via postMessage
    → worker calls WASM hyperpack_compress(buffer, params)
    → worker posts progress updates back to app.js
    → worker posts compressed result
    → app.js triggers download
```

### Modifications to hyperpack.c

Minimal changes needed — add library API wrapper:

```c
// New: Library API (in addition to existing CLI main())
#ifdef HYPERPACK_LIB
typedef struct {
    int strategy;       // 0=auto, 1=BWT, 2=LZMA, etc.
    int block_size_mb;  // 8, 32, 64, 128
    int lzma_dict_mb;   // 16, 32, 64
    int level;          // 0=fast, 1=normal, 2=max
    void (*progress_cb)(float percent, const char *status);
} hp_params_t;

int hp_compress(const uint8_t *input, size_t input_size,
                uint8_t **output, size_t *output_size,
                hp_params_t *params);

int hp_decompress(const uint8_t *input, size_t input_size,
                  uint8_t **output, size_t *output_size,
                  void (*progress_cb)(float, const char*));
#endif
```

### Emscripten Build

```bash
emcc -O2 -s WASM=1 \
     -s ALLOW_MEMORY_GROWTH=1 \
     -s MAXIMUM_MEMORY=2GB \
     -s EXPORTED_FUNCTIONS='["_hp_compress","_hp_decompress","_malloc","_free"]' \
     -s EXPORTED_RUNTIME_METHODS='["ccall","cwrap"]' \
     -DHYPERPACK_LIB \
     -o hyperpack.js src/hyperpack.c -lm
```

### WASM Limitations

| Limitation | Workaround |
|------------|------------|
| No pthreads by default | SharedArrayBuffer + COOP/COEP headers |
| Memory limit ~2 GB | Chunked processing for very large files |
| No filesystem access | File API + download blobs |
| Slow on mobile | Warn users, suggest desktop |

### Hosting on GitHub Pages

```
/docs/app/           ← or separate gh-pages branch
├── index.html
├── app.js
├── worker.js
├── hyperpack.js      ← Emscripten glue
├── hyperpack.wasm    ← Compiled WASM
└── style.css
```

URL: `https://aciderix.github.io/HyperPack/`

---

## Phase 2: Tauri Desktop App

### Technical Stack

| Component | Technology |
|-----------|------------|
| Framework | Tauri v2 |
| Backend | Rust + C FFI (calls hyperpack.c) |
| Frontend | Same HTML/CSS/JS as WASM version |
| Packaging | .msi (Windows), .dmg (macOS), .AppImage (Linux) |

### Architecture

```
Tauri App
├── src-tauri/
│   ├── src/
│   │   ├── main.rs           ← Tauri entry point
│   │   ├── hyperpack_ffi.rs  ← Rust FFI bindings to C
│   │   └── commands.rs       ← Tauri commands (compress, decompress)
│   ├── hyperpack.c           ← HyperPack source (unchanged)
│   ├── build.rs              ← cc crate compiles hyperpack.c
│   └── Cargo.toml
├── src/                       ← Frontend (shared with WASM)
│   ├── index.html
│   ├── app.js
│   └── style.css
└── tauri.conf.json
```

### Rust FFI Bridge

```rust
// hyperpack_ffi.rs
extern "C" {
    fn hp_compress(
        input: *const u8, input_size: usize,
        output: *mut *mut u8, output_size: *mut usize,
        params: *const HpParams,
    ) -> i32;

    fn hp_decompress(
        input: *const u8, input_size: usize,
        output: *mut *mut u8, output_size: *mut usize,
        progress_cb: extern "C" fn(f32, *const c_char),
    ) -> i32;
}
```

### Tauri Commands

```rust
// commands.rs
#[tauri::command]
async fn compress_file(
    path: String,
    strategy: String,
    block_size: u32,
    level: String,
) -> Result<CompressResult, String> {
    // Read file, call FFI, write output
}
```

### Build & Distribution

```bash
# Development
cargo tauri dev

# Build for current platform
cargo tauri build

# Cross-compile
# Windows: .msi installer (~5 MB)
# macOS: .dmg (~5 MB)
# Linux: .AppImage + .deb (~5 MB)
```

### Tauri Advantages Over WASM

| Feature | WASM | Tauri |
|---------|------|-------|
| File size limit | ~2 GB (browser) | Unlimited |
| Speed | ~70% native | 100% native |
| Threading | Limited | Full pthreads |
| Folder access | API-dependent | Full filesystem |
| Drag & drop from OS | Limited | Native |
| Install required | No | Yes |
| Offline | Service worker | Always |

---

## Shared Frontend Code

The UI is identical for both targets. The only difference is the backend bridge:

```javascript
// bridge.js — abstracts WASM vs Tauri
const isWasm = typeof window.__TAURI__ === 'undefined';

export async function compress(fileBuffer, params) {
    if (isWasm) {
        return wasmCompress(fileBuffer, params);
    } else {
        return tauriCompress(fileBuffer, params);
    }
}
```

---

## File Format

### Single File (.hp)

```
[HP header][compressed block 1][compressed block 2]...
```

No changes needed — current format works.

### Folder Archive (.hp)

```
[HP header]
[file table: paths + sizes + permissions]
[compressed block 1]  ← may contain data from multiple files
[compressed block 2]
...
```

The folder mode packs files into a single stream (like tar),
then compresses. This preserves HyperPack's large-block advantage.

---

## Development Roadmap

| Phase | Task | Estimated Effort |
|-------|------|-----------------|
| **1.1** | Add library API to hyperpack.c (`#ifdef HYPERPACK_LIB`) | 1-2 days |
| **1.2** | Compile with Emscripten, verify basic compress/decompress | 1 day |
| **1.3** | Build web UI (drag & drop, settings, progress) | 2-3 days |
| **1.4** | Deploy to GitHub Pages | 1 hour |
| **2.1** | Set up Tauri project with Rust FFI | 1 day |
| **2.2** | Port frontend, add native file dialogs | 1 day |
| **2.3** | Add folder support (tar-like bundling) | 1-2 days |
| **2.4** | Build installers (Windows, macOS, Linux) | 1 day |
| **2.5** | CI/CD with GitHub Actions | 1 day |

**Total estimate: ~10-12 days**

---

## Repository Structure (planned)

```
HyperPack/
├── src/
│   └── hyperpack.c          ← Core algorithm (with LIB API)
├── web/                      ← WASM web app
│   ├── index.html
│   ├── app.js
│   ├── worker.js
│   ├── bridge.js
│   ├── style.css
│   └── build.sh             ← Emscripten build script
├── tauri/                    ← Desktop app
│   ├── src-tauri/
│   │   ├── src/
│   │   ├── hyperpack.c → ../src/hyperpack.c (symlink)
│   │   ├── build.rs
│   │   └── Cargo.toml
│   └── src/ → ../web/ (shared frontend)
├── docs/
└── README.md
```

---

*Architecture designed for HyperPack v10.2. Core algorithm stays in a single C file with zero dependencies.*
