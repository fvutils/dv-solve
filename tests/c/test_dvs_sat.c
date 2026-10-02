/* Tests for the dvs_sat shim. Mirror the kissat smoke test but exercise the
 * convenience wrappers and accounting. */
#include <stdio.h>
#include <stdlib.h>

#include "dvs_sat.h"

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
        dvs_sat_t *s = dvs_sat_new(NULL);
        if (!s) return 99;
        dvs_sat_reserve(s, 2);
        dvs_sat_add_binary(s, 1, 2);
        dvs_sat_add_unit(s, -1);
        dvs_sat_add_unit(s, -2);
        failures += expect("unsat", dvs_sat_solve(s), DVS_SAT_UNSAT);
        failures += expect("num_clauses", (int)dvs_sat_num_clauses(s), 3);
        failures += expect("max_var", (int)dvs_sat_max_var(s), 2);
        dvs_sat_free(s);
    }

    /* SAT: (a v b) & ~a — b must be true */
    {
        dvs_sat_t *s = dvs_sat_new(NULL);
        if (!s) return 99;
        dvs_sat_add(s, 1); dvs_sat_add(s, 2); dvs_sat_add(s, 0);
        dvs_sat_add(s, -1); dvs_sat_add(s, 0);
        int rc = dvs_sat_solve(s);
        failures += expect("sat", rc, DVS_SAT_SAT);
        if (rc == DVS_SAT_SAT) {
            if (dvs_sat_value(s, 1) > 0) { fprintf(stderr, "FAIL: a should be false\n"); failures++; }
            if (dvs_sat_value(s, 2) < 0) { fprintf(stderr, "FAIL: b should be true\n"); failures++; }
        }
        dvs_sat_free(s);
    }

    /* 3-SAT-ish: (a v b v c) & (~a v ~b) & (~a v ~c) & (~b v ~c) — exactly one true */
    {
        dvs_sat_t *s = dvs_sat_new(NULL);
        dvs_sat_add_ternary(s, 1, 2, 3);
        dvs_sat_add_binary(s, -1, -2);
        dvs_sat_add_binary(s, -1, -3);
        dvs_sat_add_binary(s, -2, -3);
        failures += expect("3sat_exactly_one", dvs_sat_solve(s), DVS_SAT_SAT);
        dvs_sat_free(s);
    }

    return failures ? 1 : 0;
}
