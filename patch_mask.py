#!/usr/bin/env python3
"""Patch HyperPack to add strategy filter (allowed_mask) for auto mode.
Adds -S (include) and -X (exclude) CLI options."""

import re, sys

def do_replace(code, old, new, label, count=1):
    if old not in code:
        print(f"FAIL [{label}]: pattern not found")
        # Show context
        first_line = old.split('\n')[0][:60]
        print(f"  Looking for: {first_line}...")
        sys.exit(1)
    n = code.count(old)
    if count > 0 and n != count:
        print(f"WARN [{label}]: expected {count} matches, found {n}")
    result = code.replace(old, new, count)
    print(f"  OK [{label}]")
    return result

# ============================================================
# PATCH hyperpack.c
# ============================================================
with open('src/hyperpack.c', 'r') as f:
    c = f.read()

print("=== Patching hyperpack.c ===")

# 1. Add allowed_mask to GroupWork struct
c = do_replace(c,
    '    /* Strategy hint from sample (-1 = try all) */\n    int hint_strat;\n    \n    /* Working buffers (thread-private) */',
    '    /* Strategy hint from sample (-1 = try all) */\n    int hint_strat;\n    /* Bitmask of allowed strategies (0xFFFFFFFF = all) */\n    uint32_t allowed_mask;\n    \n    /* Working buffers (thread-private) */',
    "GroupWork.allowed_mask")

# 2. Add allowed_mask to BlockJob struct
c = do_replace(c,
    '    int force_strategy;\n    volatile int done;\n} BlockJob;',
    '    int force_strategy;\n    uint32_t allowed_mask;\n    volatile int done;\n} BlockJob;',
    "BlockJob.allowed_mask")

# 3. Update compress_block_worker to pass allowed_mask
c = do_replace(c,
    '    job->output_size = compress_block(job->input, job->input_size, \n                                       job->output, &job->strategy, \n                                       job->bwt_ws, job->cws, job->force_strategy);',
    '    job->output_size = compress_block(job->input, job->input_size, \n                                       job->output, &job->strategy, \n                                       job->bwt_ws, job->cws, job->force_strategy, job->allowed_mask);',
    "compress_block_worker")

# 4. thread_groups_AC: add group-level mask checks
c = do_replace(c,
    '    if (w->hint_strat == S_BWT_O0 || w->hint_strat == S_BWT_O1 || w->hint_strat == S_BWT_O2)\n        skip_lzp = 1;\n    \n    uint8_t  *bwt_buf   = w->cws->bwt_buf;\n    uint8_t  *mtf_buf   = w->cws->mtf_buf;\n    uint16_t *zrle_buf  = w->cws->zrle_buf;\n    uint8_t  *arith_buf = w->cws->arith_buf;\n    uint8_t  *lzp_buf   = w->cws->lzp_buf;',

    '    if (w->hint_strat == S_BWT_O0 || w->hint_strat == S_BWT_O1 || w->hint_strat == S_BWT_O2)\n        skip_lzp = 1;\n    /* Strategy mask: skip groups with no allowed strategies */\n    if (!(w->allowed_mask & ((1u<<S_BWT_O0)|(1u<<S_BWT_O1)|(1u<<S_BWT_O2)|(1u<<S_BWT_O0_PS)|(1u<<S_BWT_RANS)|(1u<<S_BWT_CTX2)|(1u<<S_BWT_O1_PS)|(1u<<S_LZP_BWT_O0)|(1u<<S_LZP_BWT_O1)|(1u<<S_LZP_BWT_O2))))\n        skip_bwt = 1;\n    if (!(w->allowed_mask & ((1u<<S_LZP_BWT_O0)|(1u<<S_LZP_BWT_O1)|(1u<<S_LZP_BWT_O2))))\n        skip_lzp = 1;\n    \n    uint8_t  *bwt_buf   = w->cws->bwt_buf;\n    uint8_t  *mtf_buf   = w->cws->mtf_buf;\n    uint16_t *zrle_buf  = w->cws->zrle_buf;\n    uint8_t  *arith_buf = w->cws->arith_buf;\n    uint8_t  *lzp_buf   = w->cws->lzp_buf;',
    "AC mask check")

