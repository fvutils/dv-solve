#include <stdlib.h>
#include <string.h>
#include "zsp_dpi.h"
#include "zsp_problem.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_block_alloc.h"

/* ------------------------------------------------------------------ */
/* Base64 decoder                                                      */
/* ------------------------------------------------------------------ */

static const uint8_t _b64_table[256] = {
    /* 0x00-0x2A */ 64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
                    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
                    64,64,64,64,64,64,64,64,64,64,64,
    /* '+' 0x2B */ 62,
    /* 0x2C-0x2E */ 64,64,64,
    /* '/' 0x2F */ 63,
    /* '0'-'9' */  52,53,54,55,56,57,58,59,60,61,
    /* 0x3A-0x40 */ 64,64,64,64,64,64,64,
    /* 'A'-'Z' */  0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
    /* 0x5B-0x60 */ 64,64,64,64,64,64,
    /* 'a'-'z' */  26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,
    /* 0x7B-0xFF: all 64 (invalid) -- zero-initialized below */
};

/* Decode base64 string into malloc'd buffer.  Returns NULL on error.
   *out_len receives the decoded byte count. */
static uint8_t *_b64_decode(const char *src, size_t *out_len) {
    if (!src) return NULL;
    size_t slen = strlen(src);
    /* Strip trailing '=' padding for length calc */
    size_t pad = 0;
    if (slen >= 1 && src[slen - 1] == '=') pad++;
    if (slen >= 2 && src[slen - 2] == '=') pad++;

    size_t decoded_len = (slen / 4) * 3 - pad;
    uint8_t *out = (uint8_t *)malloc(decoded_len + 4); /* +4 safety */
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < slen; ) {
        uint32_t a = (i < slen) ? _b64_table[(uint8_t)src[i++]] : 64;
        uint32_t b = (i < slen) ? _b64_table[(uint8_t)src[i++]] : 64;
        uint32_t c = (i < slen) ? _b64_table[(uint8_t)src[i++]] : 64;
        uint32_t d = (i < slen) ? _b64_table[(uint8_t)src[i++]] : 64;
        if (a > 63 || b > 63) { free(out); return NULL; }
        uint32_t triple = (a << 18) | (b << 12) | ((c & 63) << 6) | (d & 63);
        if (j < decoded_len) out[j++] = (triple >> 16) & 0xFF;
        if (j < decoded_len) out[j++] = (triple >> 8)  & 0xFF;
        if (j < decoded_len) out[j++] =  triple        & 0xFF;
    }

    *out_len = decoded_len;
    return out;
}

/* ------------------------------------------------------------------ */
/* Chandle context: holds problem buffer + last-solve results          */
/* ------------------------------------------------------------------ */

#define INTERNAL_CTX_SIZE (1 << 20)  /* 1 MiB */

typedef struct {
    void     *problem_buf;   /* malloc'd SolveProblem buffer */
    size_t    problem_size;
    uint32_t  n_vars;
    int64_t  *values;        /* malloc'd array [n_vars], filled by solve */
    int       solved;        /* 1 if values[] is valid */
} DpiHandle;

static int _do_solve(
    SolveProblem *sp,
    uint32_t      n_vars,
    uint64_t      seed,
    int64_t      *result_vals
) {
    void *ctx_buf = malloc(INTERNAL_CTX_SIZE);
    if (!ctx_buf) return -1;

    zsp_block_alloc_t *ba = zsp_block_alloc_create(NULL, 0);
    if (!ba) { free(ctx_buf); return -1; }

    SolveCtx *ctx = solver_create(ctx_buf, INTERNAL_CTX_SIZE, ba);
    if (!ctx) {
        zsp_block_alloc_destroy(ba);
        free(ctx_buf);
        return -1;
    }

    int rc = solver_compile(ctx, sp);
    if (rc < 0) {
        solver_destroy(ctx);
        zsp_block_alloc_destroy(ba);
        free(ctx_buf);
        return (rc == -2) ? 1 : -1;
    }

    SolveOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.seed = seed;

    SolveResult sr = solver_solve(ctx, &opts);

    int ret;
    if (sr == SOLVE_OK) {
        for (uint32_t i = 0; i < n_vars; i++)
            result_vals[i] = solver_get_value(ctx, i);
        ret = 0;
    } else if (sr == SOLVE_UNSAT) {
        ret = 1;
    } else {
        ret = 2;
    }

    solver_destroy(ctx);
    zsp_block_alloc_destroy(ba);
    free(ctx_buf);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Chandle API implementation                                          */
/* ------------------------------------------------------------------ */

void *zsp_dpi_compile_b64(const char *b64_data) {
    if (!b64_data) return NULL;

    size_t buf_len = 0;
    uint8_t *buf = _b64_decode(b64_data, &buf_len);
    if (!buf) return NULL;

    SolveProblem *sp = (SolveProblem *)buf;
    uint32_t n_vars = sp->n_vars;

    /* Probe-compile to verify the buffer is valid */
    void *probe_buf = malloc(INTERNAL_CTX_SIZE);
    if (!probe_buf) { free(buf); return NULL; }

    zsp_block_alloc_t *ba = zsp_block_alloc_create(NULL, 0);
    if (!ba) { free(probe_buf); free(buf); return NULL; }

    SolveCtx *probe = solver_create(probe_buf, INTERNAL_CTX_SIZE, ba);
    if (!probe) {
        zsp_block_alloc_destroy(ba);
        free(probe_buf);
        free(buf);
        return NULL;
    }

    int rc = solver_compile(probe, sp);
    solver_destroy(probe);
    zsp_block_alloc_destroy(ba);
    free(probe_buf);

    if (rc < 0) { free(buf); return NULL; }

    /* Build the handle */
    DpiHandle *h = (DpiHandle *)calloc(1, sizeof(DpiHandle));
    if (!h) { free(buf); return NULL; }

    h->problem_buf  = buf;
    h->problem_size = buf_len;
    h->n_vars       = n_vars;
    h->values       = (int64_t *)calloc(n_vars, sizeof(int64_t));
    h->solved       = 0;

    if (n_vars > 0 && !h->values) {
        free(buf);
        free(h);
        return NULL;
    }

    return (void *)h;
}

int zsp_dpi_solve_h(void *ctx, long long seed) {
    if (!ctx) return -1;
    DpiHandle *h = (DpiHandle *)ctx;

    h->solved = 0;
    int rc = _do_solve(
        (SolveProblem *)h->problem_buf,
        h->n_vars,
        (uint64_t)seed,
        h->values
    );
    if (rc == 0) h->solved = 1;
    return rc;
}

long long zsp_dpi_get_value_h(void *ctx, int var_id) {
    if (!ctx) return 0;
    DpiHandle *h = (DpiHandle *)ctx;
    if (!h->solved) return 0;
    if (var_id < 0 || (uint32_t)var_id >= h->n_vars) return 0;
    return (long long)h->values[var_id];
}

void zsp_dpi_release_h(void *ctx) {
    if (!ctx) return;
    DpiHandle *h = (DpiHandle *)ctx;
    free(h->problem_buf);
    free(h->values);
    free(h);
}
