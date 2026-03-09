/*
 * HyperPack WASM Wrapper
 * Thin wrapper around hyperpack.c for Emscripten/WASM builds.
 * Provides buffer-based API via Emscripten MEMFS.
 *
 * Build: see build-wasm.sh
 */

#define HYPERPACK_WASM 1
#include "hyperpack.c"

#include <emscripten.h>

/*
 * Compress a file previously written to /input via FS.writeFile().
 * Result is written to /output.hpk.
 * Returns 0 on success, non-zero on error.
 */
EMSCRIPTEN_KEEPALIVE
int hp_compress(int block_mb) {
    if (block_mb < 1) block_mb = 1;
    if (block_mb > 64) block_mb = 64;  /* WASM memory safety */
    return file_compress("/input", "/output.hpk", block_mb << 20, 1);
}

/*
 * Decompress a file previously written to /input.hpk via FS.writeFile().
 * Result is written to /output.
 * Returns 0 on success, non-zero on error.
 */
EMSCRIPTEN_KEEPALIVE
int hp_decompress(void) {
    return file_decompress("/input.hpk", "/output");
}
