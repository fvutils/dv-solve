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

/* Decode base64 string into malloc'd buffer. Returns NULL on error.
   *out_len receives the decoded byte count. */
static uint8_t *_b64_decode(const char *src, size_t *out_len) {
    if (!src) return NULL;
    size_t slen = strlen(src);
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
/* DpiHandle -- persistent context for the compiled problem            */
/*                                                                     */
/* The SolveCtx is compiled once and reused across solve calls.       */
/* pin_var, checkpoint, and restore operate directly on this context,  */
/* so their effects persist across solve calls until restored.         */
/* ------------------------------------------------------------------ */

#define INTERNAL_CTX_SIZE (2 << 20)  /* 2 MiB for persistent ctx */

typedef struct {
    void              *problem_buf;  /* malloc'd decoded SolveProblem buffer */
    size_t             problem_size;
    void              *ctx_buf;      /* malloc'd SolveCtx static pool */
    zsp_block_alloc_t *block_alloc;  /* dynamic stack allocator */
    SolveCtx          *ctx;          /* persistent solver context */
    uint32_t           n_vars;
    int                solved;       /* 1 if last solve returned SOLVE_OK */
} DpiHandle;

/* ------------------------------------------------------------------ */
/* Chandle API implementation                                          */
/* ------------------------------------------------------------------ */

void *zsp_dpi_compile_b64(const char *b64_data) {
    if (!b64_data) return NULL;

    /* 1. Decode the base64 problem buffer */
    size_t buf_len = 0;
    uint8_t *buf = _b64_decode(b64_data, &buf_len);
    if (!buf) return NULL;

    /* 2. Allocate the persistent SolveCtx */
    void *ctx_buf = malloc(INTERNAL_CTX_SIZE);
    if (!ctx_buf) { free(buf); return NULL; }

    zsp_block_alloc_t *ba = zsp_block_alloc_create(NULL, 0);
    if (!ba) { free(ctx_buf); free(buf); return NULL; }

    SolveCtx *ctx = solver_create(ctx_buf, INTERNAL_CTX_SIZE, ba);
    if (!ctx) {
        zsp_block_alloc_destroy(ba);
        free(ctx_buf);
        free(buf);
        return NULL;
    }

    /* 3. Compile the problem into the persistent context */
    int rc = solver_compile(ctx, (SolveProblem *)buf);
    if (rc < 0) {
        solver_destroy(ctx);
        zsp_block_alloc_destroy(ba);
        free(ctx_buf);
        free(buf);
        return NULL;
    }

    /* 4. Build the handle */
    DpiHandle *h = (DpiHandle *)calloc(1, sizeof(DpiHandle));
    if (!h) {
        solver_destroy(ctx);
        zsp_block_alloc_destroy(ba);
        free(ctx_buf);
        free(buf);
        return NULL;
    }

    h->problem_buf  = buf;
    h->problem_size = buf_len;
    h->ctx_buf      = ctx_buf;
    h->block_alloc  = ba;
    h->ctx          = ctx;
    h->n_vars       = ((SolveProblem *)buf)->n_vars;
    h->solved       = 0;

    return (void *)h;
}

int zsp_dpi_solve_h(void *ctx, long long seed) {
    if (!ctx) return -1;
    DpiHandle *h = (DpiHandle *)ctx;

    h->solved = 0;

    SolveOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.seed = (uint64_t)seed;

    SolveResult sr = solver_solve(h->ctx, &opts);

    if (sr == SOLVE_OK) {
        h->solved = 1;
        return 0;
    } else if (sr == SOLVE_UNSAT) {
        return 1;
    } else {
        return 2;
    }
}

int zsp_dpi_pin_var_h(void *ctx, int var_id, long long value) {
    if (!ctx) return -1;
    DpiHandle *h = (DpiHandle *)ctx;
    if (var_id < 0 || (uint32_t)var_id >= h->n_vars) return -1;

    int rc = solver_pin_var(h->ctx, (uint32_t)var_id, (int64_t)value);
    /* solver_pin_var returns 0 on success, -1 on conflict */
    return (rc == 0) ? 0 : -2;
}

int zsp_dpi_checkpoint_h(void *ctx) {
    if (!ctx) return -1;
    DpiHandle *h = (DpiHandle *)ctx;
    return solver_checkpoint(h->ctx);
}

void zsp_dpi_restore_h(void *ctx, int cp) {
    if (!ctx || cp < 0) return;
    DpiHandle *h = (DpiHandle *)ctx;
    solver_restore(h->ctx, (uint32_t)cp);
    h->solved = 0;  /* restored state has no guaranteed solved values */
}

long long zsp_dpi_get_value_h(void *ctx, int var_id) {
    if (!ctx) return 0;
    DpiHandle *h = (DpiHandle *)ctx;
    if (!h->solved) return 0;
    if (var_id < 0 || (uint32_t)var_id >= h->n_vars) return 0;
    return (long long)solver_get_value(h->ctx, (uint32_t)var_id);
}

void zsp_dpi_release_h(void *ctx) {
    if (!ctx) return;
    DpiHandle *h = (DpiHandle *)ctx;
    solver_destroy(h->ctx);
    zsp_block_alloc_destroy(h->block_alloc);
    free(h->ctx_buf);
    free(h->problem_buf);
    free(h);
}
