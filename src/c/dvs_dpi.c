#include <stdlib.h>
#include <string.h>
#include "dvs_dpi.h"
#include "dvs_problem.h"
#include "dvs_ctx.h"
#include "dvs_search.h"
#include "dvs_block_alloc.h"

/* ------------------------------------------------------------------ */
/* Base64 decoder                                                      */
/* ------------------------------------------------------------------ */

static int _b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode a base64 string into a malloc'd buffer. Returns NULL on malformed
   input: a length that is not a multiple of 4, a character outside the
   alphabet, or padding anywhere but the last one or two places. A problem
   buffer that decoded "mostly right" would compile into a different problem,
   so nothing is guessed. *out_len receives the decoded byte count. */
static uint8_t *_b64_decode(const char *src, size_t *out_len) {
    if (!src) return NULL;
    size_t slen = strlen(src);
    if (slen == 0 || slen % 4 != 0) return NULL;
    size_t pad = 0;
    if (src[slen - 1] == '=') pad++;
    if (src[slen - 2] == '=') pad++;

    size_t decoded_len = (slen / 4) * 3 - pad;
    uint8_t *out = (uint8_t *)malloc(decoded_len + 3);
    if (!out) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < slen; i += 4) {
        int last = (i + 4 == slen);
        int v[4];
        for (int k = 0; k < 4; k++) {
            unsigned char c = (unsigned char)src[i + k];
            if (c == '=' && last && k >= 4 - (int)pad) { v[k] = 0; continue; }
            v[k] = _b64_val(c);
            if (v[k] < 0) { free(out); return NULL; }
        }
        uint32_t triple = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                          ((uint32_t)v[2] << 6)  |  (uint32_t)v[3];
        out[j++] = (triple >> 16) & 0xFF;
        out[j++] = (triple >> 8)  & 0xFF;
        out[j++] =  triple        & 0xFF;
    }

    *out_len = decoded_len;
    return out;
}

/* ------------------------------------------------------------------ */
/* DpiHandle -- persistent context for the compiled problem            */
/*                                                                     */
/* The dvs_ctx_t is compiled once and reused across solve calls.       */
/* pin_var, checkpoint, and restore operate directly on this context,  */
/* so their effects persist across solve calls until restored.         */
/* ------------------------------------------------------------------ */

#define INTERNAL_CTX_SIZE (2 << 20)  /* 2 MiB for persistent ctx */

typedef struct {
    void              *problem_buf;  /* malloc'd decoded dvs_problem_t buffer */
    size_t             problem_size;
    void              *ctx_buf;      /* malloc'd dvs_ctx_t static pool */
    dvs_block_alloc_t *block_alloc;  /* dynamic stack allocator */
    dvs_ctx_t          *ctx;          /* persistent solver context */
    uint32_t           n_vars;
    int                solved;       /* 1 if last solve returned DVS_SOLVE_OK */
    int                n_uncompiled; /* constraints dvs_solver_compile could not take */
    int                search_cp;    /* checkpoint taken before the last search,
                                      * -1 if there is none to undo */
} DpiHandle;

/* Undo the previous search, keeping everything before it (pins included).
 *
 * A search leaves every variable assigned. Without this, the next
 * dvs_dpi_solve_h starts from that full assignment and returns it again
 * whatever the seed, so every randomize_obj() call produced the same object;
 * and a pin applied after a solve was checked against the solved values
 * rather than the constraints. Each search therefore runs inside a private
 * checkpoint, and every operation that changes or reads state for a new
 * solve rolls it back first. */
static void _undo_search(DpiHandle *h) {
    if (h->search_cp >= 0) {
        dvs_solver_restore(h->ctx, (uint32_t)h->search_cp);
        h->search_cp = -1;
    }
    h->solved = 0;
}

/* ------------------------------------------------------------------ */
/* Chandle API implementation                                          */
/* ------------------------------------------------------------------ */

void *dvs_dpi_compile_b64(const char *b64_data) {
    if (!b64_data) return NULL;

    /* 1. Decode the base64 problem buffer */
    size_t buf_len = 0;
    uint8_t *buf = _b64_decode(b64_data, &buf_len);
    if (!buf) return NULL;

    /* 2. Allocate the persistent dvs_ctx_t */
    void *ctx_buf = malloc(INTERNAL_CTX_SIZE);
    if (!ctx_buf) { free(buf); return NULL; }

    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 0);
    if (!ba) { free(ctx_buf); free(buf); return NULL; }

    dvs_ctx_t *ctx = dvs_solver_create(ctx_buf, INTERNAL_CTX_SIZE, ba);
    if (!ctx) {
        dvs_block_alloc_destroy(ba);
        free(ctx_buf);
        free(buf);
        return NULL;
    }

    /* 3. Compile the problem into the persistent context */
    int rc = dvs_solver_compile(ctx, (dvs_problem_t *)buf);
    if (rc < 0) {
        dvs_solver_destroy(ctx);
        dvs_block_alloc_destroy(ba);
        free(ctx_buf);
        free(buf);
        return NULL;
    }

    /* 4. Build the handle */
    DpiHandle *h = (DpiHandle *)calloc(1, sizeof(DpiHandle));
    if (!h) {
        dvs_solver_destroy(ctx);
        dvs_block_alloc_destroy(ba);
        free(ctx_buf);
        free(buf);
        return NULL;
    }

    h->problem_buf  = buf;
    h->problem_size = buf_len;
    h->ctx_buf      = ctx_buf;
    h->block_alloc  = ba;
    h->ctx          = ctx;
    h->n_vars       = ((dvs_problem_t *)buf)->n_vars;
    h->solved       = 0;
    h->search_cp    = -1;
    /* A POSITIVE dvs_solver_compile return is the count of constraints it could not
     * compile, and this path used to test only `rc < 0` -- so those constraints
     * were dropped and the SV/DPI consumer solved without them, producing
     * under-constrained stimulus that looks exactly like a successful solve.
     * Remember the count; dvs_dpi_solve_h validates the model against the FULL
     * problem before reporting success when it is non-zero. */
    h->n_uncompiled = rc;

    return (void *)h;
}

