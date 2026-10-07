#ifndef _keatures_h_INCLUDED
#define _keatures_h_INCLUDED

/* dv-solve fork: both macros must exist. MSVC defines neither, and in #if an
   undefined name is 0, so the bare comparison read '0 == 0' and declared
   x64 Windows big-endian. That swapped the watch bitfield order (watch.h):
   the binary flag landed in bit 0, and every large watch's 'raw' read back
   twice its clause reference, past the end of the arena. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define KISSAT_IS_BIG_ENDIAN
#endif

#if defined(_POSIX_C_SOURCE) || defined(__APPLE__)
#define KISSAT_HAS_COMPRESSION
#define KISSAT_HAS_COLORS
#define KISSAT_HAS_FILENO
#endif

#if defined(_POSIX_C_SOURCE)
#define KISSAT_HAS_UNLOCKEDIO
#endif

#endif
