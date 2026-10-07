/*
 * smt2_oracle -- check dv-solve-smt2's answers against a reference solver.
 *
 * Debug/test mode only, enabled by DV_ORACLE. The reference solver runs as a
 * child process and is kept in the same logical state as the frontend by
 * forwarding the raw text of every state-changing command. Each check-sat is
 * then checked: a `sat` by pinning every declared constant to the value
 * get-value would report and asking the oracle whether that is consistent; an
 * `unsat` or `unknown` by letting the oracle solve the query. Results are
 * recorded in a run directory (DV_ORACLE_OUT). The oracle never changes what
 * dv-solve answers. See docs/oracle_check_plan.md.
 */
#ifndef SMT2_ORACLE_H
#define SMT2_ORACLE_H

#include <stddef.h>
#include <stdio.h>
#include "smt2/smt2_frontend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Smt2Oracle Smt2Oracle;

/* Create the oracle from the DV_ORACLE* environment. Returns NULL when
 * DV_ORACLE is unset, or when setup fails (reported on `err`); dv-solve then
 * runs without it. argv is recorded in run.json. */
Smt2Oracle *smt2_oracle_from_env(int argc, char **argv, int verilator_mode,
                                 int hash_ignore, FILE *err);

/* 1 if `cmd` is a check-sat or check-sat-assuming: the caller captures the
 * answer it prints and passes it to smt2_oracle_after. */
int smt2_oracle_is_check(const Sexpr *cmd);

/* Called after the frontend has dispatched `cmd` (raw text `text`/`len`).
 * For a check command, `answer` is what dv-solve printed for it and `dvs_ms`
 * the time it took; both are ignored otherwise. */
void smt2_oracle_after(Smt2Oracle *o, Smt2Frontend *fe, const Sexpr *cmd,
                       const char *text, size_t len,
                       const char *answer, size_t answer_len, double dvs_ms);

/* Finalise run.json, stop the reference solver and free the oracle. */
void smt2_oracle_finish(Smt2Oracle *o);

#ifdef __cplusplus
}
#endif

#endif /* SMT2_ORACLE_H */
