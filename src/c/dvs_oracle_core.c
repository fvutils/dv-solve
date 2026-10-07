/*
 * dvs_oracle_core -- reference solver process, run directory and records,
 * shared by the SMT-LIB2 oracle and the API oracle. See dvs_oracle_core.h and
 * docs/api_oracle_plan.md.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "dvs_oracle_core.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef DVS_VERSION
#define DVS_VERSION "unknown"
#endif
#ifndef DVS_GIT_COMMIT
#define DVS_GIT_COMMIT "unknown"
#endif

/* ------------------------------------------------------------------ */
/* Text and JSON helpers (all platforms)                               */
/* ------------------------------------------------------------------ */

int dvs_ob_add(DvsOBuf *b, const char *s, size_t n) {
    if (n > SIZE_MAX / 4 || b->n > SIZE_MAX / 4) return -1;
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n + 1) c *= 2;
        char *g = (char *)realloc(b->p, c);
        if (!g) return -1;
        b->p = g; b->cap = c;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
    return 0;
}
int dvs_ob_str(DvsOBuf *b, const char *s) { return dvs_ob_add(b, s, strlen(s)); }
int dvs_ob_printf(DvsOBuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(tmp)) return dvs_ob_add(b, tmp, (size_t)n);
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int r = dvs_ob_add(b, big, (size_t)n);
    free(big);
    return r;
}
void dvs_ob_clear(DvsOBuf *b) { b->n = 0; if (b->p) b->p[0] = '\0'; }
void dvs_ob_free(DvsOBuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

void dvs_json_str(FILE *f, const char *s, size_t n) {
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
void dvs_json_cstr(FILE *f, const char *s) {
    if (!s) { fputs("null", f); return; }
    dvs_json_str(f, s, strlen(s));
}

static const char *const _vname[] = {
    "none", "sat", "unsat", "unknown", "timeout", "dead", "error" };
static const char *const _rname[DVS_ORC__N] = {
    "ok", "bad-model", "bad-unsat", "gap", "unchecked", "oracle-error", "skipped" };

const char *dvs_orc_verdict_name(int v) {
    return (v >= 0 && v <= DVS_ORC_V_ERROR) ? _vname[v] : "?";
}
const char *dvs_orc_result_name(int r) {
    return (r >= 0 && r < DVS_ORC__N) ? _rname[r] : "?";
}

#ifdef _WIN32

DvsOrc *dvs_orc_create(const DvsOrcConfig *cfg, FILE *err) {
    (void)cfg;
    fprintf(err, "dv-solve oracle: not available on Windows; running without it\n");
    return NULL;
}
int dvs_orc_wants(DvsOrc *o, int dv) { (void)o; (void)dv; return 0; }
int dvs_orc_check(DvsOrc *o, const DvsOrcQuery *q) { (void)o; (void)q; return DVS_ORC_SKIPPED; }
const char *dvs_orc_run_dir(const DvsOrc *o) { (void)o; return NULL; }
uint64_t dvs_orc_count(const DvsOrc *o, int res) { (void)o; (void)res; return 0; }
uint64_t dvs_orc_queries(const DvsOrc *o) { (void)o; return 0; }
void dvs_orc_times(const DvsOrc *o, double *a, double *b) { (void)o; *a = *b = 0.0; }
const char *dvs_orc_version(const DvsOrc *o) { (void)o; return NULL; }
void dvs_orc_finish(DvsOrc *o, int aborted) { (void)o; (void)aborted; }

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

static char **_split_cmd(const char *s) {
    size_t cap = 8, n = 0;
    char **v = (char **)calloc(cap, sizeof(char *));
    const char *p = s;
    while (v && *p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *q = p;
        while (*q && *q != ' ' && *q != '\t') q++;
        if (n + 2 > cap) {
            cap *= 2;
            char **g = (char **)realloc(v, cap * sizeof(char *));
            if (!g) { for (size_t i = 0; i < n; i++) free(v[i]); free(v); return NULL; }
            v = g;
        }
        v[n++] = strndup(p, (size_t)(q - p));
        p = q;
    }
    if (v) v[n] = NULL;
    return v;
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

#define ORACLE_MAX_BUNDLES   2000

struct DvsOrc {
    /* Configuration */
    char   **cmdv;              /* the oracle's argv (NULL-terminated) */
    char    *cmdline;
    char     name[64];
    double   timeout_s;
    int      chk_model, chk_unsat, chk_unknown;
    double   rate;
    int      keep;
    char    *run_dir;
    char    *tag;
    char    *mode;
    char    *verilator_hash;
    int      argc;
    char   **argv;
    FILE    *err;

    /* The reference solver */
    pid_t    pid;
    int      in_fd, out_fd;
    DvsOBuf  inbuf;             /* oracle output not consumed yet */
    int      dead;
    char     version[128];
    char    *taint;             /* first oracle error in the current check */

    /* Recording */
    FILE    *jsonl;
    FILE    *transcript;
    uint64_t q;
    uint64_t counts[DVS_ORC__N];
    uint64_t n_partial;
    uint64_t max_s;
    double   dvs_ms, orc_ms;
    int      n_bundles;
    char     start_s[32];
    uint64_t rng;
};

/* ------------------------------------------------------------------ */
/* The reference solver process                                        */
/* ------------------------------------------------------------------ */

static int _spawn(DvsOrc *o) {
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
    dvs_ob_clear(&o->inbuf);
    o->dead = 0;
    return 0;
}

static void _kill(DvsOrc *o) {
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
static int _drain(DvsOrc *o) {
    char tmp[65536];
    for (;;) {
        ssize_t r = read(o->out_fd, tmp, sizeof(tmp));
        if (r > 0) { dvs_ob_add(&o->inbuf, tmp, (size_t)r); continue; }
        if (r == 0) return 0;
        if (errno == EINTR) continue;
        return 1;   /* EAGAIN */
    }
}

/* Send raw text to the oracle (also to the transcript). Reads the oracle's
 * output while waiting so that neither side can block on a full pipe. */
static int _send_raw(DvsOrc *o, const char *s, size_t n) {
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
static int _send(DvsOrc *o, const char *s) { return _send_raw(o, s, strlen(s)); }

/* Take one complete response off inbuf: an s-expression or a line-terminated
 * atom. Comment lines are dropped. Returns a malloc'd string or NULL if no
 * complete response is buffered yet. */
static char *_take_resp(DvsOrc *o) {
    for (;;) {
        size_t i = 0, n = o->inbuf.n;
        const char *b = o->inbuf.p;
        while (i < n && isspace((unsigned char)b[i])) i++;
        if (i == n) { dvs_ob_clear(&o->inbuf); return NULL; }
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
static char *_read_resp(DvsOrc *o, double deadline, int *why) {
    for (;;) {
        char *r = _take_resp(o);
        if (r) return r;
        if (o->dead) { *why = DVS_ORC_V_DEAD; return NULL; }
        int ms = (int)((deadline - _now()) * 1000.0);
        if (ms <= 0) { *why = DVS_ORC_V_TIMEOUT; return NULL; }
        struct pollfd p = { o->out_fd, POLLIN, 0 };
        int pr = poll(&p, 1, ms);
        if (pr < 0 && errno != EINTR) { _kill(o); *why = DVS_ORC_V_DEAD; return NULL; }
        if (pr > 0 && _drain(o) == 0) {
            /* EOF: keep what was buffered, then report the death. */
            char *r2 = _take_resp(o);
            if (!r2 && o->inbuf.n) {           /* an unterminated last line */
                r2 = strdup(o->inbuf.p);
                dvs_ob_clear(&o->inbuf);
            }
            _kill(o);
            if (r2) return r2;
            *why = DVS_ORC_V_DEAD;
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
static int _read_verdict(DvsOrc *o, double deadline, DvsOBuf *errs) {
    for (;;) {
        int why = DVS_ORC_V_NONE;
        char *r = _read_resp(o, deadline, &why);
        if (!r) return why;
        int v = DVS_ORC_V_NONE;
        if (strcmp(r, "sat") == 0) v = DVS_ORC_V_SAT;
        else if (strcmp(r, "unsat") == 0) v = DVS_ORC_V_UNSAT;
        else if (strcmp(r, "unknown") == 0) v = DVS_ORC_V_UNKNOWN;
        else {
            if (errs) { if (errs->n) dvs_ob_str(errs, "\n"); dvs_ob_str(errs, r); }
            if (!o->taint && _is_error_resp(r)) o->taint = strdup(r);
        }
        free(r);
        if (v != DVS_ORC_V_NONE) return v;
    }
}

/* Replace the oracle process (after a hard timeout, or its death). Each check
 * starts from (reset), so there is no state to restore. */
static void _respawn(DvsOrc *o) {
    _kill(o);
    if (_spawn(o) < 0) o->dead = 1;
}

/* ------------------------------------------------------------------ */
/* Run directory and records                                           */
/* ------------------------------------------------------------------ */

static char *_expand_pattern(const char *pat) {
    DvsOBuf b = {0};
    char ts[32], pid[32];
    _utc(ts, sizeof(ts), 1);
    snprintf(pid, sizeof(pid), "%ld", (long)getpid());
    for (const char *p = pat; *p; p++) {
        if (*p == '%' && p[1]) {
            p++;
            if (*p == 'p') dvs_ob_str(&b, pid);
            else if (*p == 't') dvs_ob_str(&b, ts);
            else if (*p == '%') dvs_ob_add(&b, "%", 1);
            else { dvs_ob_add(&b, "%", 1); dvs_ob_add(&b, p, 1); }
        } else {
            dvs_ob_add(&b, p, 1);
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

static void _write_run_json(DvsOrc *o, const char *status) {
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
        dvs_json_cstr(f, o->argv[i]);
    }
    fprintf(f, "],\n");
    fprintf(f, "  \"mode\": ");
    dvs_json_cstr(f, o->mode);
    fprintf(f, ",\n");
    if (o->verilator_hash) {
        fprintf(f, "  \"verilator_hash\": ");
        dvs_json_cstr(f, o->verilator_hash);
        fprintf(f, ",\n");
    }
    fprintf(f, "  \"env\": {");
    extern char **environ;
    int first = 1;
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, "DV_", 3) != 0) continue;
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        fprintf(f, "%s", first ? "" : ", ");
        first = 0;
        dvs_json_str(f, *e, (size_t)(eq - *e));
        fputs(": ", f);
        dvs_json_cstr(f, eq + 1);
    }
    fprintf(f, "},\n");
    fprintf(f, "  \"oracle\": {\"name\": ");
    dvs_json_cstr(f, o->name);
    fprintf(f, ", \"command\": ");
    dvs_json_cstr(f, o->cmdline);
    fprintf(f, ", \"version\": ");
    dvs_json_cstr(f, o->version[0] ? o->version : NULL);
    fprintf(f, ", \"timeout_s\": %g},\n", o->timeout_s);
    fprintf(f, "  \"checks\": {\"model\": %s, \"unsat\": %s, \"unknown\": %s, \"rate\": %g},\n",
            o->chk_model ? "true" : "false", o->chk_unsat ? "true" : "false",
            o->chk_unknown ? "true" : "false", o->rate);
    fprintf(f, "  \"keep\": \"%s\",\n",
            o->keep == DVS_ORC_KEEP_ALL ? "all"
            : o->keep == DVS_ORC_KEEP_FAIL ? "fail" : "summary");
    fprintf(f, "  \"tag\": ");
    dvs_json_cstr(f, o->tag);
    fprintf(f, ",\n  \"pid\": %ld,\n", (long)getpid());
    fprintf(f, "  \"start\": \"%s\",\n", o->start_s);
    fprintf(f, "  \"end\": ");
    if (strcmp(status, "running") == 0) fputs("null", f); else dvs_json_cstr(f, now);
    fprintf(f, ",\n  \"summary\": {\"queries\": %llu",
            (unsigned long long)o->q);
    for (int r = 0; r < DVS_ORC__N; r++)
        fprintf(f, ", \"%s\": %llu", _rname[r], (unsigned long long)o->counts[r]);
    fprintf(f, ", \"partial\": %llu, \"sessions\": %llu, \"dvs_ms\": %.3f, \"oracle_ms\": %.3f}\n}\n",
            (unsigned long long)o->n_partial, (unsigned long long)o->max_s + 1,
            o->dvs_ms, o->orc_ms);
    fclose(f);
    rename(tmp, path);
}

static void _free_orc(DvsOrc *o) {
    if (o->cmdv) { for (char **p = o->cmdv; *p; p++) free(*p); free(o->cmdv); }
    free(o->cmdline); free(o->run_dir); free(o->tag); free(o->mode);
    free(o->verilator_hash); free(o->taint);
    dvs_ob_free(&o->inbuf);
    if (o->jsonl) fclose(o->jsonl);
    if (o->transcript) fclose(o->transcript);
    free(o);
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

DvsOrc *dvs_orc_create(const DvsOrcConfig *cfg, FILE *err) {
    DvsOrc *o = (DvsOrc *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->pid = -1; o->in_fd = o->out_fd = -1;
    o->err = err ? err : stderr;
    o->argc = cfg->argc; o->argv = cfg->argv;
    o->timeout_s = cfg->timeout_s > 0 ? cfg->timeout_s : 10.0;
    o->rate = cfg->rate;        /* 0 checks nothing */
    o->chk_model = cfg->chk_model;
    o->chk_unsat = cfg->chk_unsat;
    o->chk_unknown = cfg->chk_unknown;
    o->keep = cfg->keep;
    o->tag = cfg->tag ? strdup(cfg->tag) : NULL;
    o->mode = strdup(cfg->mode ? cfg->mode : "default");
    o->verilator_hash = cfg->verilator_hash ? strdup(cfg->verilator_hash) : NULL;
    o->rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)getpid();
    snprintf(o->name, sizeof(o->name), "%s", cfg->name ? cfg->name : "cmd");
    _utc(o->start_s, sizeof(o->start_s), 0);
    o->cmdline = strdup(cfg->cmdline);
    o->cmdv = _split_cmd(cfg->cmdline);
    if (!o->cmdv || !o->cmdv[0]) {
        fprintf(o->err, "dv-solve oracle: empty command line; running without it\n");
        _free_orc(o);
        return NULL;
    }

    /* The run directory: a pattern; never another run's. */
    const char *pat = cfg->out_pattern;
    char *dir = _expand_pattern(pat && *pat ? pat : "dvs-oracle/%t-%p");
    int c = _claim(dir);
    if (c == 1) {
        char *alt = (char *)malloc(strlen(dir) + 32);
        sprintf(alt, "%s-%ld", dir, (long)getpid());
        /* Several oracles in one process: count up until one is free. */
        for (int k = 2; (c = _claim(alt)) == 1 && k < 1000; k++)
            sprintf(alt, "%s-%ld.%d", dir, (long)getpid(), k);
        fprintf(o->err, "dv-solve oracle: %s is another run's; using %s\n", dir, alt);
        free(dir);
        dir = alt;
    }
    if (c != 0) {
        fprintf(o->err, "dv-solve oracle: cannot create run directory %s (%s); "
                        "running without it\n", dir, strerror(errno));
        free(dir);
        _free_orc(o);
        return NULL;
    }
    o->run_dir = dir;

    char path[4200];
    if (o->keep != DVS_ORC_KEEP_SUMMARY) {
        snprintf(path, sizeof(path), "%s/queries.jsonl", dir);
        o->jsonl = fopen(path, "w");
        if (o->jsonl) _cloexec(fileno(o->jsonl));
    }
    if (o->keep == DVS_ORC_KEEP_ALL) {
        snprintf(path, sizeof(path), "%s/transcript.smt2", dir);
        o->transcript = fopen(path, "w");
        if (o->transcript) _cloexec(fileno(o->transcript));
    }
    snprintf(path, sizeof(path), "%s/fail", dir);
    mkdir(path, 0777);

    if (_spawn(o) < 0) {
        fprintf(o->err, "dv-solve oracle: cannot start '%s' (%s); running without it\n",
                o->cmdline, strerror(errno));
        _write_run_json(o, "failed");
        _free_orc(o);
        return NULL;
    }
    /* Version, which also proves the oracle is alive and speaks SMT-LIB2. */
    _send(o, "(get-info :version)\n");
    int why = DVS_ORC_V_NONE;
    char *r = _read_resp(o, _now() + 10.0, &why);
    if (!r) {
        fprintf(o->err, "dv-solve oracle: '%s' did not answer (get-info :version); "
                        "running without it\n", o->cmdline);
        _kill(o);
        _write_run_json(o, "failed");
        _free_orc(o);
        return NULL;
    }
    const char *q1 = strchr(r, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (q1 && q2) snprintf(o->version, sizeof(o->version), "%.*s", (int)(q2 - q1 - 1), q1 + 1);
    else snprintf(o->version, sizeof(o->version), "%s", r);
    free(r);
    _write_run_json(o, "running");
    fprintf(o->err, "dv-solve oracle: %s %s, recording to %s\n", o->name, o->version, dir);
    return o;
}

/* ------------------------------------------------------------------ */
/* The check                                                           */
/* ------------------------------------------------------------------ */

/* Write the repro bundle for query q: the script, then the check, and a JSON
 * with the details. Returns the bundle's relative path (static storage) or
 * NULL. */
static const char *_bundle(DvsOrc *o, uint64_t q, uint64_t s, const char *check_text,
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
                (unsigned long long)q, (unsigned long long)s);
        fputs(check_text, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/%s.json", o->run_dir, rel);
    f = fopen(path, "w");
    if (f) { fputs(details_json, f); fputc('\n', f); fclose(f); }
    return rel;
}

static double _rand01(DvsOrc *o) {
    o->rng ^= o->rng << 13; o->rng ^= o->rng >> 7; o->rng ^= o->rng << 17;
    return (double)(o->rng >> 11) / 9007199254740992.0;
}

int dvs_orc_wants(DvsOrc *o, int dv) {
    if (!o) return 0;
    int want = (dv == DVS_ORC_V_SAT && o->chk_model)
            || (dv == DVS_ORC_V_UNSAT && o->chk_unsat)
            || (dv == DVS_ORC_V_UNKNOWN && o->chk_unknown);
    if (want && o->rate < 1.0 && _rand01(o) >= o->rate) want = 0;
    if (want && o->dead) _respawn(o);
    return want;
}

int dvs_orc_check(DvsOrc *o, const DvsOrcQuery *qq) {
    uint64_t q = o->q++;
    int dv = qq->dv;
    if (qq->s > o->max_s) o->max_s = qq->s;
    o->dvs_ms += qq->dvs_ms;

    int res = DVS_ORC_SKIPPED, ov = DVS_ORC_V_NONE;
    const char *check = "none";
    DvsOBuf errs = {0}, witness = {0}, check_text = {0};
    double ot = 0.0;

    if (qq->want && (o->dead || qq->error)) {
        res = DVS_ORC_ORACLE_ERROR;
        dvs_ob_str(&errs, o->dead ? "oracle not running" : qq->error);
    } else if (qq->want) {
        char note[200];
        snprintf(note, sizeof(note), "; @q %llu dvs=%s engine=%s sem=%s\n",
                 (unsigned long long)q, _vname[dv], qq->engine, qq->sem);
        if (o->transcript) fputs(note, o->transcript);
        free(o->taint);
        o->taint = NULL;
        double t0 = _now();
        double deadline = t0 + o->timeout_s + 5.0;
        if (qq->script_n) dvs_ob_add(&check_text, qq->script, qq->script_n);
        if (dv == DVS_ORC_V_SAT) {
            check = "model";
            if (qq->pins_n) dvs_ob_add(&check_text, qq->pins, qq->pins_n);
        } else {
            check = "solve";
        }
        dvs_ob_str(&check_text, qq->query);
        _send(o, "(reset)\n");
        _send_raw(o, check_text.p, check_text.n);
        ov = _read_verdict(o, deadline, &errs);
        if (dv == DVS_ORC_V_SAT)
            res = ov == DVS_ORC_V_SAT ? DVS_ORC_OK
                : ov == DVS_ORC_V_UNSAT ? DVS_ORC_BAD_MODEL
                : ov == DVS_ORC_V_UNKNOWN || ov == DVS_ORC_V_TIMEOUT ? DVS_ORC_UNCHECKED
                : DVS_ORC_ORACLE_ERROR;
        else if (dv == DVS_ORC_V_UNSAT)
            res = ov == DVS_ORC_V_UNSAT ? DVS_ORC_OK
                : ov == DVS_ORC_V_SAT ? DVS_ORC_BAD_UNSAT
                : ov == DVS_ORC_V_UNKNOWN || ov == DVS_ORC_V_TIMEOUT ? DVS_ORC_UNCHECKED
                : DVS_ORC_ORACLE_ERROR;
        else
            res = ov == DVS_ORC_V_SAT || ov == DVS_ORC_V_UNSAT ? DVS_ORC_GAP
                : ov == DVS_ORC_V_UNKNOWN || ov == DVS_ORC_V_TIMEOUT ? DVS_ORC_UNCHECKED
                : DVS_ORC_ORACLE_ERROR;
        if (res == DVS_ORC_BAD_UNSAT) {
            _send(o, "(get-model)\n");
            int why = DVS_ORC_V_NONE;
            char *m = _read_resp(o, _now() + 30.0, &why);
            if (m) { dvs_ob_str(&witness, m); free(m); }
        }
        /* An oracle that rejected a command holds fewer constraints than
         * dv-solve. That cannot turn a good model bad, so bad-model stands; any
         * other verdict is not trustworthy. */
        if (errs.n && res != DVS_ORC_BAD_MODEL) res = DVS_ORC_ORACLE_ERROR;
        ot = (_now() - t0) * 1000.0;
        o->orc_ms += ot;
        if (ov == DVS_ORC_V_TIMEOUT || ov == DVS_ORC_V_DEAD) _respawn(o);
        if (o->transcript) {
            fprintf(o->transcript, "; @q %llu orc=%s %.1fms res=%s\n",
                    (unsigned long long)q, _vname[ov], ot, _rname[res]);
        }
    }
    o->counts[res]++;
    if (qq->partial && *qq->partial) o->n_partial++;

    /* The record: details JSON shared by queries.jsonl and the bundle. */
    char *dj = NULL;
    size_t djn = 0;
    FILE *m = open_memstream(&dj, &djn);
    if (m) {
        fprintf(m, "{\"q\": %llu, \"s\": %llu, \"d\": %u, \"cmd\": \"%s\", \"sem\": \"%s\", ",
                (unsigned long long)q, (unsigned long long)qq->s, qq->d, qq->cmd, qq->sem);
        fprintf(m, "\"dvs\": {\"verdict\": \"%s\", \"engine\": ", _vname[dv]);
        dvs_json_cstr(m, qq->engine);
        fprintf(m, ", \"ms\": %.3f, \"validate\": %d}, ", qq->dvs_ms, qq->validate);
        fprintf(m, "\"orc\": {\"check\": \"%s\", \"verdict\": \"%s\", \"ms\": %.3f, \"pinned\": %u}, ",
                check, _vname[ov], ot, qq->n_pinned);
        fprintf(m, "\"res\": \"%s\"", _rname[res]);
        if (qq->extra_json && *qq->extra_json) fprintf(m, ", %s", qq->extra_json);
        if (qq->partial && *qq->partial) { fputs(", \"partial\": ", m); dvs_json_cstr(m, qq->partial); }
        if (errs.n) { fputs(", \"errors\": ", m); dvs_json_str(m, errs.p, errs.n); }
        fclose(m);
    }
    const char *file = NULL;
    if (dj && res != DVS_ORC_OK && res != DVS_ORC_SKIPPED && res != DVS_ORC_UNCHECKED) {
        DvsOBuf full = {0};
        dvs_ob_add(&full, dj, djn);      /* the object is still open: no '}' yet */
        dvs_ob_str(&full, ", \"dvs_model\": ");
        {
            char *t = NULL; size_t tn = 0;
            FILE *jm = open_memstream(&t, &tn);
            if (jm) {
                dvs_json_str(jm, qq->pins ? qq->pins : "", qq->pins ? qq->pins_n : 0);
                fputs(", \"oracle_witness\": ", jm);
                if (witness.n) dvs_json_str(jm, witness.p, witness.n); else fputs("null", jm);
                fputs(", \"query\": ", jm);
                dvs_json_str(jm, qq->raw ? qq->raw : "", qq->raw ? qq->raw_n : 0);
                fputs("}", jm);
                fclose(jm);
                dvs_ob_add(&full, t, tn);
                free(t);
            }
        }
        file = _bundle(o, q, qq->s, check_text.n ? check_text.p : qq->query, full.p);
        dvs_ob_free(&full);
    }
    if (o->jsonl && dj) {
        fwrite(dj, 1, djn, o->jsonl);
        if (file) fprintf(o->jsonl, ", \"file\": \"%s\"", file);
        fputs("}\n", o->jsonl);
        fflush(o->jsonl);
    }
    free(dj);

    if (res == DVS_ORC_BAD_MODEL || res == DVS_ORC_BAD_UNSAT || res == DVS_ORC_GAP
        || res == DVS_ORC_ORACLE_ERROR) {
        fprintf(o->err, "dv-solve oracle: q%llu %s (dvs=%s oracle=%s)%s%s/%s\n",
                (unsigned long long)q, _rname[res], _vname[dv], _vname[ov],
                file ? " -> " : "", file ? o->run_dir : "", file ? file : "");
        fflush(o->err);
    }
    dvs_ob_free(&errs); dvs_ob_free(&witness); dvs_ob_free(&check_text);
    /* Keep run.json's summary current, so a run killed mid-way still has its
     * counts (the report tool marks it truncated). */
    if (res != DVS_ORC_OK && res != DVS_ORC_SKIPPED) _write_run_json(o, "running");
    return res;
}

const char *dvs_orc_run_dir(const DvsOrc *o) { return o ? o->run_dir : NULL; }
uint64_t dvs_orc_count(const DvsOrc *o, int res) {
    return (o && res >= 0 && res < DVS_ORC__N) ? o->counts[res] : 0;
}
uint64_t dvs_orc_queries(const DvsOrc *o) { return o ? o->q : 0; }
void dvs_orc_times(const DvsOrc *o, double *dvs_ms, double *orc_ms) {
    *dvs_ms = o ? o->dvs_ms : 0.0;
    *orc_ms = o ? o->orc_ms : 0.0;
}
const char *dvs_orc_version(const DvsOrc *o) {
    return (o && o->version[0]) ? o->version : NULL;
}

void dvs_orc_finish(DvsOrc *o, int aborted) {
    if (!o) return;
    _write_run_json(o, aborted ? "aborted" : "complete");
    fprintf(o->err, "dv-solve oracle: %s: %llu queries", o->run_dir,
            (unsigned long long)o->q);
    for (int r = 0; r < DVS_ORC__N; r++)
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
    if (o->out_fd >= 0) { close(o->out_fd); o->out_fd = -1; }
    _free_orc(o);
}

#endif /* POSIX */
