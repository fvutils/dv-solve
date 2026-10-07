/*
 * smt2_oracle -- check dv-solve-smt2's answers against a reference solver.
 * See smt2_oracle.h and docs/oracle_check_plan.md.
 *
 * POSIX only for now; on Windows smt2_oracle_from_env reports that the oracle
 * is unavailable and returns NULL.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "smt2/smt2_oracle.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef DVS_VERSION
#define DVS_VERSION "unknown"
#endif
#ifndef DVS_GIT_COMMIT
#define DVS_GIT_COMMIT "unknown"
#endif

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

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
/* macOS has no MSG_NOSIGNAL; the socket gets SO_NOSIGPIPE instead (_spawn). */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

typedef struct { char *p; size_t n, cap; } OBuf;

static int _ob_add(OBuf *b, const char *s, size_t n) {
    if (n > SIZE_MAX / 4 || b->n > SIZE_MAX / 4) return -1;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n + 1) c *= 2;
        char *g = (char *)realloc(b->p, c);
        if (!g) return -1;
        b->p = g; b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
    return 0;
}
static int _ob_str(OBuf *b, const char *s) { return _ob_add(b, s, strlen(s)); }
static void _ob_clear(OBuf *b) { b->n = 0; if (b->p) b->p[0] = '\0'; }
static void _ob_free(OBuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static double _now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void _utc(char *out, size_t n, int compact) {
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, compact ? "%Y%m%dT%H%M%SZ" : "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* Write `s` as a JSON string literal. */
static void _json_str(FILE *f, const char *s, size_t n) {
    fputc('"', f);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}
static void _json_cstr(FILE *f, const char *s) {
    if (!s) { fputs("null", f); return; }
    _json_str(f, s, strlen(s));
}

static void _cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0) fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

static int _mkdir_p(const char *path) {
    char tmp[4096];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0777) < 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0777) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

enum { DK_BV, DK_BOOL, DK_ARRAY, DK_OTHER };

typedef struct {
    char    *name;      /* the symbol as written, |quotes| included */
    uint8_t  kind;
    uint64_t epoch;     /* model epoch current when it was declared */
} ODecl;

enum { V_NONE, V_SAT, V_UNSAT, V_UNKNOWN, V_TIMEOUT, V_DEAD, V_ERROR };
static const char *const _vname[] = {
    "none", "sat", "unsat", "unknown", "timeout", "dead", "error" };

enum { RES_OK, RES_BAD_MODEL, RES_BAD_UNSAT, RES_GAP, RES_UNCHECKED, RES_ORACLE_ERROR,
       RES_SKIPPED, RES__N };
static const char *const _rname[RES__N] = {
    "ok", "bad-model", "bad-unsat", "gap", "unchecked", "oracle-error", "skipped" };

enum { KEEP_SUMMARY, KEEP_FAIL, KEEP_ALL };

#define ORACLE_MAX_BUNDLES   2000
#define ORACLE_SESSION_CAP   ((size_t)512 << 20)

struct Smt2Oracle {
    /* Configuration */
    char   **cmdv;              /* the oracle's argv (NULL-terminated) */
    char    *cmdline;
    char     name[64];
    double   timeout_s;
    int      chk_model, chk_unsat, chk_unknown;
    double   rate;
    int      abort_on_fail;
    int      keep;
    char    *run_dir;
    const char *tag;
    int      verilator_mode, hash_ignore;
    int      argc;
    char   **argv;
    FILE    *err;

    /* The reference solver */
    pid_t    pid;
    int      in_fd, out_fd;
    OBuf     inbuf;             /* oracle output not consumed yet */
    int      dead;              /* gone until the next (reset) */
    char     version[128];

    /* The current session (since the last reset) */
    OBuf     session;           /* the live assertion stack, flattened (no push/pop) */
    int      session_overflow;
    int      logic_set;
    char    *taint;             /* first oracle error in the current check */
    uint64_t session_no;
    ODecl   *decls;
    uint32_t n_decls, decls_cap;
    uint32_t *scope;            /* n_decls at each push */
    size_t   *scope_pos;        /* session.n at each push */
    uint32_t n_scope, scope_cap;
    uint64_t epoch, model_epoch;

    /* Recording */
    FILE    *jsonl;
    FILE    *transcript;
    uint64_t q;
    uint64_t counts[RES__N];
    uint64_t n_partial;
    double   dvs_ms, orc_ms;
    int      n_bundles;
    char     start_s[32];
    int      aborted;
    uint64_t rng;
};

/* ------------------------------------------------------------------ */
/* The reference solver process                                        */
/* ------------------------------------------------------------------ */

