/*
 * HyperPack native library wrapper for Tauri/desktop builds.
 * Provides a file-path-based API (no MEMFS, no Emscripten).
 *
 * Build: compiled via cc crate in src-tauri/build.rs
 */

#define HYPERPACK_LIB 1
#include "hyperpack.c"

#include <stdint.h>
#include <stdio.h>

/* ── Format detection ────────────────────────────────────────────────────── */

/* Returns 5 for HPK5, 6 for HPK6, 0 for unknown. */
int hp_lib_detect_format(const char *inpath) {
    FILE *f = fopen(inpath, "rb");
    if (!f) return 0;
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return 0; }
    fclose(f);
    uint32_t m = ((uint32_t)magic[0]<<24)|((uint32_t)magic[1]<<16)|
                 ((uint32_t)magic[2]<<8)|magic[3];
    if (m == 0x48504B35u) return 5;
    if (m == 0x48504B36u) return 6;
    return 0;
}

/* ── HPK5 single-file ────────────────────────────────────────────────────── */

/* Compress inpath → outpath. block_mb: block size in MB, nthreads: 0=auto.
   Returns 0 on success. */
int hp_lib_compress(const char *inpath, const char *outpath,
                    int block_mb, int nthreads) {
    if (block_mb < 1)  block_mb = 1;
    if (nthreads < 1)  nthreads = 1;
    return file_compress(inpath, outpath, block_mb << 20, nthreads, -1, 0xFFFFFFFFu);
}

/* Decompress inpath → outpath. Returns 0 on success. */
int hp_lib_decompress(const char *inpath, const char *outpath) {
    return file_decompress(inpath, outpath);
}

/* ── HPK6 archive ────────────────────────────────────────────────────────── */

/* Compress npaths input paths into outpath archive.
   Returns 0 on success. */
int hp_lib_archive_compress(int npaths, const char **paths,
                            const char *outpath, int block_mb, int nthreads) {
    if (block_mb < 1)  block_mb = 1;
    if (nthreads < 1)  nthreads = 1;
    return archive_compress(npaths, paths, outpath, block_mb << 20, nthreads, -1, 0xFFFFFFFFu);
}

/* Decompress HPK6 archive inpath → outdir.
   pattern: if non-empty, only extract matching entries. Returns 0 on success. */
int hp_lib_archive_decompress(const char *inpath, const char *outdir,
                              const char *pattern) {
    const char *p = (pattern && pattern[0]) ? pattern : NULL;
    return archive_decompress(inpath, outdir, p);
}

/* List HPK6 archive contents to stdout. Returns 0 on success. */
int hp_lib_archive_list(const char *inpath) {
    return archive_list(inpath);
}

/* ── Strategy-forced variants ──────────────────────────────────────────── */

/* Compress with a specific strategy. force_strategy: 0..30 or -1 for auto. */
int hp_lib_compress_with_strategy(const char *inpath, const char *outpath,
                                   int block_mb, int nthreads, int force_strategy) {
    if (block_mb < 1)  block_mb = 1;
    if (nthreads < 1)  nthreads = 1;
    return file_compress(inpath, outpath, block_mb << 20, nthreads, force_strategy, 0xFFFFFFFFu);
}

/* Archive compress with a specific strategy. force_strategy: 0..30 or -1 for auto. */
int hp_lib_archive_compress_with_strategy(int npaths, const char **paths,
                                           const char *outpath, int block_mb,
                                           int nthreads, int force_strategy) {
    if (block_mb < 1)  block_mb = 1;
    if (nthreads < 1)  nthreads = 1;
    return archive_compress(npaths, paths, outpath, block_mb << 20, nthreads, force_strategy, 0xFFFFFFFFu);
}

/* List strategy names. Returns NUM_STRATEGIES (31). */
int hp_lib_num_strategies(void) { return NUM_STRATEGIES; }

/* Get strategy name by index. Returns NULL if out of range. */
const char *hp_lib_strategy_name(int idx) {
    if (idx < 0 || idx >= NUM_STRATEGIES) return NULL;
    return strat_names[idx];
}

/* ── Strategy-filtered variants ────────────────────────────────────────── */

/* Compress with strategy filter (allowed_mask = bitmask of allowed strategies, 0xFFFFFFFF = all). */
int hp_lib_compress_filtered(const char *inpath, const char *outpath,
                              int block_mb, int nthreads, uint32_t allowed_mask) {
    if (block_mb < 1)  block_mb = 1;
    if (nthreads < 1)  nthreads = 1;
    return file_compress(inpath, outpath, block_mb << 20, nthreads, -1, allowed_mask);
}

/* Archive compress with strategy filter. */
int hp_lib_archive_compress_filtered(int npaths, const char **paths,
                                      const char *outpath, int block_mb,
                                      int nthreads, uint32_t allowed_mask) {
    if (block_mb < 1)  block_mb = 1;
    if (nthreads < 1)  nthreads = 1;
    return archive_compress(npaths, paths, outpath, block_mb << 20, nthreads, -1, allowed_mask);
}