# 5. thread_groups_BDE: add group-level mask checks
c = do_replace(c,
    '    if (w->hint_strat == S_DBWT_O0 || w->hint_strat == S_DBWT_O1 || w->hint_strat == S_DBWT_O2)\n        skip_lzp = 1;\n    \n    uint8_t  *bwt_buf   = w->cws->bwt_buf;\n    uint8_t  *mtf_buf   = w->cws->mtf_buf;\n    uint16_t *zrle_buf  = w->cws->zrle_buf;\n    uint8_t  *arith_buf = w->cws->arith_buf;\n    uint8_t  *delta_buf = w->cws->delta_buf;\n    uint8_t  *lzp_buf   = w->cws->lzp_buf;',

    '    if (w->hint_strat == S_DBWT_O0 || w->hint_strat == S_DBWT_O1 || w->hint_strat == S_DBWT_O2)\n        skip_lzp = 1;\n    /* Strategy mask: skip groups with no allowed strategies */\n    if (!(w->allowed_mask & ((1u<<S_DBWT_O0)|(1u<<S_DBWT_O1)|(1u<<S_DBWT_O2)|(1u<<S_DLZP_BWT_O0)|(1u<<S_DLZP_BWT_O1)|(1u<<S_DLZP_BWT_O2))))\n        skip_bwt = 1;\n    if (!(w->allowed_mask & ((1u<<S_DLZP_BWT_O0)|(1u<<S_DLZP_BWT_O1)|(1u<<S_DLZP_BWT_O2))))\n        skip_lzp = 1;\n    \n    uint8_t  *bwt_buf   = w->cws->bwt_buf;\n    uint8_t  *mtf_buf   = w->cws->mtf_buf;\n    uint16_t *zrle_buf  = w->cws->zrle_buf;\n    uint8_t  *arith_buf = w->cws->arith_buf;\n    uint8_t  *delta_buf = w->cws->delta_buf;\n    uint8_t  *lzp_buf   = w->cws->lzp_buf;',
    "BDE mask check")

# 6. F32_BWT mask check in BDE
c = do_replace(c,
    '    if (w->hint_strat < 0 || w->hint_strat == S_F32_BWT) {\n        if (float_xor_detect(data, n)) {',
    '    if ((w->allowed_mask & (1u << S_F32_BWT)) && (w->hint_strat < 0 || w->hint_strat == S_F32_BWT)) {\n        if (float_xor_detect(data, n)) {',
    "F32 mask")

# 7. LZ77 mask check in BDE
c = do_replace(c,
    '    if (w->hint_strat < 0 || w->hint_strat == S_LZ77_O0 || w->hint_strat == S_LZ77_O1)\n    {',
    '    if ((w->allowed_mask & ((1u<<S_LZ77_O0)|(1u<<S_LZ77_O1))) && (w->hint_strat < 0 || w->hint_strat == S_LZ77_O0 || w->hint_strat == S_LZ77_O1))\n    {',
    "LZ77 mask")

# 8. Regex: add mask check to ALL "if (xxx < w->best_size)" strategy updates in group functions
pattern = r'if \((\w+) < w->best_size\) \{\n(\s+)w->best_size = \1; w->best_strat = (S_\w+);'
count_before = len(re.findall(pattern, c))
c_new = re.sub(pattern,
    r'if ((w->allowed_mask & (1u << \3)) && \1 < w->best_size) {\n\2w->best_size = \1; w->best_strat = \3;',
    c)
count_after = len(re.findall(pattern, c_new))
print(f"  OK [regex strategy gates]: {count_before} replacements ({count_after} remaining)")
c = c_new

# 9. compress_block signature
c = do_replace(c,
    'static int compress_block(const uint8_t *data, int n, uint8_t *out, int *strat, BWTWorkspace *bwt_ws, CompressWorkspace *cws, int force_strategy) {',
    'static int compress_block(const uint8_t *data, int n, uint8_t *out, int *strat, BWTWorkspace *bwt_ws, CompressWorkspace *cws, int force_strategy, uint32_t allowed_mask) {',
    "compress_block sig")

