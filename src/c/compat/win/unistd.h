/* Minimal <unistd.h> for MSVC: maps the handful of POSIX I/O/process calls the
 * kissat sources use (file.c, colors.c, kitten.c) onto their MSVC equivalents.
 * On this include path only for MSVC builds (see cmake/kissat.cmake).
 */
#ifndef ZSP_WIN_UNISTD_H
#define ZSP_WIN_UNISTD_H
#ifdef _MSC_VER

#include <io.h>
#include <process.h>
#include <stdio.h>

/* access() mode bits (POSIX values; X_OK is meaningless on Windows). */
#ifndef R_OK
#define R_OK 4
#define W_OK 2
#define X_OK 4
#define F_OK 0
#endif

#define access _access
#define isatty _isatty
#define unlink _unlink
#define getpid _getpid
#define popen  _popen
#define pclose _pclose
#ifndef fileno
#define fileno _fileno
#endif

typedef long long ssize_t;   /* matches SSIZE_T on x64 */

/* sysconf() names we answer (see win_compat.c). */
#define _SC_PAGESIZE         1
#define _SC_NPROCESSORS_ONLN 2
long sysconf(int name);

#endif /* _MSC_VER */
#endif /* ZSP_WIN_UNISTD_H */
