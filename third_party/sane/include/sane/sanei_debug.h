/* Replacement for SANE's sanei_debug.h: all DBG output goes to lide20_dbg(). */
#ifndef LIDE20_SANEI_DEBUG_H
#define LIDE20_SANEI_DEBUG_H

extern int lide20_debug_level;
void lide20_dbg(int level, const char *fmt, ...);

#define DBG        lide20_dbg
#define DBG_LEVEL  (lide20_debug_level)
#define DBG_INIT() do { } while (0)

#endif
