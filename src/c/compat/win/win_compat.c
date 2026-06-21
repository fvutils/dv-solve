/* Win32 implementations of the POSIX functions the kissat sources need:
 * gettimeofday(), getrusage() and sysconf(). Compiled into the kissat static
 * library only on MSVC builds (see cmake/kissat.cmake).
 */
#ifdef _MSC_VER

#define WIN32_LEAN_AND_MEAN   /* keep <winsock.h> (and its struct timeval) out */
#include <windows.h>
#include <psapi.h>

#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>

/* 100-ns ticks since 1601-01-01 -> microseconds since 1970-01-01. */
#define ZSP_EPOCH_DELTA_US 11644473600000000ULL

static void zsp__filetime_to_tv(FILETIME ft, struct timeval *tv) {
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    unsigned __int64 us = li.QuadPart / 10ULL;   /* 100ns -> us */
    tv->tv_sec = (long)(us / 1000000ULL);
    tv->tv_usec = (long)(us % 1000000ULL);
}

int gettimeofday(struct timeval *tp, void *tzp) {
    (void)tzp;
    FILETIME ft;
    ULARGE_INTEGER li;
    GetSystemTimePreciseAsFileTime(&ft);
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    unsigned __int64 us = li.QuadPart / 10ULL - ZSP_EPOCH_DELTA_US;
    tp->tv_sec = (long)(us / 1000000ULL);
    tp->tv_usec = (long)(us % 1000000ULL);
    return 0;
}

int getrusage(int who, struct rusage *usage) {
    (void)who;
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
        return -1;
    zsp__filetime_to_tv(user, &usage->ru_utime);
    zsp__filetime_to_tv(kernel, &usage->ru_stime);

    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        usage->ru_maxrss = (long)(pmc.PeakWorkingSetSize / 1024);  /* KB */
    else
        usage->ru_maxrss = 0;
    return 0;
}

long sysconf(int name) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (name) {
    case _SC_PAGESIZE:         return (long)si.dwPageSize;
    case _SC_NPROCESSORS_ONLN: return (long)si.dwNumberOfProcessors;
    default:                   return -1;
    }
}

int clock_gettime(int clk_id, struct timespec *ts) {
    if (clk_id == CLOCK_MONOTONIC) {
        LARGE_INTEGER freq, ctr;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&ctr);
        ts->tv_sec = (time_t)(ctr.QuadPart / freq.QuadPart);
        long long rem = ctr.QuadPart % freq.QuadPart;
        ts->tv_nsec = (long)((rem * 1000000000LL) / freq.QuadPart);
    } else {  /* CLOCK_REALTIME */
        FILETIME ft;
        ULARGE_INTEGER li;
        GetSystemTimePreciseAsFileTime(&ft);
        li.LowPart = ft.dwLowDateTime;
        li.HighPart = ft.dwHighDateTime;
        unsigned __int64 t = li.QuadPart - 116444736000000000ULL;  /* 1601->1970, 100ns */
        ts->tv_sec = (time_t)(t / 10000000ULL);
        ts->tv_nsec = (long)((t % 10000000ULL) * 100);
    }
    return 0;
}

#endif /* _MSC_VER */
