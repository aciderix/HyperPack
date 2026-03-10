/*
 * HyperPack Quantum v11 — LZP Sub-Stream Compression — Ultra-High Compression Engine (C)
 *
 * Pipeline A: [Delta] -> [LZP] -> BWT(SA) -> MTF -> ZRLE -> Range Coder (order-0/1)
 * Pipeline B: Context Mixing (7 models + logistic mixer + bit-level range coder)
 * Multi-strategy per block, entropy detection, block dedup, CRC-32 integrity.
 *
 * Compile: gcc -O3 -nodefaultlibs -o hyperpack src/hyperpack.c -lc -lm
 * Usage:   ./hyperpack c [-b SIZE_MB] input output.hpk
 *          ./hyperpack d input.hpk output
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
/* fmemopen polyfill for Windows: create a self-deleting tmpfile, write buf, rewind */
static FILE *hp_fmemopen(void *buf, size_t size, const char *mode) {
    (void)mode;
    FILE *f = tmpfile();
    if (!f) return NULL;
    if (size > 0) fwrite(buf, 1, size, f);
    rewind(f);
    return f;
}
#define fmemopen hp_fmemopen
#endif
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <zlib.h>
#ifdef HYPERPACK_WASM
/* WASM build: prevent thread creation; mutex/join are no-ops via Emscripten stubs */
static inline int pthread_create_stub(pthread_t *t, const void *a, void *(*f)(void*), void *arg)
    { (void)t;(void)a;(void)f;(void)arg; return -1; }
static inline int pthread_join_stub(pthread_t t, void **r) { (void)t;(void)r; return 0; }
#define pthread_create pthread_create_stub
#define pthread_join   pthread_join_stub
#endif

/* ===== Configuration ===== */
#define DEFAULT_BS    (128 << 20)   /* 128 MB — BWT needs large blocks */
#define ZRLE_ALPHA    258         /* RUNA=0, RUNB=1, byte+2 */
#define RC_TOP        (1u << 24)
#define RC_BOT        (1u << 16)
#define MAX_TOTAL     16383
#define MAGIC         0x48504B35u /* "HPK5" */
#define MAGIC6        0x48504B36u /* "HPK6" archive */
#define VERSION       11

/* Pre-transform types (stored in HPK5 V11 header byte after VERSION) */
#define PT_NONE  0   /* no pre-transform — raw file bytes */
#define PT_PNG   1   /* PNG: IDAT inflated; meta = non-IDAT/IEND chunks */

/* LZP configuration */
#define LZP_HTAB_BITS  20
#define LZP_HTAB_SIZE  (1 << LZP_HTAB_BITS)
#define LZP_HTAB_MASK  (LZP_HTAB_SIZE - 1)
#define LZP_CONTEXT    8
#define LZP_MIN_MATCH  12

enum Strategy {
    S_STORE=0,
    S_BWT_O0=1, S_BWT_O1=2,
    S_DBWT_O0=3, S_DBWT_O1=4,
    S_LZP_BWT_O0=5, S_LZP_BWT_O1=6,
    S_DLZP_BWT_O0=7, S_DLZP_BWT_O1=8,
    S_CM=9,
    S_BWT_CM=10,
    S_BWT_MTF_CM=11,
    S_BWT_O2=12, S_DBWT_O2=13,
    S_LZP_BWT_O2=14, S_DLZP_BWT_O2=15,
    S_PPM=16, S_BWT_PPM=17, S_BWT_MTF_PPM=18, S_AUDIO=19, S_BASE64=20, S_BASE64_V2=21,
    S_LZ77_O0=22, S_LZ77_O1=23,
    S_LZMA=24,
    S_BCJ_LZMA=25,
    S_F32_BWT=26,  /* IEEE 754 float32 XOR-delta → BWT+RC */
    S_BWT_O0_PS=27, /* BWT+RC O0 with pre-scanned frequency table (cold-start fix) */
    S_BWT_RANS=28,  /* BWT+rANS O0 (table-based decode, same ratio as O0_PS) */
    S_BWT_CTX2=29,  /* BWT+RC 2-context O0 (run-ctx vs literal-ctx models) */
    S_BWT_O1_PS=30  /* BWT+RC O1 with pre-scanned global freq init (warm start) */
};
static const char *strat_names[] = {
    "STORE","BWT+O0","BWT+O1","D+BWT+O0","D+BWT+O1",
    "LZP+BWT+O0","LZP+BWT+O1","D+LZP+BWT+O0","D+LZP+BWT+O1",
    "CM","BWT+CM","BWT+MTF+CM",
    "BWT+O2","D+BWT+O2","LZP+BWT+O2","D+LZP+BWT+O2",
    "PPM","BWT+PPM","BWT+MTF+PPM","Audio", "Base64", "Base64v2",
    "LZ77+BWT+O0","LZ77+BWT+O1",
    "LZMA",
    "BCJ+LZMA",
    "F32XOR+BWT",
    "BWT+O0PS",
    "BWT+rANS",
    "BWT+CTX2",
    "BWT+O1PS"
};

/* ===== HPK6 Archive Structures ===== */
typedef struct {
    uint8_t  type;        /* 0=file, 1=directory */
    char    *path;        /* relative path (UTF-8, / separated) */
    uint64_t size;        /* original file size */
    uint32_t perms;       /* Unix permissions */
    int64_t  mtime;       /* modification time */
    uint32_t crc;         /* CRC32 of file content */
    uint32_t first_block; /* first block index */
    uint32_t nblocks;     /* number of blocks */
    uint8_t  is_dedup;    /* 1 if duplicate of another file */
    uint32_t dedup_ref;   /* index of original file */
    char    *fullpath;    /* full filesystem path (not stored in archive) */
} HPK6Entry;

/* ===== Inline buffer helpers ===== */
static inline void put_u32be(uint8_t *p, int v) {
    p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;  p[3] = v & 0xFF;
}
static inline int get_u32be(const uint8_t *p) {
    return ((int)p[0] << 24) | ((int)p[1] << 16) | ((int)p[2] << 8) | p[3];
}

/* ===== CRC-32 ===== */
static uint32_t crc_tab[256];
static int crc_ready = 0;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        crc_tab[i] = c;
    }
    crc_ready = 1;
}

static uint32_t hp_crc32(const uint8_t *d, size_t len) {
    if (!crc_ready) crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = (c >> 8) ^ crc_tab[(c ^ d[i]) & 0xFF];
    return c ^ 0xFFFFFFFFu;
}

/* Chained CRC32: continue from a previous result (e.g. for PNG chunk CRCs) */
static uint32_t hp_crc32_chain(uint32_t prev, const uint8_t *d, size_t len) {
    if (!crc_ready) crc_init();
    uint32_t c = prev ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) c = (c >> 8) ^ crc_tab[(c ^ d[i]) & 0xFF];
    return c ^ 0xFFFFFFFFu;
}

/* ===== SA-IS: O(n) Suffix Array Construction ===== */

/* SA-IS helper: get bucket starts (end=0) or ends (end=1) */
static void sais_get_buckets(const int32_t *T, int n, int K, int32_t *bkt, int end) {
    for (int i = 0; i < K; i++) bkt[i] = 0;
    for (int i = 0; i < n; i++) bkt[T[i]]++;
    int sum = 0;
    for (int i = 0; i < K; i++) {
        sum += bkt[i];
        bkt[i] = end ? sum : (sum - bkt[i]);
    }
}

/* Core SA-IS for integer alphabet [0..K-1], T[n-1] must be 0 (sentinel) */
static void sais_core(int32_t *T, int32_t *SA, int n, int K) {
    if (n <= 2) {
        SA[0] = n - 1;
        if (n == 2) SA[1] = 0;
        return;
    }

    /* Allocate type array as bit-packed */
    int type_bytes = (n + 7) / 8;
    uint8_t *t = (uint8_t *)calloc(type_bytes, 1);

    #define SAIS_S(i) ((t[(i)>>3] >> ((i)&7)) & 1)
    #define SAIS_SET_S(i) (t[(i)>>3] |= (1 << ((i)&7)))
    #define SAIS_CLR_S(i) (t[(i)>>3] &= ~(1 << ((i)&7)))
    #define SAIS_LMS(i) ((i) > 0 && SAIS_S(i) && !SAIS_S((i)-1))

    /* Step 1: Classify suffixes */
    SAIS_SET_S(n - 1);
    for (int i = n - 2; i >= 0; i--) {
        if (T[i] < T[i + 1] || (T[i] == T[i + 1] && SAIS_S(i + 1)))
            SAIS_SET_S(i);
        else
            SAIS_CLR_S(i);
    }

    int32_t *bkt = (int32_t *)malloc(K * sizeof(int32_t));

    /* Step 2: Place LMS suffixes into bucket tails */
    sais_get_buckets(T, n, K, bkt, 1);
    for (int i = 0; i < n; i++) SA[i] = -1;
    for (int i = n - 1; i >= 1; i--) {
        if (SAIS_LMS(i)) SA[--bkt[T[i]]] = i;
    }
    /* Sentinel is always LMS and smallest — place it */
    SA[0] = n - 1;  /* bucket[0] since T[n-1]=0 */

    /* Step 3: Induced sort L-type */
    sais_get_buckets(T, n, K, bkt, 0);
    for (int i = 0; i < n; i++) {
        if (SA[i] > 0 && !SAIS_S(SA[i] - 1))
            SA[bkt[T[SA[i] - 1]]++] = SA[i] - 1;
        else if (SA[i] == 0 && !SAIS_S(n - 1))  /* position 0, predecessor wraps */
            ; /* n-1 is S-type (sentinel), skip */
    }

    /* Step 4: Induced sort S-type */
    sais_get_buckets(T, n, K, bkt, 1);
    for (int i = n - 1; i >= 0; i--) {
        if (SA[i] > 0 && SAIS_S(SA[i] - 1))
            SA[--bkt[T[SA[i] - 1]]] = SA[i] - 1;
    }

    /* Step 5: Compact sorted LMS substrings into first n1 slots */
    int n1 = 0;
    for (int i = 0; i < n; i++) {
        if (SAIS_LMS(SA[i])) SA[n1++] = SA[i];
    }

    /* Step 6: Name LMS substrings */
    for (int i = n1; i < n; i++) SA[i] = -1;

    int name = 0, prev = -1;
    for (int i = 0; i < n1; i++) {
        int pos = SA[i];
        int diff = (prev < 0);
        if (!diff) {
            /* Compare LMS substrings at prev and pos */
            for (int d = 0; ; d++) {
                if (prev + d >= n || pos + d >= n ||
                    T[prev + d] != T[pos + d] ||
                    SAIS_S(prev + d) != SAIS_S(pos + d)) {
                    diff = 1; break;
                }
                if (d > 0 && (SAIS_LMS(prev + d) || SAIS_LMS(pos + d))) {
                    /* Both reached end of LMS substring */
                    if (SAIS_LMS(prev + d) && SAIS_LMS(pos + d))
                        break;  /* same */
                    diff = 1; break;  /* different lengths */
                }
            }
        }
        if (diff) { name++; prev = pos; }
        /* Store name in second half of SA. LMS positions are never adjacent,
           so pos/2 (or pos>>1) gives unique indices in [0, n/2). */
        SA[n1 + (pos >> 1)] = name - 1;
    }

    /* Compact names into T1 at end of SA (right-to-left to avoid overlap) */
    {
        int j = n - 1;
        for (int i = n - 1; i >= n1; i--) {
            if (SA[i] >= 0) SA[j--] = SA[i];
        }
    }
    int32_t *T1 = SA + n - n1;

    /* Step 7: Solve reduced problem */
    int32_t *SA1 = SA;
    if (name < n1) {
        sais_core(T1, SA1, n1, name);
    } else {
        for (int i = 0; i < n1; i++) SA1[T1[i]] = i;
    }

    /* Step 8: Reconstruct LMS positions and do final induced sort */
    /* Collect LMS positions in text order into SA[n-n1..n-1] */
    {
        int j = n;
        for (int i = n - 1; i >= 0; i--) {
            if (SAIS_LMS(i)) SA[--j] = i;
        }
    }
    /* Now SA[n-n1..n-1] = LMS positions in text order */
    /* SA[0..n1-1] = SA1 = suffix array of reduced string */

    /* Map SA1 to original LMS positions */
    for (int i = 0; i < n1; i++) {
        SA1[i] = SA[n - n1 + SA1[i]];
    }
    /* Now SA[0..n1-1] = sorted original LMS positions */

    /* Place sorted LMS at bucket tails (Nong's method: read from SA[0..n1-1]) */
    sais_get_buckets(T, n, K, bkt, 1);
    for (int i = n1; i < n; i++) SA[i] = -1;
    for (int i = n1 - 1; i >= 0; i--) {
        int j = SA[i]; SA[i] = -1;
        SA[--bkt[T[j]]] = j;
    }

    /* Induced sort L-type */
    sais_get_buckets(T, n, K, bkt, 0);
    for (int i = 0; i < n; i++) {
        if (SA[i] > 0 && !SAIS_S(SA[i] - 1))
            SA[bkt[T[SA[i] - 1]]++] = SA[i] - 1;
    }

    /* Induced sort S-type */
    sais_get_buckets(T, n, K, bkt, 1);
    for (int i = n - 1; i >= 0; i--) {
        if (SA[i] > 0 && SAIS_S(SA[i] - 1))
            SA[--bkt[T[SA[i] - 1]]] = SA[i] - 1;
    }

    #undef SAIS_S
    #undef SAIS_SET_S
    #undef SAIS_CLR_S
    #undef SAIS_LMS
    free(t);
    free(bkt);
}

/* Wrapper matching the original build_sa signature */
static void build_sa(int32_t *T, int n, int32_t *SA) {
    sais_core(T, SA, n, 257);
}

/* ===== BWT encode (sentinel approach) ===== */
static int bwt_encode(const uint8_t *in, int n, uint8_t *out) {
    int n1 = n + 1;
    int32_t *T  = (int32_t*)malloc(n1 * sizeof(int32_t));
    int32_t *SA = (int32_t*)malloc(n1 * sizeof(int32_t));
    for (int i = 0; i < n; i++) T[i] = (int32_t)in[i] + 1;
    T[n] = 0;
    build_sa(T, n1, SA);
    for (int i = 0; i < n; i++) T[i] = (int32_t)in[i] + 1;
    T[n] = 0;
    int pidx = -1, j = 0;
    for (int i = 0; i < n1; i++) {
        if (SA[i] == 0) { pidx = j; continue; }
        out[j++] = (uint8_t)(T[SA[i] - 1] - 1);
    }
    free(T); free(SA);
    return pidx;
}


/* ===== BWT Workspace (pre-allocated) ===== */
typedef struct {
    int32_t *T;
    int32_t *SA;
    int capacity;
} BWTWorkspace;

static BWTWorkspace *bwt_ws_create(int max_n) {
    BWTWorkspace *ws = (BWTWorkspace*)malloc(sizeof(BWTWorkspace));
    int n1 = max_n + 1;
    ws->T = (int32_t*)malloc((size_t)n1 * sizeof(int32_t));
    ws->SA = (int32_t*)malloc((size_t)n1 * sizeof(int32_t));
    ws->capacity = n1;
    return ws;
}

static void bwt_ws_ensure(BWTWorkspace *ws, int n1) {
    if (n1 > ws->capacity) {
        free(ws->T); free(ws->SA);
        ws->T = (int32_t*)malloc((size_t)n1 * sizeof(int32_t));
        ws->SA = (int32_t*)malloc((size_t)n1 * sizeof(int32_t));
        ws->capacity = n1;
    }
}

static void bwt_ws_free(BWTWorkspace *ws) {
    if (ws) { free(ws->T); free(ws->SA); free(ws); }
}

static int bwt_encode_ws(const uint8_t *in, int n, uint8_t *out, BWTWorkspace *ws) {
    int n1 = n + 1;
    bwt_ws_ensure(ws, n1);
    int32_t *T = ws->T;
    int32_t *SA = ws->SA;
    for (int i = 0; i < n; i++) T[i] = (int32_t)in[i] + 1;
    T[n] = 0;
    build_sa(T, n1, SA);
    for (int i = 0; i < n; i++) T[i] = (int32_t)in[i] + 1;
    T[n] = 0;
    int pidx = -1, j = 0;
    for (int i = 0; i < n1; i++) {
        if (SA[i] == 0) { pidx = j; continue; }
        out[j++] = (uint8_t)(T[SA[i] - 1] - 1);
    }
    return pidx;
}


/* ===== Compress Workspace (pre-allocated buffers) ===== */
typedef struct {
    uint8_t  *bwt_buf;
    uint8_t  *mtf_buf;
    uint16_t *zrle_buf;
    uint8_t  *arith_buf;
    uint8_t  *delta_buf;
    uint8_t  *lzp_buf;
    int capacity;
} CompressWorkspace;

static CompressWorkspace *cws_create(int max_n) {
    CompressWorkspace *ws = (CompressWorkspace*)malloc(sizeof(CompressWorkspace));
    int alloc = max_n + 262144;
    ws->bwt_buf   = (uint8_t*)malloc(alloc);
    ws->mtf_buf   = (uint8_t*)malloc(alloc);
    ws->zrle_buf  = (uint16_t*)malloc((size_t)alloc * 2 * sizeof(uint16_t));
    ws->arith_buf = (uint8_t*)malloc(alloc + alloc/4 + 1024);
    ws->delta_buf = (uint8_t*)malloc(alloc);
    ws->lzp_buf   = (uint8_t*)malloc(alloc + alloc/4 + 1024);
    ws->capacity = alloc;
    return ws;
}

static void cws_free(CompressWorkspace *ws) {
    if (ws) {
        free(ws->bwt_buf); free(ws->mtf_buf); free(ws->zrle_buf);
        free(ws->arith_buf); free(ws->delta_buf); free(ws->lzp_buf);
        free(ws);
    }
}

/* ===== BWT decode (LF mapping) ===== */
static void bwt_decode(const uint8_t *bwt, int n, int pidx, uint8_t *out) {
    int32_t freq[256] = {0};
    for (int i = 0; i < n; i++) freq[bwt[i]]++;
    int32_t C[256]; int32_t s = 1;
    for (int c = 0; c < 256; c++) { C[c] = s; s += freq[c]; }

    int32_t *LF = (int32_t*)malloc((n + 1) * sizeof(int32_t));
    int32_t rk[256] = {0};
    for (int fi = 0; fi <= n; fi++) {
        if (fi == pidx) { LF[fi] = 0; continue; }
        int ci = (fi < pidx) ? fi : fi - 1;
        uint8_t c = bwt[ci];
        LF[fi] = C[c] + rk[c]; rk[c]++;
    }
    int pos = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (pos < pidx) out[i] = bwt[pos];
        else if (pos > pidx) out[i] = bwt[pos - 1];
        else out[i] = 0;
        pos = LF[pos];
    }
    free(LF);
}

/* ===== MTF encode/decode ===== */
static void mtf_encode(const uint8_t *in, int n, uint8_t *out) {
    /* MTF-2: first access moves to pos 1, second (from pos 1) promotes to 0 */
    /* HyperPack v10.1: +0.84% on BWT files vs standard MTF */
    uint8_t list[256];
    uint8_t pos[256];
    for (int i = 0; i < 256; i++) { list[i] = (uint8_t)i; pos[i] = (uint8_t)i; }
    for (int i = 0; i < n; i++) {
        uint8_t c = in[i];
        uint8_t idx = pos[c];
        out[i] = idx;
        if (idx == 1) {
            /* Already at pos 1 -> promote to pos 0 */
            uint8_t prev = list[0];
            list[0] = c;
            list[1] = prev;
            pos[c] = 0;
            pos[prev] = 1;
        } else if (idx > 1) {
            /* Move to pos 1 (not 0) on first access */
            for (int j = idx; j > 1; j--) {
                list[j] = list[j - 1];
                pos[list[j]] = (uint8_t)j;
            }
            list[1] = c;
            pos[c] = 1;
        }
    }
}

static void mtf_decode(const uint8_t *in, int n, uint8_t *out) {
    /* MTF-2 decode: mirror the encode logic */
    uint8_t list[256];
    for (int i = 0; i < 256; i++) list[i] = (uint8_t)i;
    for (int i = 0; i < n; i++) {
        int idx = in[i]; uint8_t c = list[idx];
        out[i] = c;
        if (idx == 1) {
            list[1] = list[0];
            list[0] = c;
        } else if (idx > 1) {
            memmove(list + 2, list + 1, idx - 1);
            list[1] = c;
        }
    }
}

/* ===== ZRLE encode/decode (bzip2-style bijective base-2) ===== */
static int zrle_encode(const uint8_t *in, int n, uint16_t *out) {
    int j = 0, i = 0;
    while (i < n) {
        if (in[i] == 0) {
            int run = 0;
            while (i < n && in[i] == 0) { run++; i++; }
            run++;
            while (run > 1) { out[j++] = (run & 1) ? 1 : 0; run >>= 1; }
        } else {
            out[j++] = (uint16_t)in[i] + 1;
            i++;
        }
    }
    return j;
}

static int zrle_decode(const uint16_t *in, int nsyms, uint8_t *out, int maxout) {
    int j = 0, i = 0;
    while (i < nsyms && j < maxout) {
        if (in[i] <= 1) {
            int run = 0, place = 1;
            while (i < nsyms && in[i] <= 1) {
                run += ((int)in[i] + 1) * place;
                place <<= 1; i++;
            }
            for (int k = 0; k < run && j < maxout; k++) out[j++] = 0;
        } else {
            out[j++] = (uint8_t)(in[i] - 1); i++;
        }
    }
    return j;
}

/* ===== Frequency Model (flat array + Fenwick tree) ===== */
typedef struct {
    int16_t freq[ZRLE_ALPHA];
    int16_t tree[ZRLE_ALPHA + 1];  /* 1-indexed Fenwick */
    int32_t total;
} Model;

static void model_build_tree(Model *m) {
    memset(m->tree, 0, sizeof(m->tree));
    for (int i = 0; i < ZRLE_ALPHA; i++) {
        int idx = i + 1;
        m->tree[idx] += m->freq[i];
        int p = idx + (idx & (-idx));
        if (p <= ZRLE_ALPHA) m->tree[p] += m->tree[idx];
    }
}

static void model_init(Model *m) {
    for (int i = 0; i < ZRLE_ALPHA; i++) m->freq[i] = 1;
    m->total = ZRLE_ALPHA;
    model_build_tree(m);
}

static inline void model_tree_add(Model *m, int i, int delta) {
    for (i += 1; i <= ZRLE_ALPHA; i += i & (-i)) m->tree[i] += delta;
}

static inline int model_prefix(Model *m, int i) {
    int s = 0;
    for (; i > 0; i -= i & (-i)) s += m->tree[i];
    return s;
}

static int model_cum(Model *m, int sym) { return model_prefix(m, sym); }
static int model_freq_of(Model *m, int sym) { return m->freq[sym]; }

static int model_find(Model *m, int target) {
    int pos = 0;
    for (int bit = 256; bit > 0; bit >>= 1) {
        if (pos + bit <= ZRLE_ALPHA && m->tree[pos + bit] <= target) {
            pos += bit; target -= m->tree[pos];
        }
    }
    return pos;
}

static void model_update(Model *m, int sym) {
    m->freq[sym]++;
    model_tree_add(m, sym, 1);
    m->total++;
    if (m->total >= MAX_TOTAL) {
        m->total = 0;
        for (int i = 0; i < ZRLE_ALPHA; i++) {
            m->freq[i] = (m->freq[i] + 1) >> 1;
            m->total += m->freq[i];
        }
        model_build_tree(m);
    }
}

/* ===== Range Coder ===== */
typedef struct { uint32_t low, range; uint8_t *buf; int pos; } RCEnc;
typedef struct { uint32_t low, code, range; const uint8_t *buf; int pos; } RCDec;

static void rc_enc_init(RCEnc *e, uint8_t *buf) {
    e->low = 0; e->range = 0xFFFFFFFFu; e->buf = buf; e->pos = 0;
}

static inline void rc_enc_norm(RCEnc *e) {
    while ((e->low ^ (e->low + e->range)) < RC_TOP ||
           (e->range < RC_BOT && ((e->range = (uint32_t)(-(int32_t)e->low) & (RC_BOT - 1)), 1))) {
        e->buf[e->pos++] = (uint8_t)(e->low >> 24);
        e->range <<= 8; e->low <<= 8;
    }
}

static void rc_enc(RCEnc *e, uint32_t cum, uint32_t freq, uint32_t total) {
    e->range /= total;
    e->low += cum * e->range;
    e->range *= freq;
    rc_enc_norm(e);
}

static int rc_enc_finish(RCEnc *e) {
    for (int i = 0; i < 4; i++) {
        e->buf[e->pos++] = (uint8_t)(e->low >> 24); e->low <<= 8;
    }
    return e->pos;
}

static void rc_dec_init(RCDec *d, const uint8_t *buf) {
    d->low = 0; d->code = 0; d->range = 0xFFFFFFFFu; d->buf = buf; d->pos = 0;
    for (int i = 0; i < 4; i++) d->code = (d->code << 8) | d->buf[d->pos++];
}

static inline void rc_dec_norm(RCDec *d) {
    while ((d->low ^ (d->low + d->range)) < RC_TOP ||
           (d->range < RC_BOT && ((d->range = (uint32_t)(-(int32_t)d->low) & (RC_BOT - 1)), 1))) {
        d->code = (d->code << 8) | d->buf[d->pos++];
        d->range <<= 8; d->low <<= 8;
    }
}

static uint32_t rc_dec_cum(RCDec *d, uint32_t total) {
    d->range /= total;
    uint32_t c = (d->code - d->low) / d->range;
    return c < total ? c : total - 1;
}

static void rc_dec_update(RCDec *d, uint32_t cum, uint32_t freq) {
    d->low += cum * d->range;
    d->range *= freq;
    rc_dec_norm(d);
}

/* ===== Arithmetic Coding Order-0 ===== */
static int arith_enc_o0(const uint16_t *syms, int nsyms, uint8_t *out) {
    RCEnc rc; rc_enc_init(&rc, out);
    Model m; model_init(&m);
    for (int i = 0; i < nsyms; i++) {
        int s = syms[i];
        rc_enc(&rc, (uint32_t)model_cum(&m, s), (uint32_t)model_freq_of(&m, s), (uint32_t)m.total);
        model_update(&m, s);
    }
    return rc_enc_finish(&rc);
}

static void arith_dec_o0(const uint8_t *in, int nsyms, uint16_t *out) {
    RCDec rc; rc_dec_init(&rc, in);
    Model m; model_init(&m);
    for (int i = 0; i < nsyms; i++) {
        uint32_t target = rc_dec_cum(&rc, (uint32_t)m.total);
        int s = model_find(&m, (int)target);
        uint32_t cum = (uint32_t)model_cum(&m, s);
        uint32_t freq = (uint32_t)model_freq_of(&m, s);
        rc_dec_update(&rc, cum, freq);
        out[i] = (uint16_t)s;
        model_update(&m, s);
    }
}

/* ===== Arithmetic Coding Order-1 ===== */
static int arith_enc_o1(const uint16_t *syms, int nsyms, uint8_t *out) {
    RCEnc rc; rc_enc_init(&rc, out);
    Model *models = (Model*)malloc(ZRLE_ALPHA * sizeof(Model));
    for (int i = 0; i < ZRLE_ALPHA; i++) model_init(&models[i]);
    int prev = 0;
    for (int i = 0; i < nsyms; i++) {
        int s = syms[i];
        Model *m = &models[prev];
        rc_enc(&rc, (uint32_t)model_cum(m, s), (uint32_t)model_freq_of(m, s), (uint32_t)m->total);
        model_update(m, s);
        prev = s;
    }
    free(models);
    return rc_enc_finish(&rc);
}

static void arith_dec_o1(const uint8_t *in, int nsyms, uint16_t *out) {
    RCDec rc; rc_dec_init(&rc, in);
    Model *models = (Model*)malloc(ZRLE_ALPHA * sizeof(Model));
    for (int i = 0; i < ZRLE_ALPHA; i++) model_init(&models[i]);
    int prev = 0;
    for (int i = 0; i < nsyms; i++) {
        Model *m = &models[prev];
        uint32_t target = rc_dec_cum(&rc, (uint32_t)m->total);
        int s = model_find(m, (int)target);
        rc_dec_update(&rc, (uint32_t)model_cum(m, s), (uint32_t)model_freq_of(m, s));
        out[i] = (uint16_t)s;
        model_update(m, s);
        prev = s;
    }
    free(models);
}

/* ===== Arithmetic Coding Order-2 (hashed context) ===== */
#define O2_NCTX 65536   /* must be power of 2, covers all 258*258 ZRLE ctx pairs */
#define O2_MASK (O2_NCTX - 1)

static inline int o2_hash(int prev2, int prev1) {
    return (int)(((uint32_t)(prev2 * 259 + prev1) * 2654435761u) >> 16) & O2_MASK;
}

static int arith_enc_o2(const uint16_t *syms, int nsyms, uint8_t *out) {
    RCEnc rc; rc_enc_init(&rc, out);
    Model *models = (Model*)malloc(O2_NCTX * sizeof(Model));
    for (int i = 0; i < O2_NCTX; i++) model_init(&models[i]);
    int prev2 = 0, prev1 = 0;
    for (int i = 0; i < nsyms; i++) {
        int s = syms[i];
        Model *m = &models[o2_hash(prev2, prev1)];
        rc_enc(&rc, (uint32_t)model_cum(m, s), (uint32_t)model_freq_of(m, s), (uint32_t)m->total);
        model_update(m, s);
        prev2 = prev1; prev1 = s;
    }
    free(models);
    return rc_enc_finish(&rc);
}

static void arith_dec_o2(const uint8_t *in, int nsyms, uint16_t *out) {
    RCDec rc; rc_dec_init(&rc, in);
    Model *models = (Model*)malloc(O2_NCTX * sizeof(Model));
    for (int i = 0; i < O2_NCTX; i++) model_init(&models[i]);
    int prev2 = 0, prev1 = 0;
    for (int i = 0; i < nsyms; i++) {
        Model *m = &models[o2_hash(prev2, prev1)];
        uint32_t target = rc_dec_cum(&rc, (uint32_t)m->total);
        int s = model_find(m, (int)target);
        rc_dec_update(&rc, (uint32_t)model_cum(m, s), (uint32_t)model_freq_of(m, s));
        out[i] = (uint16_t)s;
        model_update(m, s);
        prev2 = prev1; prev1 = s;
    }
    free(models);
}

/* ===== Arithmetic Coding O0 with Pre-Scanned frequency table (S_BWT_O0_PS) =====
 *
 * Root cause of HP's 2-3% gap vs bzip2 on small text files:
 *  1. Cold-start: model starts uniform (1/258 per symbol), wastes bits during
 *     the first ~16K symbols while converging to the true distribution.
 *  2. Dead symbols: ~200 of 258 ZRLE symbols never appear; their constant
 *     freq=1 permanently "steals" ~2% of probability mass from active symbols.
 *
 * Fix: one forward pass to count actual frequencies, then store a 258-byte
 * normalized frequency table at the start of the compressed stream.  Both
 * encoder and decoder initialise the model from this table, so the coder
 * starts with near-optimal probabilities and active symbols share 100% of the
 * probability mass.  Overhead: ZRLE_ALPHA (258) bytes per BWT block.
 *
 * Format: [258 bytes freq_table | range-coded stream]
 *   freq_table[i] = 0          → symbol i never appears (excluded from model)
 *   freq_table[i] = 1..255     → proportional initial weight (scaled so that
 *                                 the most-frequent symbol gets 255)
 */
static int arith_enc_o0_ps(const uint16_t *syms, int nsyms, uint8_t *out) {
    /* Phase 1: count raw frequencies */
    int count[ZRLE_ALPHA];
    memset(count, 0, sizeof(count));
    for (int i = 0; i < nsyms; i++) count[syms[i]]++;

    /* Scale to uint8_t [1..255] for seen symbols, 0 for never-seen */
    int maxcount = 0;
    for (int i = 0; i < ZRLE_ALPHA; i++)
        if (count[i] > maxcount) maxcount = count[i];

    uint8_t *header = out;                     /* first 258 bytes = freq table */
    for (int i = 0; i < ZRLE_ALPHA; i++) {
        if (count[i] == 0) {
            header[i] = 0;
        } else {
            int scaled = (int)((int64_t)count[i] * 254 / maxcount) + 1; /* 1..255 */
            header[i] = (uint8_t)scaled;
        }
    }

    /* Phase 2: init model from frequency table */
    Model m;
    memset(&m, 0, sizeof(m));
    m.total = 0;
    for (int i = 0; i < ZRLE_ALPHA; i++) {
        m.freq[i] = header[i];
        m.total  += header[i];
    }
    model_build_tree(&m);

    /* Phase 3: adaptive range coding (encoder and decoder stay in sync) */
    RCEnc rc; rc_enc_init(&rc, out + ZRLE_ALPHA);
    for (int i = 0; i < nsyms; i++) {
        int s = syms[i];
        rc_enc(&rc, (uint32_t)model_cum(&m, s), (uint32_t)model_freq_of(&m, s),
               (uint32_t)m.total);
        model_update(&m, s);
    }
    int arith_bytes = rc_enc_finish(&rc);
    return ZRLE_ALPHA + arith_bytes;
}

static void arith_dec_o0_ps(const uint8_t *in, int nsyms, uint16_t *out) {
    /* Read frequency table header and init model */
    Model m;
    memset(&m, 0, sizeof(m));
    m.total = 0;
    for (int i = 0; i < ZRLE_ALPHA; i++) {
        m.freq[i] = in[i];
        m.total  += in[i];
    }
    model_build_tree(&m);

    /* Decode */
    RCDec rc; rc_dec_init(&rc, in + ZRLE_ALPHA);
    for (int i = 0; i < nsyms; i++) {
        uint32_t target = rc_dec_cum(&rc, (uint32_t)m.total);
        int s = model_find(&m, (int)target);
        rc_dec_update(&rc, (uint32_t)model_cum(&m, s), (uint32_t)model_freq_of(&m, s));
        out[i] = (uint16_t)s;
        model_update(&m, s);
    }
}


/* ===== Arithmetic Coding O1 with Pre-Scanned global frequency init (S_BWT_O1_PS) =====
 *
 * Same as arith_enc_o1 but initialises all 258 context models from a single
 * global frequency table (258-byte header) instead of a uniform distribution.
 *
 * Effect:
 *   1. Dead-symbol removal: symbols that never appear in the stream get freq=0
 *      in every sub-model, eliminating permanently wasted probability mass.
 *   2. Warm start: first symbols per context already use near-global priors
 *      rather than a uniform 1/258 guess.
 *
 * Overhead: ZRLE_ALPHA (258) bytes — identical to O0_PS.
 * Format: [258 bytes global_freq_table | range-coded O1 stream]
 */
static int arith_enc_o1_ps(const uint16_t *syms, int nsyms, uint8_t *out) {
    /* Phase 1: build global frequency table */
    int count[ZRLE_ALPHA];
    memset(count, 0, sizeof(count));
    for (int i = 0; i < nsyms; i++) count[syms[i]]++;

    int maxcount = 0;
    for (int i = 0; i < ZRLE_ALPHA; i++)
        if (count[i] > maxcount) maxcount = count[i];

    uint8_t *header = out;
    if (maxcount == 0) {
        /* empty stream — write zero header */
        memset(header, 0, ZRLE_ALPHA);
    } else {
        for (int i = 0; i < ZRLE_ALPHA; i++) {
            header[i] = (count[i] == 0) ? 0
                        : (uint8_t)((int64_t)count[i] * 254 / maxcount + 1);
        }
    }

    /* Phase 2: init all 258 O1 models from the global table */
    Model *models = (Model*)malloc(ZRLE_ALPHA * sizeof(Model));
    for (int ctx = 0; ctx < ZRLE_ALPHA; ctx++) {
        Model *m = &models[ctx];
        memset(m, 0, sizeof(Model));
        for (int i = 0; i < ZRLE_ALPHA; i++) {
            m->freq[i] = header[i];
            m->total  += header[i];
        }
        /* fall back to uniform if all frequencies are zero (shouldn't happen) */
        if (m->total == 0) {
            for (int i = 0; i < ZRLE_ALPHA; i++) m->freq[i] = 1;
            m->total = ZRLE_ALPHA;
        }
        model_build_tree(m);
    }

    /* Phase 3: encode with O1 adaptive range coding */
    RCEnc rc; rc_enc_init(&rc, out + ZRLE_ALPHA);
    int prev = 0;
    for (int i = 0; i < nsyms; i++) {
        int s = syms[i];
        Model *m = &models[prev];
        rc_enc(&rc, (uint32_t)model_cum(m, s), (uint32_t)model_freq_of(m, s), (uint32_t)m->total);
        model_update(m, s);
        prev = s;
    }
    free(models);
    int arith_bytes = rc_enc_finish(&rc);
    return ZRLE_ALPHA + arith_bytes;
}

static void arith_dec_o1_ps(const uint8_t *in, int nsyms, uint16_t *out) {
    /* Read global freq header and warm-start all 258 O1 models */
    Model *models = (Model*)malloc(ZRLE_ALPHA * sizeof(Model));
    for (int ctx = 0; ctx < ZRLE_ALPHA; ctx++) {
        Model *m = &models[ctx];
        memset(m, 0, sizeof(Model));
        for (int i = 0; i < ZRLE_ALPHA; i++) {
            m->freq[i] = in[i];
            m->total  += in[i];
        }
        if (m->total == 0) {
            for (int i = 0; i < ZRLE_ALPHA; i++) m->freq[i] = 1;
            m->total = ZRLE_ALPHA;
        }
        model_build_tree(m);
    }

    RCDec rc; rc_dec_init(&rc, in + ZRLE_ALPHA);
    int prev = 0;
    for (int i = 0; i < nsyms; i++) {
        Model *m = &models[prev];
        uint32_t target = rc_dec_cum(&rc, (uint32_t)m->total);
        int s = model_find(m, (int)target);
        rc_dec_update(&rc, (uint32_t)model_cum(m, s), (uint32_t)model_freq_of(m, s));
        out[i] = (uint16_t)s;
        model_update(m, s);
        prev = s;
    }
    free(models);
}

