/*
 * solver_tap: transparent proxy placed between Verilator and an SMT solver.
 * (From solver-bench/tools/solver_tap.c. Used for the recorded runs only:
 * it is in the data path and costs dv-solve about 13% of its wall time.)
 *
 *   VERILATOR_SOLVER="solver_tap <solver> <args...>"
 *
 * Environment:
 *   DVS_TAP_JSON=<file>     at exit, write solver CPU/RSS, command counts and
 *                          per-query latency stats (JSON, one object)
 *   DVS_TAP_LOG=<file>      record a timestamped transcript: lines prefixed
 *                          "> " (to solver), "< " (from solver), "# " (meta)
 *   DVS_TAP_QTIMEOUT=<sec>  per-query timeout (default 60, 0 = none); on expiry
 *                          the solver is killed and the tap exits, which
 *                          Verilator sees as a dead solver
 *
 * Verilator spawns one solver per process lifetime but may respawn after a
 * failure; each spawn gets its own tap instance, so the JSON file is appended
 * (one line per instance).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Latencies of check-sat style queries, stored for percentile reporting */
static double *g_lat = NULL;
static size_t g_nlat = 0, g_caplat = 0;
static void lat_push(double v) {
    if (g_nlat == g_caplat) {
        g_caplat = g_caplat ? g_caplat * 2 : 4096;
        g_lat = realloc(g_lat, g_caplat * sizeof(double));
    }
    g_lat[g_nlat++] = v;
}
static int dcmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* Command counters (from the Verilator -> solver stream) */
static uint64_t n_check_sat, n_check_sat_assuming, n_get_value, n_reset, n_push,
    n_get_unsat_assumptions, n_get_unsat_core, n_bytes_in, n_bytes_out;
/* Replies */
static uint64_t n_sat, n_unsat, n_unknown, n_error;

static FILE *g_log;
static volatile sig_atomic_t g_term;
static void on_term(int sig) { (void)sig; g_term = 1; }
static double g_t0;
static int g_pending;  /* queries sent but not yet answered */
static double g_pend_t;  /* send time of the oldest pending query */
static double *g_pendq; static size_t g_pq_head, g_pq_tail, g_pq_cap;

static void pend_push(double t) {
    if (g_pq_tail == g_pq_cap) {
        if (g_pq_head > 0) {
            memmove(g_pendq, g_pendq + g_pq_head, (g_pq_tail - g_pq_head) * sizeof(double));
            g_pq_tail -= g_pq_head; g_pq_head = 0;
        }
        if (g_pq_tail == g_pq_cap) {
            g_pq_cap = g_pq_cap ? g_pq_cap * 2 : 64;
            g_pendq = realloc(g_pendq, g_pq_cap * sizeof(double));
        }
    }
    g_pendq[g_pq_tail++] = t;
    g_pending++;
    g_pend_t = g_pendq[g_pq_head];
}
static void pend_pop(double t) {
    if (g_pending <= 0) return;
    lat_push(t - g_pendq[g_pq_head++]);
    g_pending--;
    if (g_pending) g_pend_t = g_pendq[g_pq_head];
}

/* Line assemblers for both directions */
typedef struct { char *buf; size_t len, cap; } linebuf_t;
static void lb_add(linebuf_t *lb, const char *p, size_t n) {
    if (lb->len + n + 1 > lb->cap) {
        lb->cap = (lb->len + n + 1) * 2;
        lb->buf = realloc(lb->buf, lb->cap);
    }
    memcpy(lb->buf + lb->len, p, n);
    lb->len += n;
    lb->buf[lb->len] = 0;
}

static int starts(const char *s, const char *pfx) { return strncmp(s, pfx, strlen(pfx)) == 0; }

static void on_cmd_line(const char *line, double t) {
    while (*line == ' ' || *line == '\t') line++;
    if (g_log) fprintf(g_log, "%.6f > %s\n", t - g_t0, line);
    if (starts(line, "(check-sat-assuming")) { n_check_sat_assuming++; pend_push(t); }
    else if (starts(line, "(check-sat")) { n_check_sat++; pend_push(t); }
    else if (starts(line, "(get-value")) n_get_value++;
    else if (starts(line, "(reset)")) n_reset++;
    else if (starts(line, "(push")) n_push++;
    else if (starts(line, "(get-unsat-assumptions")) n_get_unsat_assumptions++;
    else if (starts(line, "(get-unsat-core")) n_get_unsat_core++;
}

