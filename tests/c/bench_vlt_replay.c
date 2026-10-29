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
 *            + solve; bitblast: dvs_bbsolver_resample with the new seed, or
 *            with --reuse rediversify the 0.3 re-flip of one model), as the
 *            lifecycle in docs/verilator_integration.md prescribes. The
 *            problem is still built every call.
 *
 * Usage: bench_vlt_replay <cmds.smt2> [--solver PATH] [--reps N]
 *        [--passes pipe,text,api,api-cache] [--quality] [--census] [--dump-values FILE]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
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
    uint32_t    call;      /* solver session: each starts with (reset); 0 is
                            * the runtime's start-up check before the first */
} Cmd;

static Cmd   *g_cmd;
static size_t g_ncmd;
static uint32_t g_ncalls;   /* solver sessions after the start-up check */
static uint32_t g_calls;    /* randomize() calls the run made (--calls; 0: one per session) */
static uint32_t g_warmup;   /* of which the first g_warmup are not timed (--warmup) */
static uint32_t g_skip;     /* sessions 1..g_skip are warm-up */
static int      g_warm_in;  /* warm-up could not be separated: it is in the average */
static int      g_vlt_mode; /* --mode verilator: the front end's Verilator mode */

/* A command or captured problem of session `call` counts toward per-call
 * time: not the start-up check, nor the warm-up calls (as the commercial
 * loop). A randomize() may take several sessions (solve-before phases, sizing
 * then elements); when every call takes the same number, the warm-up is its
 * first g_warmup calls' sessions. Otherwise (randc retries) the sessions
 * cannot be split by call, and the average includes the warm-up. */
static void split_calls(void) {
    uint32_t calls = g_calls ? g_calls : g_ncalls;
    g_warm_in = calls == 0 || g_ncalls % calls != 0;
    g_skip = g_warm_in ? 0 : g_ncalls / calls * g_warmup;
}
static int timed(uint32_t call) { return call > g_skip; }
static uint32_t n_timed(void) {
    uint32_t calls = g_calls ? g_calls : g_ncalls;
    return g_warm_in ? calls : calls > g_warmup ? calls - g_warmup : 0;
}

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
            /* One command per line: an unbalanced line is a broken capture
             * (e.g. a wrapped log line never joined), which the front end
             * would reject while the replay still timed the rest. */
            int depth = 0, quoted = 0;
            for (const char *q = p; q < e; q++) {
                if (*q == '|' || *q == '"') quoted ^= 1;
                else if (!quoted) depth += (*q == '(') - (*q == ')');
            }
            if (depth != 0) {
                fprintf(stderr, "%s: command %zu is not balanced (%d): %.60s...\n",
                        path, g_ncmd + 1, depth, p);
                exit(2);
            }
            if (g_ncmd == cap) g_cmd = realloc(g_cmd, (cap *= 2) * sizeof(Cmd));
            Cmd *c = &g_cmd[g_ncmd++];
            c->txt = p;
            c->len = (uint32_t)(e - p);
            c->kind = _kind(p);
            if (!strncmp(p, "(reset)", 7)) g_ncalls++;
            c->call = g_ncalls;
            c->replies = c->kind == K_CHECK || c->kind == K_GETVAL
                         || !strncmp(p, "(get-", 5);
        }
        if (!*e) break;
        *e = 0;
        p = e + 1;
    }
    /* Calls are the (reset)s that a check-sat follows: the session ends with one. */
    g_ncalls = 0;
    for (size_t i = 0; i < g_ncmd; i++)
        if (g_cmd[i].kind == K_CHECK && g_cmd[i].call > g_ncalls) g_ncalls = g_cmd[i].call;
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

/* --dump-values: one line per answered call of each api pass, for the
 * uniformity harness (tests/uniformity). --quality: check every model and
 * report how much each field varies. Both read the models of one extra,
 * untimed rep of the pass. */
static FILE *g_dump;
static int   g_quality;
static int   g_census;     /* --census: names the variables too */
static int   g_rediversify;/* --reuse rediversify: the 0.3 cache hit (re-flip one model) */
static int   g_collect;    /* this rep reads back models (dump and/or quality) */

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
    uint32_t       call;       /* randomize() call (Cmd.call) */
    uint32_t       n_named;    /* declared vars, for --dump-values / --quality */
    struct { char *name; uint32_t id; uint16_t width; } *named;
    int64_t       *vals;       /* the collecting rep's model of them */
    uint8_t        got;        /* ... when the call was answered sat */
} Capture;

static Capture *g_cap;
static const Capture *g_dcap;  /* the call being answered, while dumping */
static dvs_problem_t *g_pre[MAX_AUX];
static uint32_t g_pre_n;
static size_t   g_ncap, g_capcap;
static size_t   g_nocap;       /* check-sats with no problem to capture */

/* The cache key is timed, so hash a word at a time: byte-wise FNV over a
 * few-KB problem was ~15% of a small bench's per-call time. */
static uint64_t fnv(const void *d, size_t n) {
    const uint8_t *b = d;
    uint64_t h = 1469598103934665603ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 0x9e3779b97f4a7c15ull;
        h ^= h >> 32;
    }
    for (; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

/* One past the largest declared variable id. The front end numbers sparsely
 * (aux variables can sit far above n_vars), so n_vars is not a bound. */
static uint32_t var_bound(const dvs_problem_t *p) {
    uint32_t n = p->n_vars;
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL; ) {
        const VarSpec *vs = POOL_PTR((dvs_problem_t *)p, r);
        if (vs->var_id >= n) n = vs->var_id + 1;
        r = vs->next;
    }
    return n;
}

