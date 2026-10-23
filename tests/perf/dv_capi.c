/* Time dv-solve's C API directly on one compiled problem: compile once, then
 * N x (reset, solve, get values), each draw with its own seed and
 * fair_pick=1, as a simulator's randomize() would call it.
 *
 *   dv_capi <problem.dvsp> <n_vars> <N> <seed> <models.out>
 *
 * Writes one "M v0 v1 ..." line per draw ("U"/"T" for unsat/timeout) to
 * models.out, and a JSON timing summary line to stdout. Only the solver
 * calls (reset+solve+get_values) are inside the timed region, measured on
 * the calling thread's CPU clock so other load on the box doesn't skew it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dv_solve.h"

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

/* splitmix64: cheap, well-mixed per-draw seed derivation from one run seed. */
static uint64_t splitmix(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "usage: %s problem n_vars N seed out\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { perror("read"); return 2; }
    fclose(f);
    uint32_t nv = (uint32_t)atoi(argv[2]);
    int n = atoi(argv[3]);
    uint64_t seed = strtoull(argv[4], NULL, 10);
    FILE *out = fopen(argv[5], "w");

    double t0 = now_ns();
    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    int rc = dvs_solver_compile(ctx, (dvs_problem_t *)buf);
    double t_compile = now_ns() - t0;
    if (rc != DVS_COMPILE_OK) {
        printf("{\"compile_rc\": %d, \"compile_us\": %.1f}\n", rc, t_compile / 1e3);
        return 0;
    }
    uint32_t *ids = malloc(nv * sizeof *ids);
    int64_t *vals = malloc(nv * sizeof *vals);
    double *ts = malloc(n * sizeof *ts);
    for (uint32_t i = 0; i < nv; i++) ids[i] = i;
    int ok = 0, unsat = 0, tmo = 0;
    double total = 0;
    for (int i = 0; i < n; i++) {
        dvs_solve_opts_t o;
        memset(&o, 0, sizeof o);
        o.seed = splitmix(&seed) | 1;
        o.fair_pick = 1;
        double a = now_ns();
        dvs_solver_reset(ctx);
        dvs_result_t r = dvs_solver_solve(ctx, &o);
        if (r == DVS_SOLVE_OK) dvs_solver_get_values(ctx, nv, ids, vals);
        double d = now_ns() - a;
        ts[i] = d;
        total += d;
        if (r == DVS_SOLVE_OK) {
            ok++;
            fputc('M', out);
            for (uint32_t k = 0; k < nv; k++) fprintf(out, " %lld", (long long)vals[k]);
            fputc('\n', out);
        } else if (r == DVS_SOLVE_UNSAT) {
            unsat++;
            fputs("U\n", out);
        } else {
            tmo++;
            fputs("T\n", out);
        }
    }
    fclose(out);
    qsort(ts, n, sizeof *ts, cmp_d);
    printf("{\"compile_rc\": 0, \"compile_us\": %.1f, \"n\": %d, \"ok\": %d, \"unsat\": %d, "
           "\"timeout\": %d, \"mean_us\": %.3f, \"p50_us\": %.3f, \"p95_us\": %.3f, "
           "\"p99_us\": %.3f, \"max_us\": %.3f}\n",
           t_compile / 1e3, n, ok, unsat, tmo, total / n / 1e3, ts[n / 2] / 1e3,
           ts[(int)(n * .95)] / 1e3, ts[(int)(n * .99)] / 1e3, ts[n - 1] / 1e3);
    dvs_solver_free(ctx);
    return 0;
}
