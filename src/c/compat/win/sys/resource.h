/* Minimal <sys/resource.h> for MSVC: just enough of getrusage() for kissat's
 * timing/peak-memory statistics (resources.c, kitten.c). ru_maxrss is reported
 * in kilobytes, matching the Linux convention the callers assume.
 */
#ifndef ZSP_WIN_SYS_RESOURCE_H
#define ZSP_WIN_SYS_RESOURCE_H
#ifdef _MSC_VER

#include <sys/time.h>   /* struct timeval */

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)

struct rusage {
    struct timeval ru_utime;   /* user CPU time */
    struct timeval ru_stime;   /* system CPU time */
    long           ru_maxrss;  /* peak resident set size, in KB */
};

int getrusage(int who, struct rusage *usage);

#endif /* _MSC_VER */
#endif /* ZSP_WIN_SYS_RESOURCE_H */
