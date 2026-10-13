#ifndef LIDE20_SYS_TIME_H
#define LIDE20_SYS_TIME_H

#include <winsock2.h>

struct itimerval { struct timeval it_interval; struct timeval it_value; };

static __inline int gettimeofday(struct timeval *tv, void *tz)
{
    FILETIME ft;
    unsigned __int64 t;
    (void)tz;
    GetSystemTimeAsFileTime(&ft);
    t = ((unsigned __int64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t = (t - 116444736000000000ULL) / 10;
    tv->tv_sec  = (long)(t / 1000000ULL);
    tv->tv_usec = (long)(t % 1000000ULL);
    return 0;
}

#endif
