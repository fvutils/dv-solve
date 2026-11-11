/*
 * dvs_api.h -- DVS_API, the mark on every public dv-solve function.
 *
 * A shared dv-solve library exports the DVS_API functions (on Windows, define
 * DVS_BUILDING while building the DLL). The static libraries -- the writer
 * and libdv_solve.a -- are compiled, and must be used, with DVS_STATIC
 * defined: their DVS_API functions are the only global symbols they keep,
 * and are hidden, so a shared object they are linked into exports none.
 */
#ifndef DVS_API_H
#define DVS_API_H

#if defined(DVS_STATIC)
#  define DVS_API
#elif defined(_WIN32)
#  if defined(DVS_BUILDING)
#    define DVS_API __declspec(dllexport)
#  else
#    define DVS_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__)
#  define DVS_API __attribute__((visibility("default")))
#else
#  define DVS_API
#endif

#endif /* DVS_API_H */
