/* DPI-C helper: returns wall-clock nanoseconds via POSIX clock_gettime(). */
#include <time.h>
#include "svdpi.h"

#ifdef __cplusplus
extern "C" {
#endif

long long sv_bench_wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#ifdef __cplusplus
}
#endif
