/*
 * smt2_oracle -- check dv-solve-smt2's answers against a reference solver.
 * See smt2_oracle.h and docs/oracle_check_plan.md. The reference solver
 * process, the run directory and the records are dvs_oracle_core's, shared
 * with the C API's oracle (docs/api_oracle_plan.md).
 *
 * POSIX only for now; on Windows smt2_oracle_from_env reports that the oracle
 * is unavailable and returns NULL.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "smt2/smt2_oracle.h"
#include "dvs_oracle_core.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int smt2_oracle_is_check(const Sexpr *cmd) {
    if (!cmd || cmd->kind != SEXPR_LIST || cmd->list.count == 0) return 0;
    const Sexpr *h = cmd->list.items[0];
    return sexpr_is_symbol(h, "check-sat") || sexpr_is_symbol(h, "check-sat-assuming");
}

#ifdef _WIN32

Smt2Oracle *smt2_oracle_from_env(int argc, char **argv, int verilator_mode,
                                 int hash_ignore, FILE *err) {
    (void)argc; (void)argv; (void)verilator_mode; (void)hash_ignore;
    if (getenv("DV_ORACLE") && *getenv("DV_ORACLE"))
        fprintf(err, "dv-solve oracle: not available on Windows; running without it\n");
    return NULL;
}
void smt2_oracle_after(Smt2Oracle *o, Smt2Frontend *fe, const Sexpr *cmd,
                       const char *text, size_t len,
                       const char *answer, size_t answer_len, double dvs_ms) {
    (void)o; (void)fe; (void)cmd; (void)text; (void)len;
    (void)answer; (void)answer_len; (void)dvs_ms;
}
void smt2_oracle_finish(Smt2Oracle *o) { (void)o; }

#else /* POSIX */

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

enum { DK_BV, DK_BOOL, DK_ARRAY, DK_OTHER };

typedef struct {
    char    *name;      /* the symbol as written, |quotes| included */
    uint8_t  kind;
    uint64_t epoch;     /* model epoch current when it was declared */
} ODecl;

#define ORACLE_SESSION_CAP   ((size_t)512 << 20)

struct Smt2Oracle {
    DvsOrc  *core;
    int      abort_on_fail;
    int      verilator_mode;

    /* The current session (since the last reset) */
    DvsOBuf  session;           /* the live assertion stack, flattened (no push/pop) */
    int      session_overflow;
    int      logic_set;
    uint64_t session_no;
    ODecl   *decls;
    uint32_t n_decls, decls_cap;
    uint32_t *scope;            /* n_decls at each push */
    size_t   *scope_pos;        /* session.n at each push */
    uint32_t n_scope, scope_cap;
    uint64_t epoch, model_epoch;
};

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

static double _env_d(const char *name, double dflt) {
    const char *e = getenv(name);
    return (e && *e) ? atof(e) : dflt;
}