# 10. Sample recursive call
c = do_replace(c,
    '            compress_block(data, sample_n, sample_out, &sample_strat, sample_bwt, sample_cws, -1);',
    '            compress_block(data, sample_n, sample_out, &sample_strat, sample_bwt, sample_cws, -1, allowed_mask);',
    "sample recursive")

# 11. run_ac/run_bde mask override in compress_block
c = do_replace(c,
    '        /* LZMA hint: both groups skipped, handled below */\n        if (hint_strat == S_LZMA) { run_ac = 0; run_bde = 0; }\n    }\n    \n    /* === Parallel Groups A-E === */',

    '        /* LZMA hint: both groups skipped, handled below */\n        if (hint_strat == S_LZMA) { run_ac = 0; run_bde = 0; }\n    }\n    /* Strategy mask: skip groups with no allowed strategies */\n    if (!(allowed_mask & ((1u<<S_BWT_O0)|(1u<<S_BWT_O1)|(1u<<S_BWT_O2)|(1u<<S_BWT_O0_PS)|(1u<<S_BWT_RANS)|(1u<<S_BWT_CTX2)|(1u<<S_BWT_O1_PS)|(1u<<S_LZP_BWT_O0)|(1u<<S_LZP_BWT_O1)|(1u<<S_LZP_BWT_O2))))\n        run_ac = 0;\n    if (!(allowed_mask & ((1u<<S_DBWT_O0)|(1u<<S_DBWT_O1)|(1u<<S_DBWT_O2)|(1u<<S_DLZP_BWT_O0)|(1u<<S_DLZP_BWT_O1)|(1u<<S_DLZP_BWT_O2)|(1u<<S_LZ77_O0)|(1u<<S_LZ77_O1)|(1u<<S_F32_BWT))))\n        run_bde = 0;\n    \n    /* === Parallel Groups A-E === */',
    "run_ac/run_bde mask")

# 12. GroupWork.allowed_mask for AC
c = do_replace(c,
    '        work_ac.hint_strat = hint_strat;\n        work_ac.data = data;',
    '        work_ac.hint_strat = hint_strat;\n        work_ac.allowed_mask = allowed_mask;\n        work_ac.data = data;',
    "work_ac.allowed_mask")

# 13. GroupWork.allowed_mask for BDE
c = do_replace(c,
    '        work_bde.hint_strat = hint_strat;\n        work_bde.data = data;',
    '        work_bde.hint_strat = hint_strat;\n        work_bde.allowed_mask = allowed_mask;\n        work_bde.data = data;',
    "work_bde.allowed_mask")

# 14. Audio mask check
c = do_replace(c,
    '    /* ======= Group F: Audio Pipeline ======= */\n    if (audio_detect(data, n)) {',
    '    /* ======= Group F: Audio Pipeline ======= */\n    if ((allowed_mask & (1u << S_AUDIO)) && audio_detect(data, n)) {',
    "Audio mask")

# 15. Base64v2 mask check (change bare { to if (mask) {)
c = do_replace(c,
    '    /* ======= Group G: Base64 Preprocessing (S_BASE64) ======= */\n    {\n        int max_b64_runs = n / B64_MIN_RUN + 1;',
    '    /* ======= Group G: Base64 Preprocessing (S_BASE64) ======= */\n    if (allowed_mask & (1u << S_BASE64_V2)) {\n        int max_b64_runs = n / B64_MIN_RUN + 1;',
    "Base64v2 mask")

# 16. LZMA mask check (before if(try_lzma) block)
c = do_replace(c,
    '    if (try_lzma) {\n        uint8_t *lzma_out = (uint8_t*)malloc(n + n/4 + 65536);',
    '    if (!(allowed_mask & (1u << S_LZMA))) try_lzma = 0;\n    if (try_lzma) {\n        uint8_t *lzma_out = (uint8_t*)malloc(n + n/4 + 65536);',
    "LZMA mask")

