/* Replay a captured Verilator session four ways, to cost the SMT-LIB pipe.
 *
 * Input: the commands Verilator sent to the solver, one command per line, as
 * written by solver-bench/scripts/tr_gaps.py --cmds from a solver_tap
 * transcript. Each pass replays the whole session with every loop in C:
 *
 *   pipe     spawn dv-solve-smt2 --interactive --mode=verilator and drive it
 *            as Verilator does: write the commands up to a check-sat or
 *            get-value, then block reading the answer. The text is prebuilt,
 *            so this is the solver side of today's integration with a free
 *            client (no SMT-LIB generation or response parsing).
 *   text     the same commands through the SMT-LIB front end in this process:
 *            no pipe and no process switch, but lexing, parsing and
 *            translation to the builder remain. Timed per phase.
 *   api      each check-sat's problem, captured from the text pass, rebuilt
 *            through the public builder API (what a Verilator that called
 *            dv-solve directly would do instead of printing SMT-LIB), then
 *            compiled and solved on the engine the front end used, and every
 *            variable read back. Nothing is reused between randomize() calls.
 *   api-cache  as api, but when the built problem is byte-identical to an
 *            earlier one, the earlier compiled context is reused (CDCL: reset
 *            + solve; bitblast: re-diversify with the new seed), as the
 *            lifecycle in docs/verilator_integration.md prescribes. The
 *            problem is still built every call.
 *
 * Usage: bench_vlt_replay <cmds.smt2> [--solver PATH] [--reps N]
 *        [--passes pipe,text,api,api-cache]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "smt2/smt2_frontend.h"
#include "smt2/smt2_lexer.h"
#include "smt2/smt2_parser.h"
#include "dvs_bbsolver.h"
#include "dvs_builder.h"
#include "dvs_problem.h"

#define CTX_BUF_MAX  (64u * 1024u * 1024u)
#define BA_BLOCK     (64u * 1024u)
#define MAX_AUX      16

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static double cpu_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

enum { K_OTHER, K_DECL, K_ASSERT, K_CHECK, K_GETVAL, K_RESET, K_N };
static const char *k_name[K_N] = { "other", "declare/define", "assert", "check-sat",
                                   "get-value", "reset/set-option" };

typedef struct {
    const char *txt;
    uint32_t    len;
    uint8_t     kind;
    uint8_t     replies;   /* the solver answers this command */
} Cmd;

static Cmd   *g_cmd;
static size_t g_ncmd;

static uint8_t _kind(const char *s) {
    if (!strncmp(s, "(check-sat", 10)) return K_CHECK;
    if (!strncmp(s, "(get-value", 10)) return K_GETVAL;
    if (!strncmp(s, "(assert", 7)) return K_ASSERT;
    if (!strncmp(s, "(declare-", 9) || !strncmp(s, "(define-", 8)) return K_DECL;
    if (!strncmp(s, "(reset", 6) || !strncmp(s, "(set-", 5)) return K_RESET;
    return K_OTHER;
}

static void load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror("read"); exit(2); }
    buf[sz] = 0;
    fclose(f);
    size_t cap = 1 << 16;
    g_cmd = malloc(cap * sizeof(Cmd));
    for (char *p = buf; *p;) {
        char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        if (e > p && *p == '(') {
            if (g_ncmd == cap) g_cmd = realloc(g_cmd, (cap *= 2) * sizeof(Cmd));
            Cmd *c = &g_cmd[g_ncmd++];
            c->txt = p;
            c->len = (uint32_t)(e - p);
            c->kind = _kind(p);
            c->replies = c->kind == K_CHECK || c->kind == K_GETVAL
                         || !strncmp(p, "(get-", 5);
        }
        if (!*e) break;
        *e = 0;
        p = e + 1;
    }
}

/* ------------------------------------------------------------------ */
/* Pass: pipe                                                          */
/* ------------------------------------------------------------------ */

/* Read one complete answer: a line, or until the parentheses balance. */
static int read_answer(FILE *in) {
    int depth = 0, started = 0, c;
    while ((c = fgetc(in)) != EOF) {
        if (c == '(') { depth++; started = 1; }
        else if (c == ')') depth--;
        else if (c == '\n' && depth == 0 && (started || 1)) return 0;
    }
    return -1;
}