Smt2Oracle *smt2_oracle_from_env(int argc, char **argv, int verilator_mode,
                                 int hash_ignore, FILE *err) {
    const char *spec = getenv("DV_ORACLE");
    if (!spec || !*spec) return NULL;

    DvsOrcConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.argc = argc; cfg.argv = argv;
    cfg.mode = verilator_mode ? "verilator" : "default";
    cfg.verilator_hash = verilator_mode ? (hash_ignore ? "ignore" : "honor") : "n/a";
    cfg.timeout_s = _env_d("DV_ORACLE_TIMEOUT", 10.0);
    cfg.rate = _env_d("DV_ORACLE_RATE", 1.0);
    cfg.tag = getenv("DV_ORACLE_TAG");
    cfg.out_pattern = getenv("DV_ORACLE_OUT");
    const char *kp = getenv("DV_ORACLE_KEEP");
    cfg.keep = !kp || !*kp ? DVS_ORC_KEEP_FAIL
             : strcmp(kp, "all") == 0 ? DVS_ORC_KEEP_ALL
             : strcmp(kp, "summary") == 0 ? DVS_ORC_KEEP_SUMMARY : DVS_ORC_KEEP_FAIL;
    const char *ck = getenv("DV_ORACLE_CHECK");
    if (ck && *ck) {
        cfg.chk_model = strstr(ck, "model") != NULL;
        cfg.chk_unsat = strstr(ck, "unsat") != NULL;
        cfg.chk_unknown = strstr(ck, "unknown") != NULL;
    } else {
        cfg.chk_model = cfg.chk_unsat = cfg.chk_unknown = 1;
    }

    /* The oracle's command line. The soft per-query limit is passed to the
     * solver so that a slow query answers `unknown` and the process lives on;
     * the hard limit in the check is a backstop. */
    char ms[32];
    snprintf(ms, sizeof(ms), "%ld", (long)(cfg.timeout_s * 1000.0));
    char cmd[4096];
    if (strncmp(spec, "cmd:", 4) == 0) {
        snprintf(cmd, sizeof(cmd), "%s", spec + 4);
        cfg.name = "cmd";
    } else if (strcmp(spec, "z3") == 0) {
        const char *bin = getenv("DV_ORACLE_BIN");
        snprintf(cmd, sizeof(cmd), "%s -in -smt2 -t:%s", bin && *bin ? bin : "z3", ms);
        cfg.name = "z3";
    } else if (strcmp(spec, "bitwuzla") == 0) {
        const char *bin = getenv("DV_ORACLE_BIN");
        snprintf(cmd, sizeof(cmd), "%s --lang smt2 -m -T %s", bin && *bin ? bin : "bitwuzla", ms);
        cfg.name = "bitwuzla";
    } else {
        fprintf(err, "dv-solve oracle: DV_ORACLE=%s not understood "
                     "(z3 | bitwuzla | cmd:<command>); running without it\n", spec);
        return NULL;
    }
    cfg.cmdline = cmd;

    Smt2Oracle *o = (Smt2Oracle *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->core = dvs_orc_create(&cfg, err);
    if (!o->core) { free(o); return NULL; }
    const char *of = getenv("DV_ORACLE_ON_FAIL");
    o->abort_on_fail = of && strcmp(of, "abort") == 0;
    o->verilator_mode = verilator_mode;
    return o;
}

/* ------------------------------------------------------------------ */
/* Session state                                                       */
/* ------------------------------------------------------------------ */

/* Add a command to the flattened assertion stack. Nothing is sent: each check
 * replays the stack into a freshly reset oracle (see _check for why). */
static void _forward(Smt2Oracle *o, const char *text, size_t len) {
    if (o->session_overflow) return;
    if (o->session.n + len + 1 > ORACLE_SESSION_CAP) { o->session_overflow = 1; return; }
    dvs_ob_add(&o->session, text, len);
    dvs_ob_add(&o->session, "\n", 1);
}

static void _add_decl(Smt2Oracle *o, const Sexpr *name, const Sexpr *sort) {
    if (name->kind != SEXPR_SYMBOL) return;
    if (o->n_decls == o->decls_cap) {
        uint32_t c = o->decls_cap ? o->decls_cap * 2 : 64;
        ODecl *g = (ODecl *)realloc(o->decls, c * sizeof(ODecl));
        if (!g) return;
        o->decls = g; o->decls_cap = c;
    }
    ODecl *d = &o->decls[o->n_decls++];
    /* The parser keeps |quoted| symbols' contents; quote anything that is not a
     * plain simple symbol so the pin is valid SMT-LIB2 again. */
    int simple = name->sym.len > 0 && !isdigit((unsigned char)name->sym.str[0]);
    for (uint32_t i = 0; simple && i < name->sym.len; i++) {
        unsigned char c = (unsigned char)name->sym.str[i];
        if (!(isalnum(c) || strchr("~!@$%^&*_-+=<>.?/", c))) simple = 0;
    }
    d->name = (char *)malloc(name->sym.len + 3);
    if (simple) sprintf(d->name, "%.*s", (int)name->sym.len, name->sym.str);
    else sprintf(d->name, "|%.*s|", (int)name->sym.len, name->sym.str);
    d->epoch = o->epoch;
    if (sexpr_is_symbol(sort, "Bool")) d->kind = DK_BOOL;
    else if (sort->kind == SEXPR_LIST && sort->list.count == 3 &&
             sexpr_is_symbol(sort->list.items[0], "_") &&
             sexpr_is_symbol(sort->list.items[1], "BitVec")) d->kind = DK_BV;
    else if (sort->kind == SEXPR_LIST && sort->list.count == 3 &&
             sexpr_is_symbol(sort->list.items[0], "Array")) d->kind = DK_ARRAY;
    else d->kind = DK_OTHER;
}

static void _truncate_decls(Smt2Oracle *o, uint32_t n) {
    for (uint32_t i = n; i < o->n_decls; i++) free(o->decls[i].name);
    if (n < o->n_decls) o->n_decls = n;
}

static void _reset_session(Smt2Oracle *o) {
    dvs_ob_clear(&o->session);
    o->session_overflow = 0;
    o->logic_set = 0;
    _truncate_decls(o, 0);
    o->n_scope = 0;
    o->session_no++;
}

/* set-option keywords the oracle must not see: they change its output
 * protocol, or are managed by the oracle's command line. */
static int _option_filtered(const Sexpr *cmd) {
    if (cmd->list.count < 2 || cmd->list.items[1]->kind != SEXPR_KEYWORD) return 0;
    static const char *const drop[] = {
        ":print-success", ":regular-output-channel", ":diagnostic-output-channel",
        ":verbosity", ":produce-models", ":timeout",
    };
    const Sexpr *k = cmd->list.items[1];
    for (size_t i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) {
        size_t n = strlen(drop[i]);
        if (k->sym.len == n && memcmp(k->sym.str, drop[i], n) == 0) return 1;
        /* The lexer may keep the keyword without its colon. */
        if (k->sym.len == n - 1 && memcmp(k->sym.str, drop[i] + 1, n - 1) == 0) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* The check                                                           */
/* ------------------------------------------------------------------ */

static int _verdict_of(const char *a, size_t n) {
    size_t i = 0;
    while (i < n && isspace((unsigned char)a[i])) i++;
    size_t j = i;
    while (j < n && !isspace((unsigned char)a[j])) j++;
    if (j - i == 3 && memcmp(a + i, "sat", 3) == 0) return DVS_ORC_V_SAT;
    if (j - i == 5 && memcmp(a + i, "unsat", 5) == 0) return DVS_ORC_V_UNSAT;
    if (j - i == 7 && memcmp(a + i, "unknown", 7) == 0) return DVS_ORC_V_UNKNOWN;
    return DVS_ORC_V_NONE;
}

/* Text scanning over a get-value reply. _skip_ws/_span return offsets. */
static size_t _skip_ws(const char *b, size_t i, size_t n) {
    while (i < n && isspace((unsigned char)b[i])) i++;
    return i;
}
/* End (exclusive) of the s-expression or atom starting at i. */
static size_t _span(const char *b, size_t i, size_t n) {
    if (i >= n) return n;
    if (b[i] == '(') {
        int depth = 0, inbar = 0, instr = 0;
        for (; i < n; i++) {
            char c = b[i];
            if (instr) { if (c == '"') instr = 0; continue; }
            if (inbar) { if (c == '|') inbar = 0; continue; }
            if (c == '"') instr = 1;
            else if (c == '|') inbar = 1;
            else if (c == '(') depth++;
            else if (c == ')' && --depth == 0) return i + 1;
        }
        return n;
    }
    if (b[i] == '|') {
        for (i++; i < n && b[i] != '|'; i++) {}
        return i < n ? i + 1 : n;
    }
    while (i < n && !isspace((unsigned char)b[i]) && b[i] != '(' && b[i] != ')') i++;
    return i;
}

/* Build the pin block for a `sat`: one (assert (= name value)) per pinnable
 * declaration. The values are sliced verbatim from the frontend's own
 * get-value reply, so what is checked is exactly what a client would read. */
static void _build_pins(Smt2Oracle *o, Smt2Frontend *fe, DvsOBuf *pins,
                        DvsOBuf *partial, uint32_t *n_pinned) {
    DvsOBuf req = {0};
    uint32_t *idx = (uint32_t *)malloc((o->n_decls + 1) * sizeof(uint32_t));
    uint32_t n = 0;
    *n_pinned = 0;
    dvs_ob_str(&req, "(get-value (");
    for (uint32_t i = 0; idx && i < o->n_decls; i++) {
        ODecl *d = &o->decls[i];
        if (d->epoch > o->model_epoch) continue;   /* declared after the model */
        if (d->kind == DK_OTHER) {
            if (partial->n) dvs_ob_str(partial, " ");
            dvs_ob_str(partial, d->name);
            continue;
        }
        dvs_ob_str(&req, " ");
        dvs_ob_str(&req, d->name);
        idx[n++] = i;
    }
    dvs_ob_str(&req, "))");
    if (n == 0 || !idx) { free(idx); dvs_ob_free(&req); return; }

    size_t rn = 0;
    char *r = smt2_frontend_get_value_text(fe, req.p, req.n, &rn);
    dvs_ob_free(&req);
    if (!r) { free(idx); return; }

    size_t i = _skip_ws(r, 0, rn);
    uint32_t k = 0;
    if (i < rn && r[i] == '(') {
        i++;
        for (; k < n; k++) {
            i = _skip_ws(r, i, rn);
            if (i >= rn || r[i] != '(') break;
            size_t pend = _span(r, i, rn);           /* the (key value) pair */
            size_t ks = _skip_ws(r, i + 1, pend);
            size_t ke = _span(r, ks, pend);          /* the key */
            size_t vs = _skip_ws(r, ke, pend);
            size_t ve = pend - 1;                    /* before the pair's ')' */
            while (ve > vs && isspace((unsigned char)r[ve - 1])) ve--;
            if (ve <= vs) break;
            ODecl *d = &o->decls[idx[k]];
            dvs_ob_str(pins, "(assert (= ");
            dvs_ob_str(pins, d->name);
            dvs_ob_str(pins, " ");
            /* A Bool is a 1-bit vector inside dv-solve, and get-value prints
             * it that way; pin it as the Bool the declaration says it is. */
            if (d->kind == DK_BOOL && ve - vs == 3 && r[vs] == '#' && r[vs + 1] == 'b')
                dvs_ob_str(pins, r[vs + 2] == '1' ? "true" : "false");
            else
                dvs_ob_add(pins, r + vs, ve - vs);
            dvs_ob_str(pins, "))\n");
            (*n_pinned)++;
            i = pend;
        }
    }
    if (k < n) {
        if (partial->n) dvs_ob_str(partial, " ");
        dvs_ob_str(partial, "<get-value reply not understood>");
    }
    free(r);
    free(idx);
}

static void _check(Smt2Oracle *o, Smt2Frontend *fe, const Sexpr *cmd,
                   const char *text, size_t len, int dv, double dvs_ms) {
    int is_csa = sexpr_is_symbol(cmd->list.items[0], "check-sat-assuming");
    int reused = fe->model_reused;
    const char *sem = !o->verilator_mode ? "literal"
                    : is_csa ? "vlt-diversity"
                    : reused ? "vlt-parity-skip" : "literal";
    const char *engine = reused ? "reused" : smt2_frontend_engine(fe);
    if (!reused) o->model_epoch = o->epoch++;

    /* The oracle's form of the query: literal, except that verilator mode
     * answers check-sat-assuming without the assumptions (§2.2). */
    DvsOBuf query = {0}, pins = {0}, partial = {0};
    if (is_csa && !o->verilator_mode) dvs_ob_add(&query, text, len);
    else dvs_ob_str(&query, "(check-sat)");
    dvs_ob_str(&query, "\n");

    /* Each check replays the flattened assertion stack into a freshly reset
     * oracle rather than mirroring push/pop live: once z3 has seen a push it
     * switches to its incremental core, which cannot decide even a fully
     * pinned QF_ABV query from riscv-dv (unknown after 20 s, against 20 ms for
     * the same query without the scopes). The script is also the bundle's
     * repro, exactly as the oracle saw it. */
    DvsOrcQuery q;
    memset(&q, 0, sizeof(q));
    q.want = dvs_orc_wants(o->core, dv);
    if (q.want && o->session_overflow) q.error = "assertion stack over the recording cap";
    if (q.want && !q.error && dv == DVS_ORC_V_SAT)
        _build_pins(o, fe, &pins, &partial, &q.n_pinned);
    q.cmd = is_csa ? "check-sat-assuming" : "check-sat";
    q.sem = sem;
    q.engine = engine;
    q.dv = dv;
    q.dvs_ms = dvs_ms;
    q.validate = fe->last_validate_viol;
    q.s = o->session_no;
    q.d = o->n_scope;
    q.script = o->session.p;
    q.script_n = o->session.n;
    q.pins = pins.p;
    q.pins_n = pins.n;
    q.partial = partial.n ? partial.p : NULL;
    q.query = query.p;
    q.raw = text;
    q.raw_n = len;
    int res = dvs_orc_check(o->core, &q);
    dvs_ob_free(&query); dvs_ob_free(&pins); dvs_ob_free(&partial);

    if (o->abort_on_fail && (res == DVS_ORC_BAD_MODEL || res == DVS_ORC_BAD_UNSAT)) {
        dvs_orc_finish(o->core, 1);
        o->core = NULL;
        fprintf(stderr, "dv-solve oracle: aborting on a P0 result (DV_ORACLE_ON_FAIL=abort)\n");
        exit(3);
    }
}

/* ------------------------------------------------------------------ */
/* Per-command hook                                                    */
/* ------------------------------------------------------------------ */

void smt2_oracle_after(Smt2Oracle *o, Smt2Frontend *fe, const Sexpr *cmd,
                       const char *text, size_t len,
                       const char *answer, size_t answer_len, double dvs_ms) {
    if (!o || !cmd || cmd->kind != SEXPR_LIST || cmd->list.count == 0) return;
    /* The read loop hands over leading whitespace and comments too. */
    for (;;) {
        while (len > 0 && isspace((unsigned char)*text)) { text++; len--; }
        if (len == 0 || *text != ';') break;
        while (len > 0 && *text != '\n') { text++; len--; }
    }
    if (len == 0) return;
    const Sexpr *h = cmd->list.items[0];
    if (h->kind != SEXPR_SYMBOL) return;

    if (sexpr_is_symbol(h, "reset")) {
        _reset_session(o);
        return;
    }
    if (smt2_oracle_is_check(cmd)) {
        int dv = answer ? _verdict_of(answer, answer_len) : DVS_ORC_V_NONE;
        if (dv == DVS_ORC_V_NONE) return;    /* an error, not an answer */
        _check(o, fe, cmd, text, len, dv, dvs_ms);
        return;
    }
    if (sexpr_is_symbol(h, "set-logic")) {
        if (o->logic_set) return;     /* a second set-logic is an error to z3 */
        o->logic_set = 1;
        _forward(o, text, len);
        return;
    }
    if (sexpr_is_symbol(h, "set-option")) {
        if (!_option_filtered(cmd)) _forward(o, text, len);
        return;
    }
    if (sexpr_is_symbol(h, "assert")) {
        if (!fe->cmd_dropped) _forward(o, text, len);
        return;
    }
    if (sexpr_is_symbol(h, "declare-const") && cmd->list.count == 3) {
        _add_decl(o, cmd->list.items[1], cmd->list.items[2]);
        _forward(o, text, len);
        return;
    }
    if (sexpr_is_symbol(h, "declare-fun") && cmd->list.count == 4) {
        const Sexpr *ps = cmd->list.items[2];
        if (ps->kind == SEXPR_LIST && ps->list.count == 0)
            _add_decl(o, cmd->list.items[1], cmd->list.items[3]);
        else if (cmd->list.items[1]->kind == SEXPR_SYMBOL) {
            /* An uninterpreted function: left free, recorded as partial. */
            static const Sexpr other = { .kind = SEXPR_SYMBOL };
            _add_decl(o, cmd->list.items[1], &other);
        }
        _forward(o, text, len);
        return;
    }
    if (sexpr_is_symbol(h, "push")) {
        uint32_t n = 1;
        if (cmd->list.count == 2 && cmd->list.items[1]->kind == SEXPR_NUMERAL)
            n = (uint32_t)cmd->list.items[1]->numval;
        for (uint32_t i = 0; i < n; i++) {
            if (o->n_scope == o->scope_cap) {
                uint32_t c = o->scope_cap ? o->scope_cap * 2 : 16;
                uint32_t *g = (uint32_t *)realloc(o->scope, c * sizeof(uint32_t));
                size_t *gp = (size_t *)realloc(o->scope_pos, c * sizeof(size_t));
                if (g) o->scope = g;
                if (gp) o->scope_pos = gp;
                if (!g || !gp) break;
                o->scope_cap = c;
            }
            o->scope_pos[o->n_scope] = o->session.n;
            o->scope[o->n_scope++] = o->n_decls;
        }
        return;
    }
    if (sexpr_is_symbol(h, "pop")) {
        uint32_t n = 1;
        if (cmd->list.count == 2 && cmd->list.items[1]->kind == SEXPR_NUMERAL)
            n = (uint32_t)cmd->list.items[1]->numval;
        if (n > o->n_scope) n = o->n_scope;
        if (n) {
            o->n_scope -= n;
            _truncate_decls(o, o->scope[o->n_scope]);
            if (!o->session_overflow) {
                o->session.n = o->scope_pos[o->n_scope];
                if (o->session.p) o->session.p[o->session.n] = '\0';
            }
        }
        return;
    }
    if (sexpr_is_symbol(h, "reset-assertions")) {
        /* Everything goes, the logic and options too (z3 needs neither). */
        _truncate_decls(o, 0);
        o->n_scope = 0;
        dvs_ob_clear(&o->session);
        o->session_overflow = 0;
        o->logic_set = 0;
        return;
    }
    if (sexpr_is_symbol(h, "define-fun") || sexpr_is_symbol(h, "define-fun-rec") ||
        sexpr_is_symbol(h, "define-funs-rec") || sexpr_is_symbol(h, "define-sort") ||
        sexpr_is_symbol(h, "declare-sort") || sexpr_is_symbol(h, "declare-datatypes") ||
        sexpr_is_symbol(h, "declare-datatype") || sexpr_is_symbol(h, "set-info")) {
        _forward(o, text, len);
        return;
    }
    /* Queries (get-value, get-model, get-info, echo, get-unsat-*), exit and
     * anything unknown are not forwarded. */
}

void smt2_oracle_finish(Smt2Oracle *o) {
    if (!o) return;
    dvs_orc_finish(o->core, 0);
    _truncate_decls(o, 0);
    free(o->decls); free(o->scope); free(o->scope_pos);
    dvs_ob_free(&o->session);
    free(o);
}

#endif /* POSIX */