# 17. BCJ+LZMA mask check
c = do_replace(c,
    '    /* ======= Group I: BCJ+LZMA (S_BCJ_LZMA) ======= */\n    /* Phase 1.3: E8/E9 transform on x86 executables, then LZMA */\n    if (bcj_is_executable(data, n) && n >= 256) {',
    '    /* ======= Group I: BCJ+LZMA (S_BCJ_LZMA) ======= */\n    /* Phase 1.3: E8/E9 transform on x86 executables, then LZMA */\n    if ((allowed_mask & (1u << S_BCJ_LZMA)) && bcj_is_executable(data, n) && n >= 256) {',
    "BCJ+LZMA mask")

# 18. Forward declaration of file_compress_impl
c = do_replace(c,
    'static int file_compress_impl(const char *inpath, const char *outpath, int block_size, int nthreads, int force_no_png, int force_strategy);',
    'static int file_compress_impl(const char *inpath, const char *outpath, int block_size, int nthreads, int force_no_png, int force_strategy, uint32_t allowed_mask);',
    "fwd decl")

# 19. file_compress signature and body
c = do_replace(c,
    'static int file_compress(const char *inpath, const char *outpath, int block_size, int nthreads, int force_strategy) {\n    return file_compress_impl(inpath, outpath, block_size, nthreads, 0, force_strategy);\n}',
    'static int file_compress(const char *inpath, const char *outpath, int block_size, int nthreads, int force_strategy, uint32_t allowed_mask) {\n    return file_compress_impl(inpath, outpath, block_size, nthreads, 0, force_strategy, allowed_mask);\n}',
    "file_compress sig")

# 20. file_compress_impl signature
c = do_replace(c,
    'static int file_compress_impl(const char *inpath, const char *outpath, int block_size, int nthreads, int force_no_png, int force_strategy) {\n    FILE *fin = fopen(inpath, "rb");',
    'static int file_compress_impl(const char *inpath, const char *outpath, int block_size, int nthreads, int force_no_png, int force_strategy, uint32_t allowed_mask) {\n    FILE *fin = fopen(inpath, "rb");',
    "file_compress_impl sig")

# 21. Sequential compress_block call in file_compress_impl
c = do_replace(c,
    '                int csz = compress_block(inbuf, bsz, outbuf, &strat, bwt_ws, cws, force_strategy);\n                uint32_t crc = hp_crc32(inbuf, bsz);\n\n                fputc(0, fout);\n                fputc((uint8_t)strat, fout);',
    '                int csz = compress_block(inbuf, bsz, outbuf, &strat, bwt_ws, cws, force_strategy, allowed_mask);\n                uint32_t crc = hp_crc32(inbuf, bsz);\n\n                fputc(0, fout);\n                fputc((uint8_t)strat, fout);',
    "file_compress_impl sequential")

# 22. Parallel job setup - add allowed_mask
c = do_replace(c,
    '                jobs[j].force_strategy = force_strategy;\n                jobs[j].done = 0;',
    '                jobs[j].force_strategy = force_strategy;\n                jobs[j].allowed_mask = allowed_mask;\n                jobs[j].done = 0;',
    "parallel job allowed_mask")

# 23. PNG fallback
c = do_replace(c,
    '        return file_compress_impl(inpath, outpath, block_size, nthreads, 1, -1);',
    '        return file_compress_impl(inpath, outpath, block_size, nthreads, 1, -1, allowed_mask);',
    "PNG fallback")

# 24. archive_compress signature
c = do_replace(c,
    'static int archive_compress(int npaths, const char **paths, const char *outpath,\n                            int block_size, int nthreads, int force_strategy) {',
    'static int archive_compress(int npaths, const char **paths, const char *outpath,\n                            int block_size, int nthreads, int force_strategy, uint32_t allowed_mask) {',
    "archive_compress sig")

