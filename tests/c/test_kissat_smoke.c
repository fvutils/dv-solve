/* Phase B.0 smoke test: verifies kissat is linked and a trivial
 * (a OR b) AND (NOT a) AND (NOT b) instance returns UNSAT (20),
 * while (a OR b) AND (NOT a) returns SAT (10). */
#include <stdio.h>
#include <stdlib.h>

#include "kissat.h"

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

    /* UNSAT: (a v b) & ~a & ~b */
    {
        kissat *s = kissat_init();
        kissat_add(s, 1);  kissat_add(s, 2);  kissat_add(s, 0);
        kissat_add(s, -1); kissat_add(s, 0);
        kissat_add(s, -2); kissat_add(s, 0);
        failures += expect("unsat_trivial", kissat_solve(s), 20);
        kissat_release(s);
    }

    /* SAT: (a v b) & ~a — must set b=true */
    {
        kissat *s = kissat_init();
        kissat_add(s, 1);  kissat_add(s, 2);  kissat_add(s, 0);
        kissat_add(s, -1); kissat_add(s, 0);
        int rc = kissat_solve(s);
        failures += expect("sat_trivial", rc, 10);
        if (rc == 10) {
            int va = kissat_value(s, 1);
            int vb = kissat_value(s, 2);
            printf("       model: a=%d b=%d\n", va, vb);
            if (va > 0) { fprintf(stderr, "FAIL sat_trivial: a should be false\n"); failures++; }
            if (vb < 0) { fprintf(stderr, "FAIL sat_trivial: b should be true\n"); failures++; }
        }
        kissat_release(s);
    }

    return failures ? 1 : 0;
}
