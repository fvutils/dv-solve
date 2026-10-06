#ifndef _assign_h_INCLUDED
#define _assign_h_INCLUDED

#include "bitfield.h"
#include <stdbool.h>

#define DECISION_REASON UINT_MAX
#define UNIT_REASON (DECISION_REASON - 1)

#define INVALID_LEVEL UINT_MAX
#define INVALID_TRAIL UINT_MAX

typedef struct assigned assigned;
struct clause;

struct assigned {
  unsigned level;
  unsigned trail;

  KISSAT_BOOL_BITFIELD analyzed : 1;
  KISSAT_BOOL_BITFIELD binary : 1;
  KISSAT_BOOL_BITFIELD poisoned : 1;
  KISSAT_BOOL_BITFIELD removable : 1;
  KISSAT_BOOL_BITFIELD shrinkable : 1;

  unsigned reason;
};

#define ASSIGNED(LIT) \
  (assert (VALID_INTERNAL_LITERAL (LIT)), solver->assigned + IDX (LIT))

#define LEVEL(LIT) (ASSIGNED (LIT)->level)
#define TRAIL(LIT) (ASSIGNED (LIT)->trail)
#define REASON(LIT) (ASSIGNED (LIT)->reason)

#ifndef FAST_ASSIGN

#include "reference.h"

struct kissat;
struct clause;

void kissat_assign_unit (struct kissat *, unsigned lit, const char *);
void kissat_learned_unit (struct kissat *, unsigned lit);
void kissat_original_unit (struct kissat *, unsigned lit);

void kissat_assign_decision (struct kissat *, unsigned lit);

void kissat_assign_binary (struct kissat *, unsigned, unsigned);

void kissat_assign_reference (struct kissat *, unsigned lit, reference,
                              struct clause *);

#endif

#endif
