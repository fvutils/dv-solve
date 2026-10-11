/*
 * The learnt-clause arena grows (docs/dynamic_pool_plan.md P4). It was a fixed
 * 4 MiB: once full, clause_db_add failed and learning silently stopped for the
 * rest of the context's life. Clauses and watch entries are held by pointer,
 * so the arena grows in segments that never move.
 */
#include "dvs_lcg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

enum { N_VARS = 64, N_LITS = 8, N_CLAUSES = 60000 };   /* ~16 MiB of arena */

static void make(Literal *l, uint32_t k) {
    memset(l, 0, N_LITS * sizeof *l);
    for (uint32_t i = 0; i < N_LITS; i++) {
        l[i].var_id = (k + i * 7) % N_VARS;
        l[i].is_lb  = (uint8_t)((k + i) & 1);
        l[i].bound  = (int64_t)k * 31 + i;
    }
}

static int intact(const ClauseDB *db, uint32_t idx, uint32_t k) {
    Literal want[N_LITS];
    make(want, k);
    const Clause *cl = db->clauses[idx];
    if (!cl || cl->n_lits != N_LITS) return 0;
    const Literal *got = (const Literal *)(cl + 1);
    for (uint32_t i = 0; i < N_LITS; i++)
        if (got[i].var_id != want[i].var_id || got[i].is_lb != want[i].is_lb ||
            got[i].bound != want[i].bound) return 0;
    return 1;
}

int main(void) {
    LCGCtx *lcg = (LCGCtx *)calloc(1, sizeof *lcg);
    CHECK(lcg_init(lcg, N_VARS) == 0);
    ClauseDB *db = &lcg->clause_db;
    Literal l[N_LITS];
    for (int round = 0; round < 2; round++) {
        for (uint32_t k = 0; k < N_CLAUSES; k++) {
            make(l, k);
            uint32_t idx = clause_db_add(db, N_LITS, l, 2);
            CHECK(idx == k);
            if (idx != k) break;
        }
        CHECK(dvs_spool_used(&db->arena) > (4u << 20));   /* past the old cap */
        for (uint32_t k = 0; k < N_CLAUSES; k += 997) CHECK(intact(db, k, k));
        lcg_reset(lcg);                                    /* back to empty */
        CHECK(lcg_is_pristine(lcg));
    }
    lcg_destroy(lcg);
    free(lcg);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("ok\n");
    return failures ? 1 : 0;
}