static void on_reply_line(const char *line, double t) {
    while (*line == ' ' || *line == '\t') line++;
    if (g_log) fprintf(g_log, "%.6f < %s\n", t - g_t0, line);
    if (!strcmp(line, "sat")) { n_sat++; pend_pop(t); }
    else if (!strcmp(line, "unsat")) { n_unsat++; pend_pop(t); }
    else if (!strcmp(line, "unknown")) { n_unknown++; pend_pop(t); }
    else if (starts(line, "(error")) { n_error++; }
}

static void feed(linebuf_t *lb, const char *p, size_t n, int dir, double t) {
    lb_add(lb, p, n);
    char *s = lb->buf, *nl;
    while ((nl = memchr(s, '\n', lb->buf + lb->len - s))) {
        *nl = 0;
        if (nl > s || 1) {
            if (dir == 0) on_cmd_line(s, t); else on_reply_line(s, t);
        }
        s = nl + 1;
    }
    size_t rem = lb->buf + lb->len - s;
    memmove(lb->buf, s, rem);
    lb->len = rem;
}

static int write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= w;
    }
    return 0;
}

static void json_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: solver_tap <solver> [args...]\n");
        return 2;
    }
    const char *json_path = getenv("DVS_TAP_JSON");
    const char *log_path = getenv("DVS_TAP_LOG");
    const char *qto = getenv("DVS_TAP_QTIMEOUT");
    double qtimeout = qto ? atof(qto) : 60.0;
    signal(SIGPIPE, SIG_IGN);
    /* SIGTERM/SIGINT: kill the solver, still write stats */
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    /* "<json>.<pid>.running" exists while this instance is alive, so callers
     * can wait for the stats to be written */
    char running[4096] = "";
    if (json_path) {
        snprintf(running, sizeof running, "%s.%d.running", json_path, (int)getpid());
        FILE *rf = fopen(running, "w"); if (rf) fclose(rf);
    }

    g_t0 = now_s();
    if (log_path) {
        g_log = fopen(log_path, "a");
        if (g_log) {
            setvbuf(g_log, NULL, _IOFBF, 1 << 20);
            fprintf(g_log, "%.6f # open", 0.0);
            for (int i = 1; i < argc; i++) fprintf(g_log, " %s", argv[i]);
            fprintf(g_log, "\n");
        }
    }

    int to_child[2], from_child[2];
    if (pipe(to_child) || pipe(from_child)) { perror("pipe"); return 2; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 2; }
    if (pid == 0) {
        dup2(to_child[0], 0);
        dup2(from_child[1], 1);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        execvp(argv[1], argv + 1);
        perror("solver_tap: execvp");
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    int cin = to_child[1], cout = from_child[0];

    linebuf_t lb_in = {0}, lb_out = {0};
    char buf[1 << 16];
    int in_open = 1, out_open = 1, timed_out = 0;
    int terminated = 0;
    while (out_open) {
        if (g_term) {
            terminated = 1;
            if (g_log) fprintf(g_log, "%.6f # terminated\n", now_s() - g_t0);
            kill(pid, SIGKILL);
            break;
        }
        struct pollfd pfd[2];
        int np = 0;
        int idx_in = -1, idx_out;
        if (in_open) { pfd[np].fd = 0; pfd[np].events = POLLIN; idx_in = np++; }
        pfd[np].fd = cout; pfd[np].events = POLLIN; idx_out = np++;
        int tmo = -1;
        if (qtimeout > 0 && g_pending) {
            double left = g_pend_t + qtimeout - now_s();
            tmo = left <= 0 ? 0 : (int)(left * 1000) + 1;
        }
        int r = poll(pfd, np, tmo);
        if (r < 0) { if (errno == EINTR) continue; perror("poll"); break; }
        /* Verilator gone (stdin EOF) while the solver is still mid-query:
         * nobody will read the answer, so stop now rather than wait */
        if (!in_open && g_pending) { kill(pid, SIGKILL); break; }
        if (r == 0) {
            if (qtimeout > 0 && g_pending && now_s() - g_pend_t >= qtimeout) {
                timed_out = 1;
                if (g_log) fprintf(g_log, "%.6f # query timeout\n", now_s() - g_t0);
                kill(pid, SIGKILL);
                break;
            }
            continue;
        }
        if (idx_in >= 0 && (pfd[idx_in].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(0, buf, sizeof buf);
            if (n <= 0) {
                in_open = 0;
                close(cin);
            } else {
                double t = now_s();
                n_bytes_in += n;
                feed(&lb_in, buf, n, 0, t);
                if (write_all(cin, buf, n) < 0) { in_open = 0; close(cin); }
            }
        }
        if (pfd[idx_out].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(cout, buf, sizeof buf);
            if (n <= 0) {
                out_open = 0;
            } else {
                double t = now_s();
                n_bytes_out += n;
                feed(&lb_out, buf, n, 1, t);
                if (write_all(1, buf, n) < 0) { /* Verilator went away */ }
            }
        }
    }
    if (in_open) close(cin);
    close(cout);
    int status = 0;
    waitpid(pid, &status, 0);
    struct rusage ru;
    getrusage(RUSAGE_CHILDREN, &ru);
    double wall = now_s() - g_t0;
    if (g_log) {
        fprintf(g_log, "%.6f # exit status=%d\n", wall, status);
        fclose(g_log);
    }

    if (json_path) {
        FILE *f = fopen(json_path, "a");
        if (f) {
            qsort(g_lat, g_nlat, sizeof(double), dcmp);
            double sum = 0, mx = 0;
            for (size_t i = 0; i < g_nlat; i++) { sum += g_lat[i]; if (g_lat[i] > mx) mx = g_lat[i]; }
#define PCT(p) (g_nlat ? g_lat[(size_t)((p) * (g_nlat - 1))] : 0.0)
            fprintf(f, "{\"solver\":");
            json_escape(f, argv[1]);
            fprintf(f,
                    ",\"user_s\":%.6f,\"sys_s\":%.6f,\"maxrss_kb\":%ld,\"wall_s\":%.6f,"
                    "\"exit_status\":%d,\"timed_out\":%d,\"terminated\":%d,"
                    "\"check_sat\":%llu,\"check_sat_assuming\":%llu,\"get_value\":%llu,"
                    "\"reset\":%llu,\"push\":%llu,\"get_unsat_assumptions\":%llu,"
                    "\"get_unsat_core\":%llu,\"sat\":%llu,\"unsat\":%llu,\"unknown\":%llu,"
                    "\"error\":%llu,\"bytes_in\":%llu,\"bytes_out\":%llu,"
                    "\"q_n\":%zu,\"q_sum_s\":%.6f,\"q_p50_s\":%.6f,\"q_p90_s\":%.6f,"
                    "\"q_p99_s\":%.6f,\"q_max_s\":%.6f}\n",
                    ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * 1e-6,
                    ru.ru_stime.tv_sec + ru.ru_stime.tv_usec * 1e-6, ru.ru_maxrss, wall,
                    status, timed_out, terminated, (unsigned long long)n_check_sat,
                    (unsigned long long)n_check_sat_assuming, (unsigned long long)n_get_value,
                    (unsigned long long)n_reset, (unsigned long long)n_push,
                    (unsigned long long)n_get_unsat_assumptions,
                    (unsigned long long)n_get_unsat_core, (unsigned long long)n_sat,
                    (unsigned long long)n_unsat, (unsigned long long)n_unknown,
                    (unsigned long long)n_error, (unsigned long long)n_bytes_in,
                    (unsigned long long)n_bytes_out, g_nlat, sum, PCT(0.5), PCT(0.9),
                    PCT(0.99), mx);
            fclose(f);
        }
    }
    if (running[0]) unlink(running);
    if (timed_out) return 124;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