static void pass_pipe(const char *solver, double *wall, double *child_cpu) {
    int to[2], from[2];
    if (pipe(to) || pipe(from)) { perror("pipe"); exit(2); }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        close(to[1]);
        close(from[0]);
        setenv("DV_LOG", "/dev/null", 0);
        execl(solver, solver, "--interactive", "--mode=verilator", (char *)NULL);
        perror(solver);
        _exit(127);
    }
    close(to[0]);
    close(from[1]);
    FILE *in = fdopen(from[0], "r");
    /* Verilator writes each exchange with one flush; do the same. */
    char  *obuf = malloc(1 << 20);
    size_t ocap = 1 << 20, olen = 0;
    double t0 = now_s();
    for (size_t i = 0; i < g_ncmd; i++) {
        Cmd *c = &g_cmd[i];
        if (olen + c->len + 1 > ocap) obuf = realloc(obuf, ocap = 2 * (olen + c->len + 1));
        memcpy(obuf + olen, c->txt, c->len);
        olen += c->len;
        obuf[olen++] = '\n';
        if (c->replies) {
            for (size_t w = 0; w < olen;) {
                ssize_t n = write(to[1], obuf + w, olen - w);
                if (n < 0) { if (errno == EINTR) continue; perror("write"); exit(2); }
                w += (size_t)n;
            }
            olen = 0;
            if (read_answer(in) < 0) { fprintf(stderr, "solver closed at cmd %zu\n", i); exit(2); }
        }
    }
    *wall = now_s() - t0;
    close(to[1]);
    struct rusage ru;
    int st;
    wait4(pid, &st, 0, &ru);
    fclose(in);
    free(obuf);
    *child_cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6
               + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6;
}

/* ------------------------------------------------------------------ */
/* Pass: text (and problem capture)                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    dvs_problem_t *p;
    size_t         size;
    uint64_t       seed;
    uint64_t       hash;
    uint8_t        bitblast;   /* the front end answered on bitblast */
    uint8_t        unsat;      /* ... and its answer was unsat */
    dvs_problem_t *aux[MAX_AUX];   /* constraints added under (push) after the
                                    * CDCL compile (CDCL answers only) */
    uint32_t       n_aux;
} Capture;

static Capture *g_cap;
static dvs_problem_t *g_pre[MAX_AUX];
static uint32_t g_pre_n;
static size_t   g_ncap, g_capcap;
static size_t   g_nocap;       /* check-sats with no problem to capture */

