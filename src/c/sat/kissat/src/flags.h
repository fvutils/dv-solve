#ifndef _flags_h_INCLUDED
#define _flags_h_INCLUDED

#include "bitfield.h"
#include <stdbool.h>

typedef struct flags flags;

struct flags {
  KISSAT_BOOL_BITFIELD active : 1;
  KISSAT_BOOL_BITFIELD backbone0 : 1;
  KISSAT_BOOL_BITFIELD backbone1 : 1;
  KISSAT_BOOL_BITFIELD eliminate : 1;
  KISSAT_BOOL_BITFIELD eliminated : 1;
  unsigned factor : 2;
  KISSAT_BOOL_BITFIELD fixed : 1;
  KISSAT_BOOL_BITFIELD subsume : 1;
  KISSAT_BOOL_BITFIELD sweep : 1;
  KISSAT_BOOL_BITFIELD transitive : 1;
};

#define FLAGS(IDX) (assert ((IDX) < VARS), (solver->flags + (IDX)))

#define ACTIVE(IDX) (FLAGS (IDX)->active)
#define ELIMINATED(IDX) (FLAGS (IDX)->eliminated)

struct kissat;

void kissat_activate_literal (struct kissat *, unsigned);
void kissat_activate_literals (struct kissat *, unsigned, unsigned *);

void kissat_mark_eliminated_variable (struct kissat *, unsigned idx);
void kissat_mark_fixed_literal (struct kissat *, unsigned lit);

void kissat_mark_added_literals (struct kissat *, unsigned, unsigned *);
void kissat_mark_removed_literals (struct kissat *, unsigned, unsigned *);

#endif