# 25. archive_compress compress_block call
c = do_replace(c,
    '                int csz = compress_block(inbuf, bsz, outbuf, &strat, bwt_ws, cws, force_strategy);\n                uint32_t blk_crc = hp_crc32(inbuf, bsz);',
    '                int csz = compress_block(inbuf, bsz, outbuf, &strat, bwt_ws, cws, force_strategy, allowed_mask);\n                uint32_t blk_crc = hp_crc32(inbuf, bsz);',
    "archive_compress block call")

# === CLI CHANGES ===

# 26. Add variables in 'c' command
c = do_replace(c,
    '        int nthreads = 1;\n        int force_strategy = -1;\n        int i = 2;\n        while (i < argc - 2) {',
    '        int nthreads = 1;\n        int force_strategy = -1;\n        uint32_t allowed_mask = 0xFFFFFFFFu;\n        int include_set = 0, exclude_set = 0;\n        int i = 2;\n        while (i < argc - 2) {',
    "c cmd vars")

# 27. Add -S/-X parsing in 'c' command (insert before "} else break;")
c = do_replace(c,
    '                    fprintf(stderr, "Use --list-strategies to see available strategies.\\n");\n                    return 1;\n                }\n                i++;\n            } else break;\n        }\n        if (nthreads > 1) {',

    '                    fprintf(stderr, "Use --list-strategies to see available strategies.\\n");\n                    return 1;\n                }\n                i++;\n            } else if ((strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--strategies") == 0) && i + 1 < argc) {\n                if (exclude_set) { fprintf(stderr, "Error: cannot use -S and -X together\\n"); return 1; }\n                include_set = 1;\n                allowed_mask = 0;\n                char *list = argv[++i];\n                char *tok = strtok(list, ",");\n                while (tok) {\n                    int sv = atoi(tok);\n                    if (sv < 0 || sv >= NUM_STRATEGIES) {\n                        fprintf(stderr, "Error: invalid strategy %d in -S (must be 0..%d)\\n", sv, NUM_STRATEGIES - 1);\n                        fprintf(stderr, "Use --list-strategies to see available strategies.\\n");\n                        return 1;\n                    }\n                    allowed_mask |= (1u << sv);\n                    tok = strtok(NULL, ",");\n                }\n                if (allowed_mask == 0) { fprintf(stderr, "Error: -S requires at least one strategy\\n"); return 1; }\n                i++;\n            } else if ((strcmp(argv[i], "-X") == 0 || strcmp(argv[i], "--exclude-strategies") == 0) && i + 1 < argc) {\n                if (include_set) { fprintf(stderr, "Error: cannot use -S and -X together\\n"); return 1; }\n                exclude_set = 1;\n                char *list = argv[++i];\n                char *tok = strtok(list, ",");\n                while (tok) {\n                    int sv = atoi(tok);\n                    if (sv < 0 || sv >= NUM_STRATEGIES) {\n                        fprintf(stderr, "Error: invalid strategy %d in -X (must be 0..%d)\\n", sv, NUM_STRATEGIES - 1);\n                        fprintf(stderr, "Use --list-strategies to see available strategies.\\n");\n                        return 1;\n                    }\n                    allowed_mask &= ~(1u << sv);\n                    tok = strtok(NULL, ",");\n                }\n                i++;\n            } else break;\n        }\n        if (nthreads > 1) {',
    "c cmd -S/-X parsing")

# 28. Add logging and update return for 'c' command
c = do_replace(c,
    '        if (force_strategy >= 0) {\n            fprintf(stderr, "[HP5] Forcing strategy %d (%s)\\n", force_strategy, strat_names[force_strategy]);\n        }\n        return file_compress(argv[i], argv[i+1], block_mb << 20, nthreads, force_strategy);',

    '        if (force_strategy >= 0) {\n            fprintf(stderr, "[HP5] Forcing strategy %d (%s)\\n", force_strategy, strat_names[force_strategy]);\n        }\n        if (allowed_mask != 0xFFFFFFFFu && force_strategy < 0) {\n            fprintf(stderr, "[HP5] Strategy filter: ");\n            int first = 1;\n            for (int s = 0; s < NUM_STRATEGIES; s++) {\n                if (allowed_mask & (1u << s)) {\n                    if (!first) fprintf(stderr, ", ");\n                    fprintf(stderr, "%d(%s)", s, strat_names[s]);\n                    first = 0;\n                }\n            }\n            fprintf(stderr, "\\n");\n        }\n        return file_compress(argv[i], argv[i+1], block_mb << 20, nthreads, force_strategy, allowed_mask);',
    "c cmd logging+return")

