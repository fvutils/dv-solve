/* In-process randomize() benchmark.
 *
 * Models the cost of dv-solve embedded DIRECTLY in Verilator (no SMT-LIB text,
 * no pipe/IPC, no per-randomize parse/build/reset). It parses ONE solve problem
 * once (the same constraints the end-to-end sweep drives over the pipe), solves
 * it once, then loops the only work an embedded integration pays per randomize():
 *   zsp_bbsolver_rediversify(seed)  +  read back every variable's value.
 *
 * Compare the reported ms/randomize against the SMT-LIB-over-pipe numbers to see
 * the ceiling an in-process (DPI) integration would reach on the same problem.
 *
 * Usage: bench_inproc <problem.smt2> [iters]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "smt2/smt2_frontend.h"
#include "smt2/smt2_lexer.h"
#include "smt2/smt2_parser.h"
#include "zsp_bbsolver.h"
#include "zsp_problem.h"

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <problem.smt2> [iters]\n", argv[0]); return 2; }
    const char *path = argv[1];
    long iters = (argc > 2) ? atol(argv[2]) : 200000;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read error\n"); return 2; }
    buf[sz] = 0; fclose(f);

    /* Parse + solve the single problem once, exactly as the real frontend does. */
    FILE *devnull = fopen("/dev/null", "w");
    Smt2Frontend fe;
    smt2_frontend_init(&fe, devnull ? devnull : stdout, stderr);
    fe.verilator_mode = 1;

    Smt2Lexer lex; smt2_lexer_init(&lex, buf, (size_t)sz);
    SexprArena arena; sexpr_arena_init(&arena, 8192);
    for (;;) {
        sexpr_arena_reset(&arena);
        Sexpr *cmd = sexpr_parse(&lex, &arena);
        if (!cmd) break;
        smt2_frontend_dispatch(&fe, cmd);
    }

    if (!fe.bb_solver) {
        fprintf(stderr, "no bb_solver after parse (problem did not solve on the bit-blast engine)\n");
        return 1;
    }
    uint32_t nvars = fe.problem ? fe.problem->n_vars : 0;

    /* Warm the model once, then time the pure in-process randomize() loop:
     * re-diversify (new seed) + read back every variable. */
    zsp_bbsolver_rediversify(fe.bb_solver, 1);
    volatile int64_t sink = 0;
    double t0 = now_ms();
    for (long i = 1; i <= iters; i++) {
        zsp_bbsolver_rediversify(fe.bb_solver, (uint64_t)i + 1);
        for (uint32_t v = 0; v < nvars; v++) {
            int64_t val = 0;
            zsp_bbsolver_value(fe.bb_solver, v, &val);
            sink += val;
        }
    }
    double t1 = now_ms();

    double per = (t1 - t0) / (double)iters;
    printf("in-process randomize: %.6f ms/randomize  (%ld iters, %u vars)  [sink=%lld]\n",
           per, iters, nvars, (long long)sink);

    smt2_frontend_destroy(&fe);
    if (devnull) fclose(devnull);
    free(buf);
    return 0;
}
