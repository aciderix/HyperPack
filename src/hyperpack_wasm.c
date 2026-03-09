/*
 * HyperPack WASM Wrapper
 * Thin wrapper around hyperpack.c for Emscripten/WASM builds.
 * Provides buffer-based API via Emscripten MEMFS.
 *
 * Supports HPK5 (single-file) and HPK6 (multi-file archive) formats.
 *
 * Build: see build-wasm.sh
 */

#define HYPERPACK_WASM 1
#include "hyperpack.c"

#include <emscripten.h>

/* ---------- Format detection ---------- */

/* Detect if a file is HPK5 or HPK6.
   Returns 5 for HPK5, 6 for HPK6, 0 for unknown. */
EMSCRIPTEN_KEEPALIVE
int hp_detect_format(const char *inpath) {
    FILE *f = fopen(inpath, "rb");
    if (!f) return 0;
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return 0; }
    fclose(f);
    uint32_t m = ((uint32_t)magic[0]<<24)|((uint32_t)magic[1]<<16)|((uint32_t)magic[2]<<8)|magic[3];
    if (m == 0x48504B35u) return 5;  /* HPK5 */
    if (m == 0x48504B36u) return 6;  /* HPK6 */
    return 0;
}

/* ---------- HPK5 single-file ---------- */

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
 * Auto-detects HPK5 vs HPK6: if HPK6, extracts archive to /output directory.
 * Returns 0 on success, non-zero on error.
 */
EMSCRIPTEN_KEEPALIVE
int hp_decompress(void) {
    int fmt = hp_detect_format("/input.hpk");
    if (fmt == 6) {
        return archive_decompress("/input.hpk", "/output", NULL);
    }
    return file_decompress("/input.hpk", "/output");
}

/* ---------- HPK6 multi-file archive ---------- */

/* Archive compress: takes a directory path in MEMFS and output path.
   The JS side will have written all files into MEMFS under a directory before calling this.
   Returns 0 on success. */
EMSCRIPTEN_KEEPALIVE
int hp_archive_compress(const char *dirpath, const char *outpath, int block_size_mb) {
    int block_size = block_size_mb << 20;
    if (block_size < (1 << 20)) block_size = 1 << 20;
    if (block_size > (128 << 20)) block_size = 128 << 20;
    const char *paths[1] = { dirpath };
    return archive_compress(1, paths, outpath, block_size, 1);
}

/* Archive decompress: reads HPK6 from MEMFS, extracts to output dir in MEMFS.
   extract_pattern: if not NULL, only extract matching files. Pass NULL or "" for all.
   Returns 0 on success. */
EMSCRIPTEN_KEEPALIVE
int hp_archive_decompress(const char *inpath, const char *outdir, const char *extract_pattern) {
    const char *pattern = (extract_pattern && extract_pattern[0]) ? extract_pattern : NULL;
    return archive_decompress(inpath, outdir, pattern);
}

/* List archive contents. Output goes to stderr which JS captures.
   Returns 0 on success. */
EMSCRIPTEN_KEEPALIVE
int hp_archive_list(const char *inpath) {
    return archive_list(inpath);
}