# 29. Add variables in 'a' command
c = do_replace(c,
    '        int block_mb = DEFAULT_BS >> 20;\n        int force_strategy = -1;\n        int i = 2;\n        while (i < argc - 1) {',
    '        int block_mb = DEFAULT_BS >> 20;\n        int force_strategy = -1;\n        uint32_t allowed_mask = 0xFFFFFFFFu;\n        int include_set = 0, exclude_set = 0;\n        int i = 2;\n        while (i < argc - 1) {',
    "a cmd vars")

# 30. Add -S/-X parsing in 'a' command
c = do_replace(c,
    '                    fprintf(stderr, "Use --list-strategies to see available strategies.\\n");\n                    return 1;\n                }\n                i++;\n            } else break;\n        }\n        int ninputs = argc - i - 1;',

    '                    fprintf(stderr, "Use --list-strategies to see available strategies.\\n");\n                    return 1;\n                }\n                i++;\n            } else if ((strcmp(argv[i], "-S") == 0 || strcmp(argv[i], "--strategies") == 0) && i + 1 < argc - 1) {\n                if (exclude_set) { fprintf(stderr, "Error: cannot use -S and -X together\\n"); return 1; }\n                include_set = 1;\n                allowed_mask = 0;\n                char *list = argv[++i];\n                char *tok = strtok(list, ",");\n                while (tok) {\n                    int sv = atoi(tok);\n                    if (sv < 0 || sv >= NUM_STRATEGIES) {\n                        fprintf(stderr, "Error: invalid strategy %d in -S (must be 0..%d)\\n", sv, NUM_STRATEGIES - 1);\n                        fprintf(stderr, "Use --list-strategies to see available strategies.\\n");\n                        return 1;\n                    }\n                    allowed_mask |= (1u << sv);\n                    tok = strtok(NULL, ",");\n                }\n                if (allowed_mask == 0) { fprintf(stderr, "Error: -S requires at least one strategy\\n"); return 1; }\n                i++;\n            } else if ((strcmp(argv[i], "-X") == 0 || strcmp(argv[i], "--exclude-strategies") == 0) && i + 1 < argc - 1) {\n                if (include_set) { fprintf(stderr, "Error: cannot use -S and -X together\\n"); return 1; }\n                exclude_set = 1;\n                char *list = argv[++i];\n                char *tok = strtok(list, ",");\n                while (tok) {\n                    int sv = atoi(tok);\n                    if (sv < 0 || sv >= NUM_STRATEGIES) {\n                        fprintf(stderr, "Error: invalid strategy %d in -X (must be 0..%d)\\n", sv, NUM_STRATEGIES - 1);\n                        fprintf(stderr, "Use --list-strategies to see available strategies.\\n");\n                        return 1;\n                    }\n                    allowed_mask &= ~(1u << sv);\n                    tok = strtok(NULL, ",");\n                }\n                i++;\n            } else break;\n        }\n        int ninputs = argc - i - 1;',
    "a cmd -S/-X parsing")

# 31. Add logging and update return for 'a' command
c = do_replace(c,
    '        if (force_strategy >= 0) {\n            fprintf(stderr, "[HPK6] Forcing strategy %d (%s)\\n", force_strategy, strat_names[force_strategy]);\n        }\n        const char **inputs = (const char**)&argv[i];\n        const char *outpath = argv[argc - 1];\n        return archive_compress(ninputs, inputs, outpath, block_mb << 20, 1, force_strategy);',

    '        if (force_strategy >= 0) {\n            fprintf(stderr, "[HPK6] Forcing strategy %d (%s)\\n", force_strategy, strat_names[force_strategy]);\n        }\n        if (allowed_mask != 0xFFFFFFFFu && force_strategy < 0) {\n            fprintf(stderr, "[HPK6] Strategy filter: ");\n            int first = 1;\n            for (int s = 0; s < NUM_STRATEGIES; s++) {\n                if (allowed_mask & (1u << s)) {\n                    if (!first) fprintf(stderr, ", ");\n                    fprintf(stderr, "%d(%s)", s, strat_names[s]);\n                    first = 0;\n                }\n            }\n            fprintf(stderr, "\\n");\n        }\n        const char **inputs = (const char**)&argv[i];\n        const char *outpath = argv[argc - 1];\n        return archive_compress(ninputs, inputs, outpath, block_mb << 20, 1, force_strategy, allowed_mask);',
    "a cmd logging+return")