/* ===== rANS O0 Entropy Coder (S_BWT_RANS) =====
 *
 * Streaming rANS with M=4096 normalization target.
 * Same compression ratio as O0_PS range coder, but O(1) table-based decode.
 *
 * Format: [258 bytes freq_table | 4 bytes initial_state (LE) | byte stream]
 *   freq_table[i]: 0 = symbol absent, 1..255 = proportional weight
 *   Encoder emits bytes in reverse order, then reverses in-place.
 *   Decoder reads bytes forward.
 */
#define RANS_M 4096  /* normalization target; must be power of 2 and >= ZRLE_ALPHA */

typedef struct { uint16_t sym; uint16_t freq; uint16_t bias; } RansSlot;

static int rans_enc_o0(const uint16_t *syms, int nsyms, uint8_t *out) {
    /* Phase 1: count raw frequencies */
    int count[ZRLE_ALPHA];
    memset(count, 0, sizeof(count));
    for (int i = 0; i < nsyms; i++) count[syms[i]]++;

    int maxcount = 0;
    for (int i = 0; i < ZRLE_ALPHA; i++) if (count[i] > maxcount) maxcount = count[i];

    /* Phase 2: write 258-byte header (same scale as O0_PS: 1..255 for seen, 0 unseen) */
    uint8_t *header = out;
    for (int i = 0; i < ZRLE_ALPHA; i++) {
        if (count[i] == 0) { header[i] = 0; }
        else { header[i] = (uint8_t)((int64_t)count[i] * 254 / maxcount + 1); }
    }

    /* Phase 3: normalize to sum exactly RANS_M using largest-remainder method */
    int raw[ZRLE_ALPHA], nseen = 0;
    int raw_total = 0;
    for (int i = 0; i < ZRLE_ALPHA; i++) {
        if (header[i]) { raw[i] = header[i]; raw_total += header[i]; nseen++; }
        else raw[i] = 0;
    }
    /* Scale: freq_norm[i] = max(1, round(raw[i] * RANS_M / raw_total)) */
    int freq[ZRLE_ALPHA];
    int fsum = 0;
    for (int i = 0; i < ZRLE_ALPHA; i++) {
        if (raw[i] == 0) { freq[i] = 0; continue; }
        freq[i] = (int)((int64_t)raw[i] * RANS_M / raw_total);
        if (freq[i] == 0) freq[i] = 1;
        fsum += freq[i];
    }
    /* Adjust rounding error: add/remove from most frequent seen symbol */
    int diff = RANS_M - fsum;
    if (diff != 0) {
        int best = -1;
        for (int i = 0; i < ZRLE_ALPHA; i++)
            if (freq[i] > 0 && (best < 0 || freq[i] > freq[best])) best = i;
        if (best >= 0) freq[best] += diff;
    }

    /* Phase 4: compute cumulative */
    int cum[ZRLE_ALPHA + 1];
    cum[0] = 0;
    for (int i = 0; i < ZRLE_ALPHA; i++) cum[i+1] = cum[i] + freq[i];

    /* Phase 5: encode in reverse into a temporary buffer */
    int cap = nsyms * 2 + 16;
    uint8_t *tmp = (uint8_t*)malloc(cap);
    int tlen = 0;

    uint32_t x = RANS_M; /* initial state */
    for (int i = nsyms - 1; i >= 0; i--) {
        int s = syms[i];
        int fs = freq[s];
        /* renormalize: flush bytes while x >= fs * 256 */
        while (x >= (uint32_t)fs * 256) {
            if (tlen >= cap) { cap *= 2; tmp = (uint8_t*)realloc(tmp, cap); }
            tmp[tlen++] = (uint8_t)(x & 0xFF);
            x >>= 8;
        }
        /* rANS step: x = (x / fs) * RANS_M + cum[s] + (x % fs) */
        x = (x / fs) * RANS_M + cum[s] + (x % fs);
    }

    /* Phase 6: write final state (LE u32) then reversed stream */
    uint8_t *p = out + ZRLE_ALPHA;
    p[0] = (uint8_t)(x & 0xFF); p[1] = (uint8_t)((x>>8)&0xFF);
    p[2] = (uint8_t)((x>>16)&0xFF); p[3] = (uint8_t)((x>>24)&0xFF);
    p += 4;
    /* bytes in tmp are in reverse order; write them reversed = correct forward order */
    for (int i = tlen - 1; i >= 0; i--) *p++ = tmp[i];
    free(tmp);

    return ZRLE_ALPHA + 4 + tlen;
}

static void rans_dec_o0(const uint8_t *in, int nsyms, uint16_t *out) {
    /* Build M-normalized freq from header */
    int raw[ZRLE_ALPHA], raw_total = 0;
    for (int i = 0; i < ZRLE_ALPHA; i++) { raw[i] = in[i]; raw_total += in[i]; }

    int freq[ZRLE_ALPHA], fsum = 0;
    for (int i = 0; i < ZRLE_ALPHA; i++) {
        if (raw[i] == 0) { freq[i] = 0; continue; }
        freq[i] = (int)((int64_t)raw[i] * RANS_M / raw_total);
        if (freq[i] == 0) freq[i] = 1;
        fsum += freq[i];
    }
    int diff = RANS_M - fsum;
    if (diff != 0) {
        int best = -1;
        for (int i = 0; i < ZRLE_ALPHA; i++)
            if (freq[i] > 0 && (best < 0 || freq[i] > freq[best])) best = i;
        if (best >= 0) freq[best] += diff;
    }

    int cum[ZRLE_ALPHA + 1];
    cum[0] = 0;
    for (int i = 0; i < ZRLE_ALPHA; i++) cum[i+1] = cum[i] + freq[i];

    /* Build O(1) decode table: dtab[slot] = {sym, freq, bias} */
    RansSlot *dtab = (RansSlot*)malloc(RANS_M * sizeof(RansSlot));
    for (int s = 0; s < ZRLE_ALPHA; s++) {
        for (int j = cum[s]; j < cum[s+1]; j++) {
            dtab[j].sym  = (uint16_t)s;
            dtab[j].freq = (uint16_t)freq[s];
            dtab[j].bias = (uint16_t)cum[s];
        }
    }

    /* Read initial state */
    const uint8_t *p = in + ZRLE_ALPHA;
    uint32_t x = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
    p += 4;

    /* Decode */
    for (int i = 0; i < nsyms; i++) {
        uint32_t slot = x % RANS_M;
        RansSlot sl = dtab[slot];
        out[i] = sl.sym;
        x = (uint32_t)sl.freq * (x / RANS_M) + slot - sl.bias;
        /* renorm */
        while (x < (uint32_t)RANS_M) x = (x << 8) | *p++;
    }

    free(dtab);
}

/* ===== 2-Context Range Coder (S_BWT_CTX2) =====
 *
 * Two independent O0 models conditioned on whether the previous symbol
 * was a run code (RUNA/RUNB, sym < 2) or a literal (sym >= 2).
 *
 * Format: [258 bytes freq_ctx0 | 258 bytes freq_ctx1 | range-coded stream]
 * Context: ctx = (prev_sym < 2) ? 0 : 1; first symbol uses ctx=1.
 */
static int arith_enc_ctx2(const uint16_t *syms, int nsyms, uint8_t *out) {
    /* Phase 1: count per-context frequencies */
    int count[2][ZRLE_ALPHA];
    memset(count, 0, sizeof(count));
    int prev = 2; /* first sym: ctx=1 (prev>=2) */
    for (int i = 0; i < nsyms; i++) {
        int ctx = (prev < 2) ? 0 : 1;
        count[ctx][syms[i]]++;
        prev = syms[i];
    }

    /* Phase 2: write two 258-byte headers */
    for (int c = 0; c < 2; c++) {
        int maxcount = 0;
        for (int i = 0; i < ZRLE_ALPHA; i++) if (count[c][i] > maxcount) maxcount = count[c][i];
        uint8_t *hdr = out + c * ZRLE_ALPHA;
        for (int i = 0; i < ZRLE_ALPHA; i++) {
            if (count[c][i] == 0) hdr[i] = 0;
            else hdr[i] = (uint8_t)((int64_t)count[c][i] * 254 / maxcount + 1);
        }
    }

    /* Phase 3: init two models */
    Model m[2];
    for (int c = 0; c < 2; c++) {
        memset(&m[c], 0, sizeof(Model));
        for (int i = 0; i < ZRLE_ALPHA; i++) {
            m[c].freq[i] = out[c * ZRLE_ALPHA + i];
            m[c].total  += out[c * ZRLE_ALPHA + i];
        }
        model_build_tree(&m[c]);
    }

    /* Phase 4: range-encode with 2 contexts */
    RCEnc rc; rc_enc_init(&rc, out + 2 * ZRLE_ALPHA);
    prev = 2;
    for (int i = 0; i < nsyms; i++) {
        int ctx = (prev < 2) ? 0 : 1;
        int s = syms[i];
        rc_enc(&rc, (uint32_t)model_cum(&m[ctx], s),
                    (uint32_t)model_freq_of(&m[ctx], s),
                    (uint32_t)m[ctx].total);
        model_update(&m[ctx], s);
        prev = s;
    }
    int arith_bytes = rc_enc_finish(&rc);
    return 2 * ZRLE_ALPHA + arith_bytes;
}

static void arith_dec_ctx2(const uint8_t *in, int nsyms, uint16_t *out) {
    Model m[2];
    for (int c = 0; c < 2; c++) {
        memset(&m[c], 0, sizeof(Model));
        for (int i = 0; i < ZRLE_ALPHA; i++) {
            m[c].freq[i] = in[c * ZRLE_ALPHA + i];
            m[c].total  += in[c * ZRLE_ALPHA + i];
        }
        model_build_tree(&m[c]);
    }
    RCDec rc; rc_dec_init(&rc, in + 2 * ZRLE_ALPHA);
    int prev = 2;
    for (int i = 0; i < nsyms; i++) {
        int ctx = (prev < 2) ? 0 : 1;
        uint32_t target = rc_dec_cum(&rc, (uint32_t)m[ctx].total);
        int s = model_find(&m[ctx], (int)target);
        rc_dec_update(&rc, (uint32_t)model_cum(&m[ctx], s),
                           (uint32_t)model_freq_of(&m[ctx], s));
        out[i] = (uint16_t)s;
        model_update(&m[ctx], s);
        prev = s;
    }
}

/* ===== LZ77 Hash-Chain Encoder/Decoder ===== */
#define LZ_HASH_BITS  16
#define LZ_HASH_SIZE  (1 << LZ_HASH_BITS)
#define LZ_HASH_MASK  (LZ_HASH_SIZE - 1)
#define LZ_CHAIN_LEN  64
#define LZ_MIN_MATCH  4
#define LZ_MAX_MATCH  258
#define LZ_MAX_DIST   65536

static inline uint32_t lz_hash4(const uint8_t *p) {
    uint32_t v = p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (v * 2654435761u) >> (32 - LZ_HASH_BITS);
}

/*
 * LZ77 compress → packed output for subsequent BWT compression.
 * Format: [ntok:4][nlit:4][nmatch:4][flags_packed][lits][dists_le][lens]
 * Returns packed size, or 0 if LZ77 doesn't reduce size.
 */
static int lz77_encode(const uint8_t *src, int n, uint8_t *out, int out_cap) {
    if (n < 64 || n > 64*1024*1024) return 0;  /* skip very small or very large blocks */

    int *head = (int*)malloc(LZ_HASH_SIZE * sizeof(int));
    int *prev = (int*)malloc(LZ_MAX_DIST * sizeof(int));  /* circular buffer */
    if (!head || !prev) { free(head); free(prev); return 0; }
    memset(head, -1, LZ_HASH_SIZE * sizeof(int));
    memset(prev, -1, LZ_MAX_DIST * sizeof(int));

    int max_matches = n / LZ_MIN_MATCH + 1024;
    uint8_t *flags_buf = (uint8_t*)calloc((n + 7) / 8, 1);
    uint8_t *lits  = (uint8_t*)malloc(n);
    uint8_t *dists = (uint8_t*)malloc(max_matches * 2);
    uint8_t *lens  = (uint8_t*)malloc(max_matches);
    if (!flags_buf || !lits || !dists || !lens) {
        free(head); free(prev); free(flags_buf); free(lits); free(dists); free(lens);
        return 0;
    }

    int ntok = 0, nlit = 0, nmatch = 0;
    int i = 0;

    while (i < n) {
        int best_len = 0, best_dist = 0;

        if (i + LZ_MIN_MATCH <= n) {
            uint32_t h = lz_hash4(src + i);
            int pos = head[h];
            int tries = 0;

            while (pos >= 0 && (i - pos) <= LZ_MAX_DIST && tries < LZ_CHAIN_LEN) {
                int maxlen = n - i;
                if (maxlen > LZ_MAX_MATCH) maxlen = LZ_MAX_MATCH;
                int len = 0;
                while (len < maxlen && src[pos + len] == src[i + len]) len++;
                if (len >= LZ_MIN_MATCH && len > best_len) {
                    best_len = len;
                    best_dist = i - pos;
                    if (len >= maxlen) break;
                }
                pos = prev[pos & (LZ_MAX_DIST - 1)];
                tries++;
            }

            /* Lazy matching: defer if next position finds a better match */
            if (best_len >= LZ_MIN_MATCH && best_len < 32 && i + 1 + LZ_MIN_MATCH <= n) {
                uint32_t h2 = lz_hash4(src + i + 1);
                int pos2 = head[h2];
                int tries2 = 0, best2 = 0;
                while (pos2 >= 0 && (i + 1 - pos2) <= LZ_MAX_DIST && tries2 < LZ_CHAIN_LEN / 2) {
                    int ml = n - (i + 1);
                    if (ml > LZ_MAX_MATCH) ml = LZ_MAX_MATCH;
                    int l2 = 0;
                    while (l2 < ml && src[pos2 + l2] == src[i + 1 + l2]) l2++;
                    if (l2 > best2) best2 = l2;
                    pos2 = prev[pos2 & (LZ_MAX_DIST - 1)];
                    tries2++;
                }
                if (best2 > best_len + 1) best_len = 0; /* defer — emit literal */
            }

            /* Update hash chain (circular buffer) */
            prev[i & (LZ_MAX_DIST - 1)] = head[h];
            head[h] = i;
        }

        if (best_len >= LZ_MIN_MATCH) {
            flags_buf[ntok / 8] |= (uint8_t)(1 << (ntok % 8));
            dists[nmatch * 2]     = (uint8_t)((best_dist - 1) & 0xFF);
            dists[nmatch * 2 + 1] = (uint8_t)(((best_dist - 1) >> 8) & 0xFF);
            lens[nmatch] = (uint8_t)(best_len - LZ_MIN_MATCH);
            nmatch++;

            for (int j = 1; j < best_len && i + j + LZ_MIN_MATCH <= n; j++) {
                uint32_t hj = lz_hash4(src + i + j);
                prev[(i + j) & (LZ_MAX_DIST - 1)] = head[hj];
                head[hj] = i + j;
            }
            i += best_len;
        } else {
            lits[nlit++] = src[i];
            i++;
        }
        ntok++;
    }

    int flags_bytes = (ntok + 7) / 8;
    int total = 12 + flags_bytes + nlit + nmatch * 2 + nmatch;

    /* Only use LZ77 if it reduces size by at least 5% */
    if (total >= n * 95 / 100 || total >= out_cap) {
        free(head); free(prev); free(flags_buf); free(lits); free(dists); free(lens);
        return 0;
    }

    int p = 0;
    out[p++] = ntok & 0xFF; out[p++] = (ntok >> 8) & 0xFF;
    out[p++] = (ntok >> 16) & 0xFF; out[p++] = (ntok >> 24) & 0xFF;
    out[p++] = nlit & 0xFF; out[p++] = (nlit >> 8) & 0xFF;
    out[p++] = (nlit >> 16) & 0xFF; out[p++] = (nlit >> 24) & 0xFF;
    out[p++] = nmatch & 0xFF; out[p++] = (nmatch >> 8) & 0xFF;
    out[p++] = (nmatch >> 16) & 0xFF; out[p++] = (nmatch >> 24) & 0xFF;
    memcpy(out + p, flags_buf, flags_bytes); p += flags_bytes;
    memcpy(out + p, lits, nlit); p += nlit;
    memcpy(out + p, dists, nmatch * 2); p += nmatch * 2;
    memcpy(out + p, lens, nmatch); p += nmatch;

    free(head); free(prev); free(flags_buf); free(lits); free(dists); free(lens);
    return p;
}

/*
 * LZ77 decode: packed data → original output
 * Returns bytes written.
 */
static int lz77_decode(const uint8_t *in, int comp_size, uint8_t *out, int out_cap) {
    if (comp_size < 12) return 0;

    int p = 0;
    int ntok    = in[p] | (in[p+1]<<8) | (in[p+2]<<16) | (in[p+3]<<24); p += 4;
    int nlit    = in[p] | (in[p+1]<<8) | (in[p+2]<<16) | (in[p+3]<<24); p += 4;
    int nmatch  = in[p] | (in[p+1]<<8) | (in[p+2]<<16) | (in[p+3]<<24); p += 4;

    int flags_bytes = (ntok + 7) / 8;
    const uint8_t *flags = in + p;   p += flags_bytes;
    const uint8_t *litd  = in + p;   p += nlit;
    const uint8_t *distd = in + p;   p += nmatch * 2;
    const uint8_t *lend  = in + p;

    int opos = 0, li = 0, mi = 0;

    for (int t = 0; t < ntok && opos < out_cap; t++) {
        int is_match = (flags[t / 8] >> (t % 8)) & 1;
        if (is_match) {
            int dist = (distd[mi * 2] | (distd[mi * 2 + 1] << 8)) + 1;
            int len  = lend[mi] + LZ_MIN_MATCH;
            mi++;
            int src_pos = opos - dist;
            for (int j = 0; j < len && opos < out_cap; j++) {
                out[opos] = out[src_pos + j];
                opos++;
            }
        } else {
            out[opos++] = litd[li++];
        }
    }
    return opos;
}

/* ===== Delta encode/decode ===== */
static void delta_encode(const uint8_t *in, int n, uint8_t *out) {
    out[0] = in[0];
    for (int i = 1; i < n; i++) out[i] = in[i] - in[i-1];
}

static void delta_decode(const uint8_t *in, int n, uint8_t *out) {
    out[0] = in[0];
    for (int i = 1; i < n; i++) out[i] = in[i] + out[i-1];
}

