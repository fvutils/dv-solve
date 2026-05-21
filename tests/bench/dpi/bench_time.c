#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns wall-clock time in nanoseconds (monotonic). */
long long bench_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

#ifdef __cplusplus
}
#endif
