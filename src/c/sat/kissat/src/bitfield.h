#ifndef _bitfield_h_INCLUDED
#define _bitfield_h_INCLUDED

/* dv-solve fork: the type of kissat's one-bit flag bitfields.

   MSVC starts a new bitfield storage unit whenever the declared type
   changes size, so a 'bool' flag next to 'unsigned' fields gets a unit of
   its own: the clause header ('unsigned glue : 19', eight 'bool' flags,
   'unsigned used : 5') becomes 12 bytes instead of one 32-bit word, and a
   watch ('unsigned lit : 31; bool binary : 1') 8 bytes instead of 4.
   kissat depends on both being one word -- watches live in 'unsigned'
   vectors, and the arena code walks clauses assuming a one-word header --
   and only checks it with assert, which NDEBUG compiles out. On Windows
   that meant access violations in propagation and heap corruption.

   Declaring the flags 'unsigned' under MSVC packs them as GCC and Clang
   pack 'bool'. Other compilers keep 'bool', so their layout is unchanged.
   The sizes kissat relies on are checked at compile time in clause.h and
   watch.h. */

#include <stdbool.h>

#ifdef _MSC_VER
#define KISSAT_BOOL_BITFIELD unsigned
#else
#define KISSAT_BOOL_BITFIELD bool
#endif

#endif