/* ===== BCJ E8/E9 Transform (x86 CALL/JMP relative->absolute) ===== */
static int bcj_is_executable(const uint8_t *data, int n) {
    if (n >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F')
        return 1;
    if (n >= 2 && data[0] == 'M' && data[1] == 'Z')
        return 1;
    if (n >= 1024) {
        int e8_count = 0;
        int check_len = n < 65536 ? n : 65536;
        for (int i = 0; i < check_len - 4; i++) {
            if (data[i] == 0xE8 || data[i] == 0xE9) e8_count++;
        }
        if (e8_count * 50 > check_len) return 1;
    }
    return 0;
}

static void bcj_e8e9_encode(const uint8_t *in, int n, uint8_t *out) {
    memcpy(out, in, n);
    for (int i = 0; i < n - 4; i++) {
        if (out[i] == 0xE8 || out[i] == 0xE9) {
            int32_t rel = (int32_t)((uint32_t)out[i+1] |
                          ((uint32_t)out[i+2] << 8) |
                          ((uint32_t)out[i+3] << 16) |
                          ((uint32_t)out[i+4] << 24));
            int32_t abs_addr = rel + i;
            out[i+1] = (uint8_t)(abs_addr);
            out[i+2] = (uint8_t)(abs_addr >> 8);
            out[i+3] = (uint8_t)(abs_addr >> 16);
            out[i+4] = (uint8_t)(abs_addr >> 24);
            i += 4;
        }
    }
}

static void bcj_e8e9_decode(const uint8_t *in, int n, uint8_t *out) {
    memcpy(out, in, n);
    for (int i = 0; i < n - 4; i++) {
        if (out[i] == 0xE8 || out[i] == 0xE9) {
            int32_t abs_addr = (int32_t)((uint32_t)out[i+1] |
                               ((uint32_t)out[i+2] << 8) |
                               ((uint32_t)out[i+3] << 16) |
                               ((uint32_t)out[i+4] << 24));
            int32_t rel = abs_addr - i;
            out[i+1] = (uint8_t)(rel);
            out[i+2] = (uint8_t)(rel >> 8);
            out[i+3] = (uint8_t)(rel >> 16);
            out[i+4] = (uint8_t)(rel >> 24);
            i += 4;
        }
    }
}

/* ===== IEEE 754 Float32 XOR-Delta Transform ===== */
/*
 * For data containing float32 values with similar magnitudes (scientific data,
 * sensor arrays, FITS catalogs), XORing consecutive 4-byte values cancels out
 * shared exponent bits, clustering the differences near zero and making the
 * BWT suffix sort far more effective.
 *
 * Detection: if XOR-delta reduces the entropy of the high byte (exponent+sign
 * of LE float32) by ≥15%, the data is likely float-encoded and benefits from
 * this filter.
 */
static int float_xor_detect(const uint8_t *data, int n) {
    if (n < 4096) return 0;
    int tn = n < 65536 ? n : 65536;
    tn &= ~3;  /* align to 4-byte boundary */
    if (tn < 1024) return 0;

    /* Full-byte entropy of raw data */
    int freq0[256] = {0};
    for (int i = 0; i < tn; i++) freq0[data[i]]++;
    double ent0 = 0.0;
    for (int i = 0; i < 256; i++) {
        if (freq0[i] > 0) { double p = (double)freq0[i] / tn; ent0 -= p * log2(p); }
    }

    /* Full-byte entropy of XOR-delta output */
    int freq1[256] = {0};
    uint32_t prev = 0;
    for (int i = 0; i < tn; i += 4) {
        uint32_t curr = (uint32_t)data[i]          |
                        ((uint32_t)data[i+1] <<  8) |
                        ((uint32_t)data[i+2] << 16) |
                        ((uint32_t)data[i+3] << 24);
        uint32_t d = curr ^ prev;
        freq1[ d        & 0xFF]++;
        freq1[(d >>  8) & 0xFF]++;
        freq1[(d >> 16) & 0xFF]++;
        freq1[(d >> 24) & 0xFF]++;
        prev = curr;
    }
    double ent1 = 0.0;
    for (int i = 0; i < 256; i++) {
        if (freq1[i] > 0) { double p = (double)freq1[i] / tn; ent1 -= p * log2(p); }
    }

    /* Trigger if XOR-delta reduces full entropy by ≥10%.
       This catches both:
       - varying-exponent floats (ent0 normal, ent1 lower)
       - same-exponent floats (ent0 already low, ent1 even lower due to 0x00 high bytes) */
    return (ent0 > 4.0 && ent1 < ent0 * 0.90);
}

static void float_xor_encode(const uint8_t *in, int n, uint8_t *out) {
    uint32_t prev = 0;
    int i;
    for (i = 0; i + 3 < n; i += 4) {
        uint32_t curr = (uint32_t)in[i]         |
                        ((uint32_t)in[i+1] << 8)  |
                        ((uint32_t)in[i+2] << 16) |
                        ((uint32_t)in[i+3] << 24);
        uint32_t d = curr ^ prev;
        out[i]   = d & 0xFF;
        out[i+1] = (d >> 8)  & 0xFF;
        out[i+2] = (d >> 16) & 0xFF;
        out[i+3] = (d >> 24) & 0xFF;
        prev = curr;
    }
    for (; i < n; i++) out[i] = in[i];  /* tail bytes (n % 4) */
}

static void float_xor_decode(const uint8_t *in, int n, uint8_t *out) {
    uint32_t prev = 0;
    int i;
    for (i = 0; i + 3 < n; i += 4) {
        uint32_t d = (uint32_t)in[i]         |
                     ((uint32_t)in[i+1] << 8)  |
                     ((uint32_t)in[i+2] << 16) |
                     ((uint32_t)in[i+3] << 24);
        uint32_t curr = d ^ prev;
        out[i]   = curr & 0xFF;
        out[i+1] = (curr >> 8)  & 0xFF;
        out[i+2] = (curr >> 16) & 0xFF;
        out[i+3] = (curr >> 24) & 0xFF;
        prev = curr;
    }
    for (; i < n; i++) out[i] = in[i];
}

/* ===== Audio Pipeline (16-bit PCM) ===== */
static int audio_detect(const uint8_t *data, int n) {
    if (n < 4096) return 0;
    int tn = n < 65536 ? n : 65536;
    int freq0[256] = {0};
    for (int i = 0; i < tn; i++) freq0[data[i]]++;
    double ent0 = 0;
    for (int i = 0; i < 256; i++) {
        if (freq0[i] > 0) { double p = (double)freq0[i] / tn; ent0 -= p * log2(p); }
    }
    int nf = tn / 4;
    if (nf < 256) return 0;
    uint8_t *tb = (uint8_t*)malloc(nf * 4);
    int16_t pl = 0, pr = 0;
    for (int i = 0; i < nf; i++) {
        int16_t l = (int16_t)((uint16_t)data[i*4] | ((uint16_t)data[i*4+1] << 8));
        int16_t r = (int16_t)((uint16_t)data[i*4+2] | ((uint16_t)data[i*4+3] << 8));
        int16_t s = l - r, m = l - (s >> 1);
        int16_t dm = m - pl, ds = s - pr; pl = m; pr = s;
        tb[i]        = (uint8_t)(dm & 0xFF);
        tb[nf+i]     = (uint8_t)((dm >> 8) & 0xFF);
        tb[2*nf+i]   = (uint8_t)(ds & 0xFF);
        tb[3*nf+i]   = (uint8_t)((ds >> 8) & 0xFF);
    }
    int freq1[256] = {0};
    for (int i = 0; i < nf*4; i++) freq1[tb[i]]++;
    double ent1 = 0;
    for (int i = 0; i < 256; i++) {
        if (freq1[i] > 0) { double p = (double)freq1[i] / (nf*4); ent1 -= p * log2(p); }
    }
    free(tb);
    return (ent1 < ent0 * 0.70);
}

/* ===== LPC Prediction (Levinson-Durbin) ===== */
#define LPC_MAX_ORDER 12
#define LPC_BLOCK_SIZE 4096
#define LPC_QLEVEL 10

static void lpc_autocorrelation(const int16_t *data, int n, int order, double *r) {
    for (int i = 0; i <= order; i++) {
        double sum = 0;
        for (int j = 0; j < n - i; j++) sum += (double)data[j] * data[j + i];
        r[i] = sum;
    }
}

static int lpc_levinson_durbin(const double *r, int max_order, double *a, int *best_order) {
    double a_tmp[LPC_MAX_ORDER + 1];
    double e = r[0];
    if (e <= 0) { *best_order = 0; return 0; }
    
    double best_e = e;
    *best_order = 0;
    
    for (int i = 1; i <= max_order; i++) {
        double sum = 0;
        for (int j = 1; j < i; j++) sum += a[j] * r[i - j];
        double k = (r[i] - sum) / e;
        if (k > 1.0 || k < -1.0) break;
        
        a[i] = k;
        for (int j = 1; j < i; j++) a_tmp[j] = a[j] - k * a[i - j];
        for (int j = 1; j < i; j++) a[j] = a_tmp[j];
        
        e *= (1.0 - k * k);
        if (e < best_e * 0.99) {  /* Must improve by at least 1% */
            best_e = e;
            *best_order = i;
        }
    }
    return 1;
}

static int audio_encode(const uint8_t *in, int n, uint8_t *out) {
    int nf = n / 4, tail = n - nf * 4;
    
    /* Read samples */
    int16_t *L = (int16_t*)malloc(nf * sizeof(int16_t));
    int16_t *R = (int16_t*)malloc(nf * sizeof(int16_t));
    for (int i = 0; i < nf; i++) {
        L[i] = (int16_t)((uint16_t)in[i*4] | ((uint16_t)in[i*4+1] << 8));
        R[i] = (int16_t)((uint16_t)in[i*4+2] | ((uint16_t)in[i*4+3] << 8));
    }
    
    /* Mid/Side decorrelation */
    int16_t *M = (int16_t*)malloc(nf * sizeof(int16_t));
    int16_t *S = (int16_t*)malloc(nf * sizeof(int16_t));
    for (int i = 0; i < nf; i++) {
        S[i] = L[i] - R[i];
        M[i] = L[i] - (S[i] >> 1);
    }
    free(L); free(R);
    
    /* LPC prediction per block */
    int nblocks = (nf + LPC_BLOCK_SIZE - 1) / LPC_BLOCK_SIZE;
    int16_t *resM = (int16_t*)malloc(nf * sizeof(int16_t));
    int16_t *resS = (int16_t*)malloc(nf * sizeof(int16_t));
    
    /* Header format: [version:1=4][nframes:4][nblocks:4] */
    /* Per block: [orderM:1][orderS:1][coeffsM:orderM*2][coeffsS:orderS*2] */
    int op = 0;
    out[op++] = 4;  /* version 4 = LPC */
    out[op++] = (nf >> 24) & 0xFF; out[op++] = (nf >> 16) & 0xFF;
    out[op++] = (nf >> 8) & 0xFF;  out[op++] = nf & 0xFF;
    out[op++] = (nblocks >> 24) & 0xFF; out[op++] = (nblocks >> 16) & 0xFF;
    out[op++] = (nblocks >> 8) & 0xFF;  out[op++] = nblocks & 0xFF;
    
    for (int b = 0; b < nblocks; b++) {
        int start = b * LPC_BLOCK_SIZE;
        int blen = (start + LPC_BLOCK_SIZE <= nf) ? LPC_BLOCK_SIZE : (nf - start);
        
        /* LPC for Mid channel */
        double r[LPC_MAX_ORDER + 1], a[LPC_MAX_ORDER + 1] = {0};
        int orderM = 0;
        lpc_autocorrelation(M + start, blen, LPC_MAX_ORDER, r);
        lpc_levinson_durbin(r, (blen > LPC_MAX_ORDER) ? LPC_MAX_ORDER : blen - 1, a, &orderM);
        
        /* Quantize coefficients */
        int16_t qcoeffsM[LPC_MAX_ORDER];
        for (int j = 1; j <= orderM; j++)
            qcoeffsM[j-1] = (int16_t)(a[j] * (1 << LPC_QLEVEL) + (a[j] > 0 ? 0.5 : -0.5));
        
        /* Compute residuals for Mid */
        for (int i = start; i < start + blen; i++) {
            int32_t pred = 0;
            for (int j = 1; j <= orderM; j++) {
                if (i - j >= start) pred += (int32_t)qcoeffsM[j-1] * M[i - j];
            }
            resM[i] = M[i] - (int16_t)(pred >> LPC_QLEVEL);
        }
        
        /* LPC for Side channel */
        memset(a, 0, sizeof(a));
        int orderS = 0;
        lpc_autocorrelation(S + start, blen, LPC_MAX_ORDER, r);
        lpc_levinson_durbin(r, (blen > LPC_MAX_ORDER) ? LPC_MAX_ORDER : blen - 1, a, &orderS);
        
        int16_t qcoeffsS[LPC_MAX_ORDER];
        for (int j = 1; j <= orderS; j++)
            qcoeffsS[j-1] = (int16_t)(a[j] * (1 << LPC_QLEVEL) + (a[j] > 0 ? 0.5 : -0.5));
        
        for (int i = start; i < start + blen; i++) {
            int32_t pred = 0;
            for (int j = 1; j <= orderS; j++) {
                if (i - j >= start) pred += (int32_t)qcoeffsS[j-1] * S[i - j];
            }
            resS[i] = S[i] - (int16_t)(pred >> LPC_QLEVEL);
        }
        
        /* Write block header: orderM, orderS, coefficients */
        out[op++] = (uint8_t)orderM;
        out[op++] = (uint8_t)orderS;
        for (int j = 0; j < orderM; j++) {
            out[op++] = (uint8_t)(qcoeffsM[j] & 0xFF);
            out[op++] = (uint8_t)((qcoeffsM[j] >> 8) & 0xFF);
        }
        for (int j = 0; j < orderS; j++) {
            out[op++] = (uint8_t)(qcoeffsS[j] & 0xFF);
            out[op++] = (uint8_t)((qcoeffsS[j] >> 8) & 0xFF);
        }
    }
    
    /* Store header end offset (so decompressor/direct RC knows where streams start) */
    int hdr_end = op;
    /* Byte-split residuals: resM_lo, resM_hi, resS_lo, resS_hi */
    for (int i = 0; i < nf; i++) out[op++] = (uint8_t)(resM[i] & 0xFF);
    for (int i = 0; i < nf; i++) out[op++] = (uint8_t)((resM[i] >> 8) & 0xFF);
    for (int i = 0; i < nf; i++) out[op++] = (uint8_t)(resS[i] & 0xFF);
    for (int i = 0; i < nf; i++) out[op++] = (uint8_t)((resS[i] >> 8) & 0xFF);
    
    free(M); free(S); free(resM); free(resS);
    if (tail > 0) { memcpy(out + op, in + nf * 4, tail); op += tail; }
    
    fprintf(stderr, "    [LPC] %d blocks, hdr=%d bytes\n", nblocks, hdr_end);
    return op;
}

static void audio_decode(const uint8_t *in, int orig_size, uint8_t *out) {
    int version = in[0];
    int nf = (in[1]<<24) | (in[2]<<16) | (in[3]<<8) | in[4];
    int p = 5;
    
    if (version == 4) {
        /* LPC decode */
        int nblocks = (in[p]<<24) | (in[p+1]<<16) | (in[p+2]<<8) | in[p+3];
        p += 4;
        
        /* Read block headers */
        int *orderM_arr = (int*)malloc(nblocks * sizeof(int));
        int *orderS_arr = (int*)malloc(nblocks * sizeof(int));
        int16_t (*coeffM)[LPC_MAX_ORDER] = (int16_t(*)[LPC_MAX_ORDER])malloc(nblocks * LPC_MAX_ORDER * sizeof(int16_t));
        int16_t (*coeffS)[LPC_MAX_ORDER] = (int16_t(*)[LPC_MAX_ORDER])malloc(nblocks * LPC_MAX_ORDER * sizeof(int16_t));
        
        for (int b = 0; b < nblocks; b++) {
            orderM_arr[b] = in[p++];
            orderS_arr[b] = in[p++];
            for (int j = 0; j < orderM_arr[b]; j++) {
                coeffM[b][j] = (int16_t)((uint16_t)in[p] | ((uint16_t)in[p+1] << 8));
                p += 2;
            }
            for (int j = 0; j < orderS_arr[b]; j++) {
                coeffS[b][j] = (int16_t)((uint16_t)in[p] | ((uint16_t)in[p+1] << 8));
                p += 2;
            }
        }
        
        /* Read byte-split residuals */
        int16_t *resM = (int16_t*)malloc(nf * sizeof(int16_t));
        int16_t *resS = (int16_t*)malloc(nf * sizeof(int16_t));
        for (int i = 0; i < nf; i++) resM[i] = in[p + i];
        for (int i = 0; i < nf; i++) resM[i] |= (int16_t)((uint16_t)in[p + nf + i] << 8);
        for (int i = 0; i < nf; i++) resS[i] = in[p + 2*nf + i];
        for (int i = 0; i < nf; i++) resS[i] |= (int16_t)((uint16_t)in[p + 3*nf + i] << 8);
        
        /* Reconstruct M and S from residuals */
        int16_t *M = (int16_t*)malloc(nf * sizeof(int16_t));
        int16_t *S = (int16_t*)malloc(nf * sizeof(int16_t));
        
        for (int b = 0; b < nblocks; b++) {
            int start = b * LPC_BLOCK_SIZE;
            int blen = (start + LPC_BLOCK_SIZE <= nf) ? LPC_BLOCK_SIZE : (nf - start);
            int oM = orderM_arr[b], oS = orderS_arr[b];
            
            for (int i = start; i < start + blen; i++) {
                int32_t pred = 0;
                for (int j = 1; j <= oM; j++) {
                    if (i - j >= start) pred += (int32_t)coeffM[b][j-1] * M[i - j];
                }
                M[i] = resM[i] + (int16_t)(pred >> LPC_QLEVEL);
            }
            for (int i = start; i < start + blen; i++) {
                int32_t pred = 0;
                for (int j = 1; j <= oS; j++) {
                    if (i - j >= start) pred += (int32_t)coeffS[b][j-1] * S[i - j];
                }
                S[i] = resS[i] + (int16_t)(pred >> LPC_QLEVEL);
            }
        }
        
        /* Undo Mid/Side: L = M + (S>>1), R = L - S */
        for (int i = 0; i < nf; i++) {
            int16_t l = M[i] + (S[i] >> 1);
            int16_t r = l - S[i];
            out[i*4] = (uint8_t)(l & 0xFF); out[i*4+1] = (uint8_t)((l >> 8) & 0xFF);
            out[i*4+2] = (uint8_t)(r & 0xFF); out[i*4+3] = (uint8_t)((r >> 8) & 0xFF);
        }
        
        free(M); free(S); free(resM); free(resS);
        free(orderM_arr); free(orderS_arr); free(coeffM); free(coeffS);
    } else if (version == 3) {
        /* Mid/Side + delta order-1 */
        const uint8_t *dm_lo = in+p, *dm_hi = dm_lo+nf;
        const uint8_t *ds_lo = dm_hi+nf, *ds_hi = ds_lo+nf;
        
        int16_t *dM = (int16_t*)malloc(nf * sizeof(int16_t));
        int16_t *dS = (int16_t*)malloc(nf * sizeof(int16_t));
        for (int i = 0; i < nf; i++) {
            dM[i] = (int16_t)((uint16_t)dm_lo[i] | ((uint16_t)dm_hi[i] << 8));
            dS[i] = (int16_t)((uint16_t)ds_lo[i] | ((uint16_t)ds_hi[i] << 8));
        }
        
        int16_t *Md = (int16_t*)malloc(nf * sizeof(int16_t));
        int16_t *Sd = (int16_t*)malloc(nf * sizeof(int16_t));
        Md[0] = dM[0]; Sd[0] = dS[0];
        for (int i = 1; i < nf; i++) { Md[i] = Md[i-1] + dM[i]; Sd[i] = Sd[i-1] + dS[i]; }
        free(dM); free(dS);
        
        for (int i = 0; i < nf; i++) {
            int16_t l = Md[i] + (Sd[i] >> 1);
            int16_t r = l - Sd[i];
            out[i*4] = (uint8_t)(l & 0xFF); out[i*4+1] = (uint8_t)((l >> 8) & 0xFF);
            out[i*4+2] = (uint8_t)(r & 0xFF); out[i*4+3] = (uint8_t)((r >> 8) & 0xFF);
        }
        free(Md); free(Sd);
    } else if (version == 1) {
        const uint8_t *dl_lo = in+p, *dl_hi = dl_lo+nf, *dr_lo = dl_hi+nf, *dr_hi = dr_lo+nf;
        int16_t pl = 0, pr = 0;
        for (int i = 0; i < nf; i++) {
            int16_t dl = (int16_t)((uint16_t)dl_lo[i] | ((uint16_t)dl_hi[i] << 8));
            int16_t dr = (int16_t)((uint16_t)dr_lo[i] | ((uint16_t)dr_hi[i] << 8));
            int16_t l = pl + dl, r = pr + dr; pl = l; pr = r;
            out[i*4] = (uint8_t)(l & 0xFF); out[i*4+1] = (uint8_t)((l >> 8) & 0xFF);
            out[i*4+2] = (uint8_t)(r & 0xFF); out[i*4+3] = (uint8_t)((r >> 8) & 0xFF);
        }
    } else {
        /* Order-2 */
        const uint8_t *dl_lo = in+p, *dl_hi = dl_lo+nf, *dr_lo = dl_hi+nf, *dr_hi = dr_lo+nf;
        int16_t pl = 0, ppl = 0, pr = 0, ppr = 0;
        for (int i = 0; i < nf; i++) {
            int16_t dl = (int16_t)((uint16_t)dl_lo[i] | ((uint16_t)dl_hi[i] << 8));
            int16_t dr = (int16_t)((uint16_t)dr_lo[i] | ((uint16_t)dr_hi[i] << 8));
            int16_t pred_l = (int16_t)(2 * (int)pl - (int)ppl);
            int16_t pred_r = (int16_t)(2 * (int)pr - (int)ppr);
            int16_t l = pred_l + dl, r = pred_r + dr;
            ppl = pl; pl = l; ppr = pr; pr = r;
            out[i*4] = (uint8_t)(l & 0xFF); out[i*4+1] = (uint8_t)((l >> 8) & 0xFF);
            out[i*4+2] = (uint8_t)(r & 0xFF); out[i*4+3] = (uint8_t)((r >> 8) & 0xFF);
        }
    }
    int tail = orig_size - nf * 4;
    if (tail > 0) memcpy(out + nf*4, in + p + nf*4, tail);
}


/* ===== Entropy Detection ===== */
static double block_entropy(const uint8_t *data, int n) {
    int freq[256] = {0};
    for (int i = 0; i < n; i++) freq[data[i]]++;
    double ent = 0;
    for (int i = 0; i < 256; i++) {
        if (freq[i] > 0) {
            double p = (double)freq[i] / n;
            ent -= p * log2(p);
        }
    }
    return ent;
}

/* ===== LZP (Lempel-Ziv Prediction) ===== */
static inline uint32_t lzp_hash(const uint8_t *p) {
    uint64_t h = 14695981039346656037ull;
    for (int i = 0; i < LZP_CONTEXT; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return (uint32_t)(h & LZP_HTAB_MASK);
}

static int lzp_encode(const uint8_t *in, int n, uint8_t *out) {
    if (n < LZP_CONTEXT + LZP_MIN_MATCH) return 0;

    int32_t *htab = (int32_t*)malloc(LZP_HTAB_SIZE * sizeof(int32_t));
    memset(htab, 0xFF, LZP_HTAB_SIZE * sizeof(int32_t)); /* all -1 */

    /* First LZP_CONTEXT bytes are always literal */
    memcpy(out, in, LZP_CONTEXT);
    int out_pos = LZP_CONTEXT;
    int i = LZP_CONTEXT;

    while (i < n) {
        int flag_pos = out_pos++;
        uint8_t flag = 0;

        for (int bit = 0; bit < 8 && i < n; bit++) {
            uint32_t h = lzp_hash(&in[i - LZP_CONTEXT]);
            int32_t ref = htab[h];
            htab[h] = i;   /* update BEFORE match check */

            if (ref >= 0) {
                /* check match length */
                int match_len = 0;
                while (i + match_len < n && in[ref + match_len] == in[i + match_len])
                    match_len++;

                if (match_len >= LZP_MIN_MATCH) {
                    flag |= (1 << bit);
                    /* encode match length */
                    int val = match_len - LZP_MIN_MATCH;
                    while (val >= 255) {
                        out[out_pos++] = 255;
                        val -= 255;
                    }
                    out[out_pos++] = (uint8_t)val;

                    /* update hash for intermediate positions */
                    for (int j = i + 1; j < i + match_len; j++) {
                        if (j >= LZP_CONTEXT) {
                            uint32_t h2 = lzp_hash(&in[j - LZP_CONTEXT]);
                            htab[h2] = j;
                        }
                    }
                    i += match_len;
                    out[flag_pos] = flag;
                    continue;
                }
            }

            /* literal */
            out[out_pos++] = in[i++];
        }
        out[flag_pos] = flag;
    }

    free(htab);

    /* only return if compression improved */
    if (out_pos >= n) return 0;
    return out_pos;
}

static int lzp_decode(const uint8_t *in, int in_size, uint8_t *out, int orig_size) {
    int32_t *htab = (int32_t*)malloc(LZP_HTAB_SIZE * sizeof(int32_t));
    memset(htab, 0xFF, LZP_HTAB_SIZE * sizeof(int32_t));

    /* First LZP_CONTEXT bytes are literal */
    memcpy(out, in, LZP_CONTEXT);
    int in_pos = LZP_CONTEXT;
    int out_pos = LZP_CONTEXT;

    while (out_pos < orig_size && in_pos < in_size) {
        uint8_t flag = in[in_pos++];

        for (int bit = 0; bit < 8 && out_pos < orig_size; bit++) {
            uint32_t h = lzp_hash(&out[out_pos - LZP_CONTEXT]);
            int32_t ref = htab[h];
            htab[h] = out_pos;   /* update BEFORE processing */

            if (flag & (1 << bit)) {
                /* match — read length */
                int val = 0;
                uint8_t b = in[in_pos++];
                while (b == 255) {
                    val += 255;
                    b = in[in_pos++];
                }
                val += b;
                int match_len = val + LZP_MIN_MATCH;

                /* copy match data byte by byte (handles potential overlap) */
                for (int j = 0; j < match_len && out_pos + j < orig_size; j++)
                    out[out_pos + j] = out[ref + j];

                /* update hash for intermediate positions */
                for (int j = out_pos + 1; j < out_pos + match_len && j < orig_size; j++) {
                    if (j >= LZP_CONTEXT) {
                        uint32_t h2 = lzp_hash(&out[j - LZP_CONTEXT]);
                        htab[h2] = j;
                    }
                }
                out_pos += match_len;
            } else {
                /* literal */
                out[out_pos++] = in[in_pos++];
            }
        }
    }

    free(htab);
    return out_pos;
}

/* ===================================================================
 * ===== Context Mixing Engine (CM) =====
 * ===================================================================
 * Bit-level compression: 9 statistical models combined via logistic mixing.
 * Models: Order-0/1/2/3/4/6/8 contexts + Match model + Word model.
 * Each bit is predicted, encoded with the range coder, then all models
 * are updated. Encoder and decoder run identical prediction logic.
 */

#define CM_SCALE   12
#define CM_PSCALE  (1 << CM_SCALE)       /* 4096 */
#define CM_NMOD    7
#define CM_O2_BITS 20
/* Phase 6: Enlarged order-3 hash for literary text.
 * 20 bits (1M entries) → 22 bits (4M entries).
 * Order-3 captures letter trigrams and word beginnings;
 * reducing collisions improves prediction on small text files. */
#define CM_O3_BITS 22
#define CM_O4_BITS 22
#define CM_O6_BITS 22
#define CM_O2_SIZE (1 << CM_O2_BITS)     /* 1M */
#define CM_O3_SIZE (1 << CM_O3_BITS)
#define CM_O4_SIZE (1 << CM_O4_BITS)     /* 4M */
#define CM_O6_SIZE (1 << CM_O6_BITS)
#define CM_MHT_BITS 21
#define CM_MHT_SIZE (1 << CM_MHT_BITS)   /* 2M */

/* --- Stretch / Squash lookup tables for logistic mixing --- */
static int cm_str_t[CM_PSCALE];   /* stretch: p in [0,4095] -> logit */
static int cm_sqsh_t[8192];       /* squash:  logit+4096 -> [1,4095] */
static int cm_lut_ok = 0;

static void cm_init_lut(void) {
    if (cm_lut_ok) return;
    for (int i = 1; i < CM_PSCALE; i++)
        cm_str_t[i] = (int)(256.0 * log((double)i / (CM_PSCALE - i)));
    cm_str_t[0] = cm_str_t[1];
    for (int i = 0; i < 8192; i++) {
        double x = (double)(i - 4096) / 256.0;
        int v = (int)(CM_PSCALE / (1.0 + exp(-x)) + 0.5);
        if (v < 1) v = 1;
        if (v >= CM_PSCALE) v = CM_PSCALE - 1;
        cm_sqsh_t[i] = v;
    }
    cm_lut_ok = 1;
}

static inline int cm_stretch(int p) {
    if (p < 1) p = 1; else if (p >= CM_PSCALE) p = CM_PSCALE - 1;
    return cm_str_t[p];
}
static inline int cm_squash(int x) {
    int i = x + 4096;
    if (i < 0) i = 0; else if (i >= 8192) i = 8191;
    return cm_sqsh_t[i];
}

/* --- FNV-1a hash step --- */
static inline uint32_t cm_h(uint32_t h, uint8_t c) {
    return (h ^ c) * 16777619u;
}

/* --- Counter: (n0, n1) pair as uint8_t[2] --- */
static inline int cm_ctr_p(const uint8_t *c) {
    /* P(bit=1) in [1, 4095] */
    return ((int)c[1] + 1) * (CM_PSCALE - 2) / ((int)c[0] + (int)c[1] + 2) + 1;
}
/* cap controls adaptation speed: lower cap = faster learning, less precision */
static inline void cm_ctr_up(uint8_t *c, int bit) {
    if (c[bit] < 255) c[bit]++;
    else { c[0] = (c[0] >> 1) + 1; c[1] = (c[1] >> 1) + 1; }
}
static inline void cm_ctr_up_fast(uint8_t *c, int bit, int cap) {
    if (c[bit] < (uint8_t)cap) c[bit]++;
    else { c[0] = (c[0] >> 1) + 1; c[1] = (c[1] >> 1) + 1; }
}

/* --- CM Encode: data[0..n-1] -> out[], returns compressed size --- */
static int cm_encode(const uint8_t *data, int n, uint8_t *out) {
    if (n == 0) return 0;
    cm_init_lut();

    /* Allocate counter tables (zeroed = uniform prior) */
    uint8_t *ct0 = (uint8_t*)calloc(256, 2);
    uint8_t *ct1 = (uint8_t*)calloc(65536, 2);
    uint8_t *ct2 = (uint8_t*)calloc(CM_O2_SIZE, 2);
    uint8_t *ct3 = (uint8_t*)calloc(CM_O3_SIZE, 2);
    uint8_t *ct4 = (uint8_t*)calloc(CM_O4_SIZE, 2);
    uint8_t *ct6 = (uint8_t*)calloc(CM_O6_SIZE, 2);
    int32_t *mht = (int32_t*)malloc(CM_MHT_SIZE * sizeof(int32_t));
    memset(mht, 0xFF, CM_MHT_SIZE * sizeof(int32_t)); /* -1 = no entry */

    RCEnc rc; rc_enc_init(&rc, out);

    int c0 = 1;  /* partial byte with leading sentinel bit */
    uint8_t c1=0, c2=0, c3=0, c4=0, c5=0, c6=0;
    uint32_t h2=0, h3=0, h4=0, h6=0;
    int mpos = -1;  /* match position, -1 = no match */

    /* Mixer weights (fixed point, scale 2^10 = 1024) */
    int w[CM_NMOD];
    for (int k = 0; k < CM_NMOD; k++) w[k] = 1024 / CM_NMOD;

    for (int i = 0; i < n; i++) {
        int byte_val = data[i];
        for (int j = 7; j >= 0; j--) {
            int bit = (byte_val >> j) & 1;

            /* === PREDICT === */
            int p[CM_NMOD];
            uint32_t ix;

            p[0] = cm_ctr_p(&ct0[c0 * 2]);                                            /* O0 */
            p[1] = cm_ctr_p(&ct1[((uint32_t)c1 << 8 | c0) * 2]);                      /* O1 */
            ix = (h2 ^ (c0 * 0x9E3779B9u)) & (CM_O2_SIZE - 1);
            p[2] = cm_ctr_p(&ct2[ix * 2]);                                             /* O2 */
            ix = (h3 ^ (c0 * 0x9E3779B9u)) & (CM_O3_SIZE - 1);
            p[3] = cm_ctr_p(&ct3[ix * 2]);                                             /* O3 */
            ix = (h4 ^ (c0 * 0x9E3779B9u)) & (CM_O4_SIZE - 1);
            p[4] = cm_ctr_p(&ct4[ix * 2]);                                             /* O4 */
            ix = (h6 ^ (c0 * 0x9E3779B9u)) & (CM_O6_SIZE - 1);
            p[5] = cm_ctr_p(&ct6[ix * 2]);                                             /* O6 */

            int match_bit = -1;
            if (mpos >= 0 && mpos < n) {
                match_bit = (data[mpos] >> j) & 1;
                p[6] = match_bit ? 3900 : 196;                                         /* Match ~95% */
            } else {
                p[6] = CM_PSCALE / 2;                                                  /* neutral */
            }

            /* === MIX (logistic) === */
            int64_t sum = 0;
            for (int k = 0; k < CM_NMOD; k++)
                sum += (int64_t)w[k] * cm_stretch(p[k]);
            int pred = cm_squash((int)(sum >> 10));
            if (pred < 1) pred = 1;
            if (pred >= CM_PSCALE) pred = CM_PSCALE - 1;

            /* === ENCODE bit === */
            uint32_t bound = (uint32_t)(((uint64_t)rc.range * (uint32_t)(CM_PSCALE - pred)) >> CM_SCALE);
            if (bound < 1) bound = 1;
            if (bound >= rc.range) bound = rc.range - 1;
            if (bit) { rc.low += bound; rc.range -= bound; }
            else     { rc.range = bound; }
            rc_enc_norm(&rc);

            /* === UPDATE models === */
            int err = (bit << CM_SCALE) - pred;
            for (int k = 0; k < CM_NMOD; k++)
                w[k] += (int)((int64_t)err * cm_stretch(p[k]) >> 18);

            cm_ctr_up(&ct0[c0 * 2], bit);                                              /* O0: slow */
            cm_ctr_up(&ct1[((uint32_t)c1 << 8 | c0) * 2], bit);                      /* O1: slow */
            cm_ctr_up_fast(&ct2[((h2 ^ (c0 * 0x9E3779B9u)) & (CM_O2_SIZE-1)) * 2], bit, 63);  /* O2: medium */
            cm_ctr_up_fast(&ct3[((h3 ^ (c0 * 0x9E3779B9u)) & (CM_O3_SIZE-1)) * 2], bit, 31);  /* O3: fast */
            cm_ctr_up_fast(&ct4[((h4 ^ (c0 * 0x9E3779B9u)) & (CM_O4_SIZE-1)) * 2], bit, 15);  /* O4: very fast */
            cm_ctr_up_fast(&ct6[((h6 ^ (c0 * 0x9E3779B9u)) & (CM_O6_SIZE-1)) * 2], bit, 15);  /* O6: very fast */

            if (mpos >= 0 && match_bit >= 0 && bit != match_bit) mpos = -1;

            c0 = (c0 << 1) | bit;

            if (c0 >= 256) {
                uint8_t nb = (uint8_t)(c0 - 256);
                c0 = 1;
                if (mpos >= 0) { mpos++; if (mpos >= n) mpos = -1; }
                c6=c5; c5=c4; c4=c3; c3=c2; c2=c1; c1=nb;
                h2 = cm_h(cm_h(2166136261u, c2), c1);
                h3 = cm_h(h2, c3);
                h4 = cm_h(h3, c4);
                h6 = cm_h(cm_h(h4, c5), c6);
                if (i >= 3) {
                    uint32_t mh = h4 & (CM_MHT_SIZE - 1);
                    int32_t old_pos = mht[mh];
                    mht[mh] = (int32_t)(i + 1);
                    if (mpos < 0 && old_pos >= 0 && old_pos <= i && old_pos < n)
                        mpos = old_pos;
                }
            }
        }
    }

    int csize = rc_enc_finish(&rc);
    free(ct0); free(ct1); free(ct2); free(ct3); free(ct4); free(ct6); free(mht);
    return csize;
}

/* --- CM Decode: in[0..csize-1] -> out[0..n-1] --- */
static void cm_decode(const uint8_t *in, uint8_t *out, int n) {
    if (n == 0) return;
    cm_init_lut();

    uint8_t *ct0 = (uint8_t*)calloc(256, 2);
    uint8_t *ct1 = (uint8_t*)calloc(65536, 2);
    uint8_t *ct2 = (uint8_t*)calloc(CM_O2_SIZE, 2);
    uint8_t *ct3 = (uint8_t*)calloc(CM_O3_SIZE, 2);
    uint8_t *ct4 = (uint8_t*)calloc(CM_O4_SIZE, 2);
    uint8_t *ct6 = (uint8_t*)calloc(CM_O6_SIZE, 2);
    int32_t *mht = (int32_t*)malloc(CM_MHT_SIZE * sizeof(int32_t));
    memset(mht, 0xFF, CM_MHT_SIZE * sizeof(int32_t));

    RCDec rc; rc_dec_init(&rc, in);

    int c0 = 1;
    uint8_t c1=0, c2=0, c3=0, c4=0, c5=0, c6=0;
    uint32_t h2=0, h3=0, h4=0, h6=0;
    int mpos = -1;
    int w[CM_NMOD];
    for (int k = 0; k < CM_NMOD; k++) w[k] = 1024 / CM_NMOD;

    for (int i = 0; i < n; i++) {
        int byte_val = 0;
        for (int j = 7; j >= 0; j--) {
            /* === PREDICT (identical to encoder) === */
            int p[CM_NMOD];
            uint32_t ix;

            p[0] = cm_ctr_p(&ct0[c0 * 2]);
            p[1] = cm_ctr_p(&ct1[((uint32_t)c1 << 8 | c0) * 2]);
            ix = (h2 ^ (c0 * 0x9E3779B9u)) & (CM_O2_SIZE - 1);
            p[2] = cm_ctr_p(&ct2[ix * 2]);
            ix = (h3 ^ (c0 * 0x9E3779B9u)) & (CM_O3_SIZE - 1);
            p[3] = cm_ctr_p(&ct3[ix * 2]);
            ix = (h4 ^ (c0 * 0x9E3779B9u)) & (CM_O4_SIZE - 1);
            p[4] = cm_ctr_p(&ct4[ix * 2]);
            ix = (h6 ^ (c0 * 0x9E3779B9u)) & (CM_O6_SIZE - 1);
            p[5] = cm_ctr_p(&ct6[ix * 2]);

            int match_bit = -1;
            if (mpos >= 0 && mpos < n) {
                match_bit = (out[mpos] >> j) & 1;      /* decoder reads from out[] */
                p[6] = match_bit ? 3900 : 196;
            } else {
                p[6] = CM_PSCALE / 2;
            }

            int64_t sum = 0;
            for (int k = 0; k < CM_NMOD; k++)
                sum += (int64_t)w[k] * cm_stretch(p[k]);
            int pred = cm_squash((int)(sum >> 10));
            if (pred < 1) pred = 1;
            if (pred >= CM_PSCALE) pred = CM_PSCALE - 1;

            /* === DECODE bit === */
            uint32_t bound = (uint32_t)(((uint64_t)rc.range * (uint32_t)(CM_PSCALE - pred)) >> CM_SCALE);
            if (bound < 1) bound = 1;
            if (bound >= rc.range) bound = rc.range - 1;
            int bit;
            if ((rc.code - rc.low) >= bound) {
                bit = 1; rc.low += bound; rc.range -= bound;
            } else {
                bit = 0; rc.range = bound;
            }
            rc_dec_norm(&rc);

            /* === UPDATE (identical to encoder) === */
            int err = (bit << CM_SCALE) - pred;
            for (int k = 0; k < CM_NMOD; k++)
                w[k] += (int)((int64_t)err * cm_stretch(p[k]) >> 18);

            cm_ctr_up(&ct0[c0 * 2], bit);                                              /* O0: slow */
            cm_ctr_up(&ct1[((uint32_t)c1 << 8 | c0) * 2], bit);                      /* O1: slow */
            cm_ctr_up_fast(&ct2[((h2 ^ (c0 * 0x9E3779B9u)) & (CM_O2_SIZE-1)) * 2], bit, 63);  /* O2: medium */
            cm_ctr_up_fast(&ct3[((h3 ^ (c0 * 0x9E3779B9u)) & (CM_O3_SIZE-1)) * 2], bit, 31);  /* O3: fast */
            cm_ctr_up_fast(&ct4[((h4 ^ (c0 * 0x9E3779B9u)) & (CM_O4_SIZE-1)) * 2], bit, 15);  /* O4: very fast */
            cm_ctr_up_fast(&ct6[((h6 ^ (c0 * 0x9E3779B9u)) & (CM_O6_SIZE-1)) * 2], bit, 15);  /* O6: very fast */

            if (mpos >= 0 && match_bit >= 0 && bit != match_bit) mpos = -1;

            c0 = (c0 << 1) | bit;
            byte_val = (byte_val << 1) | bit;

            if (c0 >= 256) {
                uint8_t nb = (uint8_t)(c0 - 256);
                c0 = 1;
                if (mpos >= 0) { mpos++; if (mpos >= n) mpos = -1; }
                c6=c5; c5=c4; c4=c3; c3=c2; c2=c1; c1=nb;
                h2 = cm_h(cm_h(2166136261u, c2), c1);
                h3 = cm_h(h2, c3);
                h4 = cm_h(h3, c4);
                h6 = cm_h(cm_h(h4, c5), c6);
                if (i >= 3) {
                    uint32_t mh = h4 & (CM_MHT_SIZE - 1);
                    int32_t old_pos = mht[mh];
                    mht[mh] = (int32_t)(i + 1);
                    if (mpos < 0 && old_pos >= 0 && old_pos <= i && old_pos < n)
                        mpos = old_pos;
                }
            }
        }
        out[i] = (uint8_t)byte_val;
    }

    free(ct0); free(ct1); free(ct2); free(ct3); free(ct4); free(ct6); free(mht);
}


/* ===================================================================
 * ===== PPM Compression (Prediction by Partial Matching) =====
 * ===================================================================
 * PPMC method, orders 0-3, with exclusion.
 * Each context tracks per-symbol frequencies. Escape = Method C.
 * O0: 1 context, O1: 256, O2: 64K (exact), O3: 64K (hashed).
 */

#define PPM_MAXORD    3
#define PPM_O2_SIZE   65536    /* exact coverage for all 2-byte contexts */
#define PPM_O3_BITS   17
#define PPM_O3_SIZE   (1 << PPM_O3_BITS)  /* 128K hashed */
#define PPM_RESCALE   4000     /* rescale when total exceeds this */

typedef struct {
    uint8_t  count[256];   /* per-symbol frequency; 0 = unseen */
    uint16_t total;        /* sum of all counts */
    uint16_t distinct;     /* number of symbols with count > 0 */
} PPMCtx;

static void ppm_ctx_rescale(PPMCtx *c) {
    c->total = 0;
    c->distinct = 0;
    for (int i = 0; i < 256; i++) {
        c->count[i] = (c->count[i] + 1) >> 1;
        if (c->count[i] > 0) {
            c->total += c->count[i];
            c->distinct++;
        }
    }
}

static inline void ppm_ctx_update(PPMCtx *c, uint8_t sym) {
    if (c->count[sym] == 0) c->distinct++;
    c->count[sym]++;
    c->total++;
    if (c->total >= PPM_RESCALE) ppm_ctx_rescale(c);
}

typedef struct {
    PPMCtx  o0;            /* order 0: 1 context */
    PPMCtx *o1;            /* order 1: 256 contexts */
    PPMCtx *o2;            /* order 2: 64K contexts (exact) */
    PPMCtx *o3;            /* order 3: 128K hashed */
    uint8_t h[4];          /* history: h[0]=prev byte, h[1]=prev-1, etc. */
} PPMState;

static void ppm_state_init(PPMState *s) {
    memset(&s->o0, 0, sizeof(PPMCtx));
    s->o1 = (PPMCtx*)calloc(256, sizeof(PPMCtx));
    s->o2 = (PPMCtx*)calloc(PPM_O2_SIZE, sizeof(PPMCtx));
    s->o3 = (PPMCtx*)calloc(PPM_O3_SIZE, sizeof(PPMCtx));
    memset(s->h, 0, 4);
}

static void ppm_state_free(PPMState *s) {
    free(s->o1); free(s->o2); free(s->o3);
}

static inline uint32_t ppm_hash3(uint8_t a, uint8_t b, uint8_t c) {
    return ((uint32_t)c * 65536u + (uint32_t)b * 256u + a) * 2654435761u >> (32 - PPM_O3_BITS);
}

static PPMCtx* ppm_get_ctx(PPMState *s, int order) {
    switch (order) {
        case 0: return &s->o0;
        case 1: return &s->o1[s->h[0]];
        case 2: return &s->o2[((uint32_t)s->h[1] << 8) | s->h[0]];
        case 3: return &s->o3[ppm_hash3(s->h[0], s->h[1], s->h[2])];
        default: return &s->o0;
    }
}

static inline void ppm_push(PPMState *s, uint8_t b) {
    s->h[3] = s->h[2]; s->h[2] = s->h[1]; s->h[1] = s->h[0]; s->h[0] = b;
}

/* Encode one byte with PPM using the range coder */
static void ppm_encode_byte(PPMState *s, RCEnc *rc, uint8_t sym, int pos) {
    int max_ord = PPM_MAXORD;
    if (pos < max_ord) max_ord = pos;

    uint8_t excl[256];
    memset(excl, 0, 256);
    int n_excl = 0;

    for (int ord = max_ord; ord >= 0; ord--) {
        PPMCtx *ctx = ppm_get_ctx(s, ord);
        if (ctx->distinct == 0) continue;

        /* Compute effective stats (excluding already-excluded symbols) */
        int eff_total = 0, eff_distinct = 0;
        for (int c = 0; c < 256; c++) {
            if (ctx->count[c] > 0 && !excl[c]) {
                eff_total += ctx->count[c];
                eff_distinct++;
            }
        }
        if (eff_total == 0) continue;

        /* PPMC denominator: symbols + escape */
        int denom = eff_total + eff_distinct;

        if (ctx->count[sym] > 0 && !excl[sym]) {
            /* Symbol found — encode it */
            int cum = 0;
            for (int c = 0; c < (int)sym; c++)
                if (ctx->count[c] > 0 && !excl[c])
                    cum += ctx->count[c];
            rc_enc(rc, (uint32_t)cum, (uint32_t)ctx->count[sym], (uint32_t)denom);
            goto update;
        }

        /* Symbol not found — encode escape */
        rc_enc(rc, (uint32_t)eff_total, (uint32_t)eff_distinct, (uint32_t)denom);

        /* Update exclusion set */
        for (int c = 0; c < 256; c++)
            if (ctx->count[c] > 0 && !excl[c])
                { excl[c] = 1; n_excl++; }
    }

    /* Order -1: uniform distribution over non-excluded symbols */
    {
        int remaining = 256 - n_excl;
        if (remaining < 1) remaining = 1;
        int cum = 0;
        for (int c = 0; c < (int)sym; c++)
            if (!excl[c]) cum++;
        rc_enc(rc, (uint32_t)cum, 1, (uint32_t)remaining);
    }

update:
    /* Update all contexts (0..max_ord) */
    for (int ord = 0; ord <= max_ord; ord++)
        ppm_ctx_update(ppm_get_ctx(s, ord), sym);
    ppm_push(s, sym);
}

/* Decode one byte with PPM */
static uint8_t ppm_decode_byte(PPMState *s, RCDec *rc, int pos) {
    int max_ord = PPM_MAXORD;
    if (pos < max_ord) max_ord = pos;

    uint8_t excl[256];
    memset(excl, 0, 256);
    int n_excl = 0;
    uint8_t sym = 0;

    for (int ord = max_ord; ord >= 0; ord--) {
        PPMCtx *ctx = ppm_get_ctx(s, ord);
        if (ctx->distinct == 0) continue;

        int eff_total = 0, eff_distinct = 0;
        for (int c = 0; c < 256; c++)
            if (ctx->count[c] > 0 && !excl[c])
                { eff_total += ctx->count[c]; eff_distinct++; }
        if (eff_total == 0) continue;

        int denom = eff_total + eff_distinct;
        uint32_t target = rc_dec_cum(rc, (uint32_t)denom);

        if ((int)target < eff_total) {
            /* Symbol found — find which one */
            int cum = 0;
            for (int c = 0; c < 256; c++) {
                if (ctx->count[c] == 0 || excl[c]) continue;
                if (cum + ctx->count[c] > (int)target) {
                    sym = (uint8_t)c;
                    rc_dec_update(rc, (uint32_t)cum, (uint32_t)ctx->count[c]);
                    goto update;
                }
                cum += ctx->count[c];
            }
        }

        /* Escape */
        rc_dec_update(rc, (uint32_t)eff_total, (uint32_t)eff_distinct);
        for (int c = 0; c < 256; c++)
            if (ctx->count[c] > 0 && !excl[c])
                { excl[c] = 1; n_excl++; }
    }

    /* Order -1: uniform */
    {
        int remaining = 256 - n_excl;
        if (remaining < 1) remaining = 1;
        uint32_t target = rc_dec_cum(rc, (uint32_t)remaining);
        int cum = 0;
        for (int c = 0; c < 256; c++) {
            if (excl[c]) continue;
            if (cum == (int)target) {
                sym = (uint8_t)c;
                rc_dec_update(rc, (uint32_t)cum, 1);
                goto update;
            }
            cum++;
        }
        /* Fallback: last non-excluded symbol */
        rc_dec_update(rc, (uint32_t)(remaining - 1), 1);
    }

update:
    for (int ord = 0; ord <= max_ord; ord++)
        ppm_ctx_update(ppm_get_ctx(s, ord), sym);
    ppm_push(s, sym);
    return sym;
}

/* Top-level PPM encode: data[0..n-1] -> out[], returns compressed size */
static int ppm_compress(const uint8_t *data, int n, uint8_t *out) {
    PPMState s;
    ppm_state_init(&s);
    RCEnc rc;
    rc_enc_init(&rc, out);
    for (int i = 0; i < n; i++)
        ppm_encode_byte(&s, &rc, data[i], i);
    int size = rc_enc_finish(&rc);
    ppm_state_free(&s);
    return size;
}

/* Top-level PPM decode: in[0..] -> out[0..n-1] */
static void ppm_decompress(const uint8_t *in, uint8_t *out, int n) {
    PPMState s;
    ppm_state_init(&s);
    RCDec rc;
    rc_dec_init(&rc, in);
    for (int i = 0; i < n; i++)
        out[i] = ppm_decode_byte(&s, &rc, i);
    ppm_state_free(&s);
}


/* ===== Base64 Preprocessing ===== */

#define B64_MIN_RUN  76    /* minimum base64 run length to consider */

static int b64_val[256];
static int b64_tab_ready = 0;

static void b64_init(void) {
    if (b64_tab_ready) return;
    memset(b64_val, 0xFF, sizeof(b64_val));  /* fill with -1 */
    const char *ch = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) b64_val[(unsigned char)ch[i]] = i;
    b64_val['='] = 64;  /* valid but special */
    b64_tab_ready = 1;
}

typedef struct { int start; int len; int dec_len; } B64Run;

/* Find base64 runs >= B64_MIN_RUN chars. Returns count. */
static int b64_find_runs(const uint8_t *data, int n, B64Run *runs, int max_runs) {
    b64_init();
    int nruns = 0, i = 0;
    while (i < n && nruns < max_runs) {
        while (i < n && b64_val[data[i]] < 0) i++;
        if (i >= n) break;
        int start = i;
        while (i < n && b64_val[data[i]] >= 0) i++;
        int run_len = ((i - start) / 4) * 4;  /* trim to multiple of 4 */
        if (run_len < B64_MIN_RUN) continue;
        /* Validate: '=' only in last 2 positions */
        int valid = 1;
        for (int j = start; j < start + run_len - 2; j++) {
            if (data[j] == '=') { valid = 0; break; }
        }
        if (!valid) continue;
        runs[nruns].start = start;
        runs[nruns].len = run_len;
        runs[nruns].dec_len = 0;
        nruns++;
    }
    return nruns;
}

/* Decode base64 chunk. Returns decoded length, -1 on error. */
static int b64_decode_chunk(const uint8_t *in, int len, uint8_t *out) {
    b64_init();
    int j = 0;
    for (int i = 0; i < len; i += 4) {
        int a = b64_val[in[i]], b = b64_val[in[i+1]];
        int c = b64_val[in[i+2]], d = b64_val[in[i+3]];
        if (a < 0 || a > 63 || b < 0 || b > 63) return -1;
        uint32_t v = ((uint32_t)a << 18) | ((uint32_t)b << 12);
        out[j++] = (uint8_t)(v >> 16);
        if (in[i+2] != '=') {
            if (c < 0 || c > 63) return -1;
            v |= ((uint32_t)c << 6);
            out[j++] = (uint8_t)(v >> 8);
            if (in[i+3] != '=') {
                if (d < 0 || d > 63) return -1;
                v |= (uint32_t)d;
                out[j++] = (uint8_t)v;
            }
        }
    }
    return j;
}

/* Encode to base64. Returns encoded length. */
static int b64_encode_chunk(const uint8_t *in, int len, uint8_t *out) {
    static const uint8_t ch[64] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int j = 0, i;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[j++] = ch[(v>>18)&63]; out[j++] = ch[(v>>12)&63];
        out[j++] = ch[(v>>6)&63];  out[j++] = ch[v&63];
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i+1 < len) v |= (uint32_t)in[i+1] << 8;
        out[j++] = ch[(v>>18)&63]; out[j++] = ch[(v>>12)&63];
        out[j++] = (i+1 < len) ? ch[(v>>6)&63] : '=';
        out[j++] = '=';
    }
    return j;
}

/* Varint encode */
static int varint_enc(uint8_t *out, uint32_t v) {
    int p = 0;
    while (v >= 128) { out[p++] = (v & 127) | 128; v >>= 7; }
    out[p++] = (uint8_t)v;
    return p;
}

/* Varint decode */
static int varint_dec(const uint8_t *in, uint32_t *v) {
    *v = 0; int p = 0; uint32_t shift = 0;
    while (in[p] & 128) { *v |= (uint32_t)(in[p++] & 127) << shift; shift += 7; }
    *v |= (uint32_t)in[p++] << shift;
    return p;
}

/* Encode position table (delta-varint: delta_start, len, dec_len per run) */
static int b64_encode_pos(const B64Run *runs, int nruns, uint8_t *out) {
    int p = 0, prev = 0;
    for (int i = 0; i < nruns; i++) {
        p += varint_enc(out + p, (uint32_t)(runs[i].start - prev));
        p += varint_enc(out + p, (uint32_t)runs[i].len);
        p += varint_enc(out + p, (uint32_t)runs[i].dec_len);
        prev = runs[i].start + runs[i].len;
    }
    return p;
}

/* Decode position table */
static void b64_decode_pos(const uint8_t *in, int size, B64Run *runs, int nruns) {
    int p = 0, prev = 0;
    (void)size;
    for (int i = 0; i < nruns; i++) {
        uint32_t delta, len, dl;
        p += varint_dec(in + p, &delta);
        p += varint_dec(in + p, &len);
        p += varint_dec(in + p, &dl);
        runs[i].start = prev + (int)delta;
        runs[i].len = (int)len;
        runs[i].dec_len = (int)dl;
        prev = runs[i].start + runs[i].len;
    }
}

