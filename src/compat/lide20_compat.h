/* Force-included (/FI) into every translation unit of the SANE plustek
 * hardware layer so the POSIX-flavoured code builds with MSVC. */
#ifndef LIDE20_COMPAT_H
#define LIDE20_COMPAT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;
typedef SSIZE_T        ssize_t;

#define __attribute__(x)
#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

struct lide20_timeval { long tv_sec; long tv_usec; };

static __inline void lide20_usleep(unsigned long us)
{
    if (us >= 1000) {
        Sleep((DWORD)((us + 999) / 1000));
    } else {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        do { QueryPerformanceCounter(&b); }
        while ((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart < (LONGLONG)us);
    }
}
static __inline unsigned lide20_sleep(unsigned s) { Sleep(s * 1000); return 0; }

#define usleep(us) lide20_usleep((unsigned long)(us))
#define sleep(s)   lide20_sleep((unsigned)(s))

#endif