static void capture(Smt2Frontend *fe, uint32_t call) {
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
    c->call = call;
    c->n_aux = 0;
    c->n_named = 0;
    c->named = NULL;
    c->vals = NULL;
    c->got = 0;
    if (g_dump || g_quality || g_census) {
        uint32_t bound = var_bound(p);
        c->named = calloc(fe->n_vars ? fe->n_vars : 1, sizeof *c->named);
        for (uint32_t k = 0; k < fe->n_vars; k++) {
            const Smt2Var *v = &fe->vars[k];
            /* Sparse array elements (`__arr<id>_<k>`) are user fields. */
            if (v->var_id >= bound || v->width > 64 || v->name[0] == '\\'
                || (!strncmp(v->name, "__", 2) && strncmp(v->name, "__arr", 5)))
                continue;
            c->named[c->n_named].name = strdup(v->name);
            c->named[c->n_named].id = v->var_id;
            c->named[c->n_named++].width = v->width;
        }
    }
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

static void pre_add(const dvs_problem_t *a) {
    if (g_pre_n == MAX_AUX) { fprintf(stderr, "too many add-on problems\n"); exit(2); }
    g_pre[g_pre_n] = malloc(sizeof(dvs_problem_t) + a->pool.capacity);
    memcpy(g_pre[g_pre_n++], a, sizeof(dvs_problem_t) + a->pool.used);
}

/* Before a check-sat on a compiled CDCL context: the constraints it already
 * holds as add-ons, and those asserted since (under push) that the check-sat
 * is about to add. Copied now, since the front end may find the latter unsat
 * as it adds them, and never record them. Mirrors the front end's own
 * _validate_all: hand-off copies (an entry is NULL while the hand-off's items
 * live in the retained builder), the frozen vbase, and the retained builder
 * since the compile. */
static void capture_pre(Smt2Frontend *fe) {
    g_pre_n = 0;
    if (!fe->compiled || !fe->ctx) return;
    for (uint32_t k = 0; k < fe->n_aux_problems; k++)
        if (fe->aux_problems[k]) pre_add(fe->aux_problems[k]);
    if (fe->vbase) {
        dvs_problem_cut_t cut;
        if (dvs_problem_cut_since(fe->vbase, &fe->retain_mark, &cut) == 0) {
            if (fe->vbase->n_constraints || fe->vbase->n_vars)
                pre_add(fe->vbase);
            dvs_problem_uncut(fe->vbase, &cut);
        }
    }
    if (fe->cdcl_retained) {
        size_t sz;
        dvs_problem_t *a = dvs_builder_finalize_since(fe->builder, &fe->retain_mark, &sz);
        if (a && (a->n_constraints || a->n_vars)) {
            a->flags |= DVS_PROBLEM_F_EXPLICIT;
            pre_add(a);
        }
        if (a) dvs_builder_free_problem(fe->builder, a, sz);
    }
}

static void pass_text(int do_capture, double t[K_N], double *t_parse, double *wall,
                      double *cpu, double *timed_wall) {
    FILE *out = fopen("/dev/null", "w");
    Smt2Frontend fe;
    smt2_frontend_init(&fe, out, out);
    fe.verilator_mode = g_vlt_mode;
    SexprArena arena;
    sexpr_arena_init(&arena, 8192);
    memset(t, 0, K_N * sizeof(double));
    *t_parse = 0;
    *timed_wall = 0;
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
        if (timed(c->call)) *timed_wall += e - a;
        if (do_capture && c->kind == K_CHECK) capture(&fe, c->call);
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

/* --quality's bitblast check: rebuild g_pin's problem with each named
 * variable == its model value (g_pin_k < 0), or with variable g_pin_k != its
 * model value. */
static const Capture *g_pin;
static int g_pin_k = -1;

static void add_pins(dvs_builder_t *b, const Capture *cp) {
    for (uint32_t k = 0; k < cp->n_named; k++) {
        if (g_pin_k >= 0 && k != (uint32_t)g_pin_k) continue;
        uint16_t w = cp->named[k].width;
        uint64_t u = (uint64_t)cp->vals[k];
        if (w < 64) u &= (1ull << w) - 1;
        /* Same width on both sides: == is bit equality, whatever the signedness. */
        dvs_expr_t c = dvs_builder_expr_binary(b, g_pin_k < 0 ? DVS_BIN_EQ : DVS_BIN_NEQ,
                                               dvs_builder_expr_var(b, cp->named[k].id),
                                               dvs_builder_expr_const_sized(b, (int64_t)u, 0, w));
        dvs_builder_add_constraint(b, c);
    }
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
    if (g_pin && g_pin->p == p) add_pins(b, g_pin);
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
    dvs_ctx_t         *ctx;
} Cdcl;

static void cdcl_close(Cdcl *c) {
    if (!c->ctx) return;
    dvs_solver_free(c->ctx);
    c->ctx = NULL;
}

/* Compile as the front end does (a context that grows as needed), then add
 * the add-ons. 0 ok, -2 unsat, -1 failed. */
static int cdcl_open(Cdcl *c, Built *bt) {
    c->ctx = dvs_solver_new(NULL);
    if (!c->ctx) return -1;
    int rc = dvs_solver_compile(c->ctx, bt->p);
    if (rc == -2) return -2;
    if (rc < 0) { cdcl_close(c); return -1; }
    for (uint32_t k = 0; k < bt->n_aux; k++) {
        int rc = dvs_solver_add_constraint(c->ctx, bt->aux[k]);
        if (rc == -2) return -2;
        if (rc < 0 || c->ctx->pool.overflow) return -1;
    }
    return 0;
}

static volatile int64_t g_sink;

/* Keep the model of the call being answered; with --dump-values, also write
 * "V <call> <seed> name=#x... ..." with each value masked to its width. */
static void dump_model(dvs_ctx_t *ctx, dvs_bbsolver_t *bb) {
    Capture *cp = (Capture *)g_dcap;
    if (!g_collect || !cp) return;
    if (!cp->vals) cp->vals = calloc(cp->n_named ? cp->n_named : 1, sizeof *cp->vals);
    for (uint32_t k = 0; k < cp->n_named; k++) {
        int64_t x = 0;
        if (bb) dvs_bbsolver_value(bb, cp->named[k].id, &x);
        else x = dvs_solver_get_value(ctx, cp->named[k].id);
        cp->vals[k] = x;
    }
    cp->got = 1;
    if (!g_dump) return;
    fprintf(g_dump, "V %u %llu", cp->call, (unsigned long long)cp->seed);
    for (uint32_t k = 0; k < cp->n_named; k++) {
        uint64_t u = (uint64_t)cp->vals[k];
        if (cp->named[k].width < 64) u &= (1ull << cp->named[k].width) - 1;
        fprintf(g_dump, " %s=#x%llx", cp->named[k].name, (unsigned long long)u);
    }
    fputc('\n', g_dump);
}
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
    dump_model(c->ctx, NULL);
    return 0;
}

/* The front end's seeded check (smt2_frontend.c) simulates before SAT except
 * in Verilator mode; DV_SIM=0 turns it off. Set it the same way. */
static void bb_presim(dvs_bbsolver_t *bb) {
    const char *sim = getenv("DV_SIM");
    if (bb && !g_vlt_mode) dvs_bbsolver_set_presim(bb, !(sim && *sim == '0'));
}

static int bb_check(dvs_bbsolver_t *bb, uint64_t seed) {
    if (!bb) return -1;
    bb_presim(bb);
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
    dump_model(NULL, bb);
}

typedef struct {
    double build, compile, solve, hit_solve, wall, cpu;
    double timed_wall;   /* build + compile + solve of the timed calls */
    size_t fails, bb_fails, escalated, hits, distinct;
    int    reusing;      /* api-cache: contexts are kept for later calls */
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
            int rc = g_rediversify ? dvs_bbsolver_rediversify(*bb, cp->seed)
                                   : dvs_bbsolver_resample(*bb, cp->seed);
            if (rc != 0) st->bb_fails++;
            else bb_read(*bb, bt->p->n_vars);
            *t_compile = t1 - t0;
            return;
        }
        *bb = dvs_bbsolver_new(NULL, bt->p);
        /* A context kept for reuse records its clauses, for resample. */
        if (*bb && st->reusing && !g_rediversify) dvs_bbsolver_set_resample(*bb, 1);
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
    st->reusing = cached;
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
        g_dcap = cp;
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
        if (timed(cp->call)) st->timed_wall += e - a;
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
/* --census: components and constructs of the captured problems (P0.5) */
/* ------------------------------------------------------------------ */

/* Per randomize() call, the first problem answered sat (before Verilator's
 * parity asserts) is split into independent components. A variable a
 * top-level hard `var == const` fixes (a state value, rand_mode(0)) is
 * folded: it is that constant, and links nothing. The rest are joined by
 * the constraints, unique and arrays they share. Each component instance
 * gets a structural hash that includes its constants, so the repeat rate is
 * what a per-component cache keyed by (state, mode, bucket) would see.
 *
 * Classes, the sampler layer that could take the component (see
 * docs/sampling_techniques_research.md):
 *   free      no constraint: a uniform draw over the declared range
 *   interval  one variable (helper aux variables aside) under compares, inside,
 *             dist, masks and slices only: closed form
 *   small     box (product of declared ranges) <= 2^16: enumerate and index
 *   medium    box <= 2^64: size-wise a BDD candidate
 *   large     the rest
 * The box is an upper bound on the solution count. Keys hash constants as
 * written (a constant compare is not evaluated), so the component repeat rate
 * is a lower bound on what a key of the folded guards would see. Constructs
 * are those of the captured problem: Verilator's SMT-LIB lowers $countones,
 * for example, to adds of extracted bits. Calls with no constrained or free
 * variable (the start-up check) are not counted. */

enum {
    C_ARITH, C_MUL, C_DIVMOD, C_SHIFT, C_BITWISE, C_CMP, C_LOGIC, C_INSIDE, C_COUNTONES,
    C_CLOG2, C_SUM, C_ARRAY, C_SLICE, C_SOFT, C_DIST, C_UNIQUE, C_N
};
static const char *c_name[C_N] = {
    "arith", "mul", "divmod", "shift", "bitwise", "compare", "logic", "inside", "countones",
    "clog2", "sum", "array", "slice", "soft", "dist", "unique"
};
enum { K_FREE, K_INTERVAL, K_SMALL, K_MEDIUM, K_LARGE, K_NCLASS };
static const char *k_class[K_NCLASS] = {"free", "interval", "small", "medium", "large"};

typedef struct {
    dvs_problem_t *p;
    uint32_t       nv;
    uint8_t       *fixed;
    int64_t       *fixval;
    uint32_t      *parent;
    uint32_t      *stamp;     /* per node (ref / 4): generation of the last visit */
    uint64_t      *hval;      /* per node: structural hash, valid when hstamp is set */
    uint8_t       *hstamp;
    uint32_t       gen;
    uint32_t       mask;      /* constructs of the root being walked */
    uint32_t      *vars;      /* variables of the root being walked */
    uint32_t       n_vars, vars_cap;
} Cen;

static uint32_t uf_find(uint32_t *par, uint32_t x) {
    while (par[x] != x) x = par[x] = par[par[x]];
    return x;
}

static void uf_join(uint32_t *par, uint32_t a, uint32_t b) {
    a = uf_find(par, a);
    b = uf_find(par, b);
    if (a != b) par[a < b ? b : a] = a < b ? a : b;
}

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

static void cen_var(Cen *c, uint32_t v) {
    if (v >= c->nv || c->fixed[v]) return;
    if (c->n_vars == c->vars_cap)
        c->vars = realloc(c->vars, (c->vars_cap = c->vars_cap ? 2 * c->vars_cap : 64) * sizeof *c->vars);
    c->vars[c->n_vars++] = v;
}

/* The construct of a binary operator. */
static int bin_construct(int op) {
    switch (op) {
    case DVS_BIN_ADD: case DVS_BIN_SUB: return C_ARITH;
    case DVS_BIN_MUL: return C_MUL;
    case DVS_BIN_DIV: case DVS_BIN_MOD: return C_DIVMOD;
    case DVS_BIN_LSHIFT: case DVS_BIN_RSHIFT: case DVS_BIN_ASHR: return C_SHIFT;
    case DVS_BIN_BAND: case DVS_BIN_BOR: case DVS_BIN_BXOR: return C_BITWISE;
    case DVS_BIN_AND: case DVS_BIN_OR: return C_LOGIC;
    default: return C_CMP;
    }
}

/* Walk one node: collect its variables and constructs (once per root), and
 * return its structural hash (once per problem). Folded variables hash as
 * their value. */
static uint64_t cen_walk(Cen *c, dvs_expr_t ref, int depth) {
    if (ref == EXPR_NULL) return 0x51;
    uint32_t ix = ref >> 2;
    int seen = c->stamp[ix] == c->gen;
    c->stamp[ix] = c->gen;
    if (seen && c->hstamp[ix]) return c->hval[ix];
    if (depth > 20000) return mix(0x77, ref);      /* too deep: hash by position */
    dvs_problem_t *p = c->p;
    ExprKind *k = POOL_PTR(p, ref);
    uint64_t h = mix(0xabc, (uint64_t)*k);
    switch (*k) {
    case EXPR_CONST: {
        ExprConst *n = (ExprConst *)k;
        h = mix(mix(h, (uint64_t)n->value), n->width);
        break;
    }
    case EXPR_VAR: {
        uint32_t v = ((ExprVar *)k)->var_id;
        if (v < c->nv && c->fixed[v]) h = mix(mix(0xc0, (uint64_t)c->fixval[v]), 1);
        else { h = mix(h, v); cen_var(c, v); }
        break;
    }
    case EXPR_BINARY: {
        ExprBinary *n = (ExprBinary *)k;
        c->mask |= 1u << bin_construct(n->op);
        h = mix(h, n->op);
        h = mix(h, cen_walk(c, n->lhs, depth + 1));
        h = mix(h, cen_walk(c, n->rhs, depth + 1));
        break;
    }
    case EXPR_UNARY: {
        ExprUnary *n = (ExprUnary *)k;
        c->mask |= 1u << (n->op == DVS_UN_NEG ? C_ARITH : n->op == DVS_UN_NOT ? C_LOGIC : C_BITWISE);
        h = mix(mix(h, n->op), cen_walk(c, n->operand, depth + 1));
        break;
    }
    case EXPR_ITE: {
        ExprITE *n = (ExprITE *)k;
        c->mask |= 1u << C_LOGIC;
        h = mix(h, cen_walk(c, n->cond, depth + 1));
        h = mix(h, cen_walk(c, n->then_e, depth + 1));
        h = mix(h, cen_walk(c, n->else_e, depth + 1));
        break;
    }
    case EXPR_IN_RANGE: {
        ExprInRange *n = (ExprInRange *)k;
        c->mask |= 1u << C_INSIDE;
        h = mix(h, cen_walk(c, n->value, depth + 1));
        h = mix(h, cen_walk(c, n->lo, depth + 1));
        h = mix(h, cen_walk(c, n->hi, depth + 1));
        break;
    }
    case EXPR_IN_SET: {
        ExprInSet *n = (ExprInSet *)k;
        c->mask |= 1u << C_INSIDE;
        h = mix(h, cen_walk(c, n->value, depth + 1));
        dvs_expr_t *el = expr_in_set_elems(p, ref);
        for (uint32_t i = 0; i < n->n_elems; i++) h = mix(h, cen_walk(c, el[i], depth + 1));
        break;
    }
    case EXPR_IN_RANGES: {
        ExprInRanges *n = (ExprInRanges *)k;
        c->mask |= 1u << C_INSIDE;
        h = mix(h, cen_walk(c, n->value, depth + 1));
        dvs_expr_t *lo = expr_in_ranges_los(p, ref), *hi = expr_in_ranges_his(p, ref);
        for (uint32_t i = 0; i < n->n_ranges; i++) {
            h = mix(h, cen_walk(c, lo[i], depth + 1));
            h = mix(h, cen_walk(c, hi[i], depth + 1));
        }
        break;
    }
    case EXPR_EXTEND: {
        ExprExtend *n = (ExprExtend *)k;
        c->mask |= 1u << C_SLICE;
        h = mix(mix(mix(h, n->from_bits), n->to_bits * 2 + n->sign_extend),
                cen_walk(c, n->operand, depth + 1));
        break;
    }
    case EXPR_SV_CAST: {
        ExprSvCast *n = (ExprSvCast *)k;
        h = mix(mix(mix(h, n->from_bits), n->to_bits * 4 + n->sign_extend * 2 + n->dst_signed),
                cen_walk(c, n->operand, depth + 1));
        break;
    }
    case EXPR_EXTRACT: {
        ExprExtract *n = (ExprExtract *)k;
        c->mask |= 1u << C_SLICE;
        h = mix(mix(h, n->hi_bit * 256 + n->lo_bit), cen_walk(c, n->operand, depth + 1));
        break;
    }
    case EXPR_CONCAT: {
        ExprConcat *n = (ExprConcat *)k;
        c->mask |= 1u << C_SLICE;
        h = mix(h, n->lo_width);
        h = mix(h, cen_walk(c, n->hi, depth + 1));
        h = mix(h, cen_walk(c, n->lo, depth + 1));
        break;
    }
    case EXPR_SUM: {
        ExprSum *n = (ExprSum *)k;
        c->mask |= 1u << C_SUM;
        dvs_expr_t *el = (dvs_expr_t *)((char *)n + sizeof(ExprSum));
        h = mix(h, cen_walk(c, n->result, depth + 1));
        for (uint32_t i = 0; i < n->n_vars; i++) h = mix(h, cen_walk(c, el[i], depth + 1));
        break;
    }
    case EXPR_COUNTONES: {
        ExprCountones *n = (ExprCountones *)k;
        c->mask |= 1u << C_COUNTONES;
        h = mix(h, cen_walk(c, n->result, depth + 1));
        h = mix(h, cen_walk(c, n->operand, depth + 1));
        break;
    }
    case EXPR_CLOG2: {
        ExprClog2 *n = (ExprClog2 *)k;
        c->mask |= 1u << C_CLOG2;
        h = mix(h, cen_walk(c, n->result, depth + 1));
        h = mix(h, cen_walk(c, n->operand, depth + 1));
        break;
    }
    case EXPR_ARRAY_SELECT: {
        ExprArraySelect *n = (ExprArraySelect *)k;
        c->mask |= 1u << C_ARRAY;
        h = mix(mix(h, n->base_var_id), n->n_elems);
        for (uint32_t i = 0; i < n->n_elems; i++) {
            uint32_t v = n->base_var_id + i;
            if (v < c->nv && c->fixed[v]) h = mix(h, (uint64_t)c->fixval[v]);
            else cen_var(c, v);
        }
        h = mix(h, cen_walk(c, n->result, depth + 1));
        h = mix(h, cen_walk(c, n->index, depth + 1));
        break;
    }
    }
    c->hval[ix] = h;
    c->hstamp[ix] = 1;
    return h;
}

/* A component site: the same set of variables over the calls of a session. */
typedef struct {
    uint64_t  site;           /* hash of the variable set */
    uint32_t  first_var, n_vars, n_aux;
    double    box_log2;       /* max over instances */
    uint32_t  mask;           /* union over instances */
    uint32_t  cls;            /* worst over instances */
    size_t    instances;
    uint64_t *keys;           /* distinct instance hashes */
    size_t    n_keys, keys_cap;
} Site;

static int u64_add(uint64_t **k, size_t *n, size_t *cap, uint64_t v) {
    /* Open addressing, 0 = empty (a hash of 0 is stored as 1). */
    if (!v) v = 1;
    if (2 * (*n + 1) > *cap) {
        size_t oc = *cap, nc = oc ? 2 * oc : 16;
        uint64_t *old = *k, *nk = calloc(nc, sizeof *nk);
        for (size_t i = 0; i < oc; i++)
            if (old[i]) {
                size_t s = old[i] & (nc - 1);
                while (nk[s]) s = (s + 1) & (nc - 1);
                nk[s] = old[i];
            }
        free(old);
        *k = nk;
        *cap = nc;
    }
    size_t s = v & (*cap - 1);
    for (; (*k)[s]; s = (s + 1) & (*cap - 1))
        if ((*k)[s] == v) return 0;
    (*k)[s] = v;
    (*n)++;
    return 1;
}

static const char *var_name(const Capture *cp, uint32_t v) {
    for (uint32_t k = 0; k < cp->n_named; k++)
        if (cp->named[k].id == v) return cp->named[k].name;
    return NULL;
}

static double var_box_log2(const VarSpec *vs) {
    double w = vs->width;
    if (vs->hi > vs->lo) {
        double r = log2((double)vs->hi - (double)vs->lo + 1.0);
        if (r < w) w = r;
    }
    return w;
}

static void census(void) {
    size_t calls = 0, inst = 0, cls_inst[K_NCLASS] = {0}, con_inst[C_N] = {0};
    size_t calls_closed = 0, calls_small = 0, max_comp = 0;
    uint64_t *pk = NULL, *ik = NULL;
    size_t pn = 0, pcap = 0, in = 0, icap = 0;
    Site *sites = NULL;
    size_t n_sites = 0, sites_cap = 0;
    uint32_t last_call = UINT32_MAX;
    Cen c;
    memset(&c, 0, sizeof c);
    dvs_expr_t *tmp = NULL;
    uint32_t tmp_cap = 0;
    for (size_t i = 0; i < g_ncap; i++) {
        Capture *cp = &g_cap[i];
        if (cp->unsat || cp->call == last_call) continue;
        last_call = cp->call;
        dvs_problem_t *p = cp->p;
        uint32_t nv = var_bound(p);
        c.p = p;
        c.nv = nv;
        c.fixed = calloc(nv + 1, 1);
        c.fixval = calloc(nv + 1, sizeof *c.fixval);
        c.parent = malloc((nv + 1) * sizeof *c.parent);
        for (uint32_t v = 0; v < nv; v++) c.parent[v] = v;
        size_t nodes = p->pool.used / 4 + 2;
        c.stamp = calloc(nodes, sizeof *c.stamp);
        c.hval = malloc(nodes * sizeof *c.hval);
        c.hstamp = calloc(nodes, 1);
        c.gen = 0;
        VarSpec **spec = calloc(nv + 1, sizeof *spec);
        uint32_t n = walk(p, p->vars_head, &tmp, &tmp_cap);
        for (uint32_t j = 0; j < n; j++) {
            VarSpec *vs = POOL_PTR(p, tmp[j]);
            if (vs->var_id < nv) spec[vs->var_id] = vs;
        }
        /* Fold: hard `var == const` at the top level. */
        n = walk(p, p->constraints_head, &tmp, &tmp_cap);
        uint8_t *fold_root = calloc(n + 1, 1);
        for (uint32_t j = 0; j < n; j++) {
            ConstraintSpec *cs = POOL_PTR(p, tmp[j]);
            ExprKind *rk = POOL_PTR(p, cs->root);
            if (*rk != EXPR_BINARY || ((ExprBinary *)rk)->op != DVS_BIN_EQ) continue;
            ExprBinary *b = (ExprBinary *)rk;
            ExprKind *l = POOL_PTR(p, b->lhs), *r = POOL_PTR(p, b->rhs);
            if (*l == EXPR_CONST && *r == EXPR_VAR) { ExprKind *t = l; l = r; r = t; }
            if (*l != EXPR_VAR || *r != EXPR_CONST) continue;
            uint32_t v = ((ExprVar *)l)->var_id;
            if (v >= nv || c.fixed[v]) continue;
            c.fixed[v] = 1;
            c.fixval[v] = ((ExprConst *)r)->value;
            fold_root[j] = 1;
        }
        /* Roots: hard constraints, softs; each joins its variables. */
        uint64_t *rhash = NULL;
        uint32_t *rcomp = NULL, *rmask = NULL;
        size_t nr = 0, rcap = 0;
        uint8_t *used = calloc(nv + 1, 1);
        uint32_t ncons = n;
        for (int soft = 0; soft < 2; soft++) {
            uint32_t m = soft ? walk(p, p->softs_head, &tmp, &tmp_cap) : ncons;
            if (!soft) walk(p, p->constraints_head, &tmp, &tmp_cap);
            for (uint32_t j = 0; j < m; j++) {
                if (!soft && fold_root[j]) continue;
                dvs_expr_t root = soft ? ((SoftSpec *)POOL_PTR(p, tmp[j]))->root
                                       : ((ConstraintSpec *)POOL_PTR(p, tmp[j]))->root;
                c.gen++;
                c.mask = soft ? 1u << C_SOFT : 0;
                c.n_vars = 0;
                uint64_t h = mix(cen_walk(&c, root, 0), soft);
                if (!c.n_vars) continue;          /* constant after folding */
                for (uint32_t q = 1; q < c.n_vars; q++) uf_join(c.parent, c.vars[0], c.vars[q]);
                for (uint32_t q = 0; q < c.n_vars; q++) used[c.vars[q]] = 1;
                if (nr == rcap) {
                    rcap = rcap ? 2 * rcap : 64;
                    rhash = realloc(rhash, rcap * sizeof *rhash);
                    rcomp = realloc(rcomp, rcap * sizeof *rcomp);
                    rmask = realloc(rmask, rcap * sizeof *rmask);
                }
                rhash[nr] = h;
                rcomp[nr] = c.vars[0];
                rmask[nr++] = c.mask;
            }
        }
        uint32_t *vmask = calloc(nv + 1, sizeof *vmask);    /* dist / unique per var */
        n = walk(p, p->dists_head, &tmp, &tmp_cap);
        for (uint32_t j = 0; j < n; j++) {
            DistSpec *d = POOL_PTR(p, tmp[j]);
            if (d->var_id < nv && !c.fixed[d->var_id]) { vmask[d->var_id] |= 1u << C_DIST; used[d->var_id] = 1; }
        }
        n = walk(p, p->allDiff_head, &tmp, &tmp_cap);
        for (uint32_t j = 0; j < n; j++) {
            AllDiffSpec *a = POOL_PTR(p, tmp[j]);
            const uint32_t *av = (const uint32_t *)(a + 1);
            uint32_t first = UINT32_MAX;
            for (uint32_t q = 0; q < a->n_vars; q++) {
                if (av[q] >= nv || c.fixed[av[q]]) continue;
                vmask[av[q]] |= 1u << C_UNIQUE;
                used[av[q]] = 1;
                if (first == UINT32_MAX) first = av[q];
                else uf_join(c.parent, first, av[q]);
            }
        }
        /* Components: group variables (and roots) by representative. */
        size_t ncomp = 0;
        uint32_t *rep_ix = malloc((nv + 1) * sizeof *rep_ix);
        for (uint32_t v = 0; v < nv; v++) rep_ix[v] = UINT32_MAX;
        typedef struct { uint64_t site, key; uint32_t first, nvars, naux, mask; double box; } Comp;
        Comp *comp = calloc(nv + 1, sizeof *comp);
        for (uint32_t v = 0; v < nv; v++) {
            if (c.fixed[v] || !spec[v]) continue;
            if (!used[v] && spec[v]->is_aux) continue;
            uint32_t r = uf_find(c.parent, v);
            if (rep_ix[r] == UINT32_MAX) {
                rep_ix[r] = (uint32_t)ncomp;
                comp[ncomp].site = 0x5173;
                comp[ncomp].key = 0x3eed;
                comp[ncomp++].first = v;
            }
            Comp *k = &comp[rep_ix[r]];
            k->site = mix(k->site, v);          /* v ascending: order-independent */
            k->mask |= vmask[v];
            if (spec[v]->is_aux) k->naux++;
            else { k->nvars++; k->box += var_box_log2(spec[v]); }
            k->key = mix(k->key, mix(spec[v]->width, (uint64_t)spec[v]->lo ^ ((uint64_t)spec[v]->hi << 1)));
        }
        /* Root hashes into their component's key, order-independently (sum). */
        uint64_t *ksum = calloc(ncomp + 1, sizeof *ksum);
        for (size_t j = 0; j < nr; j++) {
            uint32_t ci = rep_ix[uf_find(c.parent, rcomp[j])];
            if (ci == UINT32_MAX) continue;
            ksum[ci] += mix(rhash[j], 0x99);
            comp[ci].mask |= rmask[j];
        }
        int all_closed = 1, all_small = 1;
        size_t user_comp = 0;
        for (size_t ci = 0; ci < ncomp; ci++) {
            Comp *k = &comp[ci];
            if (!k->nvars) continue;            /* aux only */
            user_comp++;
            uint64_t key = mix(mix(k->key, ksum[ci]), k->site);
            /* Constructs a closed-form single-variable sampler covers. */
            uint32_t closed = (1u << C_CMP) | (1u << C_INSIDE) | (1u << C_LOGIC) | (1u << C_DIST) |
                              (1u << C_SOFT) | (1u << C_BITWISE) | (1u << C_SLICE);
            uint32_t cls;
            int constrained = k->mask != 0 || k->naux;
            if (!constrained) cls = K_FREE;
            else if (k->nvars == 1 && !(k->mask & ~closed)) cls = K_INTERVAL;
            else if (k->box <= 16.0) cls = K_SMALL;
            else if (k->box <= 64.0) cls = K_MEDIUM;
            else cls = K_LARGE;
            if (cls > K_INTERVAL) all_closed = 0;
            if (cls > K_SMALL) all_small = 0;
            cls_inst[cls]++;
            for (int b = 0; b < C_N; b++) if (k->mask & (1u << b)) con_inst[b]++;
            inst++;
            u64_add(&ik, &in, &icap, key);
            Site *s = NULL;
            for (size_t q = 0; q < n_sites; q++) if (sites[q].site == k->site) { s = &sites[q]; break; }
            if (!s) {
                if (n_sites == sites_cap) sites = realloc(sites, (sites_cap = sites_cap ? 2 * sites_cap : 64) * sizeof *sites);
                s = &sites[n_sites++];
                memset(s, 0, sizeof *s);
                s->site = k->site;
                s->first_var = k->first;
                s->n_vars = k->nvars;
                s->n_aux = k->naux;
            }
            s->instances++;
            if (k->box > s->box_log2) s->box_log2 = k->box;
            if (cls > s->cls) s->cls = cls;
            s->mask |= k->mask;
            u64_add(&s->keys, &s->n_keys, &s->keys_cap, key);
        }
        if (user_comp) {
            calls++;
            u64_add(&pk, &pn, &pcap, cp->hash);
            if (user_comp > max_comp) max_comp = user_comp;
            calls_closed += all_closed;
            calls_small += all_small;
        }
        free(ksum); free(comp); free(rep_ix); free(vmask); free(used);
        free(rhash); free(rcomp); free(rmask); free(fold_root); free(spec);
        free(c.fixed); free(c.fixval); free(c.parent); free(c.stamp); free(c.hval); free(c.hstamp);
    }
    free(tmp);
    free(c.vars);
    double dc = calls ? (double)calls : 1, di = inst ? (double)inst : 1;
    printf("census     %zu calls, %zu distinct problems (call repeat %.3f); components per call "
           "mean %.2f max %zu; %zu component instances, %zu distinct (component repeat %.3f)\n",
           calls, pn, calls ? 1.0 - pn / dc : 0.0, inst / dc, max_comp, inst, in,
           inst ? 1.0 - in / di : 0.0);
    printf("census     calls all closed-form (free/interval) %.3f, all <= small %.3f\n",
           calls_closed / dc, calls_small / dc);
    printf("census     class");
    for (int k = 0; k < K_NCLASS; k++) printf(" %s %.3f", k_class[k], cls_inst[k] / di);
    printf("\ncensus     construct");
    for (int b = 0; b < C_N; b++) if (con_inst[b]) printf(" %s %.3f", c_name[b], con_inst[b] / di);
    printf("\n");
    /* Sites, most instances first. */
    for (size_t a = 0; a < n_sites; a++)
        for (size_t b = a + 1; b < n_sites; b++)
            if (sites[b].instances > sites[a].instances) { Site t = sites[a]; sites[a] = sites[b]; sites[b] = t; }
    for (size_t a = 0; a < n_sites && a < 20; a++) {
        Site *s = &sites[a];
        const char *nm = NULL;
        for (size_t i = 0; i < g_ncap && !nm; i++) nm = var_name(&g_cap[i], s->first_var);
        printf("             site %s vars %u aux %u box2 %.1f class %s instances %zu keys %zu constructs ",
               nm ? nm : "?", s->n_vars, s->n_aux, s->box_log2, k_class[s->cls], s->instances, s->n_keys);
        int any = 0;
        for (int b = 0; b < C_N; b++)
            if (s->mask & (1u << b)) printf("%s%s", any++ ? "," : "", c_name[b]);
        printf("%s\n", any ? "" : "-");
        free(s->keys);
    }
    for (size_t a = 20; a < n_sites; a++) free(sites[a].keys);
    free(sites); free(pk); free(ik);
}

/* ------------------------------------------------------------------ */
/* --quality: the collecting rep's models                              */
/* ------------------------------------------------------------------ */

/* A CDCL context of a call's problem, to check models on, independent of the
 * engine that answered. It compiles the API rebuild, as the api passes do
 * (the front end's own problem may number its variables sparsely). NULL when
 * CDCL cannot compile it. */
static dvs_ctx_t *check_ctx(Rebuild *r, Capture *cp) {
    dvs_ctx_t *c = dvs_solver_new(NULL);
    if (!c) return NULL;
    Built bt;
    build(r, cp, &bt);
    int rc = dvs_solver_compile(c, bt.p);
    for (uint32_t k = 0; rc >= 0 && k < bt.n_aux; k++)
        rc = dvs_solver_add_constraint(c, bt.aux[k]);
    built_free(r, &bt);
    if (rc < 0 || c->pool.overflow) { dvs_solver_free(c); return NULL; }
    return c;
}

static dvs_result_t check_solve(dvs_ctx_t *c, uint64_t seed) {
    dvs_solve_opts_t o;
    memset(&o, 0, sizeof o);
    o.seed = seed;
    o.max_conflicts = 10000;
    o.max_restarts = 1000;
    o.use_lcg = 1;
    o.fair_pick = 1;
    return dvs_solver_solve(c, &o);
}

/* Where CDCL cannot compile the problem (it refuses a sparse variable
 * numbering that bitblast accepts): a fresh bitblast solve of the problem
 * with the model pinned (k < 0) or field k excluded. Calls with add-ons
 * reach only CDCL, so they stay unchecked. */
static dvs_result_t check_bb(Rebuild *r, Capture *cp, int k) {
    if (cp->n_aux) return DVS_SOLVE_TIMEOUT;
    g_pin = cp;
    g_pin_k = k;
    Built bt;
    build(r, cp, &bt);
    g_pin = NULL;
    g_pin_k = -1;
    dvs_result_t res = DVS_SOLVE_TIMEOUT;
    dvs_bbsolver_t *bb = bt.p ? dvs_bbsolver_new(NULL, bt.p) : NULL;
    if (bb) {
        int rc = dvs_bbsolver_check(bb, cp->seed);
        res = rc == DVS_BB_SAT ? DVS_SOLVE_OK : rc == DVS_BB_UNSAT ? DVS_SOLVE_UNSAT : DVS_SOLVE_TIMEOUT;
        dvs_bbsolver_free(bb);
    }
    built_free(r, &bt);
    return res;
}

typedef struct { const char *name; uint64_t u; uint32_t cap, k; } QVal;

static int qval_cmp(const void *a, const void *b) {
    const QVal *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    if (c) return c;
    return x->u < y->u ? -1 : x->u > y->u;
}

/* Every model is pinned into a CDCL context of its problem (else a bitblast
 * one): sat means the model satisfies every constraint, whatever its construct. Per field
 * (variable name, over all calls): distinct values / models, and Shannon
 * entropy normalized by log(models), as simbench's metric.py computes them.
 * A field with one value is forced when excluding that value is unsat, and
 * stuck when it is sat: a sampler that never moves a free field. */
static void quality(const char *pass) {
    size_t n_models = 0, checked = 0, invalid = 0, unchecked = 0, n_ent = 0;
    size_t tab_cap = 1024;
    while (tab_cap < 2 * g_ncap) tab_cap <<= 1;
    struct { uint64_t hash; dvs_ctx_t *ctx; int tried; } *tab = calloc(tab_cap, sizeof *tab);
    Rebuild r;
    memset(&r, 0, sizeof r);
    r.b = dvs_builder_create(0, NULL);
    for (size_t i = 0; i < g_ncap; i++) {
        Capture *cp = &g_cap[i];
        if (!cp->got || cp->unsat) continue;
        n_models++;
        n_ent += cp->n_named;
        size_t slot = cp->hash & (tab_cap - 1);
        while (tab[slot].tried && tab[slot].hash != cp->hash) slot = (slot + 1) & (tab_cap - 1);
        if (!tab[slot].tried) {
            tab[slot].tried = 1;
            tab[slot].hash = cp->hash;
            tab[slot].ctx = check_ctx(&r, cp);
        }
        dvs_ctx_t *c = tab[slot].ctx;
        dvs_result_t res;
        if (c) {
            dvs_solver_reset(c);
            int pinned = 1;
            for (uint32_t k = 0; pinned && k < cp->n_named; k++)
                pinned = dvs_solver_pin_var(c, cp->named[k].id, cp->vals[k]) == 0;
            res = pinned ? check_solve(c, cp->seed) : DVS_SOLVE_UNSAT;
        } else {
            res = check_bb(&r, cp, -1);
        }
        if (res == DVS_SOLVE_OK) checked++;
        else if (res == DVS_SOLVE_UNSAT) {
            if (invalid++ < 5) fprintf(stderr, "%s: call %u seed %llu: model violates the "
                                       "constraints\n", pass, cp->call, (unsigned long long)cp->seed);
        } else unchecked++;
    }
    for (size_t j = 0; j < tab_cap; j++) if (tab[j].ctx) dvs_solver_free(tab[j].ctx);
    free(tab);

    QVal *e = malloc((n_ent ? n_ent : 1) * sizeof *e);
    size_t ne = 0;
    for (size_t i = 0; i < g_ncap; i++) {
        Capture *cp = &g_cap[i];
        if (!cp->got || cp->unsat) continue;
        for (uint32_t k = 0; k < cp->n_named; k++) {
            uint64_t u = (uint64_t)cp->vals[k];
            if (cp->named[k].width < 64) u &= (1ull << cp->named[k].width) - 1;
            e[ne++] = (QVal){cp->named[k].name, u, (uint32_t)i, k};
        }
    }
    qsort(e, ne, sizeof *e, qval_cmp);
    char *lines = NULL;
    size_t lines_n = 0;
    FILE *fl = open_memstream(&lines, &lines_n);
    uint32_t n_fields = 0, n_forced = 0, n_stuck = 0, n_undecided = 0;
    double min_d = 2, min_h = 2;
    for (size_t a = 0, b; a < ne; a = b) {
        size_t distinct = 0;
        double h = 0;
        for (b = a; b < ne && !strcmp(e[b].name, e[a].name); ) {
            size_t c = b;
            while (c < ne && !strcmp(e[c].name, e[a].name) && e[c].u == e[b].u) c++;
            distinct++;
            double p = (double)(c - b);
            h -= p * log(p);
            b = c;
        }
        size_t n = b - a;
        h = n > 1 ? (h / n + log((double)n)) / log((double)n) : 0;
        double d = (double)distinct / (double)n;
        const char *tag = "";
        if (distinct == 1 && n > 1) {
            /* Forced or stuck, decided on the first problem that has it. */
            Capture *cp = &g_cap[e[a].cap];
            dvs_ctx_t *c = check_ctx(&r, cp);
            int rc = c ? dvs_solver_exclude_value(c, cp->named[e[a].k].id, cp->vals[e[a].k]) : 0;
            dvs_result_t res = !c ? check_bb(&r, cp, (int)e[a].k)
                             : rc ? DVS_SOLVE_UNSAT : check_solve(c, cp->seed);
            if (c) dvs_solver_free(c);
            if (res == DVS_SOLVE_UNSAT) { tag = " forced"; n_forced++; }
            else if (res == DVS_SOLVE_OK) { tag = " stuck"; n_stuck++; }
            else { tag = " undecided"; n_undecided++; }
        }
        n_fields++;
        if (strcmp(tag, " forced")) {
            if (d < min_d) min_d = d;
            if (h < min_h) min_h = h;
        }
        fprintf(fl, "             field %s n %zu distinct %zu ratio %.4f entropy %.4f%s\n",
                e[a].name, n, distinct, d, h, tag);
    }
    fclose(fl);
    dvs_builder_destroy(r.b);
    free(r.memo.key); free(r.memo.val); free(r.tmp);
    printf("%-10s quality %zu models: %zu checked, %zu invalid, %zu unchecked; %u fields: "
           "%u forced, %u stuck, %u undecided", pass, n_models, checked, invalid, unchecked,
           n_fields, n_forced, n_stuck, n_undecided);
    if (min_d <= 1) printf("; min distinct %.4f, min entropy %.4f\n", min_d, min_h);
    else printf("; min distinct -, min entropy -\n");
    fputs(lines ? lines : "", stdout);
    free(lines);
    free(e);
}

/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Calibration                                                          */
/* ------------------------------------------------------------------ */

/* Fixed work, timed, so records from different hosts (or a loaded host) can
 * be put on one scale: a plain integer loop (host speed alone) and a fixed
 * small problem solved on a reused context and compiled afresh (host speed
 * times this library's build). Best of 3 of each. */
static void calib(void) {
    const uint64_t n_iter = 50 * 1000 * 1000;
    double best_cpu = 1e30, best_solve = 1e30, best_compile = 1e30;
    for (int rep = 0; rep < 3; rep++) {
        uint64_t x = 88172645463325252ull;
        double a = now_s();
        for (uint64_t i = 0; i < n_iter; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
        double e = now_s() - a;
        g_sink += (int64_t)x;
        if (e < best_cpu) best_cpu = e;
    }
    /* a + b == c, a < b, c < 50000, d != a, d < 1000: 16-bit, CDCL. */
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    for (uint32_t v = 0; v < 4; v++) dvs_builder_add_var(b, v, 16, 0, 0, 65535);
    dvs_expr_t va = dvs_builder_expr_var(b, 0), vb = dvs_builder_expr_var(b, 1);
    dvs_expr_t vc = dvs_builder_expr_var(b, 2), vd = dvs_builder_expr_var(b, 3);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_binary(b, DVS_BIN_ADD, va, vb), vc));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT, va, vb));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT, vc,
        dvs_builder_expr_const(b, 50000, 0)));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_NEQ, vd, va));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT, vd,
        dvs_builder_expr_const(b, 1000, 0)));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_solve_opts_t o;
    memset(&o, 0, sizeof o);
    o.fair_pick = 1;
    const int n_solve = 20000, n_compile = 2000;
    int bad = 0;
    for (int rep = 0; rep < 3; rep++) {
        dvs_ctx_t *ctx = dvs_solver_new(NULL);
        if (!ctx || dvs_solver_compile(ctx, p) != 0) { bad = 1; break; }
        double a = now_s();
        for (int i = 0; i < n_solve; i++) {
            dvs_solver_reset(ctx);
            o.seed = (uint64_t)i + 1;
            if (dvs_solver_solve(ctx, &o) != DVS_SOLVE_OK) bad = 1;
            g_sink += dvs_solver_get_value(ctx, 2);
        }
        double e = (now_s() - a) / n_solve;
        dvs_solver_free(ctx);
        if (e < best_solve) best_solve = e;
        a = now_s();
        for (int i = 0; i < n_compile; i++) {
            ctx = dvs_solver_new(NULL);
            if (!ctx || dvs_solver_compile(ctx, p) != 0) bad = 1;
            if (ctx) dvs_solver_free(ctx);
        }
        e = (now_s() - a) / n_compile;
        if (e < best_compile) best_compile = e;
    }
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    printf("calib cpu_ns_per_iter %.4f dvs_solve_us %.3f dvs_compile_us %.3f%s\n",
           best_cpu * 1e9 / (double)n_iter, best_solve * 1e6, best_compile * 1e6,
           bad ? " (solve failed)" : "");
}

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
        if (!strcmp(argv[i], "--build-info")) {
            printf("build-info %s\nlibrary %s\n", dvs_build_info(), dvs_build_library_path());
            return 0;
        }
        if (!strcmp(argv[i], "--calib")) {
            printf("build-info %s\nlibrary %s\n", dvs_build_info(), dvs_build_library_path());
            calib();
            return 0;
        }
        if (!strcmp(argv[i], "--solver") && i + 1 < argc) solver = argv[++i];
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--passes") && i + 1 < argc) passes = argv[++i];
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) g_warmup = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--calls") && i + 1 < argc) g_calls = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quality")) g_quality = 1;
        else if (!strcmp(argv[i], "--census")) g_census = 1;
        else if (!strcmp(argv[i], "--reuse") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "rediversify")) g_rediversify = 1;
            else if (strcmp(m, "resample")) { fprintf(stderr, "--reuse: resample or rediversify\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--dump-values") && i + 1 < argc) {
            if (!(g_dump = fopen(argv[++i], "w"))) { perror(argv[i]); return 2; }
        }
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            if (!strcmp(m, "verilator")) g_vlt_mode = 1;
            else if (strcmp(m, "default")) { fprintf(stderr, "--mode: default or verilator\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) {
            /* As dv-solve-smt2 --engine: sets DV_ENGINE, which the front end's
             * check-sat routing reads; the api passes replay the engine it
             * picked, and the pipe pass's solver inherits it. */
            const char *e = argv[++i];
            if (!strcmp(e, "auto")) unsetenv("DV_ENGINE");
            else if (!strcmp(e, "cdcl") || !strcmp(e, "bitblast")) setenv("DV_ENGINE", e, 1);
            else { fprintf(stderr, "--engine: auto, cdcl or bitblast\n"); return 2; }
        }
        else path = argv[i];
    }
    if (!path) {
        fprintf(stderr, "usage: %s <cmds.smt2> [--solver PATH] [--reps N] [--passes LIST]\n"
                        "         [--calls N] [--warmup N] [--mode default|verilator]\n"
                        "         [--engine auto|cdcl|bitblast]\n"
                        "         [--dump-values FILE] [--quality] [--census]\n"
                        "         [--reuse resample|rediversify]\n"
                        "       %s --build-info | --calib\n"
                        "  --mode     the front end's mode for the text pass, which also captures\n"
                        "             the problems the api passes replay; use the capture's mode\n"
                        "             (default: default, as dv-solve-smt2 without --mode)\n"
                        "  --engine   the engine every pass uses, as dv-solve-smt2 --engine\n"
                        "             (default: auto, the front end's routing)\n"
                        "  --warmup   randomize() calls after the start-up check not timed in the\n"
                        "             per-call figure\n"
                        "  --calls    randomize() calls in the session, warm-up included (default:\n"
                        "             one per solver session, each starting with (reset))\n"
                        "  --dump-values  write each api pass's models: \"V call seed name=#xVAL ...\"\n"
                        "             per answered call, for tests/uniformity\n"
                        "  --quality  check each api pass's models against the constraints, and\n"
                        "             report per field: distinct values, entropy, forced or stuck\n"
                        "             (an extra untimed rep reads the models back)\n"
                        "  --reuse    api-cache on a reused bitblast context: resample (a new\n"
                        "             seeded base model, the default) or rediversify (re-flip\n"
                        "             the cached model, as 0.3 did)\n"
                        "  --census   split each call's problem into independent components; report\n"
                        "             their classes, constructs and repeat rate (P0.5)\n"
                        "  --build-info  how the loaded dv-solve library was built, and its file\n"
                        "  --calib    that, then time fixed work (host and library speed)\n",
                argv[0], argv[0]);
        return 2;
    }
    printf("build-info %s\nlibrary %s\n", dvs_build_info(), dvs_build_library_path());
    load(path);
    size_t n_check = 0, n_bytes = 0;
    for (size_t i = 0; i < g_ncmd; i++) { n_check += g_cmd[i].kind == K_CHECK; n_bytes += g_cmd[i].len + 1; }
    printf("session %s\n  %zu commands, %zu check-sats, %.1f MB of SMT-LIB\n", path, g_ncmd,
           n_check, n_bytes / 1e6);
    split_calls();
    printf("  mode %s; %u solver sessions, %u randomize() calls, %u warm-up, %u timed%s\n",
           g_vlt_mode ? "verilator" : "default", g_ncalls, g_calls ? g_calls : g_ncalls,
           g_warmup, n_timed(),
           g_warm_in ? " (sessions per call vary: warm-up included in the average)" : "");
    double pcall = 1e6 / (double)(n_timed() ? n_timed() : 1);
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
    double t[K_N], tp, w, c, tw, bt[K_N], btp = 0, bw = 1e30, bc = 0, btw = 0;
    for (int k = 0; k < (has(passes, "text") ? reps : 1); k++) {
        pass_text(k == 0, t, &tp, &w, &c, &tw);
        if (w < bw) { bw = w; bc = c; btp = tp; btw = tw; memcpy(bt, t, sizeof t); }
    }
    if (has(passes, "text")) {
        printf("text       wall %7.3f s  %7.1f us/check-sat   (CPU %.3f s)\n", bw, bw * per, bc);
        printf("text       per call %9.1f us   (%u timed calls)\n", btw * pcall, n_timed());
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
    if (g_census) census();

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
        printf("%-10s per call %9.1f us   (%u timed calls)\n", cached ? "api-cache" : "api",
               best.timed_wall * pcall, n_timed());
        printf("             build (builder)  %7.3f s  %7.1f us\n", best.build, best.build * pc);
        printf("             compile/bitblast %7.3f s  %7.1f us\n", best.compile, best.compile * pc);
        printf("             solve+read       %7.3f s  %7.1f us\n", best.solve, best.solve * pc);
        if (cached && best.hits)
            printf("               on a reused context: %.1f us per call; on a new one: %.1f us\n",
                   1e6 * best.hit_solve / best.hits,
                   1e6 * (best.solve - best.hit_solve) / (double)(g_ncap - best.hits));
        if (g_dump || g_quality) {
            /* The passes are deterministic, so this rep returns the timed reps'
             * models, without timing their readback. */
            const char *name = cached ? "api-cache" : "api";
            if (g_dump) fprintf(g_dump, "pass %s\n", name);
            for (size_t i = 0; i < g_ncap; i++) g_cap[i].got = 0;
            g_collect = 1;
            pass_api(cached, &s);
            g_collect = 0;
            if (g_quality) quality(name);
        }
    }
    if (g_dump) fclose(g_dump);
    return 0;
}