/* Compress sub-stream: tries multiple strategies, picks best.
 * sub_order: 0-2 BWT+O0/O1/O2   format: [pidx:4][nz:4][arith_data]
 *            3   PPM direct       format: [ppm_data]
 *            4-6 Delta+BWT+O0/1/2 format: [pidx:4][nz:4][arith_data]
 *            7-9 LZP+BWT+O0/1/2   format: [lzp_size:4][pidx:4][nz:4][arith_data]
 *            15  BWT+CM            format: [pidx:4][cm_data]
 * Returns compressed size, sets *sub_order. */

/* ======= Word Preprocessing (v8) ======= */
/* Replaces frequent identifier-like words with single-byte tokens 0x80..0xFE.
   Escape byte 0xFF: any original byte >= 0x80 is stored as [0xFF][byte]. */

#define WP_HASH_BITS  18
#define WP_HASH_SIZE  (1 << WP_HASH_BITS)
#define WP_HASH_MASK  (WP_HASH_SIZE - 1)
#define WP_MAX_WLEN   31
#define WP_MAX_TOKENS 127        /* 0x80 .. 0xFE */
#define WP_ESC        0xFF
#define WP_TOK_BASE   0x80
#define WP_PROBES     64

typedef struct { uint8_t w[WP_MAX_WLEN+1]; uint8_t len; uint32_t freq; int16_t tok; } WPSlot;

static inline int wp_wc(uint8_t c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$'||(c>='0'&&c<='9');}
static inline int wp_ws(uint8_t c){return(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'||c=='$';}
static uint32_t wp_h(const uint8_t*w,int n){uint32_t h=2166136261u;for(int i=0;i<n;i++)h=(h^w[i])*16777619u;return h&WP_HASH_MASK;}

/* Returns encoded size (dict header + encoded body), or 0 if not beneficial. */
static size_t wp_encode(const uint8_t *src, size_t sn, uint8_t *dst, size_t dcap) {
    if (sn < 2048 || dcap < sn) return 0;
    WPSlot *ht = (WPSlot*)calloc(WP_HASH_SIZE, sizeof(WPSlot));
    if (!ht) return 0;

    /* --- Phase 1: count word frequencies --- */
    for (size_t i = 0; i < sn; ) {
        if (wp_ws(src[i])) {
            size_t j = i+1;
            while (j < sn && wp_wc(src[j])) j++;
            int wl = (int)(j-i);
            if (wl >= 3 && wl <= WP_MAX_WLEN) {
                uint32_t h = wp_h(src+i, wl);
                for (int k=0; k<WP_PROBES; k++) {
                    uint32_t idx = (h+k) & WP_HASH_MASK;
                    if (ht[idx].len == 0) {
                        memcpy(ht[idx].w, src+i, wl);
                        ht[idx].len = (uint8_t)wl;
                        ht[idx].freq = 1;
                        ht[idx].tok = -1;
                        break;
                    }
                    if (ht[idx].len == wl && memcmp(ht[idx].w, src+i, wl)==0)
                        { ht[idx].freq++; break; }
                }
            }
            i = j;
        } else i++;
    }

    /* --- Phase 2: pick top-127 by savings = freq*(len-1) --- */
    typedef struct { uint32_t hi; int64_t sv; } WPC;
    int nc = 0;
    /* count qualifying candidates first */
    for (uint32_t x=0; x<WP_HASH_SIZE; x++)
        if (ht[x].len>=3 && ht[x].freq>=3 && (int64_t)ht[x].freq*(ht[x].len-1)>50) nc++;

    WPC *cd = (WPC*)malloc((nc ? nc : 1) * sizeof(WPC));
    if (!cd) { free(ht); return 0; }
    int ci = 0;
    for (uint32_t x=0; x<WP_HASH_SIZE; x++) {
        if (ht[x].len>=3 && ht[x].freq>=3) {
            int64_t sv = (int64_t)ht[x].freq*(ht[x].len-1);
            if (sv > 50) { cd[ci].hi=x; cd[ci].sv=sv; ci++; }
        }
    }
    nc = ci;
    int nt = nc < WP_MAX_TOKENS ? nc : WP_MAX_TOKENS;

    /* partial selection sort — O(nc * nt), fine for nt=127 */
    for (int a=0; a<nt; a++) {
        int b_best=a;
        for (int b=a+1; b<nc; b++)
            if (cd[b].sv > cd[b_best].sv) b_best=b;
        if (b_best!=a) { WPC t=cd[a]; cd[a]=cd[b_best]; cd[b_best]=t; }
    }

    /* check total gross savings */
    int64_t gsav = 0;
    for (int a=0; a<nt; a++) gsav += cd[a].sv;
    if (gsav < 2048) { free(cd); free(ht); return 0; }

    for (int a=0; a<nt; a++) ht[cd[a].hi].tok = WP_TOK_BASE + a;

    /* --- Phase 3: write dict header [nt:1] { [len:1][word:len] }* --- */
    size_t p = 0;
    if (p >= dcap) { free(cd); free(ht); return 0; }
    dst[p++] = (uint8_t)nt;
    for (int a=0; a<nt; a++) {
        uint32_t x = cd[a].hi;
        if (p + 1 + ht[x].len > dcap) { free(cd); free(ht); return 0; }
        dst[p++] = ht[x].len;
        memcpy(dst+p, ht[x].w, ht[x].len);
        p += ht[x].len;
    }
    free(cd);

    /* --- Phase 4: encode body --- */
    for (size_t i=0; i<sn; ) {
        if (p+2 > dcap) { free(ht); return 0; }
        if (wp_ws(src[i])) {
            size_t j = i+1;
            while (j<sn && wp_wc(src[j])) j++;
            int wl = (int)(j-i);
            int ftok = -1;
            if (wl>=3 && wl<=WP_MAX_WLEN) {
                uint32_t h = wp_h(src+i, wl);
                for (int k=0; k<WP_PROBES; k++) {
                    uint32_t idx = (h+k)&WP_HASH_MASK;
                    if (ht[idx].len==0) break;
                    if (ht[idx].len==wl && memcmp(ht[idx].w, src+i, wl)==0)
                        { ftok = ht[idx].tok; break; }
                }
            }
            if (ftok >= 0) { dst[p++] = (uint8_t)ftok; i = j; }
            else { for (; i<j; i++) {
                if (p+2>dcap){free(ht);return 0;}
                if(src[i]>=WP_TOK_BASE){dst[p++]=WP_ESC;dst[p++]=src[i];}
                else dst[p++]=src[i];
            }}
        } else {
            if(src[i]>=WP_TOK_BASE){dst[p++]=WP_ESC;dst[p++]=src[i];}
            else dst[p++]=src[i];
            i++;
        }
    }
    free(ht);
    return (p < sn) ? p : 0;   /* only return if we improved */
}

/* Decode word-preprocessed data. Returns decoded size. */
static size_t wp_decode(const uint8_t *src, size_t sn, uint8_t *dst, size_t dcap) {
    if (sn < 1) return 0;
    size_t p = 0;
    int nw = src[p++];
    uint8_t ws[WP_MAX_TOKENS][WP_MAX_WLEN+1];
    uint8_t wl[WP_MAX_TOKENS];
    for (int i=0; i<nw; i++) {
        if (p>=sn) return 0;
        wl[i] = src[p++];
        if (p+wl[i]>sn || wl[i]>WP_MAX_WLEN) return 0;
        memcpy(ws[i], src+p, wl[i]);
        p += wl[i];
    }
    size_t op = 0;
    while (p < sn) {
        uint8_t c = src[p++];
        if (c>=WP_TOK_BASE && c!=WP_ESC) {
            int idx = c - WP_TOK_BASE;
            if (idx>=nw || op+wl[idx]>dcap) return 0;
            memcpy(dst+op, ws[idx], wl[idx]);
            op += wl[idx];
        } else if (c==WP_ESC) {
            if (p>=sn || op>=dcap) return 0;
            dst[op++] = src[p++];
        } else {
            if (op>=dcap) return 0;
            dst[op++] = c;
        }
    }
    return op;
}

/* Forward declarations for LZMA */
static int lzma_compress(const uint8_t *data, int n, uint8_t *out, int size_limit);
static int lzma_decompress(const uint8_t *in, uint8_t *out, int n);

/* Forward declaration for LZMA heuristic */
static double ascii_ratio(const uint8_t *data, int n);

static int compress_substream(const uint8_t *data, int n, uint8_t *out, int *sub_order, BWTWorkspace *bwt_ws) {
    if (n == 0) { *sub_order = 0; return 0; }

    int alloc = n + n/4 + 4096;
    uint8_t  *bwt_s   = (uint8_t*)malloc(n);
    uint8_t  *mtf_s   = (uint8_t*)malloc(n);
    uint16_t *zrle_s  = (uint16_t*)malloc((size_t)n * 2 * sizeof(uint16_t));
    uint8_t  *arith_s = (uint8_t*)malloc(alloc);

    int best = n + 100;
    *sub_order = 0;
    int asize, total, pidx, nz;

    /* === Strategy A: BWT + O0/O1/O2 (sub_order 0-2) === */
    pidx = bwt_encode_ws(data, n, bwt_s, bwt_ws);
    int orig_pidx = pidx;  /* save for BWT+CM strategy */
    mtf_encode(bwt_s, n, mtf_s);
    nz = zrle_encode(mtf_s, n, zrle_s);

    asize = arith_enc_o0(zrle_s, nz, arith_s);
    total = 8 + asize;
    if (total < best) {
        best = total; *sub_order = 0;
        put_u32be(out, pidx); put_u32be(out + 4, nz);
        memcpy(out + 8, arith_s, asize);
    }
    asize = arith_enc_o1(zrle_s, nz, arith_s);
    total = 8 + asize;
    if (total < best) {
        best = total; *sub_order = 1;
        put_u32be(out, pidx); put_u32be(out + 4, nz);
        memcpy(out + 8, arith_s, asize);
    }
    asize = arith_enc_o2(zrle_s, nz, arith_s);
    total = 8 + asize;
    if (total < best) {
        best = total; *sub_order = 2;
        put_u32be(out, pidx); put_u32be(out + 4, nz);
        memcpy(out + 8, arith_s, asize);
    }
    /* BWT+rANS O0 (sub_order 15): same ratio as O0, O(1) table decode */
    asize = rans_enc_o0(zrle_s, nz, arith_s);
    total = 8 + asize;
    if (total < best) {
        best = total; *sub_order = 15;
        put_u32be(out, pidx); put_u32be(out + 4, nz);
        memcpy(out + 8, arith_s, asize);
    }

    /* === Strategy B: PPM direct (sub_order 3) — only for small streams === */
    if (n <= 33554432) {  /* 32 MB */
        int ppm_sz = ppm_compress(data, n, arith_s);
        if (ppm_sz > 0 && ppm_sz < best) {
            best = ppm_sz; *sub_order = 3;
            memcpy(out, arith_s, ppm_sz);
            fprintf(stderr, "      [Sub PPM] %d -> %d (%.2fx)\n", n, ppm_sz, (double)n/ppm_sz);
        }
    }

    /* === Strategy C: Delta + BWT + O0/O1/O2 (sub_order 4-6) === */
    {
        uint8_t *delta_s = (uint8_t*)malloc(n);
        delta_encode(data, n, delta_s);
        pidx = bwt_encode_ws(delta_s, n, bwt_s, bwt_ws);
        mtf_encode(bwt_s, n, mtf_s);
        nz = zrle_encode(mtf_s, n, zrle_s);

        for (int o = 0; o < 3; o++) {
            if (o == 0) asize = arith_enc_o0(zrle_s, nz, arith_s);
            else if (o == 1) asize = arith_enc_o1(zrle_s, nz, arith_s);
            else asize = arith_enc_o2(zrle_s, nz, arith_s);
            total = 8 + asize;
            if (total < best) {
                best = total; *sub_order = 4 + o;
                put_u32be(out, pidx); put_u32be(out + 4, nz);
                memcpy(out + 8, arith_s, asize);
            }
        }
        /* Delta+BWT+rANS O0 (sub_order 17) */
        asize = rans_enc_o0(zrle_s, nz, arith_s);
        total = 8 + asize;
        if (total < best) {
            best = total; *sub_order = 17;
            put_u32be(out, pidx); put_u32be(out + 4, nz);
            memcpy(out + 8, arith_s, asize);
        }
        free(delta_s);
    }

    /* === Strategy D: LZP + BWT + O0/O1/O2 (sub_order 7-9) === */
    /* Format: [lzp_out_size:4][pidx:4][nz:4][arith_data] */
    {
        uint8_t *lzp_s = (uint8_t*)malloc(alloc);
        int lzp_size = lzp_encode(data, n, lzp_s);
        if (lzp_size > 0 && lzp_size < n) {
            fprintf(stderr, "      [Sub LZP] %d -> %d (%.2fx reduction)\n",
                    n, lzp_size, (double)n/lzp_size);
            pidx = bwt_encode_ws(lzp_s, lzp_size, bwt_s, bwt_ws);
            mtf_encode(bwt_s, lzp_size, mtf_s);
            nz = zrle_encode(mtf_s, lzp_size, zrle_s);

            for (int o = 0; o < 3; o++) {
                if (o == 0) asize = arith_enc_o0(zrle_s, nz, arith_s);
                else if (o == 1) asize = arith_enc_o1(zrle_s, nz, arith_s);
                else asize = arith_enc_o2(zrle_s, nz, arith_s);
                total = 12 + asize;  /* 4 (lzp_size) + 4 (pidx) + 4 (nz) + arith */
                if (total < best) {
                    best = total; *sub_order = 7 + o;
                    put_u32be(out, lzp_size);
                    put_u32be(out + 4, pidx);
                    put_u32be(out + 8, nz);
                    memcpy(out + 12, arith_s, asize);
                    fprintf(stderr, "      [Sub LZP+BWT+O%d] NEW BEST: %d -> %d (%.2fx)\n",
                            o, n, total, (double)n/total);
                }
            }
            /* LZP+BWT+rANS O0 (sub_order 16) */
            asize = rans_enc_o0(zrle_s, nz, arith_s);
            total = 12 + asize;
            if (total < best) {
                best = total; *sub_order = 16;
                put_u32be(out, lzp_size);
                put_u32be(out + 4, pidx);
                put_u32be(out + 8, nz);
                memcpy(out + 12, arith_s, asize);
                fprintf(stderr, "      [Sub LZP+BWT+rANS] NEW BEST: %d -> %d (%.2fx)\n",
                        n, total, (double)n/total);
            }
        }
        free(lzp_s);
    }

    /* === Strategy E: Context Mixing direct (sub_order 10) === */
    /* CM is slow (bit-by-bit) — only try for streams < 24 MB */
    if (n <= 25165824) {  /* 24 MB */
        uint8_t *cm_out = (uint8_t*)malloc(n + n/4 + 1024);
        int cm_sz = cm_encode(data, n, cm_out);
        if (cm_sz > 0 && cm_sz < best) {
            best = cm_sz; *sub_order = 10;
            memcpy(out, cm_out, cm_sz);
            fprintf(stderr, "      [Sub CM] %d -> %d (%.2fx) *** NEW BEST ***\n", n, cm_sz, (double)n/cm_sz);
        } else {
            fprintf(stderr, "      [Sub CM] %d -> %d (%.2fx) [not better]\n", n, cm_sz, (double)n/cm_sz);
        }
        free(cm_out);
    }


    /* === Strategy F: Word Preprocessing + BWT + O0/O1/O2 (sub_order 11-13) === */
    /* Format: [wp_encoded_size:4][pidx:4][nz:4][arith_data] */
    if (n >= 4096) {
        size_t wp_cap = (size_t)n + n/50 + 8192;
        uint8_t *wp_buf = (uint8_t*)malloc(wp_cap);
        if (wp_buf) {
            size_t wp_sz = wp_encode(data, n, wp_buf, wp_cap);
            if (wp_sz > 0 && (int)wp_sz < n) {
                fprintf(stderr, "      [Sub Word] %d -> %d (%.1f%% reduction)\n",
                        n, (int)wp_sz, (1.0 - (double)wp_sz/n) * 100);
                int wpn = (int)wp_sz;
                int wpa = wpn + wpn/4 + 4096;
                uint8_t  *wb = (uint8_t*)malloc(wpn);
                uint8_t  *wm = (uint8_t*)malloc(wpn);
                uint16_t *wz = (uint16_t*)malloc((size_t)wpn * 2 * sizeof(uint16_t));
                uint8_t  *wa = (uint8_t*)malloc(wpa);

                pidx = bwt_encode_ws(wp_buf, wpn, wb, bwt_ws);
                mtf_encode(wb, wpn, wm);
                nz = zrle_encode(wm, wpn, wz);

                for (int o = 0; o < 3; o++) {
                    if (o == 0) asize = arith_enc_o0(wz, nz, wa);
                    else if (o == 1) asize = arith_enc_o1(wz, nz, wa);
                    else asize = arith_enc_o2(wz, nz, wa);
                    total = 12 + asize;
                    if (total < best) {
                        best = total; *sub_order = 11 + o;
                        put_u32be(out, wpn);
                        put_u32be(out + 4, pidx);
                        put_u32be(out + 8, nz);
                        memcpy(out + 12, wa, asize);
                        fprintf(stderr, "      [Sub Word+BWT+O%d] *** NEW BEST: %d -> %d (%.2fx) ***\n",
                                o, n, total, (double)n/total);
                    }
                }
                /* Word+BWT+rANS O0 (sub_order 18) */
                asize = rans_enc_o0(wz, nz, wa);
                total = 12 + asize;
                if (total < best) {
                    best = total; *sub_order = 18;
                    put_u32be(out, wpn);
                    put_u32be(out + 4, pidx);
                    put_u32be(out + 8, nz);
                    memcpy(out + 12, wa, asize);
                    fprintf(stderr, "      [Sub Word+BWT+rANS] *** NEW BEST: %d -> %d (%.2fx) ***\n",
                            n, total, (double)n/total);
                }
                free(wb); free(wm); free(wz); free(wa);
            }
            free(wp_buf);
        }
    }

    /* === Strategy G: LZMA Optimal Parsing (sub_order 14) === */
    /* HyperPack v10.1: Smart LZMA heuristic for sub-streams too */
    {
        int try_sub_lzma = (n <= 64*1024*1024);
        if (try_sub_lzma) {
            double sub_ent = block_entropy(data, n);
            double sub_asc = ascii_ratio(data, n);
            if (sub_ent < 5.0 || (sub_asc > 0.95 && sub_ent < 6.0))
                try_sub_lzma = 0;
        }
        if (try_sub_lzma) {
        uint8_t *lzma_out = (uint8_t*)malloc(n + n/4 + 65536);
        if (lzma_out) {
            int lzma_sz = lzma_compress(data, n, lzma_out, best);
            if (lzma_sz > 0 && lzma_sz < best) {
                best = lzma_sz; *sub_order = 14;
                memcpy(out, lzma_out, lzma_sz);
                fprintf(stderr, "      [Sub LZMA] %d -> %d (%.2fx) *** NEW BEST ***\n", n, lzma_sz, (double)n/lzma_sz);
            } else {
                fprintf(stderr, "      [Sub LZMA] %d -> %d (%.2fx) [not better]\n", n, lzma_sz > 0 ? lzma_sz : n, lzma_sz > 0 ? (double)n/lzma_sz : 1.0);
            }
            free(lzma_out);
        }
        }
    }

    free(bwt_s); free(mtf_s); free(zrle_s); free(arith_s);
    return best;
}

/* Decompress sub-stream.
 * sub_order: 0-2 BWT+O0/O1/O2, 3 PPM, 4-6 D+BWT, 7-9 LZP+BWT, 10 CM, 11-13 Word+BWT, 14 LZMA */
static void decompress_substream(const uint8_t *cdata, int csize, int sub_order, int orig_n, uint8_t *out) {
    if (orig_n == 0) return;
    (void)csize;

    /* PPM direct */
    if (sub_order == 3) {
        ppm_decompress(cdata, out, orig_n);
        return;
    }

    /* CM direct */
    if (sub_order == 10) {
        cm_decode(cdata, out, orig_n);
        return;
    }

    /* LZMA direct */
    if (sub_order == 14) {
        lzma_decompress(cdata, out, orig_n);
        return;
    }

    /* Determine type and arith order
     * sub_order 15 = BWT+rANS O0
     * sub_order 16 = LZP+BWT+rANS O0
     * sub_order 17 = Delta+BWT+rANS O0
     * sub_order 18 = Word+BWT+rANS O0 */
    int is_rans  = (sub_order >= 15);
    int is_delta = (sub_order >= 4 && sub_order <= 6) || sub_order == 17;
    int is_lzp   = (sub_order >= 7 && sub_order <= 9) || sub_order == 16;
    int is_word  = (sub_order >= 11 && sub_order <= 13) || sub_order == 18;
    int arith_order = sub_order;
    if (is_delta) arith_order = (sub_order == 17) ? 0 : sub_order - 4;
    if (is_lzp)   arith_order = (sub_order == 16) ? 0 : sub_order - 7;
    if (is_word)  arith_order = (sub_order == 18) ? 0 : sub_order - 11;

    const uint8_t *p = cdata;
    int lzp_size = 0, wp_enc_size = 0;
    if (is_lzp) {
        lzp_size = get_u32be(p); p += 4;
    }
    if (is_word) {
        wp_enc_size = get_u32be(p); p += 4;
    }

    int pidx = get_u32be(p); p += 4;
    int nz   = get_u32be(p); p += 4;

    int bwt_n = is_lzp ? lzp_size : (is_word ? wp_enc_size : orig_n);

    uint16_t *zrle_s = (uint16_t*)malloc(nz * sizeof(uint16_t));
    uint8_t  *mtf_s  = (uint8_t*)malloc(bwt_n);
    uint8_t  *bwt_s  = (uint8_t*)malloc(bwt_n);

    if (is_rans)               rans_dec_o0(p, nz, zrle_s);
    else if (arith_order == 2) arith_dec_o2(p, nz, zrle_s);
    else if (arith_order == 1) arith_dec_o1(p, nz, zrle_s);
    else                       arith_dec_o0(p, nz, zrle_s);

    zrle_decode(zrle_s, nz, mtf_s, bwt_n);
    mtf_decode(mtf_s, bwt_n, bwt_s);

    if (is_word) {
        uint8_t *wp_tmp = (uint8_t*)malloc(bwt_n);
        bwt_decode(bwt_s, bwt_n, pidx, wp_tmp);
        wp_decode(wp_tmp, (size_t)bwt_n, out, (size_t)orig_n);
        free(wp_tmp);
    } else if (is_lzp) {
        uint8_t *lzp_tmp = (uint8_t*)malloc(bwt_n);
        bwt_decode(bwt_s, bwt_n, pidx, lzp_tmp);
        lzp_decode(lzp_tmp, bwt_n, out, orig_n);
        free(lzp_tmp);
    } else if (is_delta) {
        uint8_t *tmp = (uint8_t*)malloc(orig_n);
        bwt_decode(bwt_s, orig_n, pidx, tmp);
        delta_decode(tmp, orig_n, out);
        free(tmp);
    } else {
        bwt_decode(bwt_s, orig_n, pidx, out);
    }

    free(zrle_s); free(mtf_s); free(bwt_s);
}

/* Thread helper: decompress one substream in a background thread */
typedef struct { const uint8_t *cdata; int csize, order, orig_n; uint8_t *out; } SubstreamArg;
static void *decompress_substream_thread(void *arg) {
    SubstreamArg *a = (SubstreamArg*)arg;
    decompress_substream(a->cdata, a->csize, a->order, a->orig_n, a->out);
    return NULL;
}


/* ===================================================================
 * ===== LZMA Compression Engine =====
 * ===================================================================
 * LZMA-style compression with 12-state finite automaton,
 * hash-chain match finder, repeat distance slots, and
 * bit-level range coder with adaptive probabilities.
 */

/* ===== LZMA Constants ===== */
#define LZMA_NUM_STATES     12
#define LZMA_PB             2       /* position bits */
#define LZMA_LP             0       /* literal position bits */
#define LZMA_LC             3       /* literal context bits */
#define LZMA_POS_STATES     (1 << LZMA_PB)         /* 4 */
#define LZMA_POS_MASK       (LZMA_POS_STATES - 1)
#define LZMA_LIT_CTX        (1 << (LZMA_LP + LZMA_LC))  /* 8 */

#define LZMA_PROB_INIT      1024    /* 50% = 2048/2 */
#define LZMA_PROB_BITS      11
#define LZMA_MOVE_BITS      5

#define LZMA_MIN_MATCH      2
#define LZMA_MAX_MATCH      273     /* 2 + 8 + 8 + 255 */

/* Distance model */
#define LZMA_NUM_POS_SLOTS  64
#define LZMA_DIST_MODEL_START 4
#define LZMA_DIST_MODEL_END   14
#define LZMA_NUM_FULL_DISTS   (1 << (LZMA_DIST_MODEL_END >> 1))  /* 128 */
#define LZMA_ALIGN_BITS     4
#define LZMA_ALIGN_SIZE     (1 << LZMA_ALIGN_BITS)  /* 16 */
#define LZMA_NUM_LEN_STATES 4

/* Match finder */
/* Phase 5: Enlarged hash table for better match finding on binaries.
 * 20 bits (1M entries) → 22 bits (4M entries).
 * With a 64MB window, avg chain length drops from 64 to 16,
 * dramatically improving match quality especially with chain_max=32. */
#define LZMA_MF_HASH_BITS   22
#define LZMA_MF_HASH_SIZE   (1 << LZMA_MF_HASH_BITS)
#define LZMA_MF_HASH_MASK   (LZMA_MF_HASH_SIZE - 1)
#define LZMA_MF_CHAIN_MAX   128
#define LZMA_MF_WINDOW      (64 << 20)   /* 64 MB window (HyperPack v10.1) */

/* Optimal parsing */
#define OPT_AHEAD           4096
#define PRICE_BITS           6       /* fractional bits for prices */
#define INFINITY_PRICE       (1u << 28)

/* ===== State Transitions ===== */
static const uint8_t lzma_next_lit[12] =       {0,0,0,0,1,2,3,4,5,6,4,5};
static const uint8_t lzma_next_match[12] =     {7,7,7,7,7,7,7,10,10,10,10,10};
static const uint8_t lzma_next_rep[12] =       {8,8,8,8,8,8,8,11,11,11,11,11};
static const uint8_t lzma_next_short_rep[12] = {9,9,9,9,9,9,9,11,11,11,11,11};

#define LZMA_IS_LIT_STATE(s) ((s) < 7)

/* ===== Price Table ===== */
/* price_table[p] = round(-log2(p/2048) * (1<<PRICE_BITS)) for prob=p encoding bit 0 */
static uint32_t price_table_0[2048];  /* price of bit=0 given prob */
static uint32_t price_table_1[2048];  /* price of bit=1 given prob */
static int price_table_ready = 0;

static void init_price_tables(void) {
    if (price_table_ready) return;
    for (int i = 1; i < 2048; i++) {
        double p0 = (double)i / 2048.0;
        double p1 = 1.0 - p0;
        price_table_0[i] = (uint32_t)(-log2(p0) * (1 << PRICE_BITS) + 0.5);
        price_table_1[i] = (uint32_t)(-log2(p1) * (1 << PRICE_BITS) + 0.5);
    }
    price_table_0[0] = 128 << PRICE_BITS;  /* avoid infinity */
    price_table_1[0] = 0;
    price_table_ready = 1;
}

static inline uint32_t bit_price(uint16_t prob, int bit) {
    return bit ? price_table_1[prob] : price_table_0[prob];
}

static inline uint32_t bit0_price(uint16_t prob) { return price_table_0[prob]; }
static inline uint32_t bit1_price(uint16_t prob) { return price_table_1[prob]; }

/* ===== LZMA Range Coder — Encoder (with carry propagation) ===== */
typedef struct {
    uint32_t range;
    uint64_t low;
    uint8_t  *buf;
    int      pos;
    uint8_t  cache;
    uint32_t cache_size;
} LRCEnc;

static void lrc_enc_init(LRCEnc *e, uint8_t *buf) {
    e->range = 0xFFFFFFFFu;
    e->low = 0;
    e->buf = buf;
    e->pos = 0;
    e->cache = 0;
    e->cache_size = 1;
}

static void lrc_enc_shift(LRCEnc *e) {
    if ((uint32_t)(e->low) < 0xFF000000u || (e->low >> 32) != 0) {
        uint8_t temp = e->cache;
        do {
            e->buf[e->pos++] = (uint8_t)(temp + (uint8_t)(e->low >> 32));
            temp = 0xFF;
        } while (--e->cache_size != 0);
        e->cache = (uint8_t)((uint32_t)e->low >> 24);
    }
    e->cache_size++;
    e->low = (uint32_t)((uint32_t)e->low << 8);  /* 32-bit wrap discards extracted MSB */
}

static inline void lrc_enc_normalize(LRCEnc *e) {
    if (e->range < (1u << 24)) {
        e->range <<= 8;
        lrc_enc_shift(e);
    }
}

static void lrc_enc_bit(LRCEnc *e, uint16_t *prob, int bit) {
    uint32_t bound = (e->range >> LZMA_PROB_BITS) * (*prob);
    if (bit == 0) {
        e->range = bound;
        *prob += (uint16_t)((2048 - *prob) >> LZMA_MOVE_BITS);
    } else {
        e->low += bound;
        e->range -= bound;
        *prob -= (uint16_t)(*prob >> LZMA_MOVE_BITS);
    }
    lrc_enc_normalize(e);
}

static void lrc_enc_direct(LRCEnc *e, uint32_t val, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
        e->range >>= 1;
        if ((val >> i) & 1)
            e->low += e->range;
        lrc_enc_normalize(e);
    }
}

static int lrc_enc_finish(LRCEnc *e) {
    for (int i = 0; i < 5; i++)
        lrc_enc_shift(e);
    return e->pos;
}

/* Bit-tree encode: val in [0, 2^bits - 1] */
static void lrc_enc_tree(LRCEnc *e, uint16_t *probs, int bits, uint32_t val) {
    uint32_t m = 1;
    for (int i = bits - 1; i >= 0; i--) {
        int bit = (val >> i) & 1;
        lrc_enc_bit(e, &probs[m], bit);
        m = (m << 1) | bit;
    }
}

/* Reverse bit-tree encode */
static void lrc_enc_tree_rev(LRCEnc *e, uint16_t *probs, int bits, uint32_t val) {
    uint32_t m = 1;
    for (int i = 0; i < bits; i++) {
        int bit = val & 1;
        lrc_enc_bit(e, &probs[m], bit);
        m = (m << 1) | bit;
        val >>= 1;
    }
}

/* ===== LZMA Range Coder — Decoder ===== */
typedef struct {
    uint32_t range;
    uint32_t code;
    const uint8_t *buf;
    int pos;
} LRCDec;

static void lrc_dec_init(LRCDec *d, const uint8_t *buf) {
    d->range = 0xFFFFFFFFu;
    d->code = 0;
    d->buf = buf;
    d->pos = 0;
    /* Read 5 bytes: first byte is 0 (ignored), then 4 bytes of code */
    d->pos++;  /* skip leading byte */
    for (int i = 0; i < 4; i++)
        d->code = (d->code << 8) | d->buf[d->pos++];
}

static inline void lrc_dec_normalize(LRCDec *d) {
    if (d->range < (1u << 24)) {
        d->range <<= 8;
        d->code = (d->code << 8) | d->buf[d->pos++];
    }
}

static int lrc_dec_bit(LRCDec *d, uint16_t *prob) {
    uint32_t bound = (d->range >> LZMA_PROB_BITS) * (*prob);
    int bit;
    if (d->code < bound) {
        d->range = bound;
        *prob += (uint16_t)((2048 - *prob) >> LZMA_MOVE_BITS);
        bit = 0;
    } else {
        d->code -= bound;
        d->range -= bound;
        *prob -= (uint16_t)(*prob >> LZMA_MOVE_BITS);
        bit = 1;
    }
    lrc_dec_normalize(d);
    return bit;
}

static uint32_t lrc_dec_direct(LRCDec *d, int bits) {
    uint32_t val = 0;
    for (int i = 0; i < bits; i++) {
        d->range >>= 1;
        val <<= 1;
        if (d->code >= d->range) {
            d->code -= d->range;
            val |= 1;
        }
        lrc_dec_normalize(d);
    }
    return val;
}

/* Bit-tree decode */
static uint32_t lrc_dec_tree(LRCDec *d, uint16_t *probs, int bits) {
    uint32_t m = 1;
    for (int i = 0; i < bits; i++)
        m = (m << 1) | lrc_dec_bit(d, &probs[m]);
    return m - (1u << bits);
}

/* Reverse bit-tree decode */
static uint32_t lrc_dec_tree_rev(LRCDec *d, uint16_t *probs, int bits) {
    uint32_t m = 1, val = 0;
    for (int i = 0; i < bits; i++) {
        int bit = lrc_dec_bit(d, &probs[m]);
        m = (m << 1) | bit;
        val |= ((uint32_t)bit << i);
    }
    return val;
}

/* ===== LZMA Length Encoder/Decoder ===== */
typedef struct {
    uint16_t choice;     /* 0=low, 1=mid/high */
    uint16_t choice2;    /* 0=mid, 1=high */
    uint16_t low[LZMA_POS_STATES][8];    /* 3-bit tree, len 2-9 */
    uint16_t mid[LZMA_POS_STATES][8];    /* 3-bit tree, len 10-17 */
    uint16_t high[256];                   /* 8-bit tree, len 18-273 */
} LzmaLenModel;

static void lzma_len_init(LzmaLenModel *m) {
    m->choice = LZMA_PROB_INIT;
    m->choice2 = LZMA_PROB_INIT;
    for (int p = 0; p < LZMA_POS_STATES; p++)
        for (int i = 0; i < 8; i++) {
            m->low[p][i] = LZMA_PROB_INIT;
            m->mid[p][i] = LZMA_PROB_INIT;
        }
    for (int i = 0; i < 256; i++)
        m->high[i] = LZMA_PROB_INIT;
}

static void lzma_len_encode(LRCEnc *rc, LzmaLenModel *m, int len, int pos_state) {
    len -= LZMA_MIN_MATCH;  /* len now in [0, 271] */
    if (len < 8) {
        lrc_enc_bit(rc, &m->choice, 0);
        lrc_enc_tree(rc, m->low[pos_state], 3, len);
    } else if (len < 16) {
        lrc_enc_bit(rc, &m->choice, 1);
        lrc_enc_bit(rc, &m->choice2, 0);
        lrc_enc_tree(rc, m->mid[pos_state], 3, len - 8);
    } else {
        lrc_enc_bit(rc, &m->choice, 1);
        lrc_enc_bit(rc, &m->choice2, 1);
        lrc_enc_tree(rc, m->high, 8, len - 16);
    }
}

static int lzma_len_decode(LRCDec *rc, LzmaLenModel *m, int pos_state) {
    if (lrc_dec_bit(rc, &m->choice) == 0)
        return LZMA_MIN_MATCH + lrc_dec_tree(rc, m->low[pos_state], 3);
    if (lrc_dec_bit(rc, &m->choice2) == 0)
        return LZMA_MIN_MATCH + 8 + lrc_dec_tree(rc, m->mid[pos_state], 3);
    return LZMA_MIN_MATCH + 16 + lrc_dec_tree(rc, m->high, 8);
}

/* ===== Length Price ===== */
static uint32_t lzma_len_price(const LzmaLenModel *m, int len, int pos_state) {
    len -= LZMA_MIN_MATCH;
    if (len < 8) {
        uint32_t p = bit0_price(m->choice);
        uint32_t sym = 1;
        for (int i = 2; i >= 0; i--) {
            int bit = (len >> i) & 1;
            p += bit_price(m->low[pos_state][sym], bit);
            sym = (sym << 1) | bit;
        }
        return p;
    } else if (len < 16) {
        uint32_t p = bit1_price(m->choice) + bit0_price(m->choice2);
        uint32_t sym = 1;
        int v = len - 8;
        for (int i = 2; i >= 0; i--) {
            int bit = (v >> i) & 1;
            p += bit_price(m->mid[pos_state][sym], bit);
            sym = (sym << 1) | bit;
        }
        return p;
    } else {
        uint32_t p = bit1_price(m->choice) + bit1_price(m->choice2);
        uint32_t sym = 1;
        int v = len - 16;
        for (int i = 7; i >= 0; i--) {
            int bit = (v >> i) & 1;
            p += bit_price(m->high[sym], bit);
            sym = (sym << 1) | bit;
        }
        return p;
    }
}

/* ===== LZMA Probability State ===== */
typedef struct {
    uint16_t is_match[LZMA_NUM_STATES][LZMA_POS_STATES];
    uint16_t is_rep[LZMA_NUM_STATES];
    uint16_t is_rep_g0[LZMA_NUM_STATES];
    uint16_t is_rep_g1[LZMA_NUM_STATES];
    uint16_t is_rep_g2[LZMA_NUM_STATES];
    uint16_t is_rep0_long[LZMA_NUM_STATES][LZMA_POS_STATES];
    
    uint16_t lit_probs[LZMA_LIT_CTX * 0x300];  /* literal sub-coders */
    
    LzmaLenModel match_len;
    LzmaLenModel rep_len;
    
    uint16_t pos_slot[LZMA_NUM_LEN_STATES][64]; /* bit-tree, 6 bits */
    uint16_t pos_special[1 + LZMA_NUM_FULL_DISTS - LZMA_DIST_MODEL_END]; /* 115 */
    uint16_t pos_align[LZMA_ALIGN_SIZE];  /* 16 */
} LzmaProbs;

static void lzma_probs_init(LzmaProbs *p) {
    /* Set all probabilities to 50% (1024) */
    uint16_t *ptr = (uint16_t *)p;
    size_t count = sizeof(LzmaProbs) / sizeof(uint16_t);
    for (size_t i = 0; i < count; i++)
        ptr[i] = LZMA_PROB_INIT;
    /* Re-init len models properly (they have the same init value but let's be explicit) */
    lzma_len_init(&p->match_len);
    lzma_len_init(&p->rep_len);
}

/* ===== Position Slot Utilities ===== */
static int lzma_get_pos_slot(uint32_t dist) {
    if (dist < 4) return (int)dist;
    int bits = 0;
    uint32_t d = dist;
    while (d >= 2) { d >>= 1; bits++; }
    return (bits << 1) + (int)((dist >> (bits - 1)) & 1);
}

static inline int lzma_len_state(int len) {
    len -= LZMA_MIN_MATCH;
    if (len > 3) len = 3;
    return len;
}

/* ===== Literal Encoding/Decoding ===== */
static inline uint16_t* lzma_lit_probs(LzmaProbs *p, int pos, uint8_t prev_byte) {
    int lit_state = ((pos & ((1 << LZMA_LP) - 1)) << LZMA_LC) | (prev_byte >> (8 - LZMA_LC));
    return &p->lit_probs[lit_state * 0x300];
}

/* ===== Price computation for literals ===== */
static uint32_t lzma_literal_price_normal(const uint16_t *probs, uint8_t byte) {
    uint32_t p = 0;
    uint32_t sym = 1;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        p += bit_price(probs[sym], bit);
        sym = (sym << 1) | bit;
    }
    return p;
}

static uint32_t lzma_literal_price_matched(const uint16_t *probs, uint8_t byte, uint8_t match_byte) {
    uint32_t p = 0;
    uint32_t sym = 1;
    uint32_t offs = 0x100;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        int match_bit = (match_byte >> i) & 1;
        uint32_t prob_idx = offs + (match_bit << 8) + sym;
        p += bit_price(probs[prob_idx], bit);
        sym = (sym << 1) | bit;
        if (match_bit != bit) offs = 0;
    }
    return p;
}