# 32. Update help text
c = do_replace(c,
    '            "  -s N, --strategy N Force compression strategy N (0..%d, -1=auto)\\n"\n            "  --list-strategies  Show available strategies and exit\\n",',

    '            "  -s N, --strategy N Force compression strategy N (0..%d, -1=auto)\\n"\n            "  -S N,N,...         Only try listed strategies in auto mode\\n"\n            "  -X N,N,...         Exclude listed strategies from auto mode\\n"\n            "  --list-strategies  Show available strategies and exit\\n",',
    "help text")

with open('src/hyperpack.c', 'w') as f:
    f.write(c)
print(f"\nhyperpack.c: done ({len(c)} bytes)")

# ============================================================
# PATCH hyperpack_wasm.c
# ============================================================
print("\n=== Patching hyperpack_wasm.c ===")
with open('src/hyperpack_wasm.c', 'r') as f:
    w = f.read()

w = do_replace(w,
    'return file_compress("/input", "/output.hpk", block_mb << 20, 1, -1);',
    'return file_compress("/input", "/output.hpk", block_mb << 20, 1, -1, 0xFFFFFFFFu);',
    "wasm hp_compress")

w = do_replace(w,
    '    return archive_compress(1, paths, outpath, block_size, 1, -1);',
    '    return archive_compress(1, paths, outpath, block_size, 1, -1, 0xFFFFFFFFu);',
    "wasm hp_archive_compress")

with open('src/hyperpack_wasm.c', 'w') as f:
    f.write(w)
print(f"hyperpack_wasm.c: done ({len(w)} bytes)")

# ============================================================
# PATCH hyperpack_lib.c
# ============================================================
print("\n=== Patching hyperpack_lib.c ===")
with open('src/hyperpack_lib.c', 'r') as f:
    l = f.read()

l = do_replace(l,
    '    return file_compress(inpath, outpath, block_mb << 20, nthreads, -1);',
    '    return file_compress(inpath, outpath, block_mb << 20, nthreads, -1, 0xFFFFFFFFu);',
    "lib hp_lib_compress")

l = do_replace(l,
    '    return archive_compress(npaths, paths, outpath, block_mb << 20, nthreads, -1);',
    '    return archive_compress(npaths, paths, outpath, block_mb << 20, nthreads, -1, 0xFFFFFFFFu);',
    "lib hp_lib_archive_compress")

l = do_replace(l,
    '    return file_compress(inpath, outpath, block_mb << 20, nthreads, force_strategy);',
    '    return file_compress(inpath, outpath, block_mb << 20, nthreads, force_strategy, 0xFFFFFFFFu);',
    "lib hp_lib_compress_with_strategy")

l = do_replace(l,
    '    return archive_compress(npaths, paths, outpath, block_mb << 20, nthreads, force_strategy);',
    '    return archive_compress(npaths, paths, outpath, block_mb << 20, nthreads, force_strategy, 0xFFFFFFFFu);',
    "lib hp_lib_archive_compress_with_strategy")

# Add new filtered functions
lib_addition = '''
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
'''

l = l.rstrip() + '\n' + lib_addition
print(f"  OK [lib filtered functions]")

with open('src/hyperpack_lib.c', 'w') as f:
    f.write(l)
print(f"hyperpack_lib.c: done ({len(l)} bytes)")

print("\n=== All patches applied successfully ===")
