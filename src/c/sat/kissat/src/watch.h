#ifndef _watch_h_INCLUDED
#define _watch_h_INCLUDED

#include "keatures.h"
#include "reference.h"
#include "stack.h"
#include "vector.h"

#include <stdbool.h>

typedef union watch watch;

typedef struct binary_tagged_literal watch_type;
typedef struct binary_tagged_literal binary_watch;
typedef struct binary_tagged_literal blocking_watch;
typedef struct binary_tagged_reference large_watch;

/* dv-solve fork: MSVC starts a new bitfield storage unit whenever the
 * declared type changes, so 'unsigned lit : 31; bool binary : 1' is 8 bytes
 * there (lit in bytes 0-3, binary in byte 4) instead of GCC/Clang's one
 * 32-bit word. Watches live in 'unsigned' vectors and are read through
 * 'union watch' below, so every watch then lost its binary flag and read
 * half of the next slot -- an access violation in propagation on Windows.
 * Declaring the flag 'unsigned' keeps one storage unit under MSVC; other
 * compilers keep 'bool', and their layout is unchanged. */
#ifdef _MSC_VER
#define KISSAT_WATCH_FLAG unsigned
#else
#define KISSAT_WATCH_FLAG bool
#endif

struct binary_tagged_literal {
#ifdef KISSAT_IS_BIG_ENDIAN
  KISSAT_WATCH_FLAG binary : 1;
  unsigned lit : 31;
#else
  unsigned lit : 31;
  KISSAT_WATCH_FLAG binary : 1;
#endif
};

struct binary_tagged_reference {
#ifdef KISSAT_IS_BIG_ENDIAN
  KISSAT_WATCH_FLAG binary : 1;
  unsigned ref : 31;
#else
  unsigned ref : 31;
  KISSAT_WATCH_FLAG binary : 1;
#endif
};

union watch {
  watch_type type;
  binary_watch binary;
  blocking_watch blocking;
  large_watch large;
  unsigned raw;
};

/* dv-solve fork: kissat asserts this (substitute.c, watch.h) but only with
 * assertions on, and release builds define NDEBUG. Check it at compile time
 * so a compiler with another bitfield layout fails the build instead. */
typedef char kissat_watch_is_one_word[sizeof (watch) == sizeof (unsigned) ? 1 : -1];

typedef vector watches;

typedef struct litwatch litwatch;
typedef struct litpair litpair;
typedef struct litriple litriple;

typedef STACK (litwatch) litwatches;
typedef STACK (litpair) litpairs;
typedef STACK (litriple) litriples;

struct litwatch {
  unsigned lit;
  watch watch;
};

struct litpair {
  unsigned lits[2];
};

struct litriple {
  unsigned lits[3];
};

static inline litpair kissat_litpair (unsigned lit, unsigned other) {
  litpair res;
  res.lits[0] = lit < other ? lit : other;
  res.lits[1] = lit < other ? other : lit;
  return res;
}

static inline watch kissat_binary_watch (unsigned lit) {
  watch res;
  res.binary.lit = lit;
  res.binary.binary = true;
  assert (res.type.binary);
  return res;
}

static inline watch kissat_large_watch (reference ref) {
  watch res;
  res.large.ref = ref;
  res.large.binary = false;
  assert (!res.type.binary);
  return res;
}

static inline watch kissat_blocking_watch (unsigned lit) {
  watch res;
  res.blocking.lit = lit;
  res.blocking.binary = false;
  assert (!res.type.binary);
  return res;
}

#define EMPTY_WATCHES(W) kissat_empty_vector (&W)
#define SIZE_WATCHES(W) kissat_size_vector (&W)

#define PUSH_WATCHES(W, E) \
  do { \
    assert (sizeof (E) == sizeof (unsigned)); \
    kissat_push_vectors (solver, &(W), (E).raw); \
  } while (0)

#define LAST_WATCH_POINTER(WS) \
  (watch *) kissat_last_vector_pointer (solver, &WS)

#define BEGIN_WATCHES(WS) \
  ((union watch *) kissat_begin_vector (solver, &(WS)))

#define END_WATCHES(WS) ((union watch *) kissat_end_vector (solver, &(WS)))

#define BEGIN_CONST_WATCHES(WS) \
  ((union watch *) kissat_begin_const_vector (solver, &(WS)))

#define END_CONST_WATCHES(WS) \
  ((union watch *) kissat_end_const_vector (solver, &(WS)))

#define RELEASE_WATCHES(WS) kissat_release_vector (solver, &(WS))

#define SET_END_OF_WATCHES(WS, P) \
  do { \
    size_t SIZE = (unsigned *) (P) - kissat_begin_vector (solver, &WS); \
    kissat_resize_vector (solver, &WS, SIZE); \
  } while (0)

#define REMOVE_WATCHES(W, E) \
  kissat_remove_from_vector (solver, &(W), (E).raw)

#define WATCHES(LIT) (solver->watches[assert ((LIT) < LITS), (LIT)])

// This iterator is currently only used in 'testreferences.c'.
//
#define all_binary_blocking_watch_ref(WATCH, REF, WATCHES) \
  watch WATCH, \
      *WATCH##_PTR = (assert (solver->watching), BEGIN_WATCHES (WATCHES)), \
      *const WATCH##_END = END_WATCHES (WATCHES); \
  WATCH##_PTR != WATCH##_END && \
      ((WATCH = *WATCH##_PTR), \
       (REF = WATCH.type.binary ? INVALID_REF : WATCH##_PTR[1].large.ref), \
       true); \
  WATCH##_PTR += 1u + !WATCH.type.binary

#define all_binary_blocking_watches(WATCH, WATCHES) \
  watch WATCH, \
      *WATCH##_PTR = (assert (solver->watching), BEGIN_WATCHES (WATCHES)), \
      *const WATCH##_END = END_WATCHES (WATCHES); \
  WATCH##_PTR != WATCH##_END && ((WATCH = *WATCH##_PTR), true); \
  WATCH##_PTR += 1u + !WATCH.type.binary

#define all_binary_large_watches(WATCH, WATCHES) \
  watch WATCH, \
      *WATCH##_PTR = \
          (assert (!solver->watching), BEGIN_WATCHES (WATCHES)), \
      *const WATCH##_END = END_WATCHES (WATCHES); \
  WATCH##_PTR != WATCH##_END && ((WATCH = *WATCH##_PTR), true); \
  ++WATCH##_PTR

void kissat_remove_binary_watch (struct kissat *, watches *, unsigned);
void kissat_remove_blocking_watch (struct kissat *, watches *, reference);

void kissat_substitute_large_watch (struct kissat *, watches *, watch src,
                                    watch dst);

void kissat_flush_all_connected (struct kissat *);
void kissat_flush_large_watches (struct kissat *);
void kissat_watch_large_clauses (struct kissat *);
void kissat_flush_large_connected (struct kissat *);

void kissat_connect_irredundant_large_clauses (struct kissat *);

#endif