static void lzma_lit_encode(LRCEnc *rc, uint16_t *probs, uint8_t byte) {
    uint32_t sym = 1;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        lrc_enc_bit(rc, &probs[sym], bit);
        sym = (sym << 1) | bit;
    }
}

static void lzma_lit_encode_matched(LRCEnc *rc, uint16_t *probs, uint8_t byte, uint8_t match_byte) {
    uint32_t sym = 1;
    uint32_t offs = 0x100;
    for (int i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        int match_bit = (match_byte >> i) & 1;
        uint32_t prob_idx = offs + (match_bit << 8) + sym;
        lrc_enc_bit(rc, &probs[prob_idx], bit);
        sym = (sym << 1) | bit;
        if (match_bit != bit)
            offs = 0;  /* stop using match context */
    }
}

static uint8_t lzma_lit_decode(LRCDec *rc, uint16_t *probs) {
    uint32_t sym = 1;
    for (int i = 0; i < 8; i++)
        sym = (sym << 1) | lrc_dec_bit(rc, &probs[sym]);
    return (uint8_t)(sym - 0x100);
}

static uint8_t lzma_lit_decode_matched(LRCDec *rc, uint16_t *probs, uint8_t match_byte) {
    uint32_t sym = 1;
    uint32_t offs = 0x100;
    for (int i = 7; i >= 0; i--) {
        int match_bit = (match_byte >> i) & 1;
        uint32_t prob_idx = offs + (match_bit << 8) + sym;
        int bit = lrc_dec_bit(rc, &probs[prob_idx]);
        sym = (sym << 1) | bit;
        if (match_bit != bit)
            offs = 0;
    }
    return (uint8_t)(sym - 0x100);
}

/* ===== Distance Encoding/Decoding ===== */
static void lzma_dist_encode(LRCEnc *rc, LzmaProbs *p, uint32_t dist, int len) {
    int slot = lzma_get_pos_slot(dist);
    int ls = lzma_len_state(len);
    lrc_enc_tree(rc, p->pos_slot[ls], 6, slot);
    
    if (slot >= LZMA_DIST_MODEL_START) {
        int footer_bits = (slot >> 1) - 1;
        uint32_t base = ((2 | (slot & 1)) << footer_bits);
        uint32_t low_bits = dist - base;
        
        if (slot < LZMA_DIST_MODEL_END) {
            /* Context-coded bits */
            lrc_enc_tree_rev(rc, p->pos_special + base - slot, footer_bits, low_bits);
        } else {
            /* Direct bits + align bits */
            lrc_enc_direct(rc, low_bits >> LZMA_ALIGN_BITS, footer_bits - LZMA_ALIGN_BITS);
            lrc_enc_tree_rev(rc, p->pos_align, LZMA_ALIGN_BITS, low_bits & (LZMA_ALIGN_SIZE - 1));
        }
    }
}

static uint32_t lzma_dist_decode(LRCDec *rc, LzmaProbs *p, int len) {
    int ls = lzma_len_state(len);
    int slot = (int)lrc_dec_tree(rc, p->pos_slot[ls], 6);
    
    if (slot < LZMA_DIST_MODEL_START)
        return (uint32_t)slot;
    
    int footer_bits = (slot >> 1) - 1;
    uint32_t base = ((2 | (slot & 1)) << footer_bits);
    uint32_t low_bits;
    
    if (slot < LZMA_DIST_MODEL_END) {
        low_bits = lrc_dec_tree_rev(rc, p->pos_special + base - slot, footer_bits);
    } else {
        low_bits = lrc_dec_direct(rc, footer_bits - LZMA_ALIGN_BITS) << LZMA_ALIGN_BITS;
        low_bits |= lrc_dec_tree_rev(rc, p->pos_align, LZMA_ALIGN_BITS);
    }
    return base + low_bits;
}

/* ===== Distance price ===== */
static uint32_t lzma_dist_price(const LzmaProbs *p, uint32_t dist, int len) {
    int slot = lzma_get_pos_slot(dist);
    int ls = lzma_len_state(len);

    /* pos_slot tree price */
    uint32_t price = 0;
    uint32_t sym = 1;
    for (int i = 5; i >= 0; i--) {
        int bit = (slot >> i) & 1;
        price += bit_price(p->pos_slot[ls][sym], bit);
        sym = (sym << 1) | bit;
    }

    if (slot >= LZMA_DIST_MODEL_START) {
        int footer_bits = (slot >> 1) - 1;
        uint32_t base = ((2 | (slot & 1)) << footer_bits);
        uint32_t low_bits = dist - base;

        if (slot < LZMA_DIST_MODEL_END) {
            /* Context-coded bits (reverse tree) */
            const uint16_t *spec = p->pos_special + base - slot;
            uint32_t m = 1, v = low_bits;
            for (int i = 0; i < footer_bits; i++) {
                int bit = v & 1;
                price += bit_price(spec[m], bit);
                m = (m << 1) | bit;
                v >>= 1;
            }
        } else {
            /* Direct bits (flat 0.5 probability = 1 bit each) */
            price += (uint32_t)(footer_bits - LZMA_ALIGN_BITS) * (1 << PRICE_BITS);
            /* Align bits (reverse tree) */
            uint32_t align_val = low_bits & (LZMA_ALIGN_SIZE - 1);
            uint32_t m = 1, v = align_val;
            for (int i = 0; i < LZMA_ALIGN_BITS; i++) {
                int bit = v & 1;
                price += bit_price(p->pos_align[m], bit);
                m = (m << 1) | bit;
                v >>= 1;
            }
        }
    }
    return price;
}

/* ===== Match Finder (Hash Chain) ===== */
/* Hash sizes for short-length match boosting.
 * hash2: 2^16 = 65536 entries for all 2-byte pairs (exact, no collisions for len=2)
 * hash3: 2^16 = 65536 entries for 3-byte combinations (may collide, still helps)
 * These let the match finder reliably find length-2 and length-3 matches that the
 * 4-byte hash chain misses; critical for code-heavy data (C tokens, x86 opcodes). */
#define LZMA_HASH2_SIZE  (1 << 16)
#define LZMA_HASH2_MASK  (LZMA_HASH2_SIZE - 1)
#define LZMA_HASH3_BITS  16
#define LZMA_HASH3_SIZE  (1 << LZMA_HASH3_BITS)
#define LZMA_HASH3_MASK  (LZMA_HASH3_SIZE - 1)

typedef struct {
    const uint8_t *data;
    int size;
    int pos;
    int32_t *head;    /* hash4 -> last position */
    int32_t *chain;   /* circular chain buffer */
    int32_t *head2;   /* hash2 -> last position (2-byte matches) */
    int32_t *head3;   /* hash3 -> last position (3-byte matches) */
    int window_size;
    int chain_max;    /* Phase 3: configurable search depth */
    int nice_len;     /* stop chain search early when match >= nice_len */
} LzmaMF;

static inline uint32_t lzma_mf_hash4(const uint8_t *p) {
    uint32_t v = p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (v * 2654435761u) >> (32 - LZMA_MF_HASH_BITS);
}
static inline uint32_t lzma_mf_hash2(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);  /* exact 2-byte pair index */
}
static inline uint32_t lzma_mf_hash3(const uint8_t *p) {
    uint32_t v = p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    return (v * 2654435761u) >> (32 - LZMA_HASH3_BITS);
}

static void lzma_mf_init(LzmaMF *mf, const uint8_t *data, int size, int window_size, int chain_max, int nice_len) {
    mf->data = data;
    mf->size = size;
    mf->pos = 0;
    mf->window_size = window_size;
    mf->nice_len = nice_len;
    mf->chain_max = chain_max;
    mf->head  = (int32_t *)malloc(LZMA_MF_HASH_SIZE * sizeof(int32_t));
    mf->chain = (int32_t *)malloc(window_size * sizeof(int32_t));
    mf->head2 = (int32_t *)malloc(LZMA_HASH2_SIZE * sizeof(int32_t));
    mf->head3 = (int32_t *)malloc(LZMA_HASH3_SIZE * sizeof(int32_t));
    memset(mf->head,  -1, LZMA_MF_HASH_SIZE * sizeof(int32_t));
    memset(mf->chain, -1, window_size * sizeof(int32_t));
    memset(mf->head2, -1, LZMA_HASH2_SIZE * sizeof(int32_t));
    memset(mf->head3, -1, LZMA_HASH3_SIZE * sizeof(int32_t));
}

static void lzma_mf_free(LzmaMF *mf) {
    free(mf->head);
    free(mf->chain);
    free(mf->head2);
    free(mf->head3);
}

/* Match result: array of (dist, len) pairs. Returns count. */
typedef struct { uint32_t dist; int len; } MatchPair;

/* Find ALL matches at position pos. Returns them sorted by length ascending.
   Also check rep distances. max_pairs is the capacity of pairs[]. */
static int lzma_mf_find_all(LzmaMF *mf, int pos, uint32_t *reps,
                             MatchPair *pairs, int max_pairs, int chain_limit) {
    const uint8_t *data = mf->data;
    int size = mf->size;
    int np = 0;
    int best_len = 1;
    int max_len = size - pos;
    if (max_len > LZMA_MAX_MATCH) max_len = LZMA_MAX_MATCH;
    if (max_len < LZMA_MIN_MATCH) return 0;

    /* Check repeat distances first */
    for (int r = 0; r < 4; r++) {
        int ref = pos - (int)reps[r] - 1;
        if (ref < 0) continue;
        int len = 0;
        while (len < max_len && data[ref + len] == data[pos + len]) len++;
        if (len >= LZMA_MIN_MATCH && len > best_len) {
            best_len = len;
            if (np < max_pairs) {
                pairs[np].dist = reps[r];
                pairs[np].len = len;
                np++;
            }
        }
    }

    /* Hash2 search — exact 2-byte match (single lookup, no chain) */
    if (max_len >= 2 && pos >= 1) {
        uint32_t h2 = lzma_mf_hash2(data + pos);
        int ref2 = mf->head2[h2];
        if (ref2 >= 0 && (pos - ref2) <= mf->window_size &&
            data[ref2] == data[pos] && data[ref2 + 1] == data[pos + 1]) {
            int len = 2;
            while (len < max_len && data[ref2 + len] == data[pos + len]) len++;
            if (len >= LZMA_MIN_MATCH && len > best_len) {
                best_len = len;
                if (np < max_pairs) {
                    pairs[np].dist = (uint32_t)(pos - ref2 - 1);
                    pairs[np].len = len;
                    np++;
                }
            }
        }
    }

    /* Hash3 search — 3-byte match (single lookup) */
    if (max_len >= 3 && pos >= 2) {
        uint32_t h3 = lzma_mf_hash3(data + pos);
        int ref3 = mf->head3[h3];
        if (ref3 >= 0 && (pos - ref3) <= mf->window_size &&
            data[ref3] == data[pos] && data[ref3 + 1] == data[pos + 1] &&
            data[ref3 + 2] == data[pos + 2]) {
            int len = 3;
            while (len < max_len && data[ref3 + len] == data[pos + len]) len++;
            if (len >= LZMA_MIN_MATCH && len > best_len) {
                best_len = len;
                if (np < max_pairs) {
                    pairs[np].dist = (uint32_t)(pos - ref3 - 1);
                    pairs[np].len = len;
                    np++;
                }
            }
        }
    }

    /* Hash4 chain search — collect matches of increasing length */
    if (pos + 3 < size) {
        uint32_t h = lzma_mf_hash4(data + pos);
        int chain_pos = mf->head[h];
        int tries = 0;
        int max_dist = (pos < mf->window_size) ? pos : mf->window_size;

        while (chain_pos >= 0 && (pos - chain_pos) <= max_dist && tries < chain_limit) {
            if (data[chain_pos + best_len] == data[pos + best_len]) {
                int len = 0;
                while (len < max_len && data[chain_pos + len] == data[pos + len]) len++;
                if (len >= LZMA_MIN_MATCH && len > best_len) {
                    best_len = len;
                    if (np < max_pairs) {
                        pairs[np].dist = (uint32_t)(pos - chain_pos - 1);
                        pairs[np].len = len;
                        np++;
                    }
                    if (len >= max_len) break;
                    if (len >= mf->nice_len) break; /* good enough — stop early */
                }
            }
            chain_pos = mf->chain[chain_pos % mf->window_size];
            tries++;
        }
    }

    return np;
}

/* Update hash chain for position pos */
static void lzma_mf_update(LzmaMF *mf, int pos) {
    const uint8_t *p = mf->data + pos;
    if (pos + 1 < mf->size) {
        mf->head2[lzma_mf_hash2(p)] = pos;
    }
    if (pos + 2 < mf->size) {
        mf->head3[lzma_mf_hash3(p)] = pos;
    }
    if (pos + 3 < mf->size) {
        uint32_t h = lzma_mf_hash4(p);
        mf->chain[pos % mf->window_size] = mf->head[h];
        mf->head[h] = pos;
    }
}

/* ===== Optimal Parsing Node ===== */
typedef struct {
    uint32_t price;
    int state;
    uint32_t reps[4];
    uint8_t prev_byte;

    /* How we got here: */
    int back_len;    /* 0=literal, 1=shortrep, >=2=match/rep-match */
    int back_dist;   /* -1=literal/shortrep, 0-3=rep_idx (for rep match), >=4=dist+4 (normal match) */
    int back_rep;    /* 1 if this is a rep/shortrep, 0 if normal match or literal */
} OptNode;

/* ===== SA-LZMA: Suffix Array long-match supplement =====
 *
 * For blocks <= SA_LZMA_THRESHOLD (8MB), builds a suffix array + ISA + LCP
 * using the existing sais_core(). During LZMA DP, supplements hash chain by
 * finding the globally best long match via SA neighborhood scan.
 *
 * Only adds a match to pairs[] if it is strictly longer than the best
 * match the hash chain already found.
 *
 * Memory: 3 * n * 4 bytes (SA + ISA + LCP). For n=8MB: 96MB.
 */
#define SA_LZMA_THRESHOLD (8 * 1024 * 1024)
#define SA_SCAN_LIMIT 64  /* max neighbors to scan left/right in SA */

typedef struct {
    int32_t *sa;
    int32_t *isa;
    int32_t *lcp;
    const uint8_t *data;
    int n;
} SaCtx;

static SaCtx *sa_ctx_build(const uint8_t *data, int n) {
    SaCtx *ctx = (SaCtx *)malloc(sizeof(SaCtx));
    if (!ctx) return NULL;
    ctx->data = data;
    ctx->n = n;

    ctx->sa  = (int32_t *)malloc((size_t)(n + 1) * sizeof(int32_t));
    ctx->isa = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    ctx->lcp = (int32_t *)malloc((size_t)(n + 1) * sizeof(int32_t));
    if (!ctx->sa || !ctx->isa || !ctx->lcp) {
        free(ctx->sa); free(ctx->isa); free(ctx->lcp); free(ctx);
        return NULL;
    }

    /* Build integer array for sais_core: T[i] = data[i], T[n] = 0 (sentinel) */
    int32_t *T = (int32_t *)malloc((size_t)(n + 1) * sizeof(int32_t));
    if (!T) { free(ctx->sa); free(ctx->isa); free(ctx->lcp); free(ctx); return NULL; }
    for (int i = 0; i < n; i++) T[i] = (int32_t)data[i] + 1; /* shift: 1..256 */
    T[n] = 0; /* sentinel must be 0 and unique minimum */

    sais_core(T, ctx->sa, n + 1, 257); /* alphabet: 0..256 (0=sentinel, 1..256=bytes+1) */
    free(T);

    /* Build ISA (inverse SA) */
    for (int i = 0; i <= n; i++) {
        if (ctx->sa[i] < n) ctx->isa[ctx->sa[i]] = i;
    }

    /* Build LCP array using Kasai's algorithm */
    memset(ctx->lcp, 0, (size_t)(n + 1) * sizeof(int32_t));
    int h = 0;
    for (int i = 0; i < n; i++) {
        int rank = ctx->isa[i];
        if (rank > 0) {
            int j = ctx->sa[rank - 1];
            if (j < n) {
                while (i + h < n && j + h < n && data[i + h] == data[j + h]) h++;
                ctx->lcp[rank] = h;
            }
            if (h > 0) h--;
        }
    }
    return ctx;
}

static void sa_ctx_free(SaCtx *ctx) {
    if (!ctx) return;
    free(ctx->sa); free(ctx->isa); free(ctx->lcp); free(ctx);
}

/* Find best match at position pos using SA neighborhood.
 * Returns match length > min_len, or 0 if nothing better found.
 * out_dist: distance (1-based, i.e., pos - match_start) */
static int sa_find_best_match(const SaCtx *ctx, int pos, int max_dist,
                               int min_len, int *out_dist) {
    const uint8_t *data = ctx->data;
    int n = ctx->n;
    int remaining = n - pos;
    if (remaining <= min_len) return 0;
    int max_match = remaining;
    if (max_match > LZMA_MAX_MATCH) max_match = LZMA_MAX_MATCH;

    int rank = ctx->isa[pos];
    int best_len = min_len;
    int best_dist = 0;

    /* Scan left in SA */
    int min_lcp = max_match;
    for (int k = rank - 1; k >= 0 && (rank - k) <= SA_SCAN_LIMIT; k--) {
        if (ctx->lcp[k + 1] < min_lcp) min_lcp = ctx->lcp[k + 1];
        if (min_lcp <= best_len) break;
        int ref = ctx->sa[k];
        if (ref >= n) continue; /* sentinel position */
        int dist = pos - ref;
        if (dist <= 0 || dist > max_dist) continue;
        /* Verify actual match length (min_lcp is a lower bound) */
        int len = min_lcp;
        while (len < max_match && data[pos + len] == data[ref + len]) len++;
        if (len > best_len) { best_len = len; best_dist = dist; }
        if (best_len == max_match) break;
    }

    /* Scan right in SA */
    min_lcp = max_match;
    for (int k = rank + 1; k <= n && (k - rank) <= SA_SCAN_LIMIT; k++) {
        if (ctx->lcp[k] < min_lcp) min_lcp = ctx->lcp[k];
        if (min_lcp <= best_len) break;
        int ref = ctx->sa[k];
        if (ref >= n) continue;
        int dist = pos - ref;
        if (dist <= 0 || dist > max_dist) continue;
        int len = min_lcp;
        while (len < max_match && data[pos + len] == data[ref + len]) len++;
        if (len > best_len) { best_len = len; best_dist = dist; }
        if (best_len == max_match) break;
    }

    if (best_len > min_len && best_dist > 0) {
        *out_dist = best_dist;
        return best_len;
    }
    return 0;
}

/* ===== LZMA Encoder with Optimal Parsing ===== */
static int lzma_compress(const uint8_t *data, int n, uint8_t *out, int size_limit) {
    if (n == 0) return 0;

    init_price_tables();

    LzmaProbs probs;
    lzma_probs_init(&probs);

    /* We need two copies of probs: one for pricing (read-only during DP), one for encoding */
    LzmaProbs enc_probs;
    lzma_probs_init(&enc_probs);

    int window_size = LZMA_MF_WINDOW;
    if (window_size > n) window_size = n;

    /* Phase 3: Reduce chain depth when competing against existing result.
     * Full depth (128) when no limit; fast depth (32) when speculative. */
    int chain_depth = (size_limit > 0) ? 32 : LZMA_MF_CHAIN_MAX;
    int nice_len = LZMA_MAX_MATCH; /* no early exit by default */

    LzmaMF mf;
    lzma_mf_init(&mf, data, n, window_size, chain_depth, nice_len);

    LRCEnc rc;
    lrc_enc_init(&rc, out);

    int state = 0;
    uint32_t reps[4] = {1, 1, 1, 1};
    uint8_t prev_byte = 0;
    int pos = 0;

    /* Build SA for small blocks to supplement hash chain with globally best matches */
    SaCtx *sa_ctx = NULL;
    if (n <= SA_LZMA_THRESHOLD) sa_ctx = sa_ctx_build(data, n);

    /* Allocate optimal parsing array */
    OptNode *opt = (OptNode *)malloc((OPT_AHEAD + LZMA_MAX_MATCH + 1) * sizeof(OptNode));
    /* +1 slot reserved for SA match supplement */
    MatchPair *pairs = (MatchPair *)malloc((LZMA_MAX_MATCH + 1) * sizeof(MatchPair));

    /* Decision buffer for emitting */
    int *dec_lens = (int *)malloc((OPT_AHEAD + LZMA_MAX_MATCH + 1) * sizeof(int));
    int *dec_dists = (int *)malloc((OPT_AHEAD + LZMA_MAX_MATCH + 1) * sizeof(int));
    int *dec_reps_flag = (int *)malloc((OPT_AHEAD + LZMA_MAX_MATCH + 1) * sizeof(int));

    /* Price tables: precomputed once per DP round (4×272 = ~4KB, fits in L1 cache).
       Eliminates 2.2B repeated lzma_len_price() calls in the inner DP loops. */
    uint32_t rep_ptable[LZMA_POS_STATES][LZMA_MAX_MATCH - LZMA_MIN_MATCH + 1];
    uint32_t match_ptable[LZMA_POS_STATES][LZMA_MAX_MATCH - LZMA_MIN_MATCH + 1];

    while (pos < n) {
        /* === Phase 1: Forward DP pricing === */
        int ahead = n - pos;
        if (ahead > OPT_AHEAD) ahead = OPT_AHEAD;

        /* Refresh price tables for this DP round (probabilities change as symbols are emitted) */
        for (int ps = 0; ps < LZMA_POS_STATES; ps++)
            for (int l = LZMA_MIN_MATCH; l <= LZMA_MAX_MATCH; l++) {
                rep_ptable[ps][l - LZMA_MIN_MATCH]   = lzma_len_price(&probs.rep_len,   l, ps);
                match_ptable[ps][l - LZMA_MIN_MATCH] = lzma_len_price(&probs.match_len, l, ps);
            }

        /* Initialize node 0 */
        opt[0].price = 0;
        opt[0].state = state;
        memcpy(opt[0].reps, reps, sizeof(reps));
        opt[0].prev_byte = prev_byte;

        /* Initialize all other nodes to infinity */
        for (int i = 1; i <= ahead + LZMA_MAX_MATCH && i <= n - pos; i++)
            opt[i].price = INFINITY_PRICE;

        int last_opt = 0;  /* furthest position reached in the DP */

        for (int cur = 0; cur < ahead && cur <= last_opt + 1; cur++) {
            int abs_pos = pos + cur;
            int remaining = n - abs_pos;
            if (remaining <= 0) break;

            /* Always update hash for unreachable positions so future searches work */
            if (opt[cur].price >= INFINITY_PRICE) {
                lzma_mf_update(&mf, abs_pos);
                continue;
            }

            int cur_state = opt[cur].state;
            int pos_state = abs_pos & LZMA_POS_MASK;
            uint32_t cur_reps[4];
            memcpy(cur_reps, opt[cur].reps, sizeof(cur_reps));
            uint8_t cur_prev = opt[cur].prev_byte;

            /* === Try literal === */
            {
                uint32_t price = opt[cur].price;
                price += bit0_price(probs.is_match[cur_state][pos_state]);

                uint16_t *lit_p = lzma_lit_probs(&probs, abs_pos, cur_prev);
                if (LZMA_IS_LIT_STATE(cur_state)) {
                    price += lzma_literal_price_normal(lit_p, data[abs_pos]);
                } else {
                    int ref = abs_pos - (int)cur_reps[0] - 1;
                    uint8_t mb = (ref >= 0) ? data[ref] : 0;
                    price += lzma_literal_price_matched(lit_p, data[abs_pos], mb);
                }

                int next = cur + 1;
                if (next <= (int)(n - pos) && price < opt[next].price) {
                    opt[next].price = price;
                    opt[next].state = lzma_next_lit[cur_state];
                    memcpy(opt[next].reps, cur_reps, sizeof(cur_reps));
                    opt[next].prev_byte = data[abs_pos];
                    opt[next].back_len = 0;
                    opt[next].back_dist = -1;
                    opt[next].back_rep = 0;
                    if (next > last_opt) last_opt = next;
                }
            }

            /* === Try short rep (rep0, length 1) === */
            {
                int ref = abs_pos - (int)cur_reps[0] - 1;
                if (ref >= 0 && data[abs_pos] == data[ref]) {
                    uint32_t price = opt[cur].price;
                    price += bit1_price(probs.is_match[cur_state][pos_state]);
                    price += bit1_price(probs.is_rep[cur_state]);
                    price += bit0_price(probs.is_rep_g0[cur_state]);
                    price += bit0_price(probs.is_rep0_long[cur_state][pos_state]);

                    int next = cur + 1;
                    if (next <= (int)(n - pos) && price < opt[next].price) {
                        opt[next].price = price;
                        opt[next].state = lzma_next_short_rep[cur_state];
                        memcpy(opt[next].reps, cur_reps, sizeof(cur_reps));
                        opt[next].prev_byte = data[abs_pos];
                        opt[next].back_len = 1;
                        opt[next].back_dist = 0;  /* rep0 */
                        opt[next].back_rep = 1;
                        if (next > last_opt) last_opt = next;
                    }
                }
            }

            /* === Try rep matches (all 4 reps, all lengths) === */
            {
                uint32_t match_price = opt[cur].price
                    + bit1_price(probs.is_match[cur_state][pos_state])
                    + bit1_price(probs.is_rep[cur_state]);

                for (int r = 0; r < 4; r++) {
                    int ref = abs_pos - (int)cur_reps[r] - 1;
                    if (ref < 0) continue;

                    /* Find match length with this rep */
                    int max_len = remaining;
                    if (max_len > LZMA_MAX_MATCH) max_len = LZMA_MAX_MATCH;
                    int len = 0;
                    while (len < max_len && data[ref + len] == data[abs_pos + len]) len++;
                    if (len < LZMA_MIN_MATCH) continue;

                    /* Rep selection price */
                    uint32_t rep_price = match_price;
                    if (r == 0) {
                        rep_price += bit0_price(probs.is_rep_g0[cur_state]);
                        rep_price += bit1_price(probs.is_rep0_long[cur_state][pos_state]);
                    } else if (r == 1) {
                        rep_price += bit1_price(probs.is_rep_g0[cur_state]);
                        rep_price += bit0_price(probs.is_rep_g1[cur_state]);
                    } else if (r == 2) {
                        rep_price += bit1_price(probs.is_rep_g0[cur_state]);
                        rep_price += bit1_price(probs.is_rep_g1[cur_state]);
                        rep_price += bit0_price(probs.is_rep_g2[cur_state]);
                    } else {
                        rep_price += bit1_price(probs.is_rep_g0[cur_state]);
                        rep_price += bit1_price(probs.is_rep_g1[cur_state]);
                        rep_price += bit1_price(probs.is_rep_g2[cur_state]);
                    }

                    /* Try all lengths */
                    for (int l = LZMA_MIN_MATCH; l <= len; l++) {
                        uint32_t price = rep_price + rep_ptable[pos_state][l - LZMA_MIN_MATCH];
                        int next = cur + l;
                        if (next <= (int)(n - pos) && price < opt[next].price) {
                            opt[next].price = price;
                            opt[next].state = lzma_next_rep[cur_state];
                            /* Update reps for this decision */
                            if (r == 0) {
                                memcpy(opt[next].reps, cur_reps, sizeof(cur_reps));
                            } else {
                                opt[next].reps[0] = cur_reps[r];
                                int j = 1;
                                for (int k = 0; k < 4; k++) {
                                    if (k != r) opt[next].reps[j++] = cur_reps[k];
                                }
                            }
                            opt[next].prev_byte = data[abs_pos + l - 1];
                            opt[next].back_len = l;
                            opt[next].back_dist = r;  /* 0-3 = rep index */
                            opt[next].back_rep = 1;
                            if (next > last_opt) last_opt = next;
                        }
                    }
                }
            }

            /* === Try normal matches from hash chain === */
            /* IMPORTANT: find matches BEFORE updating hash to avoid self-match */
            {
                /* Interior DP positions (cur > 0) use a reduced chain depth:
                 * they only need to find matches competitive with the already-known
                 * cur=0 path, so deep search is wasteful. cur=0 always uses full depth.
                 * For large blocks (> 4MB), reduce to 8 to limit O(n*last_opt) cost.
                 * Small blocks keep full depth — the extra cost is negligible. */
                int interior_depth = (n > 4 * 1024 * 1024) ? 8 : mf.chain_max;
                int cur_chain = (cur == 0) ? mf.chain_max : interior_depth;
                int np = lzma_mf_find_all(&mf, abs_pos, cur_reps, pairs, LZMA_MAX_MATCH, cur_chain);

                /* SA supplement: for small blocks, find globally best long match */
                if (sa_ctx && abs_pos < sa_ctx->n) {
                    int best_chain_len = (np > 0) ? pairs[np - 1].len : (LZMA_MIN_MATCH - 1);
                    int sa_dist = 0;
                    int sa_len = sa_find_best_match(sa_ctx, abs_pos,
                                                     LZMA_MF_WINDOW, best_chain_len, &sa_dist);
                    if (sa_len > best_chain_len) {
                        pairs[np].len  = sa_len;
                        /* sa_dist is 1-based (pos - ref); LZMA pairs use 0-based (pos - ref - 1) */
                        pairs[np].dist = (uint32_t)(sa_dist - 1);
                        np++;
                    }
                }

                /* NOW update hash for this position (after searching) */
                lzma_mf_update(&mf, abs_pos);

                uint32_t normal_price = opt[cur].price
                    + bit1_price(probs.is_match[cur_state][pos_state])
                    + bit0_price(probs.is_rep[cur_state]);

                for (int m = 0; m < np; m++) {
                    uint32_t dist = pairs[m].dist;
                    int mlen = pairs[m].len;

                    /* Skip if this is actually a rep distance */
                    int is_rep = 0;
                    for (int r = 0; r < 4; r++) {
                        if (dist == cur_reps[r]) { is_rep = 1; break; }
                    }
                    if (is_rep) continue;

                    /* Try all lengths from LZMA_MIN_MATCH to mlen for true optimality.
                       For very long matches (>32), sample to keep DP tractable. */
                    for (int l = LZMA_MIN_MATCH; l <= mlen; l++) {
                        /* For long matches, skip intermediate lengths to save time */
                        if (l > 32 && l < mlen && (l & 7) != 0) continue;

                        uint32_t price = normal_price;
                        price += match_ptable[pos_state][l - LZMA_MIN_MATCH];
                        price += lzma_dist_price(&probs, dist, l);

                        int next = cur + l;
                        if (next <= (int)(n - pos) && price < opt[next].price) {
                            opt[next].price = price;
                            opt[next].state = lzma_next_match[cur_state];
                            opt[next].reps[0] = dist;
                            opt[next].reps[1] = cur_reps[0];
                            opt[next].reps[2] = cur_reps[1];
                            opt[next].reps[3] = cur_reps[2];
                            opt[next].prev_byte = data[abs_pos + l - 1];
                            opt[next].back_len = l;
                            opt[next].back_dist = (int)dist + 4;  /* >=4 means normal match, dist = back_dist-4 */
                            opt[next].back_rep = 0;
                            if (next > last_opt) last_opt = next;
                        }
                    }

                }
            }
        }

        /* If DP didn't produce any path, emit a literal manually */
        if (last_opt == 0) {
            last_opt = 1;
            if (opt[1].price >= INFINITY_PRICE) {
                /* Force literal */
                opt[1].price = 0;
                opt[1].state = lzma_next_lit[state];
                memcpy(opt[1].reps, reps, sizeof(reps));
                opt[1].prev_byte = data[pos];
                opt[1].back_len = 0;
                opt[1].back_dist = -1;
                opt[1].back_rep = 0;
                lzma_mf_update(&mf, pos);
            }
        }

        /* === Phase 2: Backtrack to find the optimal sequence === */
        int num_decisions = 0;
        int bp = last_opt;
        while (bp > 0) {
            int len = opt[bp].back_len;
            if (len == 0) len = 1;  /* literal */
            dec_lens[num_decisions] = opt[bp].back_len;
            dec_dists[num_decisions] = opt[bp].back_dist;
            dec_reps_flag[num_decisions] = opt[bp].back_rep;
            num_decisions++;
            bp -= len;
        }

        /* === Phase 3: Emit decisions in forward order (using enc_probs) === */
        int emit_pos = pos;
        for (int d = num_decisions - 1; d >= 0; d--) {
            int pos_st = emit_pos & LZMA_POS_MASK;
            int blen = dec_lens[d];
            int bdist = dec_dists[d];
            int brep = dec_reps_flag[d];

            if (blen == 0) {
                /* Literal */
                lrc_enc_bit(&rc, &enc_probs.is_match[state][pos_st], 0);
                uint16_t *lit_p = lzma_lit_probs(&enc_probs, emit_pos, prev_byte);
                if (LZMA_IS_LIT_STATE(state)) {
                    lzma_lit_encode(&rc, lit_p, data[emit_pos]);
                } else {
                    int ref = emit_pos - (int)reps[0] - 1;
                    uint8_t mb = (ref >= 0) ? data[ref] : 0;
                    lzma_lit_encode_matched(&rc, lit_p, data[emit_pos], mb);
                }
                state = lzma_next_lit[state];
                prev_byte = data[emit_pos];
                emit_pos++;
            } else if (blen == 1 && brep && bdist == 0) {
                /* Short rep */
                lrc_enc_bit(&rc, &enc_probs.is_match[state][pos_st], 1);
                lrc_enc_bit(&rc, &enc_probs.is_rep[state], 1);
                lrc_enc_bit(&rc, &enc_probs.is_rep_g0[state], 0);
                lrc_enc_bit(&rc, &enc_probs.is_rep0_long[state][pos_st], 0);
                state = lzma_next_short_rep[state];
                prev_byte = data[emit_pos];
                emit_pos++;
            } else if (brep) {
                /* Rep match */
                int rep_idx = bdist;  /* 0-3 */
                lrc_enc_bit(&rc, &enc_probs.is_match[state][pos_st], 1);
                lrc_enc_bit(&rc, &enc_probs.is_rep[state], 1);

                if (rep_idx == 0) {
                    lrc_enc_bit(&rc, &enc_probs.is_rep_g0[state], 0);
                    lrc_enc_bit(&rc, &enc_probs.is_rep0_long[state][pos_st], 1);
                } else if (rep_idx == 1) {
                    lrc_enc_bit(&rc, &enc_probs.is_rep_g0[state], 1);
                    lrc_enc_bit(&rc, &enc_probs.is_rep_g1[state], 0);
                } else if (rep_idx == 2) {
                    lrc_enc_bit(&rc, &enc_probs.is_rep_g0[state], 1);
                    lrc_enc_bit(&rc, &enc_probs.is_rep_g1[state], 1);
                    lrc_enc_bit(&rc, &enc_probs.is_rep_g2[state], 0);
                } else {
                    lrc_enc_bit(&rc, &enc_probs.is_rep_g0[state], 1);
                    lrc_enc_bit(&rc, &enc_probs.is_rep_g1[state], 1);
                    lrc_enc_bit(&rc, &enc_probs.is_rep_g2[state], 1);
                }

                lzma_len_encode(&rc, &enc_probs.rep_len, blen, pos_st);

                /* Rotate reps */
                if (rep_idx > 0) {
                    uint32_t dist = reps[rep_idx];
                    for (int j = rep_idx; j > 0; j--) reps[j] = reps[j-1];
                    reps[0] = dist;
                }
                state = lzma_next_rep[state];

                /* Update hash for skipped positions */
                for (int j = 1; j < blen; j++)
                    lzma_mf_update(&mf, emit_pos + j);

                prev_byte = data[emit_pos + blen - 1];
                emit_pos += blen;
            } else {
                /* Normal match */
                uint32_t dist = (uint32_t)(bdist - 4);
                lrc_enc_bit(&rc, &enc_probs.is_match[state][pos_st], 1);
                lrc_enc_bit(&rc, &enc_probs.is_rep[state], 0);
                lzma_len_encode(&rc, &enc_probs.match_len, blen, pos_st);
                lzma_dist_encode(&rc, &enc_probs, dist, blen);

                reps[3] = reps[2]; reps[2] = reps[1]; reps[1] = reps[0];
                reps[0] = dist;
                state = lzma_next_match[state];

                for (int j = 1; j < blen; j++)
                    lzma_mf_update(&mf, emit_pos + j);

                prev_byte = data[emit_pos + blen - 1];
                emit_pos += blen;
            }
        }

        pos = emit_pos;

        /* Phase 3: Early-exit — abort if LZMA can't beat current best.
         * Progressive margin: very lenient when model is cold, strict when warm.
         * margin = 1.10 + 4.0*(1-pct)^2:  5%→4.7x, 25%→3.4x, 50%→2.1x, 90%→1.14x
         * This preserves LZMA wins on compressible data (pic/ptt5) while
         * catching clearly-worse cases (kennedy/xml) early. */
        if (size_limit > 0 && pos >= 4096 && (int64_t)pos * 20 >= (int64_t)n) {
            int64_t out_est = (int64_t)(rc.pos + rc.cache_size);
            int64_t projected = out_est * n / pos;
            int64_t pct_remain = (int64_t)(n - pos) * 100 / n;
            int64_t margin_x100 = 110 + 4 * pct_remain * pct_remain / 100;
            if (projected * 100 > (int64_t)size_limit * margin_x100) {
                fprintf(stderr, "    [LZMA] early-exit at %.0f%% (projected %lld > limit*%.1f=%lld)\n",
                        (double)pos*100/n, (long long)projected,
                        (double)margin_x100/100, (long long)((int64_t)size_limit * margin_x100 / 100));
                lzma_mf_free(&mf);
                free(opt); free(pairs);
                free(dec_lens); free(dec_dists); free(dec_reps_flag);
                sa_ctx_free(sa_ctx);
                return -1;
            }
        }

        /* Update pricing probs from encoding probs (sync models) */
        memcpy(&probs, &enc_probs, sizeof(LzmaProbs));
    }

    int comp_size = lrc_enc_finish(&rc);
    lzma_mf_free(&mf);
    free(opt);
    free(pairs);
    free(dec_lens);
    free(dec_dists);
    free(dec_reps_flag);
    sa_ctx_free(sa_ctx);
    return comp_size;
}

