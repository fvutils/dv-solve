/* Tests for the zsp_sat shim. Mirror the kissat smoke test but exercise the
 * convenience wrappers and accounting. */
#include <stdio.h>
#include <stdlib.h>

#include "zsp_sat.h"

static int expect(const char *label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d, want %d\n", label, got, want);
        return 1;
    }
    printf("PASS %s: %d\n", label, got);
    return 0;
}

int main(void) {
    int failures = 0;

    /* UNSAT: (a v b) & ~a & ~b via mixed APIs */
    {
        zsp_sat_t *s = zsp_sat_new(NULL);
        if (!s) return 99;
        zsp_sat_reserve(s, 2);
        zsp_sat_add_binary(s, 1, 2);
        zsp_sat_add_unit(s, -1);
        zsp_sat_add_unit(s, -2);
        failures += expect("unsat", zsp_sat_solve(s), ZSP_SAT_UNSAT);
        failures += expect("num_clauses", (int)zsp_sat_num_clauses(s), 3);
        failures += expect("max_var", (int)zsp_sat_max_var(s), 2);
        zsp_sat_free(s);
    }

    /* SAT: (a v b) & ~a — b must be true */
    {
        zsp_sat_t *s = zsp_sat_new(NULL);
        if (!s) return 99;
        zsp_sat_add(s, 1); zsp_sat_add(s, 2); zsp_sat_add(s, 0);
        zsp_sat_add(s, -1); zsp_sat_add(s, 0);
        int rc = zsp_sat_solve(s);
        failures += expect("sat", rc, ZSP_SAT_SAT);
        if (rc == ZSP_SAT_SAT) {
            if (zsp_sat_value(s, 1) > 0) { fprintf(stderr, "FAIL: a should be false\n"); failures++; }
            if (zsp_sat_value(s, 2) < 0) { fprintf(stderr, "FAIL: b should be true\n"); failures++; }
        }
        zsp_sat_free(s);
    }

    /* 3-SAT-ish: (a v b v c) & (~a v ~b) & (~a v ~c) & (~b v ~c) — exactly one true */
    {
        zsp_sat_t *s = zsp_sat_new(NULL);
        zsp_sat_add_ternary(s, 1, 2, 3);
        zsp_sat_add_binary(s, -1, -2);
        zsp_sat_add_binary(s, -1, -3);
        zsp_sat_add_binary(s, -2, -3);
        failures += expect("3sat_exactly_one", zsp_sat_solve(s), ZSP_SAT_SAT);
        zsp_sat_free(s);
    }

    return failures ? 1 : 0;
}
