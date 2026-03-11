# Architecture & Build System

Technical overview of the HyperPack project structure, source code organization, build system, and CI/CD pipelines.

## Table of Contents

- [Project Structure](#project-structure)
- [Source Code Architecture](#source-code-architecture)
  - [hyperpack.c — Core Engine + CLI](#hyperpackc--core-engine--cli)
  - [hyperpack\_lib.c — Library API](#hyperpack_libc--library-api)
  - [hyperpack\_wasm.c — WASM Bindings](#hyperpack_wasmc--wasm-bindings)
- [Build System](#build-system)
  - [Native Build (Makefile)](#native-build-makefile)
  - [WASM Build (build-wasm.sh)](#wasm-build-build-wasmsh)
  - [Web App](#web-app)
  - [Tauri Desktop App](#tauri-desktop-app)
- [CI/CD](#cicd)
  - [build.yml — Continuous Integration](#buildyml--continuous-integration)
  - [release.yml — Release Pipeline](#releaseyml--release-pipeline)
- [Dependencies](#dependencies)

---

## Project Structure

```
HyperPack/
├── src/
│   ├── hyperpack.c           # Core engine + CLI (~7,000 lines)
│   ├── hyperpack_lib.c       # Library API (compress/decompress functions)
│   └── hyperpack_wasm.c      # WASM bindings for Emscripten
├── hyperpack-web/            # React + TypeScript frontend
│   ├── src/                  # App source
│   │   ├── App.tsx           # Main app component
│   │   ├── components/       # UI components (DropZone, FileInfo, etc.)
│   │   ├── hooks/            # useHyperPack hook
│   │   ├── lib/              # native.ts (Tauri bridge)
│   │   └── workers/          # bridge.ts (WASM worker bridge)
│   ├── src-tauri/            # Tauri desktop wrapper
│   │   ├── Cargo.toml        # Rust config
│   │   ├── src/              # lib.rs, main.rs
│   │   └── icons/            # Platform icons
│   ├── public/
│   │   ├── worker.js         # WASM Web Worker
│   │   └── manifest.json     # PWA manifest
│   ├── vite.config.ts        # Vite configuration
│   └── package.json
├── benchmark/                # Benchmark scripts & results
├── docs/                     # Documentation
├── .github/workflows/
│   ├── build.yml             # CI: build + test + deploy web
│   └── release.yml           # Release: multi-platform binaries + Tauri
├── Makefile                  # Native build
└── build-wasm.sh             # WASM build
```

---

## Source Code Architecture

### hyperpack.c — Core Engine + CLI

The entire compression engine lives in a single monolithic C file (~7,000 lines). This is intentional — it keeps the codebase self-contained with no internal header dependencies and simplifies cross-platform builds.

The file is organized in top-down sections:

| Section | Description |
|---------|-------------|
| Defines & constants | Block sizes, magic numbers, format identifiers |
| Data structures | Strategy tables, block headers, archive metadata |
| Algorithm implementations | RLE, delta, LZ, dictionary, Huffman, and other codec primitives |
| Strategy pipelines | 31 compression strategies, each a unique combination of algorithms |
| Block compression | Per-block strategy selection with automatic best-pick |
| Archive handler | HPK6 multi-file archive packing/unpacking |
| PNG handler | PNG-specific preprocessing for improved compression |
| CLI main() | Argument parsing and command dispatch |

### hyperpack\_lib.c — Library API

A thin wrapper that `#include`s `hyperpack.c` with `LIB_MODE` defined. This compiles the engine as a library rather than a CLI, exposing a clean C API:

- `hyperpack_compress()` — compress a file
- `hyperpack_decompress()` — decompress a file

Used by both the WASM build and the Tauri desktop backend.

### hyperpack\_wasm.c — WASM Bindings

Emscripten bindings that bridge the C library to JavaScript. Uses Emscripten's MEMFS (in-memory filesystem) to pass data between the browser and the C code.

**Exported functions:**

| Function | Description |
|----------|-------------|
| `hp_compress` | Compress a single file |
| `hp_decompress` | Decompress a single file |
| `hp_archive_compress` | Create a multi-file archive |
| `hp_archive_decompress` | Extract an archive |
| `hp_archive_list` | List archive contents |
| `hp_detect_format` | Detect HPK5/HPK6 format |

---

## Build System

### Native Build (Makefile)

The native CLI is built with a minimal Makefile:

```makefile
CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lm -lpthread -lz

all: hyperpack

hyperpack: src/hyperpack.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f hyperpack *.hpk
```

**Build commands:**

```bash
make            # Build the hyperpack binary
make clean      # Remove binary and .hpk files
make wasm       # Build WASM module (calls build-wasm.sh)
make wasm-clean # Remove WASM artifacts
```

**Requirements:** gcc or clang, zlib (`-lz`), pthreads (`-lpthread`), math library (`-lm`).

### WASM Build (build-wasm.sh)

Compiles `hyperpack_wasm.c` to WebAssembly using Emscripten:

```bash
emcc src/hyperpack_wasm.c \
    -O3 \
    -DHYPERPACK_WASM \
    -s MODULARIZE=1 \
    -s EXPORT_NAME='createHyperPack' \
    -s FORCE_FILESYSTEM=1 \
    -s EXPORTED_FUNCTIONS='[_hp_compress, _hp_decompress, ...]' \
    -s EXPORTED_RUNTIME_METHODS='[FS, cwrap, ccall, stringToNewUTF8]' \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s INITIAL_MEMORY=268435456 \
    -s MAXIMUM_MEMORY=2147483648 \
    -s STACK_SIZE=8388608 \
    -s USE_ZLIB=1 \
    -o hyperpack-web/public/hyperpack.js
```

| Flag | Purpose |
|------|---------|
| `-O3` | Maximum optimization |
| `MODULARIZE=1` | Wrap as ES module factory (`createHyperPack()`) |
| `FORCE_FILESYSTEM=1` | Enable MEMFS for file I/O |
| `ALLOW_MEMORY_GROWTH=1` | Dynamic memory allocation (256 MB initial, 2 GB max) |
| `STACK_SIZE=8388608` | 8 MB stack for deep recursion |
| `USE_ZLIB=1` | Emscripten-provided zlib port |

**Output:** `hyperpack-web/public/hyperpack.js` + `hyperpack.wasm`

**Requirement:** Emscripten SDK (`emcc` in PATH).

### Web App

The web frontend is built with Vite + React + TypeScript + Tailwind CSS v4.

```bash
cd hyperpack-web
npm install       # Install dependencies
npm run dev       # Development server (port 3000)
npm run build     # Production build
npm run lint      # TypeScript type-check
npm run preview   # Preview production build
```

**Key dependencies:**

| Package | Purpose |
|---------|---------|
| `react` / `react-dom` 19 | UI framework |
| `vite` 6 | Build tool and dev server |
| `tailwindcss` 4 | Utility-first CSS |
| `lucide-react` | Icon library |
| `@tauri-apps/api` | Tauri runtime bridge |
| `@tauri-apps/plugin-dialog` | Native file dialogs (desktop) |
| `typescript` 5.8 | Type checking |

**Output:** `hyperpack-web/dist/`

### Tauri Desktop App

The desktop app wraps the web frontend with a native Rust shell via [Tauri 2](https://tauri.app/). The Tauri configuration lives in `hyperpack-web/src-tauri/`.

```bash
cd hyperpack-web
npm run tauri build    # Build platform-specific installer
npm run tauri dev      # Development mode with hot reload
```

**Platform outputs:**

| Platform | Output |
|----------|--------|
| Linux | `.deb` package + `.AppImage` portable |
| macOS | `.dmg` (universal: Apple Silicon + Intel) |
| Windows | `.exe` NSIS installer |

---

## CI/CD

### build.yml — Continuous Integration

**Triggers:** Push to `main`/`master`/`claude/**`, pull requests, manual dispatch.

| Job | Runner | Description |
|-----|--------|-------------|
| **build-linux** | `ubuntu-latest` | Build CLI with gcc, run smoke test (compress → decompress → diff) |
| **build-macos** | `macos-latest` | Build universal binary (x86\_64 + arm64 via `lipo`) |
| **build-windows** | `windows-latest` | Build with MinGW-w64 (MSYS2), static linking, smoke test |
| **build-wasm** | `ubuntu-latest` | Build WASM with Emscripten 3.1.74, build frontend, type-check |
| **build-tauri-linux** | `ubuntu-latest` | Full Tauri build with WebKitGTK dependencies |
| **build-tauri-macos** | `macos-latest` | Universal Tauri build (aarch64 + x86\_64) |
| **build-tauri-windows** | `windows-latest` | Tauri build with GNU/MinGW toolchain |
| **deploy-pages** | `ubuntu-latest` | Deploy to GitHub Pages (main/master only, after WASM build) |

All jobs upload build artifacts. The Pages deployment rebuilds WASM and the frontend with `GITHUB_PAGES=1` for correct base path configuration.

### release.yml — Release Pipeline

**Trigger:** Push of tags matching `v*` (e.g., `v12.0`).

| Job | Output |
|-----|--------|
| **cli-linux** | `hyperpack-linux-x64` |
| **cli-macos** | `hyperpack-macos-universal` |
| **cli-windows** | `hyperpack-windows-x64.exe` |
| **tauri-linux** | `.deb` + `.AppImage` |
| **tauri-macos** | `.dmg` (universal) |
| **tauri-windows** | `.exe` (NSIS installer) |
| **publish** | Creates GitHub Release with all artifacts |

The **publish** job runs after all build jobs complete. It downloads all artifacts, stages them into a release directory, and creates a GitHub Release via `softprops/action-gh-release@v2`. Pre-release is automatically set if the tag contains a hyphen (e.g., `v12.0-beta`).

**Windows notes:** The Tauri Windows build uses the `x86_64-pc-windows-gnu` Rust toolchain instead of MSVC, because `hyperpack.c` uses POSIX APIs (pthreads, dirent, unistd) available only via MinGW-w64.

---

## Dependencies

### C Code (zero external dependencies beyond standard libs)

| Library | Purpose |
|---------|---------|
| `zlib` | Deflate compression (used within strategies) |
| `pthreads` | Parallel block compression |
| `math.h` | Mathematical operations |

### Web App

| Category | Technologies |
|----------|-------------|
| Framework | React 19, TypeScript 5.8 |
| Build | Vite 6 |
| Styling | Tailwind CSS 4 |
| Icons | Lucide React |
| Desktop bridge | @tauri-apps/api 2, plugin-dialog, plugin-opener |

### Desktop App

| Component | Technology |
|-----------|-----------|
| Shell | Tauri 2 (Rust) |
| Frontend | Same as web app |
| Packaging | NSIS (Windows), DMG (macOS), DEB/AppImage (Linux) |

### Build Toolchain

| Target | Requirements |
|--------|-------------|
| Native CLI | gcc or clang, make, zlib-dev |
| WASM | Emscripten SDK (3.1.74 tested) |
| Web app | Node.js 20+, npm |
| Desktop | Rust (stable), Node.js 20+, platform-specific libs |
