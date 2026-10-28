/*
 * dvs_build_info() -- how this copy of the library was built.
 *
 * Benchmark records carry it, so a result can be traced to the commit and
 * build that produced it. Optimisation and sanitizers come from the
 * compiler's predefined macros, so they describe how this file -- compiled
 * with the rest of the library's flags -- was really built, whatever the
 * build type says.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* dladdr */
#endif
#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include "dv_solve.h"
#include "dvs_git_info.h"   /* generated at build time: DVS_BI_COMMIT, DVS_BI_DIRTY */

#ifndef DVS_BI_BUILD_TYPE
#define DVS_BI_BUILD_TYPE ""
#endif

#if defined(__SANITIZE_ADDRESS__)
#define DVS_BI_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DVS_BI_ASAN 1
#endif
#endif
#ifndef DVS_BI_ASAN
#define DVS_BI_ASAN 0
#endif

#if defined(__OPTIMIZE__)
#define DVS_BI_OPT 1
#else
#define DVS_BI_OPT 0
#endif

#if defined(NDEBUG)
#define DVS_BI_NDEBUG 1
#else
#define DVS_BI_NDEBUG 0
#endif

#define DVS_BI_STR2(x) #x
#define DVS_BI_STR(x) DVS_BI_STR2(x)

#if defined(__clang__)
#define DVS_BI_CC "clang " __clang_version__
#elif defined(__GNUC__)
#define DVS_BI_CC "gcc " __VERSION__
#elif defined(_MSC_VER)
#define DVS_BI_CC "msvc " DVS_BI_STR(_MSC_VER)
#else
#define DVS_BI_CC "unknown"
#endif

const char *dvs_build_info(void) {
    return "commit=" DVS_BI_COMMIT
           " dirty=" DVS_BI_STR(DVS_BI_DIRTY)
           " build_type=" DVS_BI_BUILD_TYPE
           " optimized=" DVS_BI_STR(DVS_BI_OPT)
           " asan=" DVS_BI_STR(DVS_BI_ASAN)
           " ndebug=" DVS_BI_STR(DVS_BI_NDEBUG)
           " cc=\"" DVS_BI_CC "\"";
}

const char *dvs_build_library_path(void) {
#if !defined(_WIN32)
    Dl_info info;
    if (dladdr((const void *)&dvs_build_info, &info) && info.dli_fname)
        return info.dli_fname;
#endif
    return "unknown";
}
