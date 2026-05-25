#ifndef _kissat_h_INCLUDED
#define _kissat_h_INCLUDED

typedef struct kissat kissat;

/* dv-solve fork: optional zsp_alloc_t routing for internal allocations.
 * Forward-declared here so kissat.h doesn't pull in dv-solve headers. */
struct zsp_alloc_s;

// Default (partial) IPASIR interface.

const char *kissat_signature (void);
kissat *kissat_init (void);

/* dv-solve fork: like kissat_init but routes ALL internal allocations
 * (including the kissat struct itself) through the given allocator.
 * Pass NULL for upstream-equivalent behavior. */
kissat *kissat_init_with_alloc (struct zsp_alloc_s *alloc);
void kissat_add (kissat *solver, int lit);
int kissat_solve (kissat *solver);
int kissat_value (kissat *solver, int lit);
void kissat_release (kissat *solver);

void kissat_set_terminate (kissat *solver, void *state,
                           int (*terminate) (void *state));

// Additional API functions.

void kissat_terminate (kissat *solver);
void kissat_reserve (kissat *solver, int max_var);

const char *kissat_id (void);
const char *kissat_version (void);
const char *kissat_compiler (void);

const char **kissat_copyright (void);
void kissat_build (const char *line_prefix);
void kissat_banner (const char *line_prefix, const char *name_of_app);

int kissat_get_option (kissat *solver, const char *name);
int kissat_set_option (kissat *solver, const char *name, int new_value);

void kissat_set_prefix (kissat *solver, const char *prefix);

int kissat_has_configuration (const char *name);
int kissat_set_configuration (kissat *solver, const char *name);

void kissat_set_conflict_limit (kissat *solver, unsigned);
void kissat_set_decision_limit (kissat *solver, unsigned);

void kissat_print_statistics (kissat *solver);

/* dv-solve fork: clause-arena observation. Lets external machinery
 * (e.g., dv-solve's checkpoint/LevelMark system) measure SAT-layer
 * state without poking at kissat internals. Cheap getters; no side
 * effects. Useful for telemetry now; required substrate for step 5
 * (LevelMark integration) later. */
#include <stddef.h>
size_t kissat_arena_size_bytes (kissat *solver);
size_t kissat_arena_capacity_bytes (kissat *solver);

#endif