/* ===== LZMA Decoder ===== */
static int lzma_decompress(const uint8_t *in, uint8_t *out, int n) {
    if (n == 0) return 0;
    
    LzmaProbs probs;
    lzma_probs_init(&probs);
    
    LRCDec rc;
    lrc_dec_init(&rc, in);
    
    int state = 0;
    uint32_t reps[4] = {1, 1, 1, 1};
    uint8_t prev_byte = 0;
    int pos = 0;
    
    while (pos < n) {
        int pos_state = pos & LZMA_POS_MASK;
        
        if (lrc_dec_bit(&rc, &probs.is_match[state][pos_state]) == 0) {
            /* Literal */
            uint16_t *lit_probs = lzma_lit_probs(&probs, pos, prev_byte);
            
            if (LZMA_IS_LIT_STATE(state)) {
                prev_byte = lzma_lit_decode(&rc, lit_probs);
            } else {
                int ref = pos - (int)reps[0] - 1;
                uint8_t match_byte = (ref >= 0) ? out[ref] : 0;
                prev_byte = lzma_lit_decode_matched(&rc, lit_probs, match_byte);
            }
            
            out[pos] = prev_byte;
            state = lzma_next_lit[state];
            pos++;
        } else {
            /* Match or rep */
            int len;
            
            if (lrc_dec_bit(&rc, &probs.is_rep[state]) == 0) {
                /* Normal match */
                len = lzma_len_decode(&rc, &probs.match_len, pos_state);
                uint32_t dist = lzma_dist_decode(&rc, &probs, len);
                
                reps[3] = reps[2]; reps[2] = reps[1]; reps[1] = reps[0];
                reps[0] = dist;
                state = lzma_next_match[state];
            } else {
                /* Rep match */
                if (lrc_dec_bit(&rc, &probs.is_rep_g0[state]) == 0) {
                    /* rep0 */
                    if (lrc_dec_bit(&rc, &probs.is_rep0_long[state][pos_state]) == 0) {
                        /* Short rep (length 1) */
                        int ref = pos - (int)reps[0] - 1;
                        out[pos] = (ref >= 0) ? out[ref] : 0;
                        prev_byte = out[pos];
                        state = lzma_next_short_rep[state];
                        pos++;
                        continue;
                    }
                    len = lzma_len_decode(&rc, &probs.rep_len, pos_state);
                } else {
                    uint32_t dist;
                    if (lrc_dec_bit(&rc, &probs.is_rep_g1[state]) == 0) {
                        dist = reps[1];
                    } else {
                        if (lrc_dec_bit(&rc, &probs.is_rep_g2[state]) == 0) {
                            dist = reps[2];
                        } else {
                            dist = reps[3];
                            reps[3] = reps[2];
                        }
                        reps[2] = reps[1];
                    }
                    reps[1] = reps[0];
                    reps[0] = dist;
                    len = lzma_len_decode(&rc, &probs.rep_len, pos_state);
                }
                state = lzma_next_rep[state];
            }
            
            /* Copy match */
            int ref = pos - (int)reps[0] - 1;
            for (int i = 0; i < len && pos + i < n; i++)
                out[pos + i] = (ref + i >= 0) ? out[ref + i] : 0;
            prev_byte = out[pos + len - 1];
            pos += len;
        }
    }
    
    return pos;
}


/* ===== Parallel Group Work Structure ===== */
typedef struct {
    /* Input */
    const uint8_t *data;
    int n;
    double entropy;
    
    /* Output */
    uint8_t *out_buf;    /* thread's own output buffer */
    int best_size;
    int best_strat;
    
    /* Strategy hint from sample (-1 = try all) */
    int hint_strat;
    
    /* Working buffers (thread-private) */
    BWTWorkspace *bwt_ws;
    CompressWorkspace *cws;
} GroupWork;

/* Thread function: Groups A + C (plain BWT, LZP+BWT) */
static void *thread_groups_AC(void *arg) {
    GroupWork *w = (GroupWork*)arg;
    const uint8_t *data = w->data;
    int n = w->n;
    double ent = w->entropy;
    w->best_size = n;
    w->best_strat = S_STORE;
    memcpy(w->out_buf, data, n);
    
    int skip_bwt = (ent > 7.0);
    int skip_lzp = (ent > 6.5);
    /* v10.2: Skip LZP if sample showed plain BWT wins (saves 1 full BWT!) */
    if (w->hint_strat == S_BWT_O0 || w->hint_strat == S_BWT_O1 || w->hint_strat == S_BWT_O2)
        skip_lzp = 1;
    
    uint8_t  *bwt_buf   = w->cws->bwt_buf;
    uint8_t  *mtf_buf   = w->cws->mtf_buf;
    uint16_t *zrle_buf  = w->cws->zrle_buf;
    uint8_t  *arith_buf = w->cws->arith_buf;
    uint8_t  *lzp_buf   = w->cws->lzp_buf;
    int pidx, nz, asize, total;
    
    if (!skip_bwt) {
        /* Group A: BWT(data) + O0/O1/O2 */
        pidx = bwt_encode_ws(data, n, bwt_buf, w->bwt_ws);
        mtf_encode(bwt_buf, n, mtf_buf);
        nz = zrle_encode(mtf_buf, n, zrle_buf);
        
        /* O0 */
        asize = arith_enc_o0(zrle_buf, nz, arith_buf);
        total = 8 + asize;
        if (total < w->best_size) {
            w->best_size = total; w->best_strat = S_BWT_O0;
            put_u32be(w->out_buf, pidx);
            put_u32be(w->out_buf + 4, nz);
            memcpy(w->out_buf + 8, arith_buf, asize);
        }
        
        int o0cmp = (total <= w->best_size * 5 / 4);
        /* v10.2: Skip higher orders if sample already determined winner */
        int skip_o1o2 = (w->hint_strat == S_BWT_O0);
        int skip_o0o2 = (w->hint_strat == S_BWT_O1 || w->hint_strat == S_BWT_O1_PS);
        if (o0cmp && !skip_o1o2) {
            /* O1 */
            asize = arith_enc_o1(zrle_buf, nz, arith_buf);
            total = 8 + asize;
            if (total < w->best_size) {
                w->best_size = total; w->best_strat = S_BWT_O1;
                put_u32be(w->out_buf, pidx);
                put_u32be(w->out_buf + 4, nz);
                memcpy(w->out_buf + 8, arith_buf, asize);
            }
            /* O1_PS: O1 with global warm-start header (258 bytes overhead).
             * Removes dead symbols from all sub-models and provides a better
             * prior than uniform, especially for short blocks / sparse contexts. */
            asize = arith_enc_o1_ps(zrle_buf, nz, arith_buf);
            total = 8 + asize;
            if (total < w->best_size) {
                w->best_size = total; w->best_strat = S_BWT_O1_PS;
                put_u32be(w->out_buf, pidx);
                put_u32be(w->out_buf + 4, nz);
                memcpy(w->out_buf + 8, arith_buf, asize);
            }
            /* O2 */
            if (!skip_o0o2) {
            asize = arith_enc_o2(zrle_buf, nz, arith_buf);
            total = 8 + asize;
            if (total < w->best_size) {
                w->best_size = total; w->best_strat = S_BWT_O2;
                put_u32be(w->out_buf, pidx);
                put_u32be(w->out_buf + 4, nz);
                memcpy(w->out_buf + 8, arith_buf, asize);
            }
            } /* end skip_o2 */
        }

        /* O0_PS: pre-scanned O0 — fixes cold-start and dead-symbol wasted probability.
         * Only tried when O0 was competitive (o0cmp) and no sample hint forced O1/O2.
         * Overhead vs plain O0: ZRLE_ALPHA (258) bytes per block.
         * Expected gain: ~2% on small text files; neutral on large files. */
        if (o0cmp && w->hint_strat != S_BWT_O1 && w->hint_strat != S_BWT_O2) {
            asize = arith_enc_o0_ps(zrle_buf, nz, arith_buf);
            total = 8 + asize;
            if (total < w->best_size) {
                w->best_size = total; w->best_strat = S_BWT_O0_PS;
                put_u32be(w->out_buf, pidx);
                put_u32be(w->out_buf + 4, nz);
                memcpy(w->out_buf + 8, arith_buf, asize);
            }

            /* rANS O0: same ratio as O0_PS, O(1) table-based decode */
            asize = rans_enc_o0(zrle_buf, nz, arith_buf);
            total = 8 + asize;
            if (total < w->best_size) {
                w->best_size = total; w->best_strat = S_BWT_RANS;
                put_u32be(w->out_buf, pidx);
                put_u32be(w->out_buf + 4, nz);
                memcpy(w->out_buf + 8, arith_buf, asize);
            }

            /* 2-context RC O0: separate run-ctx vs literal-ctx models */
            asize = arith_enc_ctx2(zrle_buf, nz, arith_buf);
            total = 8 + asize;
            if (total < w->best_size) {
                w->best_size = total; w->best_strat = S_BWT_CTX2;
                put_u32be(w->out_buf, pidx);
                put_u32be(w->out_buf + 4, nz);
                memcpy(w->out_buf + 8, arith_buf, asize);
            }
        }

        /* Group C: LZP+BWT */
        if (!skip_lzp) {
            int lzp_size = lzp_encode(data, n, lzp_buf);
            if (lzp_size > 0) {
                pidx = bwt_encode_ws(lzp_buf, lzp_size, bwt_buf, w->bwt_ws);
                mtf_encode(bwt_buf, lzp_size, mtf_buf);
                nz = zrle_encode(mtf_buf, lzp_size, zrle_buf);
                
                asize = arith_enc_o0(zrle_buf, nz, arith_buf);
                total = 12 + asize;
                if (total < w->best_size) {
                    w->best_size = total; w->best_strat = S_LZP_BWT_O0;
                    put_u32be(w->out_buf, lzp_size);
                    put_u32be(w->out_buf + 4, pidx);
                    put_u32be(w->out_buf + 8, nz);
                    memcpy(w->out_buf + 12, arith_buf, asize);
                }
                o0cmp = (total <= w->best_size * 5 / 4);
                if (o0cmp) {
                    asize = arith_enc_o1(zrle_buf, nz, arith_buf);
                    total = 12 + asize;
                    if (total < w->best_size) {
                        w->best_size = total; w->best_strat = S_LZP_BWT_O1;
                        put_u32be(w->out_buf, lzp_size);
                        put_u32be(w->out_buf + 4, pidx);
                        put_u32be(w->out_buf + 8, nz);
                        memcpy(w->out_buf + 12, arith_buf, asize);
                    }
                    asize = arith_enc_o2(zrle_buf, nz, arith_buf);
                    total = 12 + asize;
                    if (total < w->best_size) {
                        w->best_size = total; w->best_strat = S_LZP_BWT_O2;
                        put_u32be(w->out_buf, lzp_size);
                        put_u32be(w->out_buf + 4, pidx);
                        put_u32be(w->out_buf + 8, nz);
                        memcpy(w->out_buf + 12, arith_buf, asize);
                    }
                }
            }
        }
    }
    return NULL;
}

/* Thread function: Groups B + D + E (Delta+BWT, Delta+LZP+BWT, LZ77+BWT) */
static void *thread_groups_BDE(void *arg) {
    GroupWork *w = (GroupWork*)arg;
    const uint8_t *data = w->data;
    int n = w->n;
    double ent = w->entropy;
    w->best_size = n;
    w->best_strat = S_STORE;
    memcpy(w->out_buf, data, n);
    
    int skip_bwt = (ent > 7.0);
    /* v10.2: Skip Delta+BWT if sample showed LZ77 wins */
    if (w->hint_strat == S_LZ77_O0 || w->hint_strat == S_LZ77_O1)
        skip_bwt = 1;
    int skip_lzp = (ent > 6.5);
    /* v10.2: Skip LZP variants if hint says Delta+BWT wins */
    if (w->hint_strat == S_DBWT_O0 || w->hint_strat == S_DBWT_O1 || w->hint_strat == S_DBWT_O2)
        skip_lzp = 1;
    
    uint8_t  *bwt_buf   = w->cws->bwt_buf;
    uint8_t  *mtf_buf   = w->cws->mtf_buf;
    uint16_t *zrle_buf  = w->cws->zrle_buf;
    uint8_t  *arith_buf = w->cws->arith_buf;
    uint8_t  *delta_buf = w->cws->delta_buf;
    uint8_t  *lzp_buf   = w->cws->lzp_buf;
    int pidx, nz, asize, total;
    
    if (!skip_bwt) {
        /* Group B: Delta+BWT */
        delta_encode(data, n, delta_buf);
        double dent = block_entropy(delta_buf, n);
        if (dent < ent + 0.5) {
            pidx = bwt_encode_ws(delta_buf, n, bwt_buf, w->bwt_ws);
            mtf_encode(bwt_buf, n, mtf_buf);
            nz = zrle_encode(mtf_buf, n, zrle_buf);
            
            asize = arith_enc_o0(zrle_buf, nz, arith_buf);
            total = 8 + asize;
            if (total < w->best_size) {
                w->best_size = total; w->best_strat = S_DBWT_O0;
                put_u32be(w->out_buf, pidx);
                put_u32be(w->out_buf + 4, nz);
                memcpy(w->out_buf + 8, arith_buf, asize);
            }
            int o0cmp = (total <= w->best_size * 5 / 4);
            if (o0cmp) {
                asize = arith_enc_o1(zrle_buf, nz, arith_buf);
                total = 8 + asize;
                if (total < w->best_size) {
                    w->best_size = total; w->best_strat = S_DBWT_O1;
                    put_u32be(w->out_buf, pidx);
                    put_u32be(w->out_buf + 4, nz);
                    memcpy(w->out_buf + 8, arith_buf, asize);
                }
                asize = arith_enc_o2(zrle_buf, nz, arith_buf);
                total = 8 + asize;
                if (total < w->best_size) {
                    w->best_size = total; w->best_strat = S_DBWT_O2;
                    put_u32be(w->out_buf, pidx);
                    put_u32be(w->out_buf + 4, nz);
                    memcpy(w->out_buf + 8, arith_buf, asize);
                }
            }
            
            /* Group D: Delta+LZP+BWT */
            if (!skip_lzp) {
                int dlzp_size = lzp_encode(delta_buf, n, lzp_buf);
                if (dlzp_size > 0) {
                    pidx = bwt_encode_ws(lzp_buf, dlzp_size, bwt_buf, w->bwt_ws);
                    mtf_encode(bwt_buf, dlzp_size, mtf_buf);
                    nz = zrle_encode(mtf_buf, dlzp_size, zrle_buf);
                    
                    asize = arith_enc_o0(zrle_buf, nz, arith_buf);
                    total = 12 + asize;
                    if (total < w->best_size) {
                        w->best_size = total; w->best_strat = S_DLZP_BWT_O0;
                        put_u32be(w->out_buf, dlzp_size);
                        put_u32be(w->out_buf + 4, pidx);
                        put_u32be(w->out_buf + 8, nz);
                        memcpy(w->out_buf + 12, arith_buf, asize);
                    }
                    o0cmp = (total <= w->best_size * 5 / 4);
                    if (o0cmp) {
                        asize = arith_enc_o1(zrle_buf, nz, arith_buf);
                        total = 12 + asize;
                        if (total < w->best_size) {
                            w->best_size = total; w->best_strat = S_DLZP_BWT_O1;
                            put_u32be(w->out_buf, dlzp_size);
                            put_u32be(w->out_buf + 4, pidx);
                            put_u32be(w->out_buf + 8, nz);
                            memcpy(w->out_buf + 12, arith_buf, asize);
                        }
                        asize = arith_enc_o2(zrle_buf, nz, arith_buf);
                        total = 12 + asize;
                        if (total < w->best_size) {
                            w->best_size = total; w->best_strat = S_DLZP_BWT_O2;
                            put_u32be(w->out_buf, dlzp_size);
                            put_u32be(w->out_buf + 4, pidx);
                            put_u32be(w->out_buf + 8, nz);
                            memcpy(w->out_buf + 12, arith_buf, asize);
                        }
                    }
                }
            }
        }
    }
    
    /* Group F32: IEEE 754 Float32 XOR-delta → BWT+RC */
    /* Targets scientific/sensor data where consecutive floats share exponent bits.
       Uses delta_buf (already allocated, size n) as the XOR-transformed buffer. */
    if (w->hint_strat < 0 || w->hint_strat == S_F32_BWT) {
        if (float_xor_detect(data, n)) {
            float_xor_encode(data, n, delta_buf);  /* delta_buf reused */
            pidx = bwt_encode_ws(delta_buf, n, bwt_buf, w->bwt_ws);
            mtf_encode(bwt_buf, n, mtf_buf);
            nz = zrle_encode(mtf_buf, n, zrle_buf);
            asize = arith_enc_o0(zrle_buf, nz, arith_buf);
            total = 8 + asize;
            if (total < w->best_size) {
                w->best_size = total; w->best_strat = S_F32_BWT;
                put_u32be(w->out_buf, pidx);
                put_u32be(w->out_buf + 4, nz);
                memcpy(w->out_buf + 8, arith_buf, asize);
                fprintf(stderr, "    [F32XOR+BWT] %d -> %d (%.2fx) *** NEW BEST ***\n",
                        n, total, (double)n / total);
            } else {
                fprintf(stderr, "    [F32XOR+BWT] %d -> %d (%.2fx)\n",
                        n, total, (double)n / total);
            }
        }
    }

    /* Group E: LZ77+BWT */
    /* v10.2: Skip LZ77 if sample showed BWT/Delta wins */
    if (w->hint_strat < 0 || w->hint_strat == S_LZ77_O0 || w->hint_strat == S_LZ77_O1)
    {
        int lz_cap = n + 4096;
        uint8_t *lz_packed = (uint8_t*)malloc(lz_cap);
        if (lz_packed) {
            int lz_size = lz77_encode(data, n, lz_packed, lz_cap);
            if (lz_size > 0) {
                int alloc_lz = lz_size + 4096;
                uint8_t  *lz_bwt   = (uint8_t*)malloc(alloc_lz);
                uint8_t  *lz_mtf   = (uint8_t*)malloc(alloc_lz);
                uint16_t *lz_zrle  = (uint16_t*)malloc(alloc_lz * 2 * sizeof(uint16_t));
                uint8_t  *lz_arith = (uint8_t*)malloc(alloc_lz + alloc_lz / 4 + 1024);
                if (lz_bwt && lz_mtf && lz_zrle && lz_arith) {
                    int lz_pidx = bwt_encode_ws(lz_packed, lz_size, lz_bwt, w->bwt_ws);
                    mtf_encode(lz_bwt, lz_size, lz_mtf);
                    int lz_nz = zrle_encode(lz_mtf, lz_size, lz_zrle);
                    
                    int lz_asize = arith_enc_o0(lz_zrle, lz_nz, lz_arith);
                    int lz_total = 12 + lz_asize;
                    if (lz_total < w->best_size) {
                        w->best_size = lz_total; w->best_strat = S_LZ77_O0;
                        put_u32be(w->out_buf, lz_size);
                        put_u32be(w->out_buf + 4, lz_pidx);
                        put_u32be(w->out_buf + 8, lz_nz);
                        memcpy(w->out_buf + 12, lz_arith, lz_asize);
                    }
                    lz_asize = arith_enc_o1(lz_zrle, lz_nz, lz_arith);
                    lz_total = 12 + lz_asize;
                    if (lz_total < w->best_size) {
                        w->best_size = lz_total; w->best_strat = S_LZ77_O1;
                        put_u32be(w->out_buf, lz_size);
                        put_u32be(w->out_buf + 4, lz_pidx);
                        put_u32be(w->out_buf + 8, lz_nz);
                        memcpy(w->out_buf + 12, lz_arith, lz_asize);
                    }
                }
                free(lz_bwt); free(lz_mtf); free(lz_zrle); free(lz_arith);
            }
            free(lz_packed);
        }
    }
    
    return NULL;
}

/* ===== Block Compression (Parallel) ===== */

/* Fast ASCII ratio calculation for LZMA heuristic */
static double ascii_ratio(const uint8_t *data, int n) {
    int count = 0;
    /* Sample first 64KB for speed on large files */
    int limit = n < 65536 ? n : 65536;
    for (int i = 0; i < limit; i++) {
        uint8_t b = data[i];
        if ((b >= 0x20 && b <= 0x7E) || b == 0x09 || b == 0x0A || b == 0x0D)
            count++;
    }
    return (double)count / limit;
}

/* Returns compressed size, writes strategy to *strat */

/* === Sample-based fast strategy selection (HyperPack v10.2) === */
#define SAMPLE_SIZE (1024*1024)       /* 1 MB sample */
#define SAMPLE_THRESHOLD (4*1024*1024) /* only for blocks > 4 MB */

static int is_ac_strategy(int s) {
    return s == S_BWT_O0 || s == S_BWT_O1 || s == S_BWT_O2 || s == S_BWT_O0_PS ||
           s == S_BWT_RANS || s == S_BWT_CTX2 || s == S_BWT_O1_PS ||
           s == S_LZP_BWT_O0 || s == S_LZP_BWT_O1 || s == S_LZP_BWT_O2;
}
static int is_bde_strategy(int s) {
    return s == S_DBWT_O0 || s == S_DBWT_O1 || s == S_DBWT_O2 ||
           s == S_DLZP_BWT_O0 || s == S_DLZP_BWT_O1 || s == S_DLZP_BWT_O2 ||
           s == S_LZ77_O0 || s == S_LZ77_O1 ||
           s == S_F32_BWT;
}

static int compress_block(const uint8_t *data, int n, uint8_t *out, int *strat, BWTWorkspace *bwt_ws, CompressWorkspace *cws) {
    int best_size = n;
    *strat = S_STORE;
    memcpy(out, data, n); /* default: store */

    /* Entropy check — skip compression for incompressible data */
    double ent = block_entropy(data, n);
    if (ent > 7.95) return best_size;  /* only skip truly random data */

    /* === SAMPLE-BASED FAST STRATEGY SELECTION (v10.2) === */
    /* For large blocks: test 1 MB sample first, then only run winning group */
    int hint_strat = -1;  /* -1 = try all (small blocks or fallback) */
    
    if (n > SAMPLE_THRESHOLD) {
        int sample_n = SAMPLE_SIZE;
        uint8_t *sample_out = (uint8_t*)malloc(sample_n + 262144);
        BWTWorkspace *sample_bwt = bwt_ws_create(sample_n + 262144);
        CompressWorkspace *sample_cws = cws_create(sample_n);
        if (sample_out && sample_bwt && sample_cws) {
            int sample_strat;
            compress_block(data, sample_n, sample_out, &sample_strat, sample_bwt, sample_cws);
            hint_strat = sample_strat;
        }
        if (sample_out) free(sample_out);
        if (sample_bwt) bwt_ws_free(sample_bwt);
        if (sample_cws) cws_free(sample_cws);
    }
    
    /* Determine which groups to run */
    int run_ac = 1, run_bde = 1;
    if (hint_strat >= 0) {
        if (is_ac_strategy(hint_strat)) {
            run_bde = 0;  /* BWT won on sample -> skip Delta/LZ77 group */
        } else if (is_bde_strategy(hint_strat)) {
            run_ac = 0;   /* Delta/LZ77 won on sample -> skip BWT group */
        }
        /* LZMA hint: both groups skipped, handled below */
        if (hint_strat == S_LZMA) { run_ac = 0; run_bde = 0; }
    }
    
    /* === Parallel Groups A-E === */
    BWTWorkspace *bwt_ws2 = NULL;
    CompressWorkspace *cws2 = NULL;
    uint8_t *out_buf_ac  = NULL;
    uint8_t *out_buf_bde = NULL;
    GroupWork work_ac = {0};
    GroupWork work_bde = {0};
    pthread_t worker;
    int worker_launched = 0;
    
    if (run_ac) {
        out_buf_ac = (uint8_t*)malloc(n + 262144);
        work_ac.hint_strat = hint_strat;
        work_ac.data = data;
        work_ac.n = n;
        work_ac.entropy = ent;
        work_ac.out_buf = out_buf_ac;
        work_ac.bwt_ws = bwt_ws;
        work_ac.cws = cws;
    }
    
    if (run_bde) {
        bwt_ws2 = bwt_ws_create(n + 262144);
        cws2 = cws_create(n);
        out_buf_bde = (uint8_t*)malloc(n + 262144);
        work_bde.hint_strat = hint_strat;
        work_bde.data = data;
        work_bde.n = n;
        work_bde.entropy = ent;
        work_bde.out_buf = out_buf_bde;
        work_bde.bwt_ws = bwt_ws2;
        work_bde.cws = cws2;
    }
    
#ifdef HYPERPACK_WASM
    /* WASM: run strategy groups sequentially (no pthreads) */
    if (run_ac) thread_groups_AC(&work_ac);
    if (run_bde) thread_groups_BDE(&work_bde);
#else
    /* Launch threads only for groups we need */
    if (run_ac && run_bde) {
        /* Both groups: parallel (original behavior) */
        pthread_create(&worker, NULL, thread_groups_BDE, &work_bde);
        worker_launched = 1;
        thread_groups_AC(&work_ac);
        pthread_join(worker, NULL);
    } else if (run_ac) {
        /* Only AC group */
        thread_groups_AC(&work_ac);
    } else if (run_bde) {
        /* Only BDE group */
        thread_groups_BDE(&work_bde);
    }
#endif
    
    /* Pick best from whichever groups ran */
    if (run_ac && run_bde) {
        if (work_ac.best_size <= work_bde.best_size) {
            best_size = work_ac.best_size;
            *strat = work_ac.best_strat;
            if (best_size < n) memcpy(out, work_ac.out_buf, best_size);
        } else {
            best_size = work_bde.best_size;
            *strat = work_bde.best_strat;
            if (best_size < n) memcpy(out, work_bde.out_buf, best_size);
        }
    } else if (run_ac && work_ac.best_size < best_size) {
        best_size = work_ac.best_size;
        *strat = work_ac.best_strat;
        if (best_size < n) memcpy(out, work_ac.out_buf, best_size);
    } else if (run_bde && work_bde.best_size < best_size) {
        best_size = work_bde.best_size;
        *strat = work_bde.best_strat;
        if (best_size < n) memcpy(out, work_bde.out_buf, best_size);
    }
    
    /* Free workspaces */
    if (bwt_ws2) bwt_ws_free(bwt_ws2);
    if (cws2) cws_free(cws2);
    if (out_buf_ac) free(out_buf_ac);
    if (out_buf_bde) free(out_buf_bde);
    
    /* saved_bwt/saved_mtf not needed (CM/PPM strategies are disabled) */
    uint8_t *saved_bwt = NULL, *saved_mtf = NULL;
    int saved_pidx = 0;

    /* Groups A-E handled by parallel threads above */

    /* arith_buf needed for Audio group below */
    uint8_t *arith_buf = cws->arith_buf;

    /* ======= Group F: Audio Pipeline ======= */
    if (audio_detect(data, n)) {
        /* LPC headers can add ~100KB overhead (2037 blocks * ~46 bytes each) */
        int audio_alloc = n + 262144;
        uint8_t *audio_buf = (uint8_t*)malloc(audio_alloc);
        int audio_n = audio_encode(data, n, audio_buf);
        int audio_hdr = 5;  /* order(1) + nframes(4) */
        int nframes_audio = (n / 4);
        
        /* Audio + Direct RC — 4 streams independently */
        /* We need to find where the 4 byte-split streams are in audio_buf.
           For LPC (version=4): header is variable-length, streams follow.
           For simple delta (version=3): header is 5 bytes, streams follow. */
        {
            uint8_t *direct_out = (uint8_t*)malloc(n + 131072);
            int dp = 0;
            
            /* Copy the entire audio header (everything before the residual streams) */
            int version = audio_buf[0];
            int nf_audio = (audio_buf[1]<<24) | (audio_buf[2]<<16) | (audio_buf[3]<<8) | audio_buf[4];
            int stream_start;
            if (version == 4) {
                /* LPC: skip 9-byte header + all block headers */
                int nblk = (audio_buf[5]<<24) | (audio_buf[6]<<16) | (audio_buf[7]<<8) | audio_buf[8];
                int p = 9;
                for (int b = 0; b < nblk; b++) {
                    int oM = audio_buf[p++], oS = audio_buf[p++];
                    p += 2 * (oM + oS);
                }
                stream_start = p;
            } else {
                stream_start = 5;
            }
            
            /* Write marker + full header to output */
            direct_out[dp++] = 0xFF;  /* marker: direct mode */
            memcpy(direct_out + dp, audio_buf, stream_start); dp += stream_start;
            
            uint8_t *stream_data = audio_buf + stream_start;
            int stream_len = nf_audio;
            int tail_audio = n - nf_audio * 4;
            
            int direct_ok = 1;
            for (int s = 0; s < 4; s++) {
                /* Convert each byte stream to uint16_t for RC */
                uint16_t *s16 = (uint16_t*)malloc(stream_len * sizeof(uint16_t));
                for (int j = 0; j < stream_len; j++) s16[j] = stream_data[s * stream_len + j];
                
                int s_enc = arith_enc_o1(s16, stream_len, arith_buf);
                free(s16);
                
                put_u32be(direct_out + dp, s_enc); dp += 4;
                memcpy(direct_out + dp, arith_buf, s_enc); dp += s_enc;
            }
            if (tail_audio > 0) {
                memcpy(direct_out + dp, stream_data + 4 * stream_len, tail_audio);
                dp += tail_audio;
            }
            
            if (direct_ok && dp < best_size) {
                best_size = dp;
                *strat = S_AUDIO;
                memcpy(out, direct_out, dp);
            fprintf(stderr, "    [Audio+Direct RC] %d -> %d (%.2fx)\n", n, dp, (double)n / dp);
            }
            fprintf(stderr, "    [Audio] Direct size: %d (%.2fx)\n", dp, (double)n / dp);
            free(direct_out);
        }
        
        free(audio_buf);
    } else {
        fprintf(stderr, "    [Audio] not detected, skipping\n");
    }


    /* ======= Group G: Base64 Preprocessing (S_BASE64) ======= */
    {
        int max_b64_runs = n / B64_MIN_RUN + 1;
        if (max_b64_runs > 1000000) max_b64_runs = 1000000;
        B64Run *b64_runs = (B64Run*)malloc(max_b64_runs * sizeof(B64Run));
        int n_b64_runs = b64_find_runs(data, n, b64_runs, max_b64_runs);

        int total_b64 = 0;
        for (int i = 0; i < n_b64_runs; i++) total_b64 += b64_runs[i].len;

        if (n_b64_runs > 0 && (double)total_b64 / n > 0.05) {
            fprintf(stderr, "    [Base64] Found %d runs, %d bytes (%.1f%%)\n",
                    n_b64_runs, total_b64, 100.0 * total_b64 / n);

            int skeleton_size = n - total_b64;
            uint8_t *skeleton = (uint8_t*)malloc(skeleton_size + 1);
            uint8_t *decoded  = (uint8_t*)malloc(total_b64);  /* always smaller */

            int sk_pos = 0, dec_pos = 0, src_pos = 0;
            int valid = 1;

            for (int r = 0; r < n_b64_runs; r++) {
                /* Copy non-b64 gap */
                int gap = b64_runs[r].start - src_pos;
                if (gap > 0) { memcpy(skeleton + sk_pos, data + src_pos, gap); sk_pos += gap; }

                /* Decode run */
                int dl = b64_decode_chunk(data + b64_runs[r].start, b64_runs[r].len, decoded + dec_pos);
                if (dl < 0) { valid = 0; break; }

                /* Round-trip verify */
                uint8_t *vbuf = (uint8_t*)malloc(b64_runs[r].len + 4);
                int el = b64_encode_chunk(decoded + dec_pos, dl, vbuf);
                if (el != b64_runs[r].len || memcmp(vbuf, data + b64_runs[r].start, el) != 0) {
                    free(vbuf); valid = 0; break;
                }
                free(vbuf);

                b64_runs[r].dec_len = dl;
                dec_pos += dl;
                src_pos = b64_runs[r].start + b64_runs[r].len;
            }

            /* Remaining skeleton after last run */
            if (valid && src_pos < n) {
                memcpy(skeleton + sk_pos, data + src_pos, n - src_pos);
                sk_pos += n - src_pos;
            }

            if (valid && sk_pos == skeleton_size) {
                int decoded_size = dec_pos;

                /* Position table (delta-varint) */
                uint8_t *pos_tbl = (uint8_t*)malloc(n_b64_runs * 15 + 16);
                int pos_tbl_sz = b64_encode_pos(b64_runs, n_b64_runs, pos_tbl);

                /* Compress skeleton */
                uint8_t *sk_comp = (uint8_t*)malloc(skeleton_size + skeleton_size / 4 + 1024);
                int sk_order;
                int sk_comp_sz = compress_substream(skeleton, skeleton_size, sk_comp, &sk_order, bwt_ws);

                /* Compress decoded data */
                uint8_t *dec_comp = (uint8_t*)malloc(decoded_size + decoded_size / 4 + 1024);
                int dec_order;
                int dec_comp_sz = compress_substream(decoded, decoded_size, dec_comp, &dec_order, bwt_ws);

                /* Compress position table too */
                uint8_t *pos_comp = (uint8_t*)malloc(pos_tbl_sz + pos_tbl_sz/4 + 1024);
                int pos_order;
                int pos_comp_sz = compress_substream(pos_tbl, pos_tbl_sz, pos_comp, &pos_order, bwt_ws);

                /*
                 * Format (S_BASE64_V2):
                 *   [n_runs:4][skel_orig:4][dec_orig:4][pos_tbl_orig:4]
                 *   [sk_comp_sz:4][sk_order:1][dec_comp_sz:4][dec_order:1]
                 *   [pos_comp_sz:4][pos_order:1]
                 *   [compressed_pos_table][compressed_skeleton][compressed_decoded]
                 * Header = 31 bytes
                 */
                int b64v2_total = 31 + pos_comp_sz + sk_comp_sz + dec_comp_sz;

                fprintf(stderr, "    [Base64] skel: %d->%d (%.2fx), dec: %d->%d (%.2fx), "
                        "pos_tbl: %d->%d (%.2fx), total: %d (%.2fx)\n",
                        skeleton_size, sk_comp_sz, skeleton_size > 0 ? (double)skeleton_size / sk_comp_sz : 0,
                        decoded_size, dec_comp_sz, decoded_size > 0 ? (double)decoded_size / dec_comp_sz : 0,
                        pos_tbl_sz, pos_comp_sz, pos_tbl_sz > 0 ? (double)pos_tbl_sz / pos_comp_sz : 0,
                        b64v2_total, (double)n / b64v2_total);

                if (b64v2_total < best_size) {
                    best_size = b64v2_total;
                    *strat = S_BASE64_V2;
                    uint8_t *p = out;
                    put_u32be(p, n_b64_runs);      p += 4;
                    put_u32be(p, skeleton_size);    p += 4;
                    put_u32be(p, decoded_size);     p += 4;
                    put_u32be(p, pos_tbl_sz);       p += 4;  /* original pos table size */
                    put_u32be(p, sk_comp_sz);       p += 4;
                    *p++ = (uint8_t)sk_order;
                    put_u32be(p, dec_comp_sz);      p += 4;
                    *p++ = (uint8_t)dec_order;
                    put_u32be(p, pos_comp_sz);      p += 4;  /* compressed pos table size */
                    *p++ = (uint8_t)pos_order;
                    memcpy(p, pos_comp, pos_comp_sz); p += pos_comp_sz;
                    memcpy(p, sk_comp, sk_comp_sz);   p += sk_comp_sz;
                    memcpy(p, dec_comp, dec_comp_sz);
                    fprintf(stderr, "    [Base64v2] *** NEW BEST: %d -> %d (%.2fx) ***\n",
                            n, b64v2_total, (double)n / b64v2_total);
                }
                free(pos_comp);
                free(pos_tbl); free(sk_comp); free(dec_comp);
            } else if (!valid) {
                fprintf(stderr, "    [Base64] round-trip failed, skipping\n");
            }
            free(skeleton); free(decoded);
        } else {
            fprintf(stderr, "    [Base64] not enough b64 (%d runs, %.1f%%), skipping\n",
                    n_b64_runs, n > 0 ? 100.0 * total_b64 / n : 0.0);
        }
        free(b64_runs);
    }

    /* CM strategies disabled for speed (rarely win vs BWT pipeline) */
    /* Uncomment to enable: */
    /*
    {
        int cm_size = cm_encode(data, n, arith_buf);
        if (cm_size > 0 && cm_size < best_size) {
            best_size = cm_size;
            *strat = S_CM;
            memcpy(out, arith_buf, cm_size);
        }
    }
    */

    /* BWT+CM strategies disabled for speed (uncomment to enable) */
    /*
    {
        int bwt_cm_size = cm_encode(saved_bwt, n, arith_buf);
        int total = 4 + bwt_cm_size;
        if (bwt_cm_size > 0 && total < best_size) {
            best_size = total;
            *strat = S_BWT_CM;
            put_u32be(out, saved_pidx);
            memcpy(out + 4, arith_buf, bwt_cm_size);
        }
    }
    {
        int mtf_cm_size = cm_encode(saved_mtf, n, arith_buf);
        int total = 4 + mtf_cm_size;
        if (mtf_cm_size > 0 && total < best_size) {
            best_size = total;
            *strat = S_BWT_MTF_CM;
            put_u32be(out, saved_pidx);
            memcpy(out + 4, arith_buf, mtf_cm_size);
        }
    }
    */

    /* PPM and direct O1 strategies disabled (never win vs BWT+ZRLE on text/code) */
#if 0
    { /* PPM raw */
        int ppm_size = ppm_compress(data, n, arith_buf);
        if (ppm_size > 0 && ppm_size < best_size) {
            best_size = ppm_size; *strat = S_PPM;
            memcpy(out, arith_buf, ppm_size);
        }
    }
    { /* BWT+PPM */
        int bppm_size = ppm_compress(saved_bwt, n, arith_buf);
        int bppm_total = 4 + bppm_size;
        if (bppm_size > 0 && bppm_total < best_size) {
            best_size = bppm_total; *strat = S_BWT_PPM;
            put_u32be(out, saved_pidx);
            memcpy(out + 4, arith_buf, bppm_size);
        }
    }
    { /* BWT+MTF+PPM */
        int mtfppm_size = ppm_compress(saved_mtf, n, arith_buf);
        int mtfppm_total = 4 + mtfppm_size;
        if (mtfppm_size > 0 && mtfppm_total < best_size) {
            best_size = mtfppm_total; *strat = S_BWT_MTF_PPM;
            put_u32be(out, saved_pidx);
            memcpy(out + 4, arith_buf, mtfppm_size);
        }
    }
#endif


    /* ======= Group H: LZMA (S_LZMA) ======= */
    /* HyperPack v10.1: Smart LZMA selection heuristic.
     * Skip LZMA on data where BWT always wins (text, repetitive data).
     * Heuristic: skip if entropy < 5.0 OR (ASCII% > 95% AND entropy < 6.0)
     * This saves ~50% compression time with zero ratio loss. */
    int try_lzma = (n <= 64*1024*1024);
    if (try_lzma && n > 1024*1024) {
        /* Skip heuristic only for blocks > 1 MB where BWT has enough context */
        double asc = ascii_ratio(data, n);
        if (ent < 5.0 || (asc > 0.95 && ent < 6.0)) {
            try_lzma = 0;
            fprintf(stderr, "    [LZMA] skipped by heuristic (ent=%.2f, ascii=%.1f%%)\n", ent, asc*100);
        }
    }
    /* Phase 4: Smart LZMA for small blocks (< 1MB).
     * Phase 1 forced LZMA on ALL small blocks, but this wastes time on
     * large natural language text (novels, plays) where BWT always wins.
     * Heuristic: skip LZMA ONLY on clearly literary text:
     *   - Very high ASCII ratio (>95% = natural language, not code)
     *   - Moderate entropy (<5.5 = text, not binary)
     *   - Large enough to matter (>100KB = the slow ones)
     * Small source code (<100KB) keeps LZMA where it wins +5-12%. */
    if (n <= 1024*1024) {
        double asc = ascii_ratio(data, n);
        if (asc > 0.95 && ent < 5.5 && n > 100*1024) {
            try_lzma = 0;
            fprintf(stderr, "    [LZMA] skipped: large text detected (ent=%.2f, ascii=%.1f%%, %d bytes)\n", ent, asc*100, n);
        } else {
            try_lzma = 1;
            fprintf(stderr, "    [LZMA] forced for small block (%d bytes, ent=%.2f, ascii=%.1f%%)\n", n, ent, asc*100);
        }
    }
    if (try_lzma) {
        uint8_t *lzma_out = (uint8_t*)malloc(n + n/4 + 65536);
        if (lzma_out) {
            int lzma_sz = lzma_compress(data, n, lzma_out, best_size);
            if (lzma_sz > 0 && lzma_sz < best_size) {
                best_size = lzma_sz;
                *strat = S_LZMA;
                memcpy(out, lzma_out, lzma_sz);
                fprintf(stderr, "    [LZMA] %d -> %d (%.2fx) *** NEW BEST ***\n", n, lzma_sz, (double)n/lzma_sz);
            } else {
                fprintf(stderr, "    [LZMA] %d -> %d (%.2fx) [not better]\n", n, lzma_sz, (double)n/lzma_sz);
            }
            free(lzma_out);
        }
    } else {
        fprintf(stderr, "    [LZMA] skipped (block %d > 64MB threshold)\n", n);
    }

    /* ======= Group I: BCJ+LZMA (S_BCJ_LZMA) ======= */
    /* Phase 1.3: E8/E9 transform on x86 executables, then LZMA */
    if (bcj_is_executable(data, n) && n >= 256) {
        uint8_t *bcj_buf = (uint8_t*)malloc(n);
        uint8_t *bcj_lzma_out = (uint8_t*)malloc(n + n/4 + 65536);
        if (bcj_buf && bcj_lzma_out) {
            bcj_e8e9_encode(data, n, bcj_buf);
            int bcj_lzma_sz = lzma_compress(bcj_buf, n, bcj_lzma_out, best_size);
            if (bcj_lzma_sz > 0 && bcj_lzma_sz < best_size) {
                best_size = bcj_lzma_sz;
                *strat = S_BCJ_LZMA;
                memcpy(out, bcj_lzma_out, bcj_lzma_sz);
                fprintf(stderr, "    [BCJ+LZMA] %d -> %d (%.2fx) *** NEW BEST ***\n", n, bcj_lzma_sz, (double)n/bcj_lzma_sz);
            } else {
                fprintf(stderr, "    [BCJ+LZMA] %d -> %d (%.2fx)\n", n, bcj_lzma_sz > 0 ? bcj_lzma_sz : n, bcj_lzma_sz > 0 ? (double)n/bcj_lzma_sz : 1.0);
            }
        }
        if (bcj_buf) free(bcj_buf);
        if (bcj_lzma_out) free(bcj_lzma_out);
    }

    free(saved_bwt); free(saved_mtf);
    /* Buffers owned by CompressWorkspace - not freed here */
    return best_size;
}