static int _spawn(Smt2Oracle *o) {
    int sv[2], outp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -1;
    if (pipe(outp) < 0) { close(sv[0]); close(sv[1]); return -1; }
    char errpath[4200];
    snprintf(errpath, sizeof(errpath), "%s/oracle.stderr", o->run_dir);
    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]); close(sv[1]); close(outp[0]); close(outp[1]);
        return -1;
    }
    if (pid == 0) {
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
        dup2(sv[1], 0);
        dup2(outp[1], 1);
        int ef = open(errpath, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (ef >= 0) dup2(ef, 2);
        close(sv[0]); close(sv[1]); close(outp[0]); close(outp[1]);
        if (ef > 2) close(ef);
        execvp(o->cmdv[0], o->cmdv);
        _exit(127);
    }
    close(sv[1]);
    close(outp[1]);
    o->pid = pid;
    o->in_fd = sv[0];
    o->out_fd = outp[0];
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(o->in_fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    _cloexec(o->in_fd);
    _cloexec(o->out_fd);
    fcntl(o->in_fd, F_SETFL, fcntl(o->in_fd, F_GETFL) | O_NONBLOCK);
    fcntl(o->out_fd, F_SETFL, fcntl(o->out_fd, F_GETFL) | O_NONBLOCK);
    _ob_clear(&o->inbuf);
    o->dead = 0;
    return 0;
}

static void _kill(Smt2Oracle *o) {
    if (o->pid > 0) {
        kill(o->pid, SIGKILL);
        waitpid(o->pid, NULL, 0);
    }
    if (o->in_fd >= 0) close(o->in_fd);
    if (o->out_fd >= 0) close(o->out_fd);
    o->pid = -1;
    o->in_fd = o->out_fd = -1;
    o->dead = 1;
}

/* Read whatever the oracle has written; 0 on EOF. */
static int _drain(Smt2Oracle *o) {
    char tmp[65536];
    for (;;) {
        ssize_t r = read(o->out_fd, tmp, sizeof(tmp));
        if (r > 0) { _ob_add(&o->inbuf, tmp, (size_t)r); continue; }
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        return 1;   /* EAGAIN */
    }
}

/* Send raw text to the oracle (also to the transcript). Reads the oracle's
 * output while waiting so that neither side can block on a full pipe. */
static int _send_raw(Smt2Oracle *o, const char *s, size_t n) {
    if (o->transcript) fwrite(s, 1, n, o->transcript);
    if (o->dead) return -1;
    double deadline = _now() + 60.0;
    while (n > 0) {
        ssize_t w = send(o->in_fd, s, n, MSG_NOSIGNAL);
        if (w > 0) { s += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { _kill(o); return -1; }
        struct pollfd p[2] = { { o->in_fd, POLLOUT, 0 }, { o->out_fd, POLLIN, 0 } };
        int ms = (int)((deadline - _now()) * 1000.0);
        if (ms <= 0) { _kill(o); return -1; }
        if (poll(p, 2, ms) < 0 && errno != EINTR) { _kill(o); return -1; }
        if (p[1].revents & (POLLIN | POLLHUP)) {
            if (_drain(o) == 0 && !(p[0].revents & POLLOUT)) { _kill(o); return -1; }
        }
    }
    return 0;
}
static int _send(Smt2Oracle *o, const char *s) { return _send_raw(o, s, strlen(s)); }

/* Take one complete response off inbuf: an s-expression or a line-terminated
 * atom. Comment lines are dropped. Returns a malloc'd string or NULL if no
 * complete response is buffered yet. */
static char *_take_resp(Smt2Oracle *o) {
    for (;;) {
        size_t i = 0, n = o->inbuf.n;
        const char *b = o->inbuf.p;
        while (i < n && isspace((unsigned char)b[i])) i++;
        if (i == n) { _ob_clear(&o->inbuf); return NULL; }
        size_t start = i, end;
        if (b[i] == ';') {
            while (i < n && b[i] != '\n') i++;
            if (i == n) return NULL;
            memmove(o->inbuf.p, b + i + 1, n - i - 1);
            o->inbuf.n = n - i - 1;
            continue;
        }
        if (b[i] == '(') {
            int depth = 0, instr = 0, inbar = 0;
            for (; i < n; i++) {
                char c = b[i];
                if (instr) { if (c == '"') instr = 0; continue; }
                if (inbar) { if (c == '|') inbar = 0; continue; }
                if (c == '"') instr = 1;
                else if (c == '|') inbar = 1;
                else if (c == '(') depth++;
                else if (c == ')') { if (--depth == 0) break; }
            }
            if (i == n) return NULL;
            end = i + 1;
        } else {
            while (i < n && b[i] != '\n') i++;
            if (i == n) return NULL;
            end = i;
            while (end > start && isspace((unsigned char)b[end - 1])) end--;
            i++;    /* past the newline */
            if (i > n) i = n;
        }
        char *r = (char *)malloc(end - start + 1);
        if (r) { memcpy(r, b + start, end - start); r[end - start] = '\0'; }
        size_t used = (b[start] == '(') ? end : i;
        memmove(o->inbuf.p, b + used, n - used);
        o->inbuf.n = n - used;
        o->inbuf.p[o->inbuf.n] = '\0';
        return r;
    }
}

/* Wait for one response until `deadline`. Returns it (malloc'd), or NULL with
 * *why = V_TIMEOUT / V_DEAD. */
static char *_read_resp(Smt2Oracle *o, double deadline, int *why) {
    for (;;) {
        char *r = _take_resp(o);
        if (r) return r;
        if (o->dead) { *why = V_DEAD; return NULL; }
        int ms = (int)((deadline - _now()) * 1000.0);
        if (ms <= 0) { *why = V_TIMEOUT; return NULL; }
        struct pollfd p = { o->out_fd, POLLIN, 0 };
        int pr = poll(&p, 1, ms);
        if (pr < 0 && errno != EINTR) { _kill(o); *why = V_DEAD; return NULL; }
        if (pr > 0 && _drain(o) == 0) {
            /* EOF: keep what was buffered, then report the death. */
            char *r2 = _take_resp(o);
            if (!r2 && o->inbuf.n) {           /* an unterminated last line */
                r2 = strdup(o->inbuf.p);
                _ob_clear(&o->inbuf);
            }
            _kill(o);
            if (r2) return r2;
            *why = V_DEAD;
            return NULL;
        }
    }
}

static int _is_error_resp(const char *r) {
    return strncmp(r, "(error", 6) == 0 || strcmp(r, "unsupported") == 0
        || strncmp(r, "[error]", 7) == 0;
}

/* Read until a verdict; error responses on the way (from earlier forwarded
 * commands) are appended to `errs`. */
static int _read_verdict(Smt2Oracle *o, double deadline, OBuf *errs) {
    for (;;) {
        int why = V_NONE;
        char *r = _read_resp(o, deadline, &why);
        if (!r) return why;
        int v = V_NONE;
        if (strcmp(r, "sat") == 0) v = V_SAT;
        else if (strcmp(r, "unsat") == 0) v = V_UNSAT;
        else if (strcmp(r, "unknown") == 0) v = V_UNKNOWN;
        else {
            if (errs) { if (errs->n) _ob_str(errs, "\n"); _ob_str(errs, r); }
            if (!o->taint && _is_error_resp(r)) o->taint = strdup(r);
        }
        free(r);
        if (v != V_NONE) return v;
    }
}

/* Replace the oracle process (after a hard timeout, or its death). Each check
 * starts from (reset), so there is no state to restore. */
static void _respawn(Smt2Oracle *o) {
    _kill(o);
    if (_spawn(o) < 0) o->dead = 1;
}

/* ------------------------------------------------------------------ */
/* Run directory and records                                           */
/* ------------------------------------------------------------------ */

static char *_expand_pattern(const char *pat) {
    OBuf b = {0};
    char ts[32], pid[32];
    _utc(ts, sizeof(ts), 1);
    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    for (const char *p = pat; *p; p++) {
        if (*p == '%' && p[1]) {
            p++;
            if (*p == 'p') _ob_str(&b, pid);
            else if (*p == 't') _ob_str(&b, ts);
            else if (*p == '%') _ob_add(&b, "%", 1);
            else { _ob_add(&b, "%", 1); _ob_add(&b, p, 1); }
        } else {
            _ob_add(&b, p, 1);
        }
    }
    return b.p ? b.p : strdup("");
}

static int _claim(const char *dir) {
    if (_mkdir_p(dir) < 0) return -1;
    char path[4200];
    snprintf(path, sizeof(path), "%s/run.json", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) return errno == EEXIST ? 1 : -1;
    close(fd);
    return 0;
}

static void _write_run_json(Smt2Oracle *o, const char *status) {
    char path[4200], tmp[4210];
    snprintf(path, sizeof(path), "%s/run.json", o->run_dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    char now[32];
    _utc(now, sizeof(now), 0);
    fprintf(f, "{\n  \"schema\": 1,\n  \"kind\": \"dv-solve-oracle-run\",\n");
    fprintf(f, "  \"status\": \"%s\",\n", status);
    fprintf(f, "  \"dv_solve\": {\"version\": \"%s\", \"commit\": \"%s\"},\n",
            DVS_VERSION, DVS_GIT_COMMIT);
    fprintf(f, "  \"argv\": [");
    for (int i = 0; i < o->argc; i++) {
        if (i) fputs(", ", f);
        _json_cstr(f, o->argv[i]);
    }
    fprintf(f, "],\n");
    fprintf(f, "  \"mode\": \"%s\",\n  \"verilator_hash\": \"%s\",\n",
            o->verilator_mode ? "verilator" : "default",
            o->verilator_mode ? (o->hash_ignore ? "ignore" : "honor") : "n/a");
    fprintf(f, "  \"env\": {");
    extern char **environ;
    int first = 1;
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, "DV_", 3) != 0) continue;
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        fprintf(f, "%s", first ? "" : ", ");
        first = 0;
        _json_str(f, *e, (size_t)(eq - *e));
        fputs(": ", f);
        _json_cstr(f, eq + 1);
    }
    fprintf(f, "},\n");
    fprintf(f, "  \"oracle\": {\"name\": ");
    _json_cstr(f, o->name);
    fprintf(f, ", \"command\": ");
    _json_cstr(f, o->cmdline);
    fprintf(f, ", \"version\": ");
    _json_cstr(f, o->version[0] ? o->version : NULL);
    fprintf(f, ", \"timeout_s\": %g},\n", o->timeout_s);
    fprintf(f, "  \"checks\": {\"model\": %s, \"unsat\": %s, \"unknown\": %s, \"rate\": %g},\n",
            o->chk_model ? "true" : "false", o->chk_unsat ? "true" : "false",
            o->chk_unknown ? "true" : "false", o->rate);
    fprintf(f, "  \"keep\": \"%s\",\n",
            o->keep == KEEP_ALL ? "all" : o->keep == KEEP_FAIL ? "fail" : "summary");
    fprintf(f, "  \"tag\": ");
    _json_cstr(f, o->tag);
    fprintf(f, ",\n  \"pid\": %ld,\n", (long)getpid());
    fprintf(f, "  \"start\": \"%s\",\n", o->start_s);
    fprintf(f, "  \"end\": ");
    if (strcmp(status, "running") == 0) fputs("null", f); else _json_cstr(f, now);
    fprintf(f, ",\n  \"summary\": {\"queries\": %llu",
            (unsigned long long)o->q);
    for (int r = 0; r < RES__N; r++)
        fprintf(f, ", \"%s\": %llu", _rname[r], (unsigned long long)o->counts[r]);
    fprintf(f, ", \"partial\": %llu, \"sessions\": %llu, \"dvs_ms\": %.3f, \"oracle_ms\": %.3f}\n}\n",
            (unsigned long long)o->n_partial, (unsigned long long)o->session_no + 1,
            o->dvs_ms, o->orc_ms);
    fclose(f);
    rename(tmp, path);
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

static char **_split_cmd(const char *s) {
    size_t cap = 8, n = 0;
    char **v = (char **)calloc(cap, sizeof(char *));
    const char *p = s;
    while (v && *p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *q = p;
        while (*q && *q != ' ' && *q != '\t') q++;
        if (n + 2 > cap) { cap *= 2; v = (char **)realloc(v, cap * sizeof(char *)); if (!v) return NULL; }
        v[n++] = strndup(p, (size_t)(q - p));
        p = q;
    }
    if (v) v[n] = NULL;
    return v;
}

static double _env_d(const char *name, double dflt) {
    const char *e = getenv(name);
    return (e && *e) ? atof(e) : dflt;
}

Smt2Oracle *smt2_oracle_from_env(int argc, char **argv, int verilator_mode,
                                 int hash_ignore, FILE *err) {
    const char *spec = getenv("DV_ORACLE");
    if (!spec || !*spec) return NULL;

    Smt2Oracle *o = (Smt2Oracle *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->pid = -1; o->in_fd = o->out_fd = -1;
    o->err = err;
    o->argc = argc; o->argv = argv;
    o->verilator_mode = verilator_mode;
    o->hash_ignore = hash_ignore;
    o->timeout_s = _env_d("DV_ORACLE_TIMEOUT", 10.0);
    o->rate = _env_d("DV_ORACLE_RATE", 1.0);
    o->tag = getenv("DV_ORACLE_TAG");
    o->rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)getpid();
    const char *of = getenv("DV_ORACLE_ON_FAIL");
    o->abort_on_fail = of && strcmp(of, "abort") == 0;
    const char *kp = getenv("DV_ORACLE_KEEP");
    o->keep = !kp || !*kp ? KEEP_FAIL
            : strcmp(kp, "all") == 0 ? KEEP_ALL
            : strcmp(kp, "summary") == 0 ? KEEP_SUMMARY : KEEP_FAIL;
    const char *ck = getenv("DV_ORACLE_CHECK");
    if (ck && *ck) {
        o->chk_model = strstr(ck, "model") != NULL;
        o->chk_unsat = strstr(ck, "unsat") != NULL;
        o->chk_unknown = strstr(ck, "unknown") != NULL;
    } else {
        o->chk_model = o->chk_unsat = o->chk_unknown = 1;
    }
    _utc(o->start_s, sizeof(o->start_s), 0);

    /* The oracle's command line. The soft per-query limit is passed to the
     * solver so that a slow query answers `unknown` and the process lives on;
     * the hard limit in _check is a backstop. */
    char ms[32];
    snprintf(ms, sizeof(ms), "%ld", (long)(o->timeout_s * 1000.0));
    char cmd[4096];
    if (strncmp(spec, "cmd:", 4) == 0) {
        snprintf(cmd, sizeof(cmd), "%s", spec + 4);
        snprintf(o->name, sizeof(o->name), "cmd");
    } else if (strcmp(spec, "z3") == 0) {
        const char *bin = getenv("DV_ORACLE_BIN");
        snprintf(cmd, sizeof(cmd), "%s -in -smt2 -t:%s", bin && *bin ? bin : "z3", ms);
        snprintf(o->name, sizeof(o->name), "z3");
    } else if (strcmp(spec, "bitwuzla") == 0) {
        const char *bin = getenv("DV_ORACLE_BIN");
        snprintf(cmd, sizeof(cmd), "%s --lang smt2 -m -T %s", bin && *bin ? bin : "bitwuzla", ms);
        snprintf(o->name, sizeof(o->name), "bitwuzla");
    } else {
        fprintf(err, "dv-solve oracle: DV_ORACLE=%s not understood "
                     "(z3 | bitwuzla | cmd:<command>); running without it\n", spec);
        free(o);
        return NULL;
    }
    o->cmdline = strdup(cmd);
    o->cmdv = _split_cmd(cmd);

    /* The run directory: DV_ORACLE_OUT, a pattern; never another run's. */
    const char *pat = getenv("DV_ORACLE_OUT");
    char *dir = _expand_pattern(pat && *pat ? pat : "dvs-oracle/%t-%p");
    int c = _claim(dir);
    if (c == 1) {
        char *alt = (char *)malloc(strlen(dir) + 32);
        sprintf(alt, "%s-%ld", dir, (long)getpid());
        fprintf(err, "dv-solve oracle: %s is another run's; using %s\n", dir, alt);
        free(dir);
        dir = alt;
        c = _claim(dir);
    }
    if (c != 0) {
        fprintf(err, "dv-solve oracle: cannot create run directory %s (%s); "
                     "running without it\n", dir, strerror(errno));
        free(dir);
        free(o->cmdline);
        free(o);
        return NULL;
    }
    o->run_dir = dir;

    char path[4200];
    if (o->keep != KEEP_SUMMARY) {
        snprintf(path, sizeof(path), "%s/queries.jsonl", dir);
        o->jsonl = fopen(path, "w");
        if (o->jsonl) _cloexec(fileno(o->jsonl));
    }
    if (o->keep == KEEP_ALL) {
        snprintf(path, sizeof(path), "%s/transcript.smt2", dir);
        o->transcript = fopen(path, "w");
        if (o->transcript) _cloexec(fileno(o->transcript));
    }
    snprintf(path, sizeof(path), "%s/fail", dir);
    mkdir(path, 0777);

    if (_spawn(o) < 0) {
        fprintf(err, "dv-solve oracle: cannot start '%s' (%s); running without it\n",
                cmd, strerror(errno));
        _write_run_json(o, "failed");
        free(o);
        return NULL;
    }
    /* Version, which also proves the oracle is alive and speaks SMT-LIB2. */
    _send(o, "(get-info :version)\n");
    int why = V_NONE;
    char *r = _read_resp(o, _now() + 10.0, &why);
    if (!r) {
        fprintf(err, "dv-solve oracle: '%s' did not answer (get-info :version); "
                     "running without it\n", cmd);
        _kill(o);
        _write_run_json(o, "failed");
        free(o);
        return NULL;
    }
    const char *q1 = strchr(r, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (q1 && q2) snprintf(o->version, sizeof(o->version), "%.*s", (int)(q2 - q1 - 1), q1 + 1);
    else snprintf(o->version, sizeof(o->version), "%s", r);
    free(r);
    _write_run_json(o, "running");
    fprintf(err, "dv-solve oracle: %s %s, recording to %s\n", o->name, o->version, dir);
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
    _ob_add(&o->session, text, len);
    _ob_add(&o->session, "\n", 1);
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
    _ob_clear(&o->session);
    o->session_overflow = 0;
    o->logic_set = 0;
    free(o->taint);
    o->taint = NULL;
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
    if (j - i == 3 && memcmp(a + i, "sat", 3) == 0) return V_SAT;
    if (j - i == 5 && memcmp(a + i, "unsat", 5) == 0) return V_UNSAT;
    if (j - i == 7 && memcmp(a + i, "unknown", 7) == 0) return V_UNKNOWN;
    return V_NONE;
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
static void _build_pins(Smt2Oracle *o, Smt2Frontend *fe, OBuf *pins,
                        OBuf *partial, uint32_t *n_pinned) {
    OBuf req = {0};
    uint32_t *idx = (uint32_t *)malloc((o->n_decls + 1) * sizeof(uint32_t));
    uint32_t n = 0;
    *n_pinned = 0;
    _ob_str(&req, "(get-value (");
    for (uint32_t i = 0; idx && i < o->n_decls; i++) {
        ODecl *d = &o->decls[i];
        if (d->epoch > o->model_epoch) continue;   /* declared after the model */
        if (d->kind == DK_OTHER) {
            if (partial->n) _ob_str(partial, " ");
            _ob_str(partial, d->name);
            continue;
        }
        _ob_str(&req, " ");
        _ob_str(&req, d->name);
        idx[n++] = i;
    }
    _ob_str(&req, "))");
    if (n == 0 || !idx) { free(idx); _ob_free(&req); return; }

    size_t rn = 0;
    char *r = smt2_frontend_get_value_text(fe, req.p, req.n, &rn);
    _ob_free(&req);
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
            _ob_str(pins, "(assert (= ");
            _ob_str(pins, d->name);
            _ob_str(pins, " ");
            /* A Bool is a 1-bit vector inside dv-solve, and get-value prints
             * it that way; pin it as the Bool the declaration says it is. */
            if (d->kind == DK_BOOL && ve - vs == 3 && r[vs] == '#' && r[vs + 1] == 'b')
                _ob_str(pins, r[vs + 2] == '1' ? "true" : "false");
            else
                _ob_add(pins, r + vs, ve - vs);
            _ob_str(pins, "))\n");
            (*n_pinned)++;
            i = pend;
        }
    }
    if (k < n) {
        if (partial->n) _ob_str(partial, " ");
        _ob_str(partial, "<get-value reply not understood>");
    }
    free(r);
    free(idx);
}

/* Write the repro bundle for query q: the session's forwarded commands, then
 * the check, and a JSON with the details. Returns the bundle's relative path
 * (static storage) or NULL. */
static const char *_bundle(Smt2Oracle *o, uint64_t q, const char *check_text,
                           const char *details_json) {
    static char rel[64];
    if (o->n_bundles >= ORACLE_MAX_BUNDLES) return NULL;
    o->n_bundles++;
    snprintf(rel, sizeof(rel), "fail/q%06llu", (unsigned long long)q);
    char path[4300];
    snprintf(path, sizeof(path), "%s/%s.smt2", o->run_dir, rel);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "; dv-solve oracle repro: query %llu, session %llu\n",
                (unsigned long long)q, (unsigned long long)o->session_no);
        fputs(check_text, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/%s.json", o->run_dir, rel);
    f = fopen(path, "w");
    if (f) { fputs(details_json, f); fputc('\n', f); fclose(f); }
    return rel;
}

static double _rand01(Smt2Oracle *o) {
    o->rng ^= o->rng << 13; o->rng ^= o->rng >> 7; o->rng ^= o->rng << 17;
    return (double)(o->rng >> 11) / 9007199254740992.0;
}

static void _check(Smt2Oracle *o, Smt2Frontend *fe, const Sexpr *cmd,
                   const char *text, size_t len, int dv, double dvs_ms) {
    uint64_t q = o->q++;
    int is_csa = sexpr_is_symbol(cmd->list.items[0], "check-sat-assuming");
    int reused = fe->model_reused;
    const char *sem = !o->verilator_mode ? "literal"
                    : is_csa ? "vlt-diversity"
                    : reused ? "vlt-parity-skip" : "literal";
    const char *engine = reused ? "reused" : smt2_frontend_engine(fe);
    if (!reused) o->model_epoch = o->epoch++;
    o->dvs_ms += dvs_ms;

    /* The oracle's form of the query: literal, except that verilator mode
     * answers check-sat-assuming without the assumptions (§2.2). */
    OBuf query = {0};
    if (is_csa && !o->verilator_mode) _ob_add(&query, text, len);
    else _ob_str(&query, "(check-sat)");
    _ob_str(&query, "\n");

    int res = RES_SKIPPED, ov = V_NONE;
    const char *check = "none";
    OBuf errs = {0}, pins = {0}, partial = {0}, witness = {0}, check_text = {0};
    uint32_t n_pinned = 0;
    double ot = 0.0;

    int want = (dv == V_SAT && o->chk_model) || (dv == V_UNSAT && o->chk_unsat)
            || (dv == V_UNKNOWN && o->chk_unknown);
    if (want && o->rate < 1.0 && _rand01(o) >= o->rate) want = 0;

    if (want && o->dead) _respawn(o);
    if (want && (o->dead || o->session_overflow)) {
        res = RES_ORACLE_ERROR;
        _ob_str(&errs, o->dead ? "oracle not running"
                               : "assertion stack over the recording cap");
    } else if (want) {
        /* Each check replays the flattened assertion stack into a freshly reset
         * oracle rather than mirroring push/pop live: once z3 has seen a push it
         * switches to its incremental core, which cannot decide even a fully
         * pinned QF_ABV query from riscv-dv (unknown after 20 s, against 20 ms
         * for the same query without the scopes). The script is also the
         * bundle's repro, exactly as the oracle saw it. */
        char note[160];
        snprintf(note, sizeof(note), "; @q %llu dvs=%s engine=%s sem=%s\n",
                 (unsigned long long)q, _vname[dv], engine, sem);
        if (o->transcript) fputs(note, o->transcript);
        free(o->taint);
        o->taint = NULL;
        double t0 = _now();
        double deadline = t0 + o->timeout_s + 5.0;
        if (o->session.n) _ob_add(&check_text, o->session.p, o->session.n);
        if (dv == V_SAT) {
            check = "model";
            _build_pins(o, fe, &pins, &partial, &n_pinned);
            if (pins.n) _ob_add(&check_text, pins.p, pins.n);
        } else {
            check = "solve";
        }
        _ob_add(&check_text, query.p, query.n);
        _send(o, "(reset)\n");
        _send_raw(o, check_text.p, check_text.n);
        ov = _read_verdict(o, deadline, &errs);
        if (dv == V_SAT)
            res = ov == V_SAT ? RES_OK : ov == V_UNSAT ? RES_BAD_MODEL
                : ov == V_UNKNOWN || ov == V_TIMEOUT ? RES_UNCHECKED : RES_ORACLE_ERROR;
        else if (dv == V_UNSAT)
            res = ov == V_UNSAT ? RES_OK : ov == V_SAT ? RES_BAD_UNSAT
                : ov == V_UNKNOWN || ov == V_TIMEOUT ? RES_UNCHECKED : RES_ORACLE_ERROR;
        else
            res = ov == V_SAT || ov == V_UNSAT ? RES_GAP
                : ov == V_UNKNOWN || ov == V_TIMEOUT ? RES_UNCHECKED : RES_ORACLE_ERROR;
        if (res == RES_BAD_UNSAT) {
            _send(o, "(get-model)\n");
            int why = V_NONE;
            char *m = _read_resp(o, _now() + 30.0, &why);
            if (m) { _ob_str(&witness, m); free(m); }
        }
        /* An oracle that rejected a command holds fewer constraints than
         * dv-solve. That cannot turn a good model bad, so bad-model stands; any
         * other verdict is not trustworthy. */
        if (errs.n && res != RES_BAD_MODEL) res = RES_ORACLE_ERROR;
        ot = (_now() - t0) * 1000.0;
        o->orc_ms += ot;
        if (ov == V_TIMEOUT || ov == V_DEAD) _respawn(o);
        if (o->transcript) {
            fprintf(o->transcript, "; @q %llu orc=%s %.1fms res=%s\n",
                    (unsigned long long)q, _vname[ov], ot, _rname[res]);
        }
    }
    o->counts[res]++;
    if (partial.n) o->n_partial++;

    /* The record: details JSON shared by queries.jsonl and the bundle. */
    char *dj = NULL;
    size_t djn = 0;
    FILE *m = open_memstream(&dj, &djn);
    if (m) {
        fprintf(m, "{\"q\": %llu, \"s\": %llu, \"d\": %u, \"cmd\": \"%s\", \"sem\": \"%s\", ",
                (unsigned long long)q, (unsigned long long)o->session_no, o->n_scope,
                is_csa ? "check-sat-assuming" : "check-sat", sem);
        fprintf(m, "\"dvs\": {\"verdict\": \"%s\", \"engine\": ", _vname[dv]);
        _json_cstr(m, engine);
        fprintf(m, ", \"ms\": %.3f, \"validate\": %d}, ", dvs_ms, fe->last_validate_viol);
        fprintf(m, "\"orc\": {\"check\": \"%s\", \"verdict\": \"%s\", \"ms\": %.3f, \"pinned\": %u}, ",
                check, _vname[ov], ot, n_pinned);
        fprintf(m, "\"res\": \"%s\"", _rname[res]);
        if (partial.n) { fputs(", \"partial\": ", m); _json_str(m, partial.p, partial.n); }
        if (errs.n) { fputs(", \"errors\": ", m); _json_str(m, errs.p, errs.n); }
        fclose(m);
    }
    const char *file = NULL;
    if (dj && res != RES_OK && res != RES_SKIPPED && res != RES_UNCHECKED) {
        OBuf full = {0};
        _ob_add(&full, dj, djn);      /* the object is still open: no '}' yet */
        _ob_str(&full, ", \"dvs_model\": ");
        {
            char *t = NULL; size_t tn = 0;
            FILE *jm = open_memstream(&t, &tn);
            if (jm) {
                _json_str(jm, pins.p ? pins.p : "", pins.n);
                fputs(", \"oracle_witness\": ", jm);
                if (witness.n) _json_str(jm, witness.p, witness.n); else fputs("null", jm);
                fputs(", \"query\": ", jm);
                _json_str(jm, text, len);
                fputs("}", jm);
                fclose(jm);
                _ob_add(&full, t, tn);
                free(t);
            }
        }
        file = _bundle(o, q, check_text.n ? check_text.p : query.p, full.p);
        _ob_free(&full);
    }
    if (o->jsonl && dj) {
        fwrite(dj, 1, djn, o->jsonl);
        if (file) fprintf(o->jsonl, ", \"file\": \"%s\"", file);
        fputs("}\n", o->jsonl);
        fflush(o->jsonl);
    }
    free(dj);

    if (res == RES_BAD_MODEL || res == RES_BAD_UNSAT || res == RES_GAP || res == RES_ORACLE_ERROR) {
        fprintf(o->err, "dv-solve oracle: q%llu %s (dvs=%s oracle=%s)%s%s/%s\n",
                (unsigned long long)q, _rname[res], _vname[dv], _vname[ov],
                file ? " -> " : "", file ? o->run_dir : "", file ? file : "");
        fflush(o->err);
    }
    _ob_free(&query); _ob_free(&errs); _ob_free(&pins); _ob_free(&partial);
    _ob_free(&witness); _ob_free(&check_text);

    if (o->abort_on_fail && (res == RES_BAD_MODEL || res == RES_BAD_UNSAT)) {
        o->aborted = 1;
        smt2_oracle_finish(o);
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
        int dv = answer ? _verdict_of(answer, answer_len) : V_NONE;
        if (dv == V_NONE) return;    /* an error, not an answer */
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
        _ob_clear(&o->session);
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
    _write_run_json(o, o->aborted ? "aborted" : "complete");
    fprintf(o->err, "dv-solve oracle: %s: %llu queries", o->run_dir,
            (unsigned long long)o->q);
    for (int r = 0; r < RES__N; r++)
        if (o->counts[r]) fprintf(o->err, ", %s %llu", _rname[r], (unsigned long long)o->counts[r]);
    if (o->n_partial) fprintf(o->err, ", partial %llu", (unsigned long long)o->n_partial);
    fprintf(o->err, "\n");
    fflush(o->err);
    if (!o->dead && o->in_fd >= 0) _send(o, "(exit)\n");
    if (o->in_fd >= 0) { close(o->in_fd); o->in_fd = -1; }
    if (o->pid > 0) {
        /* Give it a moment to exit on its own, then make sure. */
        for (int i = 0; i < 50; i++) {
            if (waitpid(o->pid, NULL, WNOHANG) == o->pid) { o->pid = -1; break; }
            usleep(10000);
        }
        if (o->pid > 0) { kill(o->pid, SIGKILL); waitpid(o->pid, NULL, 0); }
    }
    if (o->out_fd >= 0) close(o->out_fd);
    if (o->jsonl) fclose(o->jsonl);
    if (o->transcript) fclose(o->transcript);
    _truncate_decls(o, 0);
    free(o->decls); free(o->scope); free(o->scope_pos); free(o->taint);
    _ob_free(&o->session); _ob_free(&o->inbuf);
    if (o->cmdv) { for (char **p = o->cmdv; *p; p++) free(*p); free(o->cmdv); }
    free(o->cmdline); free(o->run_dir);
    free(o);
}

#endif /* POSIX */
