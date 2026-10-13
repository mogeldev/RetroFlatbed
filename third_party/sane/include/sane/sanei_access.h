/* Device access locking is not needed: WinUSB already gives exclusive access. */
#ifndef LIDE20_SANEI_ACCESS_H
#define LIDE20_SANEI_ACCESS_H

#include "sane.h"

static __inline SANE_Status sanei_access_lock(SANE_String_Const dev, SANE_Word delay)
{ (void)dev; (void)delay; return SANE_STATUS_GOOD; }

static __inline SANE_Status sanei_access_unlock(SANE_String_Const dev)
{ (void)dev; return SANE_STATUS_GOOD; }

#endif