/* ===== Block Decompression ===== */
static int decompress_block(const uint8_t *cdata, int csize, int strat, int orig_size, uint8_t *out) {
    if (strat == S_STORE) {
        memcpy(out, cdata, orig_size);
        return orig_size;
    }

    if (strat == S_LZMA) {
        lzma_decompress(cdata, out, orig_size);
        return orig_size;
    }
    if (strat == S_BCJ_LZMA) {
        uint8_t *tmp = (uint8_t*)malloc(orig_size);
        lzma_decompress(cdata, tmp, orig_size);
        bcj_e8e9_decode(tmp, orig_size, out);
        free(tmp);
        return orig_size;
    }

    if (strat == S_CM) {
        cm_decode(cdata, out, orig_size);
        return orig_size;
    }

    if (strat == S_BWT_CM) {
        int bwt_pidx = get_u32be(cdata);
        uint8_t *bwt_tmp = (uint8_t*)malloc(orig_size);
        cm_decode(cdata + 4, bwt_tmp, orig_size);
        bwt_decode(bwt_tmp, orig_size, bwt_pidx, out);
        free(bwt_tmp);
        return orig_size;
    }

    if (strat == S_BWT_MTF_CM) {
        int bwt_pidx = get_u32be(cdata);
        uint8_t *mtf_tmp = (uint8_t*)malloc(orig_size);
        uint8_t *bwt_tmp = (uint8_t*)malloc(orig_size);
        cm_decode(cdata + 4, mtf_tmp, orig_size);
        mtf_decode(mtf_tmp, orig_size, bwt_tmp);
        bwt_decode(bwt_tmp, orig_size, bwt_pidx, out);
        free(mtf_tmp);
        free(bwt_tmp);
        return orig_size;
    }

    if (strat == S_PPM) {
        ppm_decompress(cdata, out, orig_size);
        return orig_size;
    }

    if (strat == S_BWT_PPM) {
        int bwt_pidx = get_u32be(cdata);
        uint8_t *bwt_tmp = (uint8_t*)malloc(orig_size);
        ppm_decompress(cdata + 4, bwt_tmp, orig_size);
        bwt_decode(bwt_tmp, orig_size, bwt_pidx, out);
        free(bwt_tmp);
        return orig_size;
    }

    if (strat == S_BWT_MTF_PPM) {
        int bwt_pidx = get_u32be(cdata);
        uint8_t *mtf_tmp = (uint8_t*)malloc(orig_size);
        uint8_t *bwt_tmp = (uint8_t*)malloc(orig_size);
        ppm_decompress(cdata + 4, mtf_tmp, orig_size);
        mtf_decode(mtf_tmp, orig_size, bwt_tmp);
        bwt_decode(bwt_tmp, orig_size, bwt_pidx, out);
        free(mtf_tmp);
        free(bwt_tmp);
        return orig_size;
    }

    if (strat == S_AUDIO) {
        if (cdata[0] == 0xFF) {
            /* Direct RC mode — parse audio header to find stream layout */
            int version = cdata[1];
            int nframes = (cdata[2]<<24) | (cdata[3]<<16) | (cdata[4]<<8) | cdata[5];
            int stream_len = nframes;
            int tail = orig_size - nframes * 4;
            
            /* Find header size (same parsing as encoder) */
            int hdr_size;
            if (version == 4) {
                int nblk = (cdata[6]<<24) | (cdata[7]<<16) | (cdata[8]<<8) | cdata[9];
                int p = 9;
                for (int b = 0; b < nblk; b++) {
                    int oM = cdata[1 + p]; p++;
                    int oS = cdata[1 + p]; p++;
                    p += 2 * (oM + oS);
                }
                hdr_size = p;
            } else {
                hdr_size = 5;
            }
            
            uint8_t *audio_buf = (uint8_t*)malloc(orig_size + 131072);
            memcpy(audio_buf, cdata + 1, hdr_size);
            
            int cp = 1 + hdr_size;
            uint8_t *stream_data = audio_buf + hdr_size;
            
            for (int s = 0; s < 4; s++) {
                int s_len = get_u32be(cdata + cp); cp += 4;
                uint16_t *s16 = (uint16_t*)malloc(stream_len * sizeof(uint16_t));
                arith_dec_o1(cdata + cp, stream_len, s16);
                cp += s_len;
                for (int j = 0; j < stream_len; j++) stream_data[s * stream_len + j] = (uint8_t)s16[j];
                free(s16);
            }
            if (tail > 0) memcpy(stream_data + 4 * stream_len, cdata + cp, tail);
            
            audio_decode(audio_buf, orig_size, out);
            free(audio_buf);
        } else {
            /* BWT mode */
            int apidx = get_u32be(cdata);
            int raw_nz = get_u32be(cdata + 4);
            int use_o1 = (raw_nz & 0x80000000) != 0;
            int nz = raw_nz & 0x7FFFFFFF;
            const uint8_t *arith_data = cdata + 8;
            
            int audio_n = orig_size + 5;
            uint16_t *zrle_buf_d = (uint16_t*)malloc(nz * sizeof(uint16_t));
            if (use_o1) arith_dec_o1(arith_data, nz, zrle_buf_d);
            else        arith_dec_o0(arith_data, nz, zrle_buf_d);
            
            uint8_t *mtf_buf_d = (uint8_t*)malloc(audio_n);
            zrle_decode(zrle_buf_d, nz, mtf_buf_d, audio_n);
            free(zrle_buf_d);
            
            uint8_t *bwt_buf_d = (uint8_t*)malloc(audio_n);
            mtf_decode(mtf_buf_d, audio_n, bwt_buf_d);
            free(mtf_buf_d);
            
            uint8_t *audio_buf = (uint8_t*)malloc(audio_n);
            bwt_decode(bwt_buf_d, audio_n, apidx, audio_buf);
            free(bwt_buf_d);
            
            audio_decode(audio_buf, orig_size, out);
            free(audio_buf);
        }
        return orig_size;
    }


    if (strat == S_BASE64) {
        const uint8_t *p = cdata;
        int n_runs      = get_u32be(p); p += 4;
        int skel_orig   = get_u32be(p); p += 4;
        int dec_orig    = get_u32be(p); p += 4;
        int pos_tbl_sz  = get_u32be(p); p += 4;
        int sk_comp_sz  = get_u32be(p); p += 4;
        int sk_order    = *p++;
        int dec_comp_sz = get_u32be(p); p += 4;
        int dec_order   = *p++;

        B64Run *runs = (B64Run*)malloc(n_runs * sizeof(B64Run));
        b64_decode_pos(p, pos_tbl_sz, runs, n_runs);
        p += pos_tbl_sz;

        /* Decompress skeleton and decoded substreams in parallel (independent) */
        const uint8_t *sk_data  = p;
        const uint8_t *dec_data = p + sk_comp_sz;
        uint8_t *skeleton = (uint8_t*)malloc(skel_orig > 0 ? skel_orig : 1);
        uint8_t *decoded  = (uint8_t*)malloc(dec_orig > 0 ? dec_orig : 1);
        SubstreamArg sarg = {sk_data,  sk_comp_sz,  sk_order,  skel_orig, skeleton};
        SubstreamArg darg = {dec_data, dec_comp_sz, dec_order, dec_orig,  decoded};
        pthread_t sth, dth;
        pthread_create(&sth, NULL, decompress_substream_thread, &sarg);
        pthread_create(&dth, NULL, decompress_substream_thread, &darg);
        pthread_join(sth, NULL);
        pthread_join(dth, NULL);

        /* Reconstruct: interleave skeleton and base64-re-encoded data */
        int o = 0, sk = 0, dc = 0, prev_end = 0;
        for (int i = 0; i < n_runs; i++) {
            int gap = runs[i].start - prev_end;
            if (gap > 0) { memcpy(out + o, skeleton + sk, gap); o += gap; sk += gap; }
            o += b64_encode_chunk(decoded + dc, runs[i].dec_len, out + o);
            dc += runs[i].dec_len;
            prev_end = runs[i].start + runs[i].len;
        }
        int remaining = skel_orig - sk;
        if (remaining > 0) { memcpy(out + o, skeleton + sk, remaining); o += remaining; }

        free(runs); free(skeleton); free(decoded);
        return orig_size;
    }

    if (strat == S_BASE64_V2) {
        const uint8_t *p = cdata;
        int n_runs       = get_u32be(p); p += 4;
        int skel_orig    = get_u32be(p); p += 4;
        int dec_orig     = get_u32be(p); p += 4;
        int pos_tbl_orig = get_u32be(p); p += 4;
        int sk_comp_sz   = get_u32be(p); p += 4;
        int sk_order     = *p++;
        int dec_comp_sz  = get_u32be(p); p += 4;
        int dec_order    = *p++;
        int pos_comp_sz  = get_u32be(p); p += 4;
        int pos_order    = *p++;

        /* Decompress all 3 substreams in parallel (pos_tbl, skeleton, decoded are independent) */
        const uint8_t *pos_data = p;
        const uint8_t *sk_data  = p + pos_comp_sz;
        const uint8_t *dec_data = p + pos_comp_sz + sk_comp_sz;
        uint8_t *pos_tbl  = (uint8_t*)malloc(pos_tbl_orig > 0 ? pos_tbl_orig : 1);
        uint8_t *skeleton = (uint8_t*)malloc(skel_orig > 0 ? skel_orig : 1);
        uint8_t *decoded  = (uint8_t*)malloc(dec_orig > 0 ? dec_orig : 1);
        SubstreamArg parg = {pos_data, pos_comp_sz, pos_order, pos_tbl_orig, pos_tbl};
        SubstreamArg sarg = {sk_data,  sk_comp_sz,  sk_order,  skel_orig,    skeleton};
        SubstreamArg darg = {dec_data, dec_comp_sz, dec_order, dec_orig,     decoded};
        pthread_t pth, sth, dth;
        pthread_create(&pth, NULL, decompress_substream_thread, &parg);
        pthread_create(&sth, NULL, decompress_substream_thread, &sarg);
        pthread_create(&dth, NULL, decompress_substream_thread, &darg);
        pthread_join(pth, NULL);
        pthread_join(sth, NULL);
        pthread_join(dth, NULL);

        B64Run *runs = (B64Run*)malloc(n_runs * sizeof(B64Run));
        b64_decode_pos(pos_tbl, pos_tbl_orig, runs, n_runs);
        free(pos_tbl);

        /* Reconstruct */
        int o = 0, sk = 0, dc = 0, prev_end = 0;
        for (int i = 0; i < n_runs; i++) {
            int gap = runs[i].start - prev_end;
            if (gap > 0) { memcpy(out + o, skeleton + sk, gap); o += gap; sk += gap; }
            o += b64_encode_chunk(decoded + dc, runs[i].dec_len, out + o);
            dc += runs[i].dec_len;
            prev_end = runs[i].start + runs[i].len;
        }
        int remaining = skel_orig - sk;
        if (remaining > 0) { memcpy(out + o, skeleton + sk, remaining); o += remaining; }

        free(runs); free(skeleton); free(decoded);
        return orig_size;
    }


    /* LZ77+BWT decompression */
    if (strat == S_LZ77_O0 || strat == S_LZ77_O1) {
        int lz_raw_size = get_u32be(cdata);
        int lz_pidx     = get_u32be(cdata + 4);
        int lz_nz       = get_u32be(cdata + 8);
        const uint8_t *lz_arith = cdata + 12;

        uint16_t *lz_zrle = (uint16_t*)malloc(lz_nz * sizeof(uint16_t));
        uint8_t  *lz_mtf  = (uint8_t*)malloc(lz_raw_size);
        uint8_t  *lz_bwt  = (uint8_t*)malloc(lz_raw_size);
        uint8_t  *lz_packed = (uint8_t*)malloc(lz_raw_size);

        if (strat == S_LZ77_O1) arith_dec_o1(lz_arith, lz_nz, lz_zrle);
        else                    arith_dec_o0(lz_arith, lz_nz, lz_zrle);

        zrle_decode(lz_zrle, lz_nz, lz_mtf, lz_raw_size);
        mtf_decode(lz_mtf, lz_raw_size, lz_bwt);
        bwt_decode(lz_bwt, lz_raw_size, lz_pidx, lz_packed);
        lz77_decode(lz_packed, lz_raw_size, out, orig_size);

        free(lz_zrle); free(lz_mtf); free(lz_bwt); free(lz_packed);
        return orig_size;
    }

        int is_lzp = (strat == S_LZP_BWT_O0 || strat == S_LZP_BWT_O1 || strat == S_LZP_BWT_O2 ||
                  strat == S_DLZP_BWT_O0 || strat == S_DLZP_BWT_O1 || strat == S_DLZP_BWT_O2);
    int offset = 0;
    int lzp_size = 0;

    if (is_lzp) {
        lzp_size = get_u32be(cdata);
        offset = 4;
    }

    int pidx = get_u32be(cdata + offset);
    int nz   = get_u32be(cdata + offset + 4);
    const uint8_t *arith_data = cdata + offset + 8;

    int decode_size = is_lzp ? lzp_size : orig_size;

    uint16_t *zrle_buf = (uint16_t*)malloc(nz * sizeof(uint16_t));
    uint8_t  *mtf_buf  = (uint8_t*)malloc(decode_size);
    uint8_t  *bwt_buf  = (uint8_t*)malloc(decode_size);

    int is_o2 = (strat == S_BWT_O2 || strat == S_DBWT_O2 ||
                 strat == S_LZP_BWT_O2 || strat == S_DLZP_BWT_O2);
    int is_o1 = (strat == S_BWT_O1 || strat == S_DBWT_O1 ||
                 strat == S_LZP_BWT_O1 || strat == S_DLZP_BWT_O1);
    if (is_o2)                        arith_dec_o2(arith_data, nz, zrle_buf);
    else if (is_o1)                   arith_dec_o1(arith_data, nz, zrle_buf);
    else if (strat == S_BWT_O0_PS)    arith_dec_o0_ps(arith_data, nz, zrle_buf);
    else if (strat == S_BWT_RANS)     rans_dec_o0(arith_data, nz, zrle_buf);
    else if (strat == S_BWT_CTX2)     arith_dec_ctx2(arith_data, nz, zrle_buf);
    else if (strat == S_BWT_O1_PS)    arith_dec_o1_ps(arith_data, nz, zrle_buf);
    else                              arith_dec_o0(arith_data, nz, zrle_buf);

    zrle_decode(zrle_buf, nz, mtf_buf, decode_size);
    mtf_decode(mtf_buf, decode_size, bwt_buf);

    if (is_lzp) {
        /* BWT decode into temp buffer, then LZP decode to out */
        uint8_t *lzp_dec_buf = (uint8_t*)malloc(decode_size);
        bwt_decode(bwt_buf, decode_size, pidx, lzp_dec_buf);
        lzp_decode(lzp_dec_buf, lzp_size, out, orig_size);
        free(lzp_dec_buf);
    } else {
        bwt_decode(bwt_buf, decode_size, pidx, out);
    }

    /* Handle delta decode */
    int is_delta = (strat == S_DBWT_O0 || strat == S_DBWT_O1 || strat == S_DBWT_O2 ||
                    strat == S_DLZP_BWT_O0 || strat == S_DLZP_BWT_O1 || strat == S_DLZP_BWT_O2);
    if (is_delta) {
        uint8_t *tmp = (uint8_t*)malloc(orig_size);
        memcpy(tmp, out, orig_size);
        delta_decode(tmp, orig_size, out);
        free(tmp);
    }

    if (strat == S_F32_BWT) {
        uint8_t *tmp = (uint8_t*)malloc(orig_size);
        memcpy(tmp, out, orig_size);
        float_xor_decode(tmp, orig_size, out);
        free(tmp);
    }

    free(zrle_buf); free(mtf_buf); free(bwt_buf);
    return orig_size;
}

/* ===== FNV-1a Hash (for block dedup) ===== */
static uint64_t fnv1a(const uint8_t *data, int n) {
    uint64_t h = 14695981039346656037ull;
    for (int i = 0; i < n; i++) h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

/* ===== File I/O Helpers ===== */
static void write32(FILE *f, uint32_t v) {
    uint8_t b[4] = { v>>24, v>>16, v>>8, v };
    fwrite(b, 1, 4, f);
}
static uint32_t read32(FILE *f) {
    uint8_t b[4]; fread(b, 1, 4, f);
    return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
}
static void write64(FILE *f, uint64_t v) {
    write32(f, (uint32_t)(v >> 32));
    write32(f, (uint32_t)(v & 0xFFFFFFFF));
}
static uint64_t read64(FILE *f) {
    uint64_t hi = read32(f), lo = read32(f);
    return (hi << 32) | lo;
}


/* ===== Phase 2: Parallel Block Compression ===== */
typedef struct {
    const uint8_t *input;
    int input_size;
    uint8_t *output;
    int output_size;
    int strategy;
    uint32_t crc;
    uint64_t hash;
    BWTWorkspace *bwt_ws;
    CompressWorkspace *cws;
    volatile int done;
} BlockJob;

static void *compress_block_worker(void *arg) {
    BlockJob *job = (BlockJob *)arg;
    job->output_size = compress_block(job->input, job->input_size, 
                                       job->output, &job->strategy, 
                                       job->bwt_ws, job->cws);
    job->crc = hp_crc32(job->input, job->input_size);
    job->done = 1;
    return NULL;
}

/* ===== PNG Pre-Transform ===== */

static const uint8_t HP_PNG_SIG[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};

/* PNG chunk CRC: CRC32 over type(4 bytes) + data(len bytes) */
static uint32_t png_chunk_crc(const uint8_t *type, const uint8_t *data, uint32_t len) {
    uint32_t c = hp_crc32_chain(0, type, 4);
    if (len > 0) c = hp_crc32_chain(c, data, len);
    return c;
}

static void fwrite_be32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v};
    fwrite(b, 1, 4, f);
}

static uint32_t buf_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

/*
 * png_extract: read a PNG file, inflate its IDAT stream into raw filtered scanlines.
 *   meta_out / meta_size  = PNG signature + all non-IDAT/IEND chunks (caller frees)
 *   data_out / data_size  = inflated IDAT scanlines (caller frees)
 * Returns 0 on success.
 */
static int png_extract(FILE *fin, int64_t fsize,
                        uint8_t **meta_out, uint32_t *meta_size,
                        uint8_t **data_out, size_t *data_size)
{
    uint8_t *filebuf = (uint8_t*)malloc((size_t)fsize);
    if (!filebuf) return 1;
    fseek(fin, 0, SEEK_SET);
    if ((int64_t)fread(filebuf, 1, (size_t)fsize, fin) != fsize) {
        free(filebuf); return 1;
    }
    if (fsize < 8 || memcmp(filebuf, HP_PNG_SIG, 8) != 0) { free(filebuf); return 1; }

    uint8_t *meta_buf = (uint8_t*)malloc((size_t)fsize);
    uint8_t *idat_buf = (uint8_t*)malloc((size_t)fsize);
    if (!meta_buf || !idat_buf) {
        free(filebuf); free(meta_buf); free(idat_buf); return 1;
    }
    size_t meta_len = 0, idat_len = 0;
    memcpy(meta_buf, HP_PNG_SIG, 8);
    meta_len = 8;

    size_t pos = 8;
    while (pos + 12 <= (size_t)fsize) {
        uint32_t chunk_len = buf_be32(filebuf + pos);
        if (pos + 12 + chunk_len > (size_t)fsize) break;
        const uint8_t *chunk_type = filebuf + pos + 4;
        const uint8_t *chunk_data = filebuf + pos + 8;
        if (memcmp(chunk_type, "IDAT", 4) == 0) {
            memcpy(idat_buf + idat_len, chunk_data, chunk_len);
            idat_len += chunk_len;
        } else if (memcmp(chunk_type, "IEND", 4) == 0) {
            break;
        } else {
            memcpy(meta_buf + meta_len, filebuf + pos, 12 + chunk_len);
            meta_len += 12 + chunk_len;
        }
        pos += 12 + chunk_len;
    }
    free(filebuf);
    if (idat_len == 0) { free(meta_buf); free(idat_buf); return 1; }

    /* Inflate IDAT (zlib-wrapped deflate); retry with larger buffer if needed */
    size_t out_alloc = idat_len * 4 + (1 << 20);
    uint8_t *out_buf = (uint8_t*)malloc(out_alloc);
    if (!out_buf) { free(meta_buf); free(idat_buf); return 1; }
    for (;;) {
        z_stream zs; memset(&zs, 0, sizeof(zs));
        zs.next_in  = idat_buf;
        zs.avail_in = (uInt)idat_len;
        zs.next_out = out_buf;
        zs.avail_out = (uInt)(out_alloc < 0xFFFFFFFFu ? out_alloc : 0xFFFFFFFFu);
        if (inflateInit(&zs) != Z_OK) {
            free(meta_buf); free(idat_buf); free(out_buf); return 1;
        }
        int ret = inflate(&zs, Z_FINISH);
        size_t got = zs.total_out;
        inflateEnd(&zs);
        if (ret == Z_STREAM_END) {
            free(idat_buf);
            *meta_out  = meta_buf; *meta_size = (uint32_t)meta_len;
            *data_out  = out_buf;  *data_size = got;
            return 0;
        } else if (ret == Z_BUF_ERROR || zs.avail_out == 0) {
            out_alloc *= 2;
            free(out_buf);
            out_buf = (uint8_t*)malloc(out_alloc);
            if (!out_buf) { free(meta_buf); free(idat_buf); return 1; }
        } else {
            free(meta_buf); free(idat_buf); free(out_buf); return 1;
        }
    }
}

/*
 * png_reconstruct: deflate the inflated scanlines and write a valid PNG to outpath.
 * meta = PNG signature + all original non-IDAT/IEND chunks.
 * Returns 0 on success.
 */
static int png_reconstruct(const uint8_t *meta, uint32_t meta_size,
                             const uint8_t *inflated, size_t inflated_size,
                             const char *outpath)
{
    uLong deflate_bound = compressBound((uLong)inflated_size);
    uint8_t *idat_data = (uint8_t*)malloc((size_t)deflate_bound);
    if (!idat_data) return 1;
    uLong idat_size = deflate_bound;
    int zret = compress2(idat_data, &idat_size,
                         (const Bytef*)inflated, (uLong)inflated_size,
                         Z_DEFAULT_COMPRESSION);
    if (zret != Z_OK) { free(idat_data); return 1; }

    FILE *fout = fopen(outpath, "wb");
    if (!fout) { free(idat_data); return 1; }
    fwrite(meta, 1, meta_size, fout);
    /* IDAT chunk */
    fwrite_be32(fout, (uint32_t)idat_size);
    fwrite("IDAT", 1, 4, fout);
    fwrite(idat_data, 1, (size_t)idat_size, fout);
    fwrite_be32(fout, png_chunk_crc((const uint8_t*)"IDAT", idat_data, (uint32_t)idat_size));
    /* IEND chunk */
    fwrite_be32(fout, 0);
    fwrite("IEND", 1, 4, fout);
    fwrite_be32(fout, png_chunk_crc((const uint8_t*)"IEND", NULL, 0));
    fclose(fout);
    free(idat_data);
    return 0;
}

/* ===== File Compress ===== */
/*
 * File format V11:
 *   MAGIC(4) VERSION(1) PRETRANSFORM(1) [if PT!=PT_NONE: META_SIZE(4) META(meta_size)]
 *   BLOCK_SIZE(4) ORIG_SIZE(8) NBLOCKS(4)
 * File format V5-V10 (legacy):
 *   MAGIC(4) VERSION(1) BLOCK_SIZE(4) ORIG_SIZE(8) NBLOCKS(4)
 *   For each block:
 *     FLAGS(1)  -- bit 0: is_dup
 *     if is_dup: DUP_REF(4)
 *     else: STRATEGY(1) COMP_SIZE(4) ORIG_BLOCK_SIZE(4) CRC32(4) DATA(comp_size)
 */
static int file_compress(const char *inpath, const char *outpath, int block_size, int nthreads) {
    FILE *fin = fopen(inpath, "rb");
    if (!fin) { fprintf(stderr, "Cannot open %s\n", inpath); return 1; }
    fseek(fin, 0, SEEK_END);
    int64_t fsize = ftell(fin); fseek(fin, 0, SEEK_SET);

    /* Auto-detect PNG and inflate its IDAT for better HP compression */
    uint8_t pretransform = PT_NONE;
    uint8_t *png_meta = NULL; uint32_t png_meta_size = 0;
    uint8_t *transformed_data = NULL; size_t transformed_size = 0;
    {
        uint8_t sig[8];
        if (fsize >= 8 && fread(sig, 1, 8, fin) == 8 &&
            memcmp(sig, HP_PNG_SIG, 8) == 0) {
            if (png_extract(fin, fsize, &png_meta, &png_meta_size,
                            &transformed_data, &transformed_size) == 0) {
                pretransform = PT_PNG;
                fprintf(stderr, "[HP5] PNG detected: %.2f MB compressed → %.2f MB inflated scanlines\n",
                        fsize/1048576.0, transformed_size/1048576.0);
                /* Replace fin with an in-memory stream over the inflated data */
                fclose(fin);
                fin = fmemopen(transformed_data, transformed_size, "rb");
                fsize = (int64_t)transformed_size;
            }
        }
        if (pretransform == PT_NONE) fseek(fin, 0, SEEK_SET);
    }

    int nblocks = (int)((fsize + block_size - 1) / block_size);
    fprintf(stderr, "[HP5] Compressing %s (%.2f MB, %d blocks of %d MB)\n",
            inpath, fsize / 1048576.0, nblocks, block_size >> 20);

    FILE *fout = fopen(outpath, "wb");
    if (!fout) { fprintf(stderr, "Cannot create %s\n", outpath); fclose(fin); return 1; }

    /* Write V11 header: MAGIC VERSION PRETRANSFORM [META_SIZE META] BLOCK_SIZE ORIG_SIZE NBLOCKS */
    write32(fout, MAGIC);
    fputc(VERSION, fout);
    fputc(pretransform, fout);
    if (pretransform != PT_NONE) {
        write32(fout, png_meta_size);
        fwrite(png_meta, 1, png_meta_size, fout);
    }
    write32(fout, (uint32_t)block_size);
    write64(fout, (uint64_t)fsize);
    write32(fout, (uint32_t)nblocks);

    uint8_t *inbuf  = (uint8_t*)malloc(block_size);
    uint8_t *outbuf = (uint8_t*)malloc(block_size + block_size/4 + 4096);

    /* Dedup tracking */
    uint64_t *hashes = (uint64_t*)calloc(nblocks, sizeof(uint64_t));
    int *comp_sizes  = (int*)calloc(nblocks, sizeof(int));
    int *strategies  = (int*)calloc(nblocks, sizeof(int));

    int64_t total_comp = 0;
    clock_t start = clock();

    /* Phase 2: Parallel block compression with nthreads workers */
    if (nthreads <= 1 || nblocks <= 1) {
        /* Sequential path (original behavior) */
        BWTWorkspace *bwt_ws = bwt_ws_create(block_size + 262144);
        CompressWorkspace *cws = cws_create(block_size);
        
        for (int b = 0; b < nblocks; b++) {
            int bsz = (int)fread(inbuf, 1, block_size, fin);
            if (bsz <= 0) break;
            uint64_t hash = fnv1a(inbuf, bsz);
            hashes[b] = hash;

            int dup_ref = -1;
            for (int p = 0; p < b; p++) {
                if (hashes[p] == hash) { dup_ref = p; break; }
            }

            if (dup_ref >= 0) {
                fputc(1, fout);
                write32(fout, (uint32_t)dup_ref);
                total_comp += 5;
                fprintf(stderr, "  Block %d/%d: DUP of %d\n", b+1, nblocks, dup_ref+1);
            } else {
                int strat;
                int csz = compress_block(inbuf, bsz, outbuf, &strat, bwt_ws, cws);
                uint32_t crc = hp_crc32(inbuf, bsz);

                fputc(0, fout);
                fputc((uint8_t)strat, fout);
                write32(fout, (uint32_t)csz);
                write32(fout, (uint32_t)bsz);
                write32(fout, crc);
                fwrite(outbuf, 1, csz, fout);

                total_comp += 14 + csz;
                strategies[b] = strat;
                comp_sizes[b] = csz;

                double ratio = (double)bsz / csz;
                fprintf(stderr, "  Block %d/%d: %d -> %d (%.2fx) [%s]\n",
                        b+1, nblocks, bsz, csz, ratio, strat_names[strat]);
            }
        }
        bwt_ws_free(bwt_ws);
        cws_free(cws);
    } else {
        /* Parallel path: process blocks in batches of nthreads */
        int jt = nthreads;
        if (jt > nblocks) jt = nblocks;
        
        BlockJob *jobs = (BlockJob*)calloc(jt, sizeof(BlockJob));
        pthread_t *threads = (pthread_t*)calloc(jt, sizeof(pthread_t));
        
        for (int j = 0; j < jt; j++) {
            jobs[j].bwt_ws = bwt_ws_create(block_size + 262144);
            jobs[j].cws = cws_create(block_size);
            jobs[j].output = (uint8_t*)malloc(block_size + block_size/4 + 4096);
        }
        
        int b = 0;
        while (b < nblocks) {
            int batch = nblocks - b;
            if (batch > jt) batch = jt;
            
            /* Read blocks and compute hashes */
            uint8_t **block_data = (uint8_t**)calloc(batch, sizeof(uint8_t*));
            int *block_sizes_arr = (int*)calloc(batch, sizeof(int));
            int *dup_refs = (int*)calloc(batch, sizeof(int));
            
            for (int i = 0; i < batch; i++) {
                block_data[i] = (uint8_t*)malloc(block_size);
                block_sizes_arr[i] = (int)fread(block_data[i], 1, block_size, fin);
                if (block_sizes_arr[i] <= 0) { batch = i; break; }
                
                uint64_t hash = fnv1a(block_data[i], block_sizes_arr[i]);
                hashes[b + i] = hash;
                
                dup_refs[i] = -1;
                for (int p = 0; p < b + i; p++) {
                    if (hashes[p] == hash) { dup_refs[i] = p; break; }
                }
            }
            
            /* Launch compression threads for non-duplicate blocks */
            int launched = 0;
            for (int i = 0; i < batch; i++) {
                if (dup_refs[i] >= 0) continue;
                
                int j = launched % jt;
                jobs[j].input = block_data[i];
                jobs[j].input_size = block_sizes_arr[i];
                jobs[j].done = 0;
                pthread_create(&threads[launched], NULL, compress_block_worker, &jobs[j]);
                launched++;
            }
            
            /* Wait for all threads */
            for (int i = 0; i < launched; i++) {
                pthread_join(threads[i], NULL);
            }
            
            /* Write results in order */
            int job_idx = 0;
            for (int i = 0; i < batch; i++) {
                if (dup_refs[i] >= 0) {
                    fputc(1, fout);
                    write32(fout, (uint32_t)dup_refs[i]);
                    total_comp += 5;
                    fprintf(stderr, "  Block %d/%d: DUP of %d\n", b+i+1, nblocks, dup_refs[i]+1);
                } else {
                    int j = job_idx % jt;
                    int strat = jobs[j].strategy;
                    int csz = jobs[j].output_size;
                    uint32_t crc = jobs[j].crc;
                    
                    fputc(0, fout);
                    fputc((uint8_t)strat, fout);
                    write32(fout, (uint32_t)csz);
                    write32(fout, (uint32_t)block_sizes_arr[i]);
                    write32(fout, crc);
                    fwrite(jobs[j].output, 1, csz, fout);
                    
                    total_comp += 14 + csz;
                    strategies[b + i] = strat;
                    comp_sizes[b + i] = csz;
                    
                    double ratio = (double)block_sizes_arr[i] / csz;
                    fprintf(stderr, "  Block %d/%d: %d -> %d (%.2fx) [%s]\n",
                            b+i+1, nblocks, block_sizes_arr[i], csz, ratio, strat_names[strat]);
                    job_idx++;
                }
            }
            
            for (int i = 0; i < batch; i++) free(block_data[i]);
            free(block_data);
            free(block_sizes_arr);
            free(dup_refs);
            b += batch;
        }
        
        for (int j = 0; j < jt; j++) {
            bwt_ws_free(jobs[j].bwt_ws);
            cws_free(jobs[j].cws);
            free(jobs[j].output);
        }
        free(jobs);
        free(threads);
    }

    double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
    /* V11 header: MAGIC(4) VERSION(1) PRETRANSFORM(1) [META_SIZE(4) META] BLOCK_SIZE(4) ORIG_SIZE(8) NBLOCKS(4) */
    int64_t header_size = 6 + (pretransform != PT_NONE ? 4 + (int64_t)png_meta_size : 0) + 16;
    int64_t file_total = header_size + total_comp;
    fprintf(stderr, "[HP5] Done: %lld -> %lld bytes (%.2fx) in %.1fs\n",
            (long long)fsize, (long long)file_total,
            (double)fsize / file_total, elapsed);

    fclose(fin); fclose(fout);
    free(inbuf); free(outbuf); free(hashes); free(comp_sizes); free(strategies);
    if (transformed_data) free(transformed_data);
    if (png_meta) free(png_meta);
    return 0;
}

