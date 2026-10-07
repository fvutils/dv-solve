/* Minimal <sys/time.h> for MSVC: struct timeval + gettimeofday().
 * Defining _TIMEVAL_DEFINED prevents a later <winsock2.h> from redefining
 * struct timeval (win_compat.c builds with WIN32_LEAN_AND_MEAN to avoid it).
 */
#ifndef DVS_WIN_SYS_TIME_H
#define DVS_WIN_SYS_TIME_H
#ifdef _MSC_VER

#include <time.h>

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval {
    long tv_sec;
    long tv_usec;
};
#endif

#ifdef __cplusplus
extern "C"
#endif
int gettimeofday(struct timeval *tp, void *tzp);

#endif /* _MSC_VER */
#endif /* DVS_WIN_SYS_TIME_H */