static uint64_t fnv(const void *d, size_t n) {
    const uint8_t *b = d;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

static void capture(Smt2Frontend *fe) {
    dvs_problem_t *p = fe->bb_model_valid && fe->bb_problem ? fe->bb_problem : fe->problem;
    if (!p || fe->last_result == DVS_SOLVE_TIMEOUT) {
        g_nocap++;
        for (uint32_t k = 0; k < g_pre_n; k++) free(g_pre[k]);
        g_pre_n = 0;
        return;
    }
    /* Offsets stay inside the used part of the pool: copy that, with room
     * for the rest of the pool's capacity. */
    size_t sz = sizeof(dvs_problem_t) + p->pool.capacity;
    if (g_ncap == g_capcap) g_cap = realloc(g_cap, (g_capcap = g_capcap ? 2 * g_capcap : 4096) * sizeof(Capture));
    Capture *c = &g_cap[g_ncap++];
    c->p = malloc(sz);
    memcpy(c->p, p, sizeof(dvs_problem_t) + p->pool.used);
    c->size = sz;
    c->seed = fe->seed;
    c->hash = fnv(p, sizeof(dvs_problem_t) + p->pool.used);
    c->bitblast = fe->bb_model_valid;
    c->unsat = fe->last_result == DVS_SOLVE_UNSAT;
    c->n_aux = 0;
    if (!c->bitblast) {
        c->n_aux = g_pre_n;
        memcpy(c->aux, g_pre, g_pre_n * sizeof(dvs_problem_t *));
        for (uint32_t k = 0; k < g_pre_n; k++)
            c->hash = c->hash * 31 + fnv(g_pre[k], sizeof(dvs_problem_t) + g_pre[k]->pool.used);
    } else {
        for (uint32_t k = 0; k < g_pre_n; k++) free(g_pre[k]);
    }
    g_pre_n = 0;
}

/* Before a check-sat on a compiled CDCL context: the constraints it already
 * holds as add-ons, and those asserted since (under push) that the check-sat
 * is about to add. Copied now, since the front end may find the latter unsat
 * as it adds them, and never record them. */
static void capture_pre(Smt2Frontend *fe) {
    g_pre_n = 0;
    if (!fe->compiled || !fe->ctx) return;
    for (uint32_t k = 0; k < fe->n_aux_problems; k++) {
        dvs_problem_t *a = fe->aux_problems[k];
        g_pre[g_pre_n] = malloc(sizeof(dvs_problem_t) + a->pool.capacity);
        memcpy(g_pre[g_pre_n++], a, sizeof(dvs_problem_t) + a->pool.used);
    }
    if (fe->cdcl_retained) {
        size_t sz;
        dvs_problem_t *a = dvs_builder_finalize_since(fe->builder, &fe->aux_mark, &sz);
        if (a && (a->n_constraints || a->n_vars)) {
            if (g_pre_n == MAX_AUX) { fprintf(stderr, "too many add-on problems\n"); exit(2); }
            a->flags |= DVS_PROBLEM_F_EXPLICIT;
            g_pre[g_pre_n] = malloc(sizeof(dvs_problem_t) + a->pool.capacity);
            memcpy(g_pre[g_pre_n++], a, sizeof(dvs_problem_t) + a->pool.used);
        }
        if (a) dvs_builder_free_problem(fe->builder, a, sz);
    }
}

static void pass_text(int do_capture, double t[K_N], double *t_parse, double *wall,
                      double *cpu) {
    FILE *out = fopen("/dev/null", "w");
    Smt2Frontend fe;
    smt2_frontend_init(&fe, out, out);
    fe.verilator_mode = 1;
    SexprArena arena;
    sexpr_arena_init(&arena, 8192);
    memset(t, 0, K_N * sizeof(double));
    *t_parse = 0;
    double c0 = cpu_s(), w0 = now_s();
    for (size_t i = 0; i < g_ncmd; i++) {
        Cmd *c = &g_cmd[i];
        double a = now_s();
        Smt2Lexer lex;
        smt2_lexer_init(&lex, c->txt, c->len);
        sexpr_arena_reset(&arena);
        Sexpr *s = sexpr_parse(&lex, &arena);
        if (do_capture && c->kind == K_CHECK) capture_pre(&fe);
        double b = now_s();
        if (s) smt2_frontend_dispatch(&fe, s);
        double e = now_s();
        *t_parse += b - a;
        t[c->kind] += e - b;
        if (do_capture && c->kind == K_CHECK) capture(&fe);
    }
    *wall = now_s() - w0;
    *cpu = cpu_s() - c0;
    smt2_frontend_destroy(&fe);
    sexpr_arena_destroy(&arena);
    fclose(out);
}

/* ------------------------------------------------------------------ */
/* Rebuild a captured problem through the public builder API           */
/* ------------------------------------------------------------------ */

typedef struct { dvs_expr_t *key, *val; uint32_t cap, n; } Memo;

static void memo_clear(Memo *m, uint32_t want) {
    uint32_t cap = 1024;
    while (cap < want * 2) cap <<= 1;
    if (cap > m->cap) {
        free(m->key); free(m->val);
        m->key = malloc(cap * sizeof(dvs_expr_t));
        m->val = malloc(cap * sizeof(dvs_expr_t));
        m->cap = cap;
    }
    memset(m->key, 0xff, m->cap * sizeof(dvs_expr_t));
    m->n = 0;
}

static dvs_expr_t *memo_slot(Memo *m, dvs_expr_t k) {
    uint32_t i = (k * 2654435761u) & (m->cap - 1);
    while (m->key[i] != EXPR_NULL && m->key[i] != k) i = (i + 1) & (m->cap - 1);
    return &m->key[i];
}

typedef struct {
    dvs_problem_t *p;
    dvs_builder_t *b;
    Memo           memo;
    dvs_expr_t    *tmp;
    uint32_t       tmp_cap;
} Rebuild;

static dvs_expr_t rb(Rebuild *r, dvs_expr_t ref);

static dvs_expr_t rb_uncached(Rebuild *r, dvs_expr_t ref) {
    dvs_problem_t *p = r->p;
    dvs_builder_t *b = r->b;
    ExprKind *k = POOL_PTR(p, ref);
    switch (*k) {
    case EXPR_CONST: {
        ExprConst *n = (ExprConst *)k;
        return dvs_builder_expr_const_sized(b, n->value, n->is_signed, n->width);
    }
    case EXPR_VAR:
        return dvs_builder_expr_var(b, ((ExprVar *)k)->var_id);
    case EXPR_BINARY: {
        ExprBinary *n = (ExprBinary *)k;
        dvs_expr_t l = rb(r, n->lhs), rr = rb(r, n->rhs);
        return dvs_builder_expr_binary(b, n->op, l, rr);
    }
    case EXPR_UNARY: {
        ExprUnary *n = (ExprUnary *)k;
        return dvs_builder_expr_unary(b, n->op, rb(r, n->operand));
    }
    case EXPR_ITE: {
        ExprITE *n = (ExprITE *)k;
        dvs_expr_t c = rb(r, n->cond), a = rb(r, n->then_e), e = rb(r, n->else_e);
        return dvs_builder_expr_ite(b, c, a, e);
    }
    case EXPR_IN_RANGE: {
        ExprInRange *n = (ExprInRange *)k;
        dvs_expr_t v = rb(r, n->value), lo = rb(r, n->lo), hi = rb(r, n->hi);
        return dvs_builder_expr_in_range(b, v, lo, hi);
    }
    case EXPR_IN_SET: {
        ExprInSet *n = (ExprInSet *)k;
        uint32_t ne = n->n_elems;
        dvs_expr_t v = rb(r, n->value);
        dvs_expr_t *src = expr_in_set_elems(p, ref);
        dvs_expr_t *el = malloc((ne ? ne : 1) * sizeof(dvs_expr_t));
        for (uint32_t i = 0; i < ne; i++) el[i] = rb(r, src[i]);
        dvs_expr_t out = dvs_builder_expr_in_set(b, v, ne, el);
        free(el);
        return out;
    }
    case EXPR_IN_RANGES: {
        ExprInRanges *n = (ExprInRanges *)k;
        uint32_t nr = n->n_ranges;
        dvs_expr_t v = rb(r, n->value);
        dvs_expr_t *los = expr_in_ranges_los(p, ref), *his = expr_in_ranges_his(p, ref);
        dvs_expr_t *el = malloc((2 * nr ? 2 * nr : 1) * sizeof(dvs_expr_t));
        for (uint32_t i = 0; i < nr; i++) { el[i] = rb(r, los[i]); el[nr + i] = rb(r, his[i]); }
        dvs_expr_t out = dvs_builder_expr_in_ranges(b, v, nr, el, el + nr);
        free(el);
        return out;
    }
    case EXPR_EXTEND: {
        ExprExtend *n = (ExprExtend *)k;
        return dvs_builder_expr_extend(b, rb(r, n->operand), n->from_bits, n->to_bits,
                                       n->sign_extend);
    }
    case EXPR_SV_CAST: {
        ExprSvCast *n = (ExprSvCast *)k;
        return dvs_builder_expr_sv_cast(b, rb(r, n->operand), n->from_bits, n->to_bits,
                                        n->sign_extend, n->dst_signed);
    }
    case EXPR_EXTRACT: {
        ExprExtract *n = (ExprExtract *)k;
        return dvs_builder_expr_extract(b, rb(r, n->operand), n->hi_bit, n->lo_bit);
    }
    case EXPR_CONCAT: {
        ExprConcat *n = (ExprConcat *)k;
        dvs_expr_t h = rb(r, n->hi), l = rb(r, n->lo);
        return dvs_builder_expr_concat(b, h, l, n->lo_width);
    }
    case EXPR_SUM: {
        ExprSum *n = (ExprSum *)k;
        dvs_expr_t *src = (dvs_expr_t *)((char *)n + sizeof(ExprSum));
        dvs_expr_t res = rb(r, n->result);
        dvs_expr_t *el = malloc((n->n_vars ? n->n_vars : 1) * sizeof(dvs_expr_t));
        for (uint32_t i = 0; i < n->n_vars; i++) el[i] = rb(r, src[i]);
        dvs_expr_t out = dvs_builder_expr_sum(b, res, n->n_vars, el);
        free(el);
        return out;
    }
    case EXPR_COUNTONES: {
        ExprCountones *n = (ExprCountones *)k;
        dvs_expr_t res = rb(r, n->result);
        return dvs_builder_expr_countones(b, res, rb(r, n->operand));
    }
    case EXPR_CLOG2: {
        ExprClog2 *n = (ExprClog2 *)k;
        dvs_expr_t res = rb(r, n->result);
        return dvs_builder_expr_clog2(b, res, rb(r, n->operand));
    }
    case EXPR_ARRAY_SELECT: {
        ExprArraySelect *n = (ExprArraySelect *)k;
        dvs_expr_t res = rb(r, n->result), idx = rb(r, n->index);
        return dvs_builder_expr_array_select(b, n->base_var_id, n->n_elems, res, idx);
    }
    }
    fprintf(stderr, "rebuild: unknown expression kind %d\n", (int)*k);
    exit(2);
}

static dvs_expr_t rb(Rebuild *r, dvs_expr_t ref) {
    if (ref == EXPR_NULL) return EXPR_NULL;
    dvs_expr_t *slot = memo_slot(&r->memo, ref);
    if (*slot == ref) return r->memo.val[slot - r->memo.key];
    dvs_expr_t out = rb_uncached(r, ref);
    slot = memo_slot(&r->memo, ref);   /* the table did not grow; same slot */
    *slot = ref;
    r->memo.val[slot - r->memo.key] = out;
    return out;
}

/* Lists are built by prepending: walk into an array, re-add back to front. */
static uint32_t walk(dvs_problem_t *p, dvs_expr_t head, dvs_expr_t **arr, uint32_t *cap) {
    uint32_t n = 0;
    for (dvs_expr_t r = head; r != EXPR_NULL; r = *(dvs_expr_t *)POOL_PTR(p, r)) {
        if (n == *cap) *arr = realloc(*arr, (*cap = *cap ? 2 * *cap : 256) * sizeof(dvs_expr_t));
        (*arr)[n++] = r;
    }
    return n;
}

static dvs_problem_t *rebuild(Rebuild *r, dvs_problem_t *p, size_t *size) {
    dvs_builder_t *b = r->b;
    dvs_builder_reset(b);
    r->p = p;
    memo_clear(&r->memo, p->pool.used / 8);
    uint32_t n;
    n = walk(p, p->vars_head, &r->tmp, &r->tmp_cap);
    for (uint32_t i = n; i-- > 0;) {
        VarSpec *v = POOL_PTR(p, r->tmp[i]);
        dvs_expr_t vr = dvs_builder_add_var(b, v->var_id, v->width, v->is_signed, v->lo, v->hi);
        if (v->is_aux) dvs_builder_mark_var_aux(b, vr);
    }
    n = walk(p, p->constraints_head, &r->tmp, &r->tmp_cap);
    for (uint32_t i = n; i-- > 0;) {
        ConstraintSpec *c = POOL_PTR(p, r->tmp[i]);
        dvs_builder_add_constraint(b, rb(r, c->root));
    }
    n = walk(p, p->softs_head, &r->tmp, &r->tmp_cap);
    for (uint32_t i = n; i-- > 0;) {
        SoftSpec *s = POOL_PTR(p, r->tmp[i]);
        dvs_builder_add_soft_constraint(b, rb(r, s->root), s->priority);
    }
    n = walk(p, p->dists_head, &r->tmp, &r->tmp_cap);
    for (uint32_t i = n; i-- > 0;) {
        DistSpec *d = POOL_PTR(p, r->tmp[i]);
        dvs_builder_add_dist(b, d->var_id, d->n_entries, dist_spec_entries(p, r->tmp[i]));
    }
    n = walk(p, p->allDiff_head, &r->tmp, &r->tmp_cap);
    for (uint32_t i = n; i-- > 0;) {
        AllDiffSpec *a = POOL_PTR(p, r->tmp[i]);
        dvs_builder_add_all_different(b, a->n_vars, (const uint32_t *)(a + 1));
    }
    n = walk(p, p->sources_head, &r->tmp, &r->tmp_cap);
    for (uint32_t i = n; i-- > 0;) {
        SourceSpec *s = POOL_PTR(p, r->tmp[i]);
        dvs_builder_add_source(b, s->n_vars, source_spec_vars(p, r->tmp[i]));
    }
    dvs_problem_t *q = dvs_builder_finalize(b, size);
    if (q) q->flags = p->flags;
    return q;
}

/* ------------------------------------------------------------------ */
/* Pass: api / api-cache                                               */
/* ------------------------------------------------------------------ */

/* One randomize() call's problem as built through the API: the base problem
 * plus any constraints asserted under (push) after it was compiled, which
 * reach a CDCL context as add-ons (dvs_solver_add_constraint). */
typedef struct {
    dvs_problem_t *p;
    size_t         size;
    dvs_problem_t *aux[MAX_AUX];
    size_t         aux_size[MAX_AUX];
    uint32_t       n_aux;
} Built;

static void build(Rebuild *r, Capture *cp, Built *bt) {
    bt->p = rebuild(r, cp->p, &bt->size);
    bt->n_aux = cp->n_aux;
    for (uint32_t k = 0; k < cp->n_aux; k++) bt->aux[k] = rebuild(r, cp->aux[k], &bt->aux_size[k]);
}

static void built_free(Rebuild *r, Built *bt) {
    for (uint32_t k = 0; k < bt->n_aux; k++) dvs_builder_free_problem(r->b, bt->aux[k], bt->aux_size[k]);
    if (bt->p) dvs_builder_free_problem(r->b, bt->p, bt->size);
    bt->p = NULL;
    bt->n_aux = 0;
}

static uint64_t built_hash(Built *bt) {
    uint64_t h = fnv(bt->p, sizeof(dvs_problem_t) + bt->p->pool.used);
    for (uint32_t k = 0; k < bt->n_aux; k++)
        h = h * 31 + fnv(bt->aux[k], sizeof(dvs_problem_t) + bt->aux[k]->pool.used);
    return h;
}

typedef struct {
    void              *buf;
    dvs_block_alloc_t *ba;
    dvs_ctx_t         *ctx;
} Cdcl;

static size_t ctx_size_for(const dvs_problem_t *p) {
    size_t nc = (size_t)p->n_constraints + p->n_softs + p->n_dists + p->n_alldiffs;
    size_t est = (size_t)2 * 1024 * 1024 + (size_t)p->n_vars * 256 + nc * 512;
    return est > CTX_BUF_MAX ? CTX_BUF_MAX : est;
}

static void cdcl_close(Cdcl *c) {
    if (!c->ctx) return;
    dvs_solver_destroy(c->ctx);
    dvs_block_alloc_destroy(c->ba);
    free(c->buf);
    c->ctx = NULL;
}

/* Compile, growing the pool as the front end does (keeping a quarter of it
 * free for the add-ons), then add the add-ons. 0 ok, -2 unsat, -1 failed. */
static int cdcl_open(Cdcl *c, Built *bt) {
    for (size_t sz = ctx_size_for(bt->p);;) {
        c->buf = malloc(sz);
        c->ba = dvs_block_alloc_create(NULL, BA_BLOCK);
        c->ctx = dvs_solver_create(c->buf, sz, c->ba);
        c->ctx->incremental_capacity_hint = 8192;
        int rc = dvs_solver_compile(c->ctx, bt->p);
        if (rc == -2) return -2;
        size_t used = c->ctx->pool.used, cap = c->ctx->pool.capacity;
        size_t want = used / 4 > ((size_t)512 << 10) ? used / 4 : ((size_t)512 << 10);
        int grow = rc >= 0 ? cap - used < want && sz < CTX_BUF_MAX
                           : c->ctx->pool.overflow && sz < CTX_BUF_MAX;
        if (rc >= 0 && !grow) break;
        cdcl_close(c);
        if (!grow) return -1;
        sz = sz * 4 < CTX_BUF_MAX ? sz * 4 : CTX_BUF_MAX;
    }
    for (uint32_t k = 0; k < bt->n_aux; k++) {
        int rc = dvs_solver_add_constraint(c->ctx, bt->aux[k]);
        if (rc == -2) return -2;
        if (rc < 0 || c->ctx->pool.overflow) return -1;
    }
    return 0;
}

static volatile int64_t g_sink;
static int g_want_unsat;   /* the call being replayed was answered unsat */

/* The front end's CDCL solve (smt2_frontend.c, _cmd_check_sat_body): the same
 * options, and the same model validation before the answer is trusted.
 * 0 when the verdict matches the recorded one. */
static int cdcl_solve(Cdcl *c, Built *bt, uint64_t seed) {
    dvs_solve_opts_t o;
    memset(&o, 0, sizeof o);
    o.seed = seed;
    o.max_conflicts = 10000;
    o.max_restarts = 1000;
    o.use_lcg = 1;
    o.use_phase_save = 1;
    o.fair_pick = 1;
    dvs_result_t res = dvs_solver_solve(c->ctx, &o);
    if (g_want_unsat) return res == DVS_SOLVE_UNSAT ? 0 : -1;
    if (res != DVS_SOLVE_OK) return -1;
    int viol = dvs_solver_validate_model(c->ctx, bt->p, NULL);
    for (uint32_t k = 0; k < bt->n_aux; k++) viol += dvs_solver_validate_model(c->ctx, bt->aux[k], NULL);
    if (viol > 0) return -1;
    for (uint32_t v = 0; v < bt->p->n_vars; v++) g_sink += dvs_solver_get_value(c->ctx, v);
    return 0;
}

static int bb_check(dvs_bbsolver_t *bb, uint64_t seed) {
    if (!bb) return -1;
    int rc = dvs_bbsolver_check(bb, seed);
    return rc == (g_want_unsat ? DVS_BB_UNSAT : DVS_BB_SAT) ? 0 : -1;
}

static void bb_read(dvs_bbsolver_t *bb, uint32_t n_vars) {
    if (g_want_unsat) return;
    for (uint32_t v = 0; v < n_vars; v++) {
        int64_t x = 0;
        dvs_bbsolver_value(bb, v, &x);
        g_sink += x;
    }
}

typedef struct {
    double build, compile, solve, hit_solve, wall, cpu;
    size_t fails, bb_fails, escalated, hits, distinct;
} ApiStats;

/* A CDCL answer the front end would not accept: escalate to a fresh bitblast
 * solve, as it does. (Only calls with no add-ons reach bitblast this way.) */
static void cdcl_fail(ApiStats *st, Built *bt, uint64_t seed) {
    st->escalated++;
    if (bt->n_aux) { st->fails++; return; }
    dvs_bbsolver_t *bb = dvs_bbsolver_new(NULL, bt->p);
    if (bb_check(bb, seed)) st->fails++;
    else bb_read(bb, bt->p->n_vars);
    if (bb) dvs_bbsolver_free(bb);
}

/* Answer one call on a context that may already exist. */
static void answer(ApiStats *st, Capture *cp, Built *bt, Cdcl *cd, dvs_bbsolver_t **bb,
                   double *t_compile) {
    double t0 = now_s();
    if (cp->bitblast) {
        if (*bb) {
            double t1 = now_s();
            if (dvs_bbsolver_rediversify(*bb, cp->seed) != 0) st->bb_fails++;
            else bb_read(*bb, bt->p->n_vars);
            *t_compile = t1 - t0;
            return;
        }
        *bb = dvs_bbsolver_new(NULL, bt->p);
        *t_compile = now_s() - t0;
        if (bb_check(*bb, cp->seed)) st->bb_fails++;
        else bb_read(*bb, bt->p->n_vars);
        return;
    }
    int rc = 0;
    if (cd->ctx) dvs_solver_reset(cd->ctx);
    else rc = cdcl_open(cd, bt);
    *t_compile = now_s() - t0;
    if (rc == -2 ? !g_want_unsat : rc || cdcl_solve(cd, bt, cp->seed))
        cdcl_fail(st, bt, cp->seed);
}

typedef struct {
    uint64_t        hash;
    Cdcl            cdcl;
    dvs_bbsolver_t *bb;
    Built           bt;
} CacheEnt;

static void pass_api(int cached, ApiStats *st) {
    memset(st, 0, sizeof *st);
    Rebuild r;
    memset(&r, 0, sizeof r);
    r.b = dvs_builder_create(0, NULL);
    CacheEnt *cache = cached ? calloc(g_ncap ? g_ncap : 1, sizeof(CacheEnt)) : NULL;
    size_t ncache = 0, tab_cap = 1024;
    while (tab_cap < 2 * g_ncap) tab_cap <<= 1;
    CacheEnt **tab = calloc(tab_cap, sizeof(CacheEnt *));
    double c0 = cpu_s(), w0 = now_s();
    for (size_t i = 0; i < g_ncap; i++) {
        Capture *cp = &g_cap[i];
        g_want_unsat = cp->unsat;
        /* Every randomize() builds its problem (Verilator cannot know it is
         * unchanged without doing so); the cache only saves compile. */
        double a = now_s();
        Built bt;
        build(&r, cp, &bt);
        double b = now_s();
        st->build += b - a;
        CacheEnt *ce = NULL;
        int hit = 0;
        if (cached && !cp->unsat) {
            uint64_t h = built_hash(&bt);
            size_t slot = h & (tab_cap - 1);
            for (; tab[slot]; slot = (slot + 1) & (tab_cap - 1))
                if (tab[slot]->hash == h) { ce = tab[slot]; hit = 1; break; }
            if (!ce) {
                ce = tab[slot] = &cache[ncache++];
                ce->hash = h;
                ce->bt = bt;
            }
        }
        double tc;
        if (ce) {
            /* The front end moves a shape from CDCL to bitblast once CDCL
             * proves slow on it, so the engine this call needs may not exist
             * yet on its entry: answer() builds it then. */
            answer(st, cp, &ce->bt, &ce->cdcl, &ce->bb, &tc);
            if (hit) built_free(&r, &bt);
        } else {
            Cdcl cd = {0};
            dvs_bbsolver_t *bb = NULL;
            answer(st, cp, &bt, &cd, &bb, &tc);
            if (bb) dvs_bbsolver_free(bb);
            cdcl_close(&cd);
            built_free(&r, &bt);
        }
        double e = now_s();
        st->compile += tc;
        st->solve += e - b - tc;
        if (hit) { st->hits++; st->hit_solve += e - b - tc; }
    }
    st->wall = now_s() - w0;
    st->cpu = cpu_s() - c0;
    st->distinct = cached ? ncache : g_ncap;
    for (size_t j = 0; j < ncache; j++) {
        if (cache[j].bb) dvs_bbsolver_free(cache[j].bb);
        cdcl_close(&cache[j].cdcl);
        built_free(&r, &cache[j].bt);
    }
    free(cache);
    free(tab);
    dvs_builder_destroy(r.b);
    free(r.memo.key); free(r.memo.val); free(r.tmp);
}

/* ------------------------------------------------------------------ */

static int has(const char *list, const char *w) {
    size_t n = strlen(w);
    for (const char *p = list; (p = strstr(p, w)); p += n)
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *path = NULL, *solver = NULL, *passes = "pipe,text,api,api-cache";
    int reps = 3;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--solver") && i + 1 < argc) solver = argv[++i];
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--passes") && i + 1 < argc) passes = argv[++i];
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: %s <cmds.smt2> [--solver PATH] [--reps N] [--passes LIST]\n", argv[0]);
        return 2;
    }
    load(path);
    size_t n_check = 0, n_bytes = 0;
    for (size_t i = 0; i < g_ncmd; i++) { n_check += g_cmd[i].kind == K_CHECK; n_bytes += g_cmd[i].len + 1; }
    printf("session %s\n  %zu commands, %zu check-sats, %.1f MB of SMT-LIB\n", path, g_ncmd,
           n_check, n_bytes / 1e6);
    double per = 1e6 / (double)(n_check ? n_check : 1);

    if (has(passes, "pipe")) {
        if (!solver) { fprintf(stderr, "pipe pass needs --solver\n"); return 2; }
        double bw = 1e30, bc = 0;
        for (int k = 0; k < reps; k++) {
            double w, c;
            pass_pipe(solver, &w, &c);
            if (w < bw) { bw = w; bc = c; }
        }
        printf("pipe       wall %7.3f s  %7.1f us/check-sat   (solver CPU %.3f s)\n", bw, bw * per, bc);
    }

    /* The text pass always runs once, to capture the problems. */
    double t[K_N], tp, w, c, bt[K_N], btp = 0, bw = 1e30, bc = 0;
    for (int k = 0; k < (has(passes, "text") ? reps : 1); k++) {
        pass_text(k == 0, t, &tp, &w, &c);
        if (w < bw) { bw = w; bc = c; btp = tp; memcpy(bt, t, sizeof t); }
    }
    if (has(passes, "text")) {
        printf("text       wall %7.3f s  %7.1f us/check-sat   (CPU %.3f s)\n", bw, bw * per, bc);
        printf("             lex+parse        %7.3f s  %7.1f us\n", btp, btp * per);
        for (int k = 0; k < K_N; k++)
            if (bt[k] > 0) printf("             %-16s %7.3f s  %7.1f us\n", k_name[k], bt[k], bt[k] * per);
    }
    size_t nbb = 0, naux = 0, nun = 0;
    for (size_t i = 0; i < g_ncap; i++) {
        nbb += g_cap[i].bitblast;
        naux += g_cap[i].n_aux > 0;
        nun += g_cap[i].unsat;
    }
    printf("  captured %zu problems (%zu answered on bitblast, %zu with constraints added "
           "under push, %zu unsat), %zu check-sats not captured\n", g_ncap, nbb, naux, nun, g_nocap);

    for (int cached = 0; cached < 2; cached++) {
        if (!has(passes, cached ? "api-cache" : "api")) continue;
        ApiStats best = {0}, s;
        best.wall = 1e30;
        for (int k = 0; k < reps; k++) {
            pass_api(cached, &s);
            if (s.wall < best.wall) best = s;
        }
        double pc = 1e6 / (double)(g_ncap ? g_ncap : 1);
        printf("%-10s wall %7.3f s  %7.1f us/check-sat   (CPU %.3f s; %zu distinct, %zu reused; "
               "%zu CDCL answers escalated to bitblast, %zu bitblast failures, %zu unsolved)\n",
               cached ? "api-cache" : "api", best.wall, best.wall * pc, best.cpu, best.distinct,
               best.hits, best.escalated, best.bb_fails, best.fails);
        printf("             build (builder)  %7.3f s  %7.1f us\n", best.build, best.build * pc);
        printf("             compile/bitblast %7.3f s  %7.1f us\n", best.compile, best.compile * pc);
        printf("             solve+read       %7.3f s  %7.1f us\n", best.solve, best.solve * pc);
        if (cached && best.hits)
            printf("               on a reused context: %.1f us per call; on a new one: %.1f us\n",
                   1e6 * best.hit_solve / best.hits,
                   1e6 * (best.solve - best.hit_solve) / (double)(g_ncap - best.hits));
    }
    return 0;
}