/* ===== Parallel Block Decompression Helpers ===== */
#define HP_PAR_THREADS 4

typedef struct {
    int is_dup;
    uint32_t dup_ref;
    int strat, csz, orig_b;
    uint32_t expected_crc;
    uint8_t *cdata;   /* compressed data (malloced, NULL for dup) */
    uint8_t *outbuf;  /* decompressed output (malloced, NULL for dup) */
    int crc_ok;
} HpParBlock;

typedef struct {
    HpParBlock *blocks;
    int n;
    pthread_mutex_t mu;
    int next;
} HpParQ;

static void *hp_par_decomp_worker(void *arg) {
    HpParQ *q = (HpParQ*)arg;
    for (;;) {
        pthread_mutex_lock(&q->mu);
        int i = q->next++;
        pthread_mutex_unlock(&q->mu);
        if (i >= q->n) break;
        HpParBlock *b = &q->blocks[i];
        if (b->is_dup) continue;
        decompress_block(b->cdata, b->csz, b->strat, b->orig_b, b->outbuf);
        b->crc_ok = (hp_crc32(b->outbuf, b->orig_b) == b->expected_crc);
    }
    return NULL;
}

/* ===== File Decompress ===== */
static int file_decompress(const char *inpath, const char *outpath) {
    FILE *fin = fopen(inpath, "rb");
    if (!fin) { fprintf(stderr, "Cannot open %s\n", inpath); return 1; }

    uint32_t magic = read32(fin);
    if (magic != MAGIC) { fprintf(stderr, "Not a HPK5 file\n"); fclose(fin); return 1; }
    int ver = fgetc(fin);
    if (ver < 5 || ver > VERSION) { fprintf(stderr, "Unsupported version %d\n", ver); fclose(fin); return 1; }

    /* V11+: read PRETRANSFORM byte and optional metadata */
    uint8_t pretransform = PT_NONE;
    uint8_t *png_meta = NULL; uint32_t png_meta_size = 0;
    if (ver >= 11) {
        pretransform = (uint8_t)fgetc(fin);
        if (pretransform == PT_PNG) {
            png_meta_size = read32(fin);
            png_meta = (uint8_t*)malloc(png_meta_size > 0 ? png_meta_size : 1);
            if (!png_meta) { fclose(fin); return 1; }
            fread(png_meta, 1, png_meta_size, fin);
        }
    }

    uint32_t block_size = read32(fin);
    uint64_t orig_size  = read64(fin);
    uint32_t nblocks    = read32(fin);

    fprintf(stderr, "[HP5] Decompressing (%.2f MB, %d blocks)%s\n",
            orig_size/1048576.0, nblocks,
            pretransform == PT_PNG ? " [PNG rebuild]" : "");

    FILE *fout = fopen(outpath, "wb");
    if (!fout) { fprintf(stderr, "Cannot create %s\n", outpath); fclose(fin); return 1; }

    /* Store decompressed blocks for dedup references */
    uint8_t **block_cache = (uint8_t**)calloc(nblocks, sizeof(uint8_t*));
    int *block_sizes = (int*)calloc(nblocks, sizeof(int));

    clock_t start = clock();
    int error = 0;

    /* Process blocks in parallel batches of HP_PAR_THREADS.
     * Non-dup blocks within a batch are decompressed simultaneously.
     * Dup blocks (fast memcpy) are handled after their batch's threads join. */
    HpParBlock batch[HP_PAR_THREADS];
    uint32_t b = 0;
    while (b < nblocks && !error) {
        /* Fill batch */
        int bsz = 0;
        while (bsz < HP_PAR_THREADS && b < nblocks) {
            HpParBlock *pb = &batch[bsz];
            int flags = fgetc(fin);
            pb->is_dup = (flags & 1);
            pb->cdata  = NULL;
            pb->outbuf = NULL;
            pb->crc_ok = 0;
            if (pb->is_dup) {
                pb->dup_ref = read32(fin);
            } else {
                pb->strat  = fgetc(fin);
                pb->csz    = (int)read32(fin);
                pb->orig_b = (int)read32(fin);
                pb->expected_crc = read32(fin);
                pb->cdata  = (uint8_t*)malloc(pb->csz);
                pb->outbuf = (uint8_t*)malloc(pb->orig_b > 0 ? pb->orig_b : 1);
                fread(pb->cdata, 1, pb->csz, fin);
            }
            bsz++;
            b++;
        }

        /* Launch parallel decompression for non-dup blocks in this batch */
        HpParQ q;
        q.blocks = batch;
        q.n = bsz;
        q.next = 0;
        pthread_mutex_init(&q.mu, NULL);
        int nt = bsz < HP_PAR_THREADS ? bsz : HP_PAR_THREADS;
        pthread_t threads[HP_PAR_THREADS];
        for (int t = 0; t < nt; t++)
            pthread_create(&threads[t], NULL, hp_par_decomp_worker, &q);
        for (int t = 0; t < nt; t++)
            pthread_join(threads[t], NULL);
        pthread_mutex_destroy(&q.mu);

        /* Write batch output in order, handle dup blocks */
        uint32_t base_b = b - bsz;
        for (int i = 0; i < bsz; i++) {
            HpParBlock *pb = &batch[i];
            uint32_t blk_idx = base_b + (uint32_t)i;
            if (pb->is_dup) {
                uint32_t ref = pb->dup_ref;
                fwrite(block_cache[ref], 1, block_sizes[ref], fout);
                block_cache[blk_idx] = (uint8_t*)malloc(block_sizes[ref]);
                memcpy(block_cache[blk_idx], block_cache[ref], block_sizes[ref]);
                block_sizes[blk_idx] = block_sizes[ref];
                fprintf(stderr, "  Block %d/%d: DUP of %d\n", blk_idx+1, nblocks, ref+1);
            } else {
                if (!pb->crc_ok) {
                    fprintf(stderr, "  Block %d: CRC MISMATCH!\n", blk_idx+1);
                    error = 1;
                }
                fwrite(pb->outbuf, 1, pb->orig_b, fout);
                block_cache[blk_idx] = pb->outbuf;  /* transfer ownership */
                pb->outbuf = NULL;
                block_sizes[blk_idx] = pb->orig_b;
                fprintf(stderr, "  Block %d/%d: %d -> %d [%s] CRC %s\n",
                        blk_idx+1, nblocks, pb->csz, pb->orig_b,
                        strat_names[pb->strat], pb->crc_ok ? "OK" : "FAIL");
                free(pb->cdata);
                pb->cdata = NULL;
            }
        }
        /* Free any unwritten outbufs on error */
        if (error) {
            for (int i = 0; i < bsz; i++) {
                if (batch[i].cdata)  free(batch[i].cdata);
                if (batch[i].outbuf) free(batch[i].outbuf);
            }
        }
    }

    double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
    fprintf(stderr, "[HP5] Decompressed in %.1fs (%.1f MB/s)\n",
            elapsed, orig_size / 1048576.0 / (elapsed > 0.001 ? elapsed : 0.001));

    for (uint32_t b2 = 0; b2 < nblocks; b2++) free(block_cache[b2]);
    free(block_cache); free(block_sizes);
    fclose(fin); fclose(fout); fout = NULL;

    /* PT_PNG post-processing: read back inflated scanlines, rebuild PNG */
    if (pretransform == PT_PNG && !error) {
        FILE *ftmp = fopen(outpath, "rb");
        if (ftmp) {
            fseek(ftmp, 0, SEEK_END);
            size_t infl_sz = (size_t)ftell(ftmp);
            fseek(ftmp, 0, SEEK_SET);
            uint8_t *infl_buf = (uint8_t*)malloc(infl_sz > 0 ? infl_sz : 1);
            if (infl_buf && fread(infl_buf, 1, infl_sz, ftmp) == infl_sz) {
                fclose(ftmp);
                fprintf(stderr, "[HP5] Rebuilding PNG from %.2f MB inflated scanlines...\n",
                        infl_sz/1048576.0);
                error = png_reconstruct(png_meta, png_meta_size, infl_buf, infl_sz, outpath);
                if (!error) fprintf(stderr, "[HP5] PNG reconstructed successfully\n");
            } else {
                fclose(ftmp);
                error = 1;
            }
            free(infl_buf);
        } else {
            error = 1;
        }
    }
    if (png_meta) free(png_meta);
    return error;
}

/* ===== HPK6 Archive Functions ===== */

static void write16(FILE *f, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v>>8), (uint8_t)v };
    fwrite(b, 1, 2, f);
}
static uint16_t read16(FILE *f) {
    uint8_t b[2]; fread(b, 1, 2, f);
    return ((uint16_t)b[0]<<8)|b[1];
}

static uint32_t file_crc32(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (!crc_ready) crc_init();
    uint32_t c = 0xFFFFFFFFu;
    uint8_t buf[1 << 20]; /* 1MB */
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < n; i++)
            c = (c >> 8) ^ crc_tab[(c ^ buf[i]) & 0xFF];
    }
    fclose(f);
    return c ^ 0xFFFFFFFFu;
}

static int scan_path(const char *base, const char *rel,
                     HPK6Entry **entries, int *count, int *cap) {
    char fullpath[4096];
    if (rel[0])
        snprintf(fullpath, sizeof(fullpath), "%s/%s", base, rel);
    else
        snprintf(fullpath, sizeof(fullpath), "%s", base);

    struct stat st;
    if (stat(fullpath, &st) != 0) {
        fprintf(stderr, "Cannot stat '%s': %s\n", fullpath, strerror(errno));
        return -1;
    }

    if (*count >= *cap) {
        *cap = (*cap < 64) ? 64 : (*cap * 2);
        *entries = (HPK6Entry*)realloc(*entries, (size_t)*cap * sizeof(HPK6Entry));
    }

    HPK6Entry *e = &(*entries)[*count];
    memset(e, 0, sizeof(HPK6Entry));
    e->path = strdup(rel[0] ? rel : "");
    e->fullpath = strdup(fullpath);
    e->perms = (uint32_t)(st.st_mode & 07777);
    e->mtime = (int64_t)st.st_mtime;

    if (S_ISDIR(st.st_mode)) {
        e->type = 1;
        e->size = 0;
        e->first_block = 0xFFFFFFFF;
        (*count)++;

        DIR *d = opendir(fullpath);
        if (!d) { fprintf(stderr, "Cannot open dir '%s'\n", fullpath); return -1; }
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.' && (de->d_name[1] == 0 ||
                (de->d_name[1] == '.' && de->d_name[2] == 0))) continue;
            char child_rel[4096];
            if (rel[0])
                snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, de->d_name);
            else
                snprintf(child_rel, sizeof(child_rel), "%s", de->d_name);
            if (scan_path(base, child_rel, entries, count, cap) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        e->type = 0;
        e->size = (uint64_t)st.st_size;
        (*count)++;
    }
    return 0;
}

static void detect_file_duplicates(HPK6Entry *entries, int count) {
    for (int i = 0; i < count; i++) {
        if (entries[i].type != 0 || entries[i].is_dedup) continue;
        if (entries[i].size == 0) continue;
        for (int j = i + 1; j < count; j++) {
            if (entries[j].type != 0 || entries[j].is_dedup) continue;
            if (entries[j].size == entries[i].size && entries[j].crc == entries[i].crc) {
                entries[j].is_dedup = 1;
                entries[j].dedup_ref = (uint32_t)i;
                entries[j].first_block = 0xFFFFFFFF;
                entries[j].nblocks = 0;
                fprintf(stderr, "  [DEDUP] '%s' == '%s'\n", entries[j].path, entries[i].path);
            }
        }
    }
}

static void write_catalog(FILE *f, HPK6Entry *entries, int count) {
    for (int i = 0; i < count; i++) {
        HPK6Entry *e = &entries[i];
        fputc(e->type, f);
        uint16_t pathlen = (uint16_t)strlen(e->path);
        write16(f, pathlen);
        fwrite(e->path, 1, pathlen, f);
        write64(f, e->size);
        write32(f, e->perms);
        write64(f, (uint64_t)e->mtime);
        write32(f, e->crc);
        write32(f, e->first_block);
        write32(f, e->nblocks);
        fputc(e->is_dedup, f);
        write32(f, e->dedup_ref);
    }
}

static HPK6Entry *read_catalog(FILE *f, int count) {
    HPK6Entry *entries = (HPK6Entry*)calloc(count, sizeof(HPK6Entry));
    for (int i = 0; i < count; i++) {
        HPK6Entry *e = &entries[i];
        e->type = (uint8_t)fgetc(f);
        uint16_t pathlen = read16(f);
        e->path = (char*)malloc(pathlen + 1);
        fread(e->path, 1, pathlen, f);
        e->path[pathlen] = '\0';
        e->size = read64(f);
        e->perms = read32(f);
        e->mtime = (int64_t)read64(f);
        e->crc = read32(f);
        e->first_block = read32(f);
        e->nblocks = read32(f);
        e->is_dedup = (uint8_t)fgetc(f);
        e->dedup_ref = read32(f);
    }
    return entries;
}

static void mkdirs(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int archive_compress(int npaths, const char **paths, const char *outpath,
                            int block_size, int nthreads) {
    (void)nthreads;
    clock_t start = clock();

    /* Phase 1: Scan all input paths */
    HPK6Entry *entries = NULL;
    int entry_count = 0, entry_cap = 0;

    for (int p = 0; p < npaths; p++) {
        struct stat st;
        if (stat(paths[p], &st) != 0) {
            fprintf(stderr, "Cannot access '%s': %s\n", paths[p], strerror(errno));
            return 1;
        }

        if (S_ISDIR(st.st_mode)) {
            char *base = strdup(paths[p]);
            int len = (int)strlen(base);
            while (len > 1 && base[len-1] == '/') base[--len] = '\0';

            const char *bname = strrchr(base, '/');
            bname = bname ? bname + 1 : base;

            int root_idx = entry_count;
            scan_path(base, "", &entries, &entry_count, &entry_cap);
            if (root_idx < entry_count) {
                free(entries[root_idx].path);
                entries[root_idx].path = strdup(bname);
            }
            for (int i = root_idx + 1; i < entry_count; i++) {
                char newpath[4096];
                snprintf(newpath, sizeof(newpath), "%s/%s", bname, entries[i].path);
                free(entries[i].path);
                entries[i].path = strdup(newpath);
            }
            free(base);
        } else {
            if (entry_count >= entry_cap) {
                entry_cap = (entry_cap < 64) ? 64 : (entry_cap * 2);
                entries = (HPK6Entry*)realloc(entries, (size_t)entry_cap * sizeof(HPK6Entry));
            }
            HPK6Entry *e = &entries[entry_count];
            memset(e, 0, sizeof(HPK6Entry));

            const char *bname = strrchr(paths[p], '/');
            bname = bname ? bname + 1 : paths[p];
            e->path = strdup(bname);
            e->fullpath = strdup(paths[p]);
            e->type = 0;
            e->size = (uint64_t)st.st_size;
            e->perms = (uint32_t)(st.st_mode & 07777);
            e->mtime = (int64_t)st.st_mtime;
            entry_count++;
        }
    }

    fprintf(stderr, "[HPK6] Scanned %d entries\n", entry_count);

    /* Phase 2: Compute CRC32 for all files + detect dedup */
    uint64_t total_size = 0;
    int file_count = 0;
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].type == 0) {
            entries[i].crc = file_crc32(entries[i].fullpath);
            total_size += entries[i].size;
            file_count++;
        }
    }
    detect_file_duplicates(entries, entry_count);

    /* Count unique files and compute total blocks */
    uint32_t total_blocks = 0;
    uint32_t block_idx = 0;
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].type != 0 || entries[i].is_dedup) continue;
        uint32_t nblocks = (uint32_t)((entries[i].size + block_size - 1) / block_size);
        if (entries[i].size == 0) nblocks = 0;
        entries[i].first_block = block_idx;
        entries[i].nblocks = nblocks;
        block_idx += nblocks;
        total_blocks += nblocks;
    }
    for (int i = 0; i < entry_count; i++) {
        if (entries[i].is_dedup) {
            entries[i].first_block = entries[entries[i].dedup_ref].first_block;
            entries[i].nblocks = entries[entries[i].dedup_ref].nblocks;
        }
    }

    fprintf(stderr, "[HPK6] %d files, %d dirs, %.2f MB total, %d blocks\n",
            file_count, entry_count - file_count,
            total_size / 1048576.0, total_blocks);

    /* Phase 3: Write header + catalog */
    FILE *fout = fopen(outpath, "wb");
    if (!fout) { fprintf(stderr, "Cannot create '%s'\n", outpath); return 1; }

    write32(fout, MAGIC6);
    fputc(VERSION, fout);
    write32(fout, (uint32_t)block_size);
    write32(fout, (uint32_t)entry_count);
    write32(fout, total_blocks);
    write64(fout, total_size);

    write_catalog(fout, entries, entry_count);

    /* Phase 4: Compress blocks for each unique file */
    BWTWorkspace *bwt_ws = bwt_ws_create(block_size);
    CompressWorkspace *cws = cws_create(block_size);
    uint8_t *inbuf = (uint8_t*)malloc(block_size);
    uint8_t *outbuf = (uint8_t*)malloc(block_size + block_size/4 + 4096);

    uint64_t *block_offsets = (uint64_t*)calloc(total_blocks ? total_blocks : 1, sizeof(uint64_t));
    uint64_t *block_hashes = (uint64_t*)calloc(total_blocks ? total_blocks : 1, sizeof(uint64_t));
    int64_t total_compressed = 0;
    uint32_t global_block = 0;

    for (int i = 0; i < entry_count; i++) {
        if (entries[i].type != 0 || entries[i].is_dedup || entries[i].size == 0) continue;

        FILE *fin = fopen(entries[i].fullpath, "rb");
        if (!fin) {
            fprintf(stderr, "Cannot open '%s'\n", entries[i].fullpath);
            bwt_ws_free(bwt_ws); cws_free(cws);
            free(inbuf); free(outbuf); free(block_offsets); free(block_hashes);
            fclose(fout);
            return 1;
        }

        uint64_t remaining = entries[i].size;
        uint32_t file_block = 0;

        while (remaining > 0) {
            int bsz = (remaining > (uint64_t)block_size) ? block_size : (int)remaining;
            fread(inbuf, 1, bsz, fin);
            remaining -= bsz;

            block_offsets[global_block] = (uint64_t)ftell(fout);

            uint64_t hash = fnv1a(inbuf, bsz);
            block_hashes[global_block] = hash;
            int dup_ref = -1;
            for (uint32_t pp = 0; pp < global_block; pp++) {
                if (block_hashes[pp] == hash) { dup_ref = (int)pp; break; }
            }

            if (dup_ref >= 0) {
                fputc(1, fout);
                write32(fout, (uint32_t)dup_ref);
                total_compressed += 5;
                fprintf(stderr, "  Block %d [%s:%d/%d]: DUP of block %d\n",
                        global_block+1, entries[i].path, file_block+1, entries[i].nblocks, dup_ref+1);
            } else {
                int strat;
                int csz = compress_block(inbuf, bsz, outbuf, &strat, bwt_ws, cws);
                uint32_t blk_crc = hp_crc32(inbuf, bsz);

                fputc(0, fout);
                fputc(strat, fout);
                write32(fout, (uint32_t)csz);
                write32(fout, (uint32_t)bsz);
                write32(fout, blk_crc);
                fwrite(outbuf, 1, csz, fout);
                total_compressed += 14 + csz;

                double ratio = (double)bsz / csz;
                fprintf(stderr, "  Block %d [%s:%d/%d]: %d -> %d (%.2fx) [%s]\n",
                        global_block+1, entries[i].path, file_block+1, entries[i].nblocks,
                        bsz, csz, ratio, strat_names[strat]);
            }

            global_block++;
            file_block++;
        }
        fclose(fin);
    }

    /* Phase 5: Write block index table */
    uint64_t index_offset = (uint64_t)ftell(fout);
    for (uint32_t b = 0; b < total_blocks; b++) {
        write64(fout, block_offsets[b]);
    }
    write64(fout, index_offset);
    write32(fout, MAGIC6);

    int64_t file_total = ftell(fout);
    double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
    fprintf(stderr, "[HPK6] Done: %lld -> %lld bytes (%.2fx) in %.1fs\n",
            (long long)total_size, (long long)file_total,
            total_size > 0 ? (double)total_size / file_total : 1.0, elapsed);

    /* Cleanup */
    fclose(fout);
    bwt_ws_free(bwt_ws);
    cws_free(cws);
    free(inbuf); free(outbuf);
    free(block_offsets); free(block_hashes);
    for (int i = 0; i < entry_count; i++) {
        free(entries[i].path);
        free(entries[i].fullpath);
    }
    free(entries);
    return 0;
}

static int archive_decompress(const char *inpath, const char *outdir,
                              const char *extract_name) {
    FILE *fin = fopen(inpath, "rb");
    if (!fin) { fprintf(stderr, "Cannot open '%s'\n", inpath); return 1; }

    uint32_t magic = read32(fin);
    if (magic != MAGIC6) {
        fprintf(stderr, "Not an HPK6 archive (magic: %08X)\n", magic);
        fclose(fin); return 1;
    }
    int ver = fgetc(fin);
    if (ver < 5 || ver > VERSION) {
        fprintf(stderr, "Unsupported version %d\n", ver);
        fclose(fin); return 1;
    }
    uint32_t block_size = read32(fin);
    uint32_t nentries = read32(fin);
    uint32_t total_blocks = read32(fin);
    uint64_t total_size = read64(fin);

    fprintf(stderr, "[HPK6] Archive: %d entries, %d blocks, %.2f MB\n",
            nentries, total_blocks, total_size / 1048576.0);

    HPK6Entry *cat_entries = read_catalog(fin, (int)nentries);

    long block_data_start = ftell(fin);

    fseek(fin, -12, SEEK_END);
    uint64_t index_offset = read64(fin);
    uint32_t footer_magic = read32(fin);
    if (footer_magic != MAGIC6) {
        fprintf(stderr, "Warning: footer magic mismatch\n");
    }

    uint64_t *block_offsets = NULL;
    if (total_blocks > 0) {
        block_offsets = (uint64_t*)malloc(total_blocks * sizeof(uint64_t));
        fseek(fin, (long)index_offset, SEEK_SET);
        for (uint32_t b = 0; b < total_blocks; b++) {
            block_offsets[b] = read64(fin);
        }
    }

    mkdirs(outdir);

    uint8_t *dec_outbuf = (uint8_t*)malloc(block_size);
    uint8_t *cbuf = (uint8_t*)malloc(block_size + block_size/4 + 4096);

    uint8_t **block_cache = (uint8_t**)calloc(total_blocks ? total_blocks : 1, sizeof(uint8_t*));
    int *block_sizes = (int*)calloc(total_blocks ? total_blocks : 1, sizeof(int));

    clock_t dec_start = clock();

    /* Decompress all blocks sequentially (needed for dedup references) */
    fseek(fin, block_data_start, SEEK_SET);
    for (uint32_t b = 0; b < total_blocks; b++) {
        if (block_offsets) fseek(fin, (long)block_offsets[b], SEEK_SET);

        int flags = fgetc(fin);
        if (flags & 1) {
            uint32_t ref = read32(fin);
            if (ref < total_blocks && block_cache[ref]) {
                block_cache[b] = (uint8_t*)malloc(block_sizes[ref]);
                memcpy(block_cache[b], block_cache[ref], block_sizes[ref]);
                block_sizes[b] = block_sizes[ref];
            }
        } else {
            int strat = fgetc(fin);
            int csz = (int)read32(fin);
            int orig_b = (int)read32(fin);
            uint32_t expected_crc = read32(fin);
            fread(cbuf, 1, csz, fin);

            decompress_block(cbuf, csz, strat, orig_b, dec_outbuf);

            uint32_t actual_crc = hp_crc32(dec_outbuf, orig_b);
            if (actual_crc != expected_crc) {
                fprintf(stderr, "  Block %d: CRC MISMATCH! Expected %08X got %08X\n",
                        b+1, expected_crc, actual_crc);
            }

            block_cache[b] = (uint8_t*)malloc(orig_b);
            memcpy(block_cache[b], dec_outbuf, orig_b);
            block_sizes[b] = orig_b;
        }
    }

    /* Write files from decompressed blocks */
    int files_extracted = 0;
    for (uint32_t i = 0; i < nentries; i++) {
        char filepath[4096];
        snprintf(filepath, sizeof(filepath), "%s/%s", outdir, cat_entries[i].path);

        if (cat_entries[i].type == 1) {
            if (extract_name == NULL) mkdirs(filepath);
            continue;
        }

        if (extract_name != NULL) {
            if (strcmp(cat_entries[i].path, extract_name) != 0 &&
                strstr(cat_entries[i].path, extract_name) == NULL) continue;
        }

        char *last_slash = strrchr(filepath, '/');
        if (last_slash) {
            char parent[4096];
            int plen = (int)(last_slash - filepath);
            memcpy(parent, filepath, plen);
            parent[plen] = '\0';
            mkdirs(parent);
        }

        uint32_t fb = cat_entries[i].first_block;
        uint32_t nb = cat_entries[i].nblocks;

        if (cat_entries[i].size == 0) {
            FILE *fout = fopen(filepath, "wb");
            if (fout) fclose(fout);
            fprintf(stderr, "  Extracted: %s (empty)\n", cat_entries[i].path);
            files_extracted++;
            continue;
        }

        FILE *fout = fopen(filepath, "wb");
        if (!fout) {
            fprintf(stderr, "Cannot create '%s'\n", filepath);
            continue;
        }

        for (uint32_t b = 0; b < nb; b++) {
            uint32_t bi = fb + b;
            if (bi < total_blocks && block_cache[bi]) {
                fwrite(block_cache[bi], 1, block_sizes[bi], fout);
            }
        }
        fclose(fout);

        uint32_t actual_crc = file_crc32(filepath);
        if (actual_crc != cat_entries[i].crc) {
            fprintf(stderr, "  WARNING: File CRC mismatch for '%s'! Expected %08X got %08X\n",
                    cat_entries[i].path, cat_entries[i].crc, actual_crc);
        }

#ifndef _WIN32
        chmod(filepath, cat_entries[i].perms);
#endif

        fprintf(stderr, "  Extracted: %s (%lld bytes)\n", cat_entries[i].path, (long long)cat_entries[i].size);
        files_extracted++;
    }

    double dec_elapsed = (double)(clock() - dec_start) / CLOCKS_PER_SEC;
    fprintf(stderr, "[HPK6] Extracted %d files in %.1fs\n", files_extracted, dec_elapsed);

    /* Cleanup */
    fclose(fin);
    for (uint32_t b = 0; b < total_blocks; b++) free(block_cache[b]);
    free(block_cache); free(block_sizes);
    free(dec_outbuf); free(cbuf);
    free(block_offsets);
    for (uint32_t i = 0; i < nentries; i++) free(cat_entries[i].path);
    free(cat_entries);
    return 0;
}

static int archive_list(const char *inpath) {
    FILE *fin = fopen(inpath, "rb");
    if (!fin) { fprintf(stderr, "Cannot open '%s'\n", inpath); return 1; }

    uint32_t magic = read32(fin);
    if (magic != MAGIC6) {
        fprintf(stderr, "Not an HPK6 archive\n");
        fclose(fin); return 1;
    }
    int ver = fgetc(fin);
    uint32_t block_size = read32(fin);
    uint32_t nentries = read32(fin);
    uint32_t total_blocks = read32(fin);
    uint64_t total_size = read64(fin);

    HPK6Entry *lst_entries = read_catalog(fin, (int)nentries);
    fclose(fin);

    fprintf(stderr, "HyperPack HPK6 Archive (v%d, block size: %d MB)\n", ver, block_size >> 20);
    fprintf(stderr, "%-8s %-8s %-6s %-8s %s\n", "Type", "Size", "Blocks", "CRC32", "Path");
    fprintf(stderr, "-------- -------- ------ -------- ----\n");

    uint64_t unique_size = 0;
    int file_cnt = 0, dir_cnt = 0, dedup_cnt = 0;

    for (uint32_t i = 0; i < nentries; i++) {
        const char *type_str;
        if (lst_entries[i].type == 1) {
            type_str = "DIR";
            dir_cnt++;
        } else if (lst_entries[i].is_dedup) {
            type_str = "DEDUP";
            dedup_cnt++;
        } else {
            type_str = "FILE";
            unique_size += lst_entries[i].size;
        }
        file_cnt += (lst_entries[i].type == 0);

        char size_str[32];
        if (lst_entries[i].size >= (1 << 20))
            snprintf(size_str, sizeof(size_str), "%.1fMB", lst_entries[i].size / 1048576.0);
        else if (lst_entries[i].size >= 1024)
            snprintf(size_str, sizeof(size_str), "%.1fKB", lst_entries[i].size / 1024.0);
        else
            snprintf(size_str, sizeof(size_str), "%lluB", (unsigned long long)lst_entries[i].size);

        fprintf(stderr, "%-8s %-8s %-6d %08X %s\n",
                type_str, size_str, lst_entries[i].nblocks, lst_entries[i].crc, lst_entries[i].path);
    }

    fprintf(stderr, "\nTotal: %d files, %d dirs, %d blocks\n", file_cnt, dir_cnt, total_blocks);
    fprintf(stderr, "Original: %.2f MB | Unique: %.2f MB | Dedup saved: %d files\n",
            total_size / 1048576.0, unique_size / 1048576.0, dedup_cnt);

    for (uint32_t i = 0; i < nentries; i++) free(lst_entries[i].path);
    free(lst_entries);
    return 0;
}

/* ===== Main ===== */
#if !defined(HYPERPACK_WASM) && !defined(HYPERPACK_LIB)
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "HyperPack Quantum v11 — Ultra-High Compression Engine\n"
            "Usage:\n"
            "  %s c [-b SIZE_MB] [-j THREADS] input output.hpk     Compress single file (HPK5)\n"
            "  %s a [-b SIZE_MB] input... output.hpk                Archive compress (HPK6)\n"
            "  %s d input.hpk [output]                              Decompress (auto-detect)\n"
            "  %s l input.hpk                                       List archive contents\n"
            "  %s x input.hpk output_dir [-e pattern]               Extract from archive\n",
            argv[0], argv[0], argv[0], argv[0], argv[0]);
        return 1;
    }

    if (argv[1][0] == 'c' && argv[1][1] == '\0') {
        /* Single file compress — HPK5 (backward compatible) */
        if (argc < 4) {
            fprintf(stderr, "Usage: %s c [-b SIZE_MB] [-j THREADS] input output.hpk\n", argv[0]);
            return 1;
        }
        int block_mb = DEFAULT_BS >> 20;
        int nthreads = 1;
        int i = 2;
        while (i < argc - 2) {
            if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
                block_mb = atoi(argv[++i]);
                if (block_mb < 1) block_mb = 1;
                if (block_mb > 128) block_mb = 128;
                i++;
            } else if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
                nthreads = atoi(argv[++i]);
                if (nthreads < 1) nthreads = 1;
                if (nthreads > 16) nthreads = 16;
                i++;
            } else break;
        }
        if (nthreads > 1) {
            fprintf(stderr, "[HP5] Using %d parallel threads\n", nthreads);
        }
        return file_compress(argv[i], argv[i+1], block_mb << 20, nthreads);

    } else if (argv[1][0] == 'a' && argv[1][1] == '\0') {
        /* Archive compress — HPK6 */
        if (argc < 4) {
            fprintf(stderr, "Usage: %s a [-b SIZE_MB] input... output.hpk\n", argv[0]);
            return 1;
        }
        int block_mb = DEFAULT_BS >> 20;
        int i = 2;
        while (i < argc - 1) {
            if (strcmp(argv[i], "-b") == 0 && i + 1 < argc - 1) {
                block_mb = atoi(argv[++i]);
                if (block_mb < 1) block_mb = 1;
                if (block_mb > 128) block_mb = 128;
                i++;
            } else break;
        }
        int ninputs = argc - i - 1;
        if (ninputs < 1) {
            fprintf(stderr, "No input files specified\n");
            return 1;
        }
        const char **inputs = (const char**)&argv[i];
        const char *outpath = argv[argc - 1];
        return archive_compress(ninputs, inputs, outpath, block_mb << 20, 1);

    } else if (argv[1][0] == 'd' && argv[1][1] == '\0') {
        /* Decompress — auto-detect HPK5 or HPK6 */
        if (argc < 3) {
            fprintf(stderr, "Usage: %s d input.hpk [output]\n", argv[0]);
            return 1;
        }
        FILE *fin = fopen(argv[2], "rb");
        if (!fin) { fprintf(stderr, "Cannot open '%s'\n", argv[2]); return 1; }
        uint32_t magic = read32(fin);
        fclose(fin);

        if (magic == MAGIC6) {
            const char *outdir = (argc > 3) ? argv[3] : ".";
            return archive_decompress(argv[2], outdir, NULL);
        } else if (magic == MAGIC) {
            if (argc < 4) {
                fprintf(stderr, "Usage: %s d input.hpk output\n", argv[0]);
                return 1;
            }
            return file_decompress(argv[2], argv[3]);
        } else {
            fprintf(stderr, "Unknown file format (magic: %08X)\n", magic);
            return 1;
        }

    } else if (argv[1][0] == 'l' && argv[1][1] == '\0') {
        /* List archive contents */
        if (argc < 3) {
            fprintf(stderr, "Usage: %s l input.hpk\n", argv[0]);
            return 1;
        }
        return archive_list(argv[2]);

    } else if (argv[1][0] == 'x' && argv[1][1] == '\0') {
        /* Extract from archive with optional pattern */
        if (argc < 4) {
            fprintf(stderr, "Usage: %s x input.hpk output_dir [-e pattern]\n", argv[0]);
            return 1;
        }
        const char *extract_pattern = NULL;
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
                extract_pattern = argv[++i];
            }
        }
        return archive_decompress(argv[2], argv[3], extract_pattern);

    } else {
        fprintf(stderr, "Unknown mode '%s'. Use 'c', 'a', 'd', 'l', or 'x'.\n", argv[1]);
        return 1;
    }
}
#endif /* !HYPERPACK_WASM && !HYPERPACK_LIB */