int dvs_dpi_n_uncompiled_h(void *ctx) {
    if (!ctx) return -1;
    return ((DpiHandle *)ctx)->n_uncompiled;
}

int dvs_dpi_solve_h(void *ctx, long long seed) {
    if (!ctx) return -1;
    DpiHandle *h = (DpiHandle *)ctx;

    _undo_search(h);
    h->search_cp = dvs_solver_checkpoint(h->ctx);
    if (h->search_cp < 0) return -1;   /* every checkpoint slot is in use */

    dvs_solve_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.seed = (uint64_t)seed;
    /* DPI callers are randomizing stimulus: break decision ties at random so
     * solutions spread across the solution space (see dv_solve.h). */
    opts.fair_pick = 1;

    dvs_result_t sr = dvs_solver_solve(h->ctx, &opts);

    if (sr == DVS_SOLVE_OK) {
        /* The post-solve net, and the only one this path has: re-evaluate every
         * constraint in the original problem against the assignment. It is
         * needed exactly when compile dropped something -- an assignment that
         * satisfies the compiled subset says nothing about the constraints that
         * never reached a propagator. Reporting 3 ("solved, but the model
         * violates the problem") is the whole point: the alternative is handing
         * back stimulus that silently ignores a constraint the user wrote. */
        if (h->n_uncompiled > 0 &&
            dvs_solver_validate_model(h->ctx, (dvs_problem_t *)h->problem_buf,
                                  NULL) != 0) {
            return 3;
        }
        h->solved = 1;
        return 0;
    } else if (sr == DVS_SOLVE_UNSAT) {
        return 1;
    } else {
        return 2;
    }
}

int dvs_dpi_pin_var_h(void *ctx, int var_id, long long value) {
    if (!ctx) return -1;
    DpiHandle *h = (DpiHandle *)ctx;
    if (var_id < 0 || (uint32_t)var_id >= h->n_vars) return -1;

    _undo_search(h);
    int rc = dvs_solver_pin_var(h->ctx, (uint32_t)var_id, (int64_t)value);
    /* dvs_solver_pin_var returns 0 on success, -1 on conflict */
    return (rc == 0) ? 0 : -2;
}

int dvs_dpi_checkpoint_h(void *ctx) {
    if (!ctx) return -1;
    DpiHandle *h = (DpiHandle *)ctx;
    _undo_search(h);
    /* Keep one slot free for the checkpoint dvs_dpi_solve_h takes. */
    if (h->ctx->n_checkpoints + 1 >= MAX_CHECKPOINTS) return -1;
    return dvs_solver_checkpoint(h->ctx);
}

void dvs_dpi_restore_h(void *ctx, int cp) {
    if (!ctx || cp < 0) return;
    DpiHandle *h = (DpiHandle *)ctx;
    _undo_search(h);
    if ((uint32_t)cp >= h->ctx->n_checkpoints) return;
    /* dvs_solver_restore pops checkpoint cp and every later one. Taking a new
     * checkpoint straight away puts the same state back at the same index, so
     * cp stays valid and can be restored again, as this API promises. */
    dvs_solver_restore(h->ctx, (uint32_t)cp);
    dvs_solver_checkpoint(h->ctx);
}

long long dvs_dpi_get_value_h(void *ctx, int var_id) {
    if (!ctx) return 0;
    DpiHandle *h = (DpiHandle *)ctx;
    if (!h->solved) return 0;
    if (var_id < 0 || (uint32_t)var_id >= h->n_vars) return 0;
    return (long long)dvs_solver_get_value(h->ctx, (uint32_t)var_id);
}

void dvs_dpi_release_h(void *ctx) {
    if (!ctx) return;
    DpiHandle *h = (DpiHandle *)ctx;
    dvs_solver_destroy(h->ctx);
    dvs_block_alloc_destroy(h->block_alloc);
    free(h->ctx_buf);
    free(h->problem_buf);
    free(h);
}
