/* TWAIN 2.x data source for the Canon CanoScan LiDE 20 (user space, WinUSB).
 *
 * Install as  %WINDIR%\twain_64\RetroFlatbed\RetroFlatbed.ds (32-bit build: twain_32)  (64-bit TWAIN DSM).
 * Flatbed scanner, one image per acquisition, native (DIB) and memory
 * transfers, optional settings dialog.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stddef.h>

#include "twain.h"
#include "lide20_core.h"
#include "ds_res.h"

#define MM_PER_INCH     25.4
#define WM_DS_READY     (WM_APP + 1)
#define WM_DS_CLOSEREQ  (WM_APP + 2)
#define MAX_NATIVE_DPI_BYTES  (1500u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int    pixeltype;          /* TWPT_BW / TWPT_GRAY / TWPT_RGB            */
    int    depth;              /* bits per sample: 1, 8, 16                 */
    int    dpi;
    int    units;              /* TWUN_*                                    */
    double left, top, right, bottom;   /* inches                            */
    int    brightness, contrast;       /* -1000 .. 1000                     */
    double gamma;
    int    xfermech;           /* TWSX_NATIVE / TWSX_MEMORY                 */
    int    xfercount;
} settings_t;

static HINSTANCE        g_inst;
static CRITICAL_SECTION g_cs;
static int              g_cs_init;

static int              g_state = 3;                 /* TWAIN state 3..7 */
static TW_IDENTITY      g_app, g_ds;
static int              g_have_app;
static TW_UINT16        g_cc = TWCC_SUCCESS;
static TW_ENTRYPOINT    g_ep;
static int              g_have_ep;
static DSMENTRYPROC     g_dsm;

static lide20_t        *g_scanner;
static lide20_caps_t    g_caps;
static settings_t       g_cur, g_def;

static HWND             g_hwnd;
static HANDLE           g_thread;
static volatile LONG    g_lines_done;
static volatile LONG    g_scan_error;       /* 0 or a LIDE20_E_* code     */
static volatile LONG    g_scan_finished;
static lide20_image_t   g_img;
static lide20_params_t  g_params;
static unsigned char   *g_raw;              /* memory-transfer buffer     */
static size_t           g_raw_size;
static HGLOBAL          g_dib;              /* native-transfer image      */
static size_t           g_dib_hdr;          /* header + palette bytes     */
static size_t           g_dib_stride;
static int              g_img_valid;
static int              g_pending;          /* images still pending       */
static int              g_mem_rows_sent;
static int              g_ui_cancelled;

#define LINEART_ONE_IS_WHITE 0   /* hardware 1-bit data: 1 = black (verified on device) */

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static TW_UINT16 fail(TW_UINT16 cc) { g_cc = cc; return TWRC_FAILURE; }

static TW_FIX32 to_fix(double f)
{
    TW_FIX32 r;
    LONG v = (LONG)floor(f * 65536.0 + 0.5);
    r.Whole = (TW_INT16)(v >> 16);
    r.Frac  = (TW_UINT16)(v & 0xFFFF);
    return r;
}
static double from_fix(TW_FIX32 f) { return (double)f.Whole + (double)f.Frac / 65536.0; }
static TW_UINT32 fix_item(double f)
{
    TW_FIX32 x = to_fix(f);
    TW_UINT32 u = 0;
    memcpy(&u, &x, 4);
    return u;
}
static double fix_from_item(TW_UINT32 u)
{
    TW_FIX32 x;
    memcpy(&x, &u, 4);
    return from_fix(x);
}

static TW_HANDLE h_alloc(size_t n)
{
    if (g_have_ep && g_ep.DSM_MemAllocate)
        return g_ep.DSM_MemAllocate((TW_UINT32)n);
    return (TW_HANDLE)GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, n);
}
static TW_MEMREF h_lock(TW_HANDLE h)
{
    if (g_have_ep && g_ep.DSM_MemLock) return g_ep.DSM_MemLock(h);
    return GlobalLock((HGLOBAL)h);
}
static void h_unlock(TW_HANDLE h)
{
    if (g_have_ep && g_ep.DSM_MemUnlock) g_ep.DSM_MemUnlock(h);
    else GlobalUnlock((HGLOBAL)h);
}

static size_t item_size(TW_UINT16 t)
{
    switch (t) {
    case TWTY_INT8: case TWTY_UINT8: return 1;
    case TWTY_INT16: case TWTY_UINT16: case TWTY_BOOL: return 2;
    case TWTY_INT32: case TWTY_UINT32: case TWTY_FIX32: return 4;
    case TWTY_FRAME: return sizeof(TW_FRAME);
    }
    return 4;
}

static void put_item(unsigned char *dst, TW_UINT16 t, TW_UINT32 v)
{
    memcpy(dst, &v, item_size(t) <= 4 ? item_size(t) : 4);
}
static TW_UINT32 get_item(const unsigned char *src, TW_UINT16 t)
{
    TW_UINT32 v = 0;
    size_t n = item_size(t);
    if (n > 4) n = 4;
    memcpy(&v, src, n);
    if (t == TWTY_INT16) v = (TW_UINT32)(LONG)(short)v;
    return v;
}

static int pack_one(TW_CAPABILITY *c, TW_UINT16 type, TW_UINT32 v)
{
    TW_ONEVALUE *p;
    c->ConType = TWON_ONEVALUE;
    c->hContainer = h_alloc(sizeof(TW_ONEVALUE));
    if (!c->hContainer) return 0;
    p = (TW_ONEVALUE *)h_lock(c->hContainer);
    p->ItemType = type;
    p->Item = v;
    h_unlock(c->hContainer);
    return 1;
}

static int pack_enum(TW_CAPABILITY *c, TW_UINT16 type, const TW_UINT32 *vals, int n, int cur, int def)
{
    TW_ENUMERATION *p;
    size_t is = item_size(type);
    int i;
    c->ConType = TWON_ENUMERATION;
    c->hContainer = h_alloc(offsetof(TW_ENUMERATION, ItemList) + is * (size_t)n);
    if (!c->hContainer) return 0;
    p = (TW_ENUMERATION *)h_lock(c->hContainer);
    p->ItemType = type;
    p->NumItems = n;
    p->CurrentIndex = cur;
    p->DefaultIndex = def;
    for (i = 0; i < n; i++)
        put_item(p->ItemList + is * (size_t)i, type, vals[i]);
    h_unlock(c->hContainer);
    return 1;
}

static int pack_array(TW_CAPABILITY *c, TW_UINT16 type, const TW_UINT32 *vals, int n)
{
    TW_ARRAY *p;
    size_t is = item_size(type);
    int i;
    c->ConType = TWON_ARRAY;
    c->hContainer = h_alloc(offsetof(TW_ARRAY, ItemList) + is * (size_t)n);
    if (!c->hContainer) return 0;
    p = (TW_ARRAY *)h_lock(c->hContainer);
    p->ItemType = type;
    p->NumItems = n;
    for (i = 0; i < n; i++)
        put_item(p->ItemList + is * (size_t)i, type, vals[i]);
    h_unlock(c->hContainer);
    return 1;
}

static int pack_range(TW_CAPABILITY *c, TW_UINT16 type, TW_UINT32 mn, TW_UINT32 mx,
                      TW_UINT32 step, TW_UINT32 def, TW_UINT32 cur)
{
    TW_RANGE *p;
    c->ConType = TWON_RANGE;
    c->hContainer = h_alloc(sizeof(TW_RANGE));
    if (!c->hContainer) return 0;
    p = (TW_RANGE *)h_lock(c->hContainer);
    p->ItemType = type;
    p->MinValue = mn; p->MaxValue = mx; p->StepSize = step;
    p->DefaultValue = def; p->CurrentValue = cur;
    h_unlock(c->hContainer);
    return 1;
}

static int pack_frame_enum(TW_CAPABILITY *c, const TW_FRAME *fr)
{
    TW_ENUMERATION *p;
    c->ConType = TWON_ENUMERATION;
    c->hContainer = h_alloc(offsetof(TW_ENUMERATION, ItemList) + sizeof(TW_FRAME));
    if (!c->hContainer) return 0;
    p = (TW_ENUMERATION *)h_lock(c->hContainer);
    p->ItemType = TWTY_FRAME;
    p->NumItems = 1; p->CurrentIndex = 0; p->DefaultIndex = 0;
    memcpy(p->ItemList, fr, sizeof(TW_FRAME));
    h_unlock(c->hContainer);
    return 1;
}

/* Reads the single value out of a ONEVALUE / ENUMERATION / RANGE container.
 * Frames are returned through *frame. */
static int unpack_value(const TW_CAPABILITY *c, TW_UINT16 *type, TW_UINT32 *val, TW_FRAME *frame)
{
    unsigned char *base;
    int ok = 0;

    if (!c->hContainer) return 0;
    base = (unsigned char *)h_lock(c->hContainer);
    if (!base) return 0;

    switch (c->ConType) {
    case TWON_ONEVALUE: {
        TW_ONEVALUE *p = (TW_ONEVALUE *)base;
        *type = p->ItemType;
        if (p->ItemType == TWTY_FRAME) {
            if (frame) memcpy(frame, base + offsetof(TW_ONEVALUE, Item), sizeof(TW_FRAME));
        } else {
            *val = p->Item;
            if (p->ItemType == TWTY_INT16) *val = (TW_UINT32)(LONG)(short)p->Item;
        }
        ok = 1;
        break;
    }
    case TWON_ENUMERATION: {
        TW_ENUMERATION *p = (TW_ENUMERATION *)base;
        *type = p->ItemType;
        if (p->NumItems > 0 && p->CurrentIndex < p->NumItems) {
            const unsigned char *it = p->ItemList + item_size(p->ItemType) * p->CurrentIndex;
            if (p->ItemType == TWTY_FRAME) { if (frame) memcpy(frame, it, sizeof(TW_FRAME)); }
            else *val = get_item(it, p->ItemType);
            ok = 1;
        }
        break;
    }
    case TWON_RANGE: {
        TW_RANGE *p = (TW_RANGE *)base;
        *type = p->ItemType;
        *val = p->CurrentValue;
        ok = 1;
        break;
    }
    }
    h_unlock(c->hContainer);
    return ok;
}

/* ------------------------------------------------------------------ */
/* unit conversion                                                     */
/* ------------------------------------------------------------------ */

static double units_per_inch(int units)
{
    switch (units) {
    case TWUN_INCHES:      return 1.0;
    case TWUN_CENTIMETERS: return 2.54;
    case TWUN_MILLIMETERS: return 25.4;
    case TWUN_PICAS:       return 6.0;
    case TWUN_POINTS:      return 72.0;
    case TWUN_TWIPS:       return 1440.0;
    case TWUN_PIXELS:      return (double)g_cur.dpi;
    }
    return 1.0;
}
static double inch_to_units(double in) { return in * units_per_inch(g_cur.units); }
static double units_to_inch(double u)  { return u / units_per_inch(g_cur.units); }

static double max_w_in(void) { return g_caps.max_width_mm  / MM_PER_INCH; }
static double max_h_in(void) { return g_caps.max_height_mm / MM_PER_INCH; }

static void reset_settings(void)
{
    memset(&g_def, 0, sizeof(g_def));
    g_def.pixeltype = TWPT_RGB;
    g_def.depth     = 8;
    g_def.dpi       = 300;
    g_def.units     = TWUN_INCHES;
    g_def.left = g_def.top = 0;
    g_def.right  = max_w_in();
    g_def.bottom = max_h_in();
    g_def.gamma  = 1.0;
    g_def.xfermech  = TWSX_NATIVE;
    g_def.xfercount = -1;
    g_cur = g_def;
}

static void get_frame(TW_FRAME *f)
{
    f->Left   = to_fix(inch_to_units(g_cur.left));
    f->Top    = to_fix(inch_to_units(g_cur.top));
    f->Right  = to_fix(inch_to_units(g_cur.right));
    f->Bottom = to_fix(inch_to_units(g_cur.bottom));
}

static void default_frame(TW_FRAME *f)
{
    settings_t save = g_cur;
    g_cur.left = g_def.left; g_cur.top = g_def.top;
    g_cur.right = g_def.right; g_cur.bottom = g_def.bottom;
    get_frame(f);
    g_cur = save;
}

/* returns 1 if the frame had to be adjusted */
static int set_frame(const TW_FRAME *f)
{
    double l = units_to_inch(from_fix(f->Left)),  t = units_to_inch(from_fix(f->Top));
    double r = units_to_inch(from_fix(f->Right)), b = units_to_inch(from_fix(f->Bottom));
    int adj = 0;
    if (l < 0) { l = 0; adj = 1; }
    if (t < 0) { t = 0; adj = 1; }
    if (r > max_w_in()) { r = max_w_in(); adj = 1; }
    if (b > max_h_in()) { b = max_h_in(); adj = 1; }
    if (r - l < 0.05 || b - t < 0.05) return -1;
    g_cur.left = l; g_cur.top = t; g_cur.right = r; g_cur.bottom = b;
    return adj;
}

/* ------------------------------------------------------------------ */
/* capabilities                                                        */
/* ------------------------------------------------------------------ */

static const TW_UINT16 k_supported_caps[] = {
    CAP_XFERCOUNT, CAP_SUPPORTEDCAPS, CAP_DEVICEONLINE, CAP_UICONTROLLABLE,
    CAP_INDICATORS, CAP_FEEDERENABLED, CAP_ENABLEDSUIONLY,
    ICAP_COMPRESSION, ICAP_PIXELTYPE, ICAP_UNITS, ICAP_XFERMECH, ICAP_BITDEPTH,
    ICAP_XRESOLUTION, ICAP_YRESOLUTION, ICAP_XNATIVERESOLUTION, ICAP_YNATIVERESOLUTION,
    ICAP_PHYSICALWIDTH, ICAP_PHYSICALHEIGHT, ICAP_FRAMES, ICAP_PIXELFLAVOR,
    ICAP_BRIGHTNESS, ICAP_CONTRAST, ICAP_GAMMA, ICAP_ORIENTATION,
    ICAP_BITORDER, ICAP_PLANARCHUNKY
};
#define N_SUPPORTED_CAPS ((int)(sizeof(k_supported_caps) / sizeof(k_supported_caps[0])))

static const TW_UINT32 k_dpis[] = { 50, 75, 100, 150, 200, 300, 400, 600, 800, 1200 };
#define N_DPIS ((int)(sizeof(k_dpis) / sizeof(k_dpis[0])))
static const TW_UINT32 k_units[] = { TWUN_INCHES, TWUN_CENTIMETERS, TWUN_MILLIMETERS, TWUN_PIXELS };
static const TW_UINT32 k_ptypes[] = { TWPT_BW, TWPT_GRAY, TWPT_RGB };

static int idx_of(const TW_UINT32 *a, int n, TW_UINT32 v)
{
    int i;
    for (i = 0; i < n; i++) if (a[i] == v) return i;
    return 0;
}

static int bitdepth_list(int pixeltype, TW_UINT32 *out)
{
    if (pixeltype == TWPT_BW) { out[0] = 1; return 1; }
    if (pixeltype == TWPT_GRAY) { out[0] = 8; out[1] = 16; return 2; }
    out[0] = 24; out[1] = 48; return 2;      /* RGB: bits per pixel */
}
static TW_UINT32 bitdepth_value(const settings_t *s)
{
    if (s->pixeltype == TWPT_BW) return 1;
    if (s->pixeltype == TWPT_GRAY) return s->depth;
    return s->depth * 3;
}

static int max_dpi(void) { return g_caps.max_dpi > 0 ? g_caps.max_dpi : 1200; }

/* which: 0 = all (GET), 1 = current, 2 = default */
static int cap_build(TW_UINT16 cap, int which, const settings_t *s, TW_CAPABILITY *c)
{
    TW_UINT32 list[16];
    int n, cur, def;
    c->Cap = cap;
    c->hContainer = NULL;

#define ONE(type, v)  return pack_one(c, type, (TW_UINT32)(v))

    switch (cap) {
    case CAP_SUPPORTEDCAPS: {
        TW_UINT32 *all = (TW_UINT32 *)malloc(sizeof(TW_UINT32) * N_SUPPORTED_CAPS);
        int i, ok;
        if (!all) return 0;
        for (i = 0; i < N_SUPPORTED_CAPS; i++) all[i] = k_supported_caps[i];
        ok = pack_array(c, TWTY_UINT16, all, N_SUPPORTED_CAPS);
        free(all);
        return ok;
    }
    case CAP_XFERCOUNT:       ONE(TWTY_INT16, s->xfercount);
    case CAP_DEVICEONLINE:    ONE(TWTY_BOOL, TRUE);
    case CAP_UICONTROLLABLE:  ONE(TWTY_BOOL, TRUE);
    case CAP_INDICATORS:      ONE(TWTY_BOOL, FALSE);
    case CAP_FEEDERENABLED:   ONE(TWTY_BOOL, FALSE);
    case CAP_ENABLEDSUIONLY:  ONE(TWTY_BOOL, TRUE);

    case ICAP_COMPRESSION:    list[0] = TWCP_NONE;
        return which ? pack_one(c, TWTY_UINT16, TWCP_NONE) : pack_enum(c, TWTY_UINT16, list, 1, 0, 0);
    case ICAP_PIXELFLAVOR:    ONE(TWTY_UINT16, TWPF_CHOCOLATE);
    case ICAP_ORIENTATION:    ONE(TWTY_UINT16, TWOR_ROT0);
    case ICAP_BITORDER:       ONE(TWTY_UINT16, TWBO_MSBFIRST);
    case ICAP_PLANARCHUNKY:   ONE(TWTY_UINT16, TWPC_CHUNKY);

    case ICAP_PIXELTYPE:
        if (which == 1) ONE(TWTY_UINT16, s->pixeltype);
        if (which == 2) ONE(TWTY_UINT16, g_def.pixeltype);
        return pack_enum(c, TWTY_UINT16, k_ptypes, 3, idx_of(k_ptypes, 3, s->pixeltype),
                         idx_of(k_ptypes, 3, g_def.pixeltype));
    case ICAP_UNITS:
        if (which == 1) ONE(TWTY_UINT16, s->units);
        if (which == 2) ONE(TWTY_UINT16, g_def.units);
        return pack_enum(c, TWTY_UINT16, k_units, 4, idx_of(k_units, 4, s->units),
                         idx_of(k_units, 4, g_def.units));
    case ICAP_XFERMECH:
        list[0] = TWSX_NATIVE; list[1] = TWSX_MEMORY;
        if (which == 1) ONE(TWTY_UINT16, s->xfermech);
        if (which == 2) ONE(TWTY_UINT16, g_def.xfermech);
        return pack_enum(c, TWTY_UINT16, list, 2, s->xfermech == TWSX_MEMORY, 0);
    case ICAP_BITDEPTH:
        if (which == 1) ONE(TWTY_UINT16, bitdepth_value(s));
        if (which == 2) ONE(TWTY_UINT16, bitdepth_value(&g_def));
        n = bitdepth_list(s->pixeltype, list);
        cur = idx_of(list, n, bitdepth_value(s));
        return pack_enum(c, TWTY_UINT16, list, n, cur, 0);
    case ICAP_XRESOLUTION:
    case ICAP_YRESOLUTION: {
        TW_UINT32 d[16];
        int i, m = 0;
        for (i = 0; i < N_DPIS; i++)
            if ((int)k_dpis[i] <= max_dpi()) d[m++] = fix_item((double)k_dpis[i]);
        if (which == 1) ONE(TWTY_FIX32, fix_item(s->dpi));
        if (which == 2) ONE(TWTY_FIX32, fix_item(g_def.dpi));
        cur = 0; def = 0;
        for (i = 0; i < m; i++) {
            if ((int)fix_from_item(d[i]) == s->dpi) cur = i;
            if ((int)fix_from_item(d[i]) == g_def.dpi) def = i;
        }
        return pack_enum(c, TWTY_FIX32, d, m, cur, def);
    }
    case ICAP_XNATIVERESOLUTION:
    case ICAP_YNATIVERESOLUTION:
        ONE(TWTY_FIX32, fix_item(g_caps.optic_dpi ? g_caps.optic_dpi : 600));
    case ICAP_PHYSICALWIDTH:  ONE(TWTY_FIX32, fix_item(inch_to_units(max_w_in())));
    case ICAP_PHYSICALHEIGHT: ONE(TWTY_FIX32, fix_item(inch_to_units(max_h_in())));
    case ICAP_FRAMES: {
        TW_FRAME fr;
        if (which == 2) default_frame(&fr); else get_frame(&fr);
        return pack_frame_enum(c, &fr);
    }
    case ICAP_BRIGHTNESS:
    case ICAP_CONTRAST: {
        int v = (cap == ICAP_BRIGHTNESS) ? s->brightness : s->contrast;
        if (which == 1) ONE(TWTY_FIX32, fix_item(v));
        if (which == 2) ONE(TWTY_FIX32, fix_item(0));
        return pack_range(c, TWTY_FIX32, fix_item(-1000), fix_item(1000), fix_item(1),
                          fix_item(0), fix_item(v));
    }
    case ICAP_GAMMA:
        if (which == 1) ONE(TWTY_FIX32, fix_item(s->gamma));
        if (which == 2) ONE(TWTY_FIX32, fix_item(1.0));
        return pack_range(c, TWTY_FIX32, fix_item(0.5), fix_item(4.0), fix_item(0.1),
                          fix_item(1.0), fix_item(s->gamma));
    }
#undef ONE
    return -1;      /* unsupported */
}

static void set_pixeltype(int pt)
{
    g_cur.pixeltype = pt;
    if (pt == TWPT_BW) g_cur.depth = 1;
    else if (g_cur.depth != 8 && g_cur.depth != 16) g_cur.depth = 8;
}

/* returns TWRC_* ; sets g_cc on failure */
static TW_UINT16 cap_set(TW_CAPABILITY *c)
{
    TW_UINT16 type = 0;
    TW_UINT32 v = 0;
    TW_FRAME  fr;
    int       adjusted = 0;

    if (!unpack_value(c, &type, &v, &fr))
        return fail(TWCC_BADVALUE);

    switch (c->Cap) {
    case CAP_XFERCOUNT:
        g_cur.xfercount = (int)(LONG)v;
        if (g_cur.xfercount == 0 || g_cur.xfercount > 1) { g_cur.xfercount = -1; adjusted = 1; }
        break;
    case ICAP_PIXELTYPE:
        if (v != TWPT_BW && v != TWPT_GRAY && v != TWPT_RGB) return fail(TWCC_BADVALUE);
        set_pixeltype((int)v);
        break;
    case ICAP_UNITS:
        if (v != TWUN_INCHES && v != TWUN_CENTIMETERS && v != TWUN_MILLIMETERS && v != TWUN_PIXELS)
            return fail(TWCC_BADVALUE);
        g_cur.units = (int)v;
        break;
    case ICAP_XFERMECH:
        if (v != TWSX_NATIVE && v != TWSX_MEMORY) return fail(TWCC_BADVALUE);
        g_cur.xfermech = (int)v;
        break;
    case ICAP_COMPRESSION:
        if (v != TWCP_NONE) return fail(TWCC_BADVALUE);
        break;
    case ICAP_BITDEPTH:
        if (g_cur.pixeltype == TWPT_BW) {
            if (v != 1) return fail(TWCC_BADVALUE);
        } else if (v == 8 || v == 24) {
            g_cur.depth = 8;
        } else if (v == 16 || v == 48) {
            g_cur.depth = 16;
        } else {
            return fail(TWCC_BADVALUE);
        }
        break;
    case ICAP_XRESOLUTION:
    case ICAP_YRESOLUTION: {
        int dpi = (int)(fix_from_item(v) + 0.5);
        if (dpi < 50) { dpi = 50; adjusted = 1; }
        if (dpi > max_dpi()) { dpi = max_dpi(); adjusted = 1; }
        if (dpi != (int)(fix_from_item(v) + 0.5)) adjusted = 1;
        g_cur.dpi = dpi;     /* X and Y are always identical */
        break;
    }
    case ICAP_FRAMES: {
        int r = set_frame(&fr);
        if (r < 0) return fail(TWCC_BADVALUE);
        adjusted = r;
        break;
    }
    case ICAP_BRIGHTNESS:
    case ICAP_CONTRAST: {
        int iv = (int)floor(fix_from_item(v) + 0.5);
        if (iv < -1000) { iv = -1000; adjusted = 1; }
        if (iv > 1000)  { iv = 1000;  adjusted = 1; }
        if (c->Cap == ICAP_BRIGHTNESS) g_cur.brightness = iv; else g_cur.contrast = iv;
        break;
    }
    case ICAP_GAMMA: {
        double gm = fix_from_item(v);
        if (gm < 0.5) { gm = 0.5; adjusted = 1; }
        if (gm > 4.0) { gm = 4.0; adjusted = 1; }
        g_cur.gamma = gm;
        break;
    }
    case ICAP_PIXELFLAVOR:
    case ICAP_ORIENTATION:
    case ICAP_BITORDER:
    case ICAP_PLANARCHUNKY:
    case CAP_FEEDERENABLED:
    case CAP_INDICATORS:
        break;      /* accepted, only the single supported value exists */
    default:
        return fail(TWCC_CAPBADOPERATION);
    }
    g_cc = TWCC_SUCCESS;
    return adjusted ? TWRC_CHECKSTATUS : TWRC_SUCCESS;
}

static int cap_is_writable(TW_UINT16 cap)
{
    switch (cap) {
    case CAP_XFERCOUNT: case ICAP_PIXELTYPE: case ICAP_UNITS: case ICAP_XFERMECH:
    case ICAP_BITDEPTH: case ICAP_XRESOLUTION: case ICAP_YRESOLUTION: case ICAP_FRAMES:
    case ICAP_BRIGHTNESS: case ICAP_CONTRAST: case ICAP_GAMMA: case ICAP_COMPRESSION:
        return 1;
    }
    return 0;
}

static int cap_supported(TW_UINT16 cap)
{
    int i;
    for (i = 0; i < N_SUPPORTED_CAPS; i++) if (k_supported_caps[i] == cap) return 1;
    return 0;
}

static TW_UINT16 do_capability(TW_UINT16 msg, TW_CAPABILITY *c)
{
    int r;
    if (!c) return fail(TWCC_BADVALUE);

    if (msg == MSG_QUERYSUPPORT) {
        TW_UINT32 bits;
        if (!cap_supported(c->Cap)) return fail(TWCC_CAPUNSUPPORTED);
        bits = TWQC_GET | TWQC_GETCURRENT | TWQC_GETDEFAULT;
        if (cap_is_writable(c->Cap)) bits |= TWQC_SET | TWQC_RESET;
        if (!pack_one(c, TWTY_UINT32, bits)) return fail(TWCC_LOWMEMORY);
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    }

    if (!cap_supported(c->Cap)) return fail(TWCC_CAPUNSUPPORTED);

    switch (msg) {
    case MSG_GET:
    case MSG_GETCURRENT:
    case MSG_GETDEFAULT:
        r = cap_build(c->Cap, msg == MSG_GET ? 0 : (msg == MSG_GETCURRENT ? 1 : 2), &g_cur, c);
        if (r < 0) return fail(TWCC_CAPUNSUPPORTED);
        if (!r) return fail(TWCC_LOWMEMORY);
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;

    case MSG_SET:
        if (g_state > 6 || g_state < 4) return fail(TWCC_SEQERROR);
        if (!cap_is_writable(c->Cap) && c->Cap != ICAP_PIXELFLAVOR && c->Cap != ICAP_ORIENTATION &&
            c->Cap != ICAP_BITORDER && c->Cap != ICAP_PLANARCHUNKY && c->Cap != CAP_FEEDERENABLED &&
            c->Cap != CAP_INDICATORS)
            return fail(TWCC_CAPBADOPERATION);
        return cap_set(c);

    case MSG_RESET: {
        settings_t d = g_def;
        switch (c->Cap) {
        case CAP_XFERCOUNT:     g_cur.xfercount = d.xfercount; break;
        case ICAP_PIXELTYPE:    g_cur.pixeltype = d.pixeltype; g_cur.depth = d.depth; break;
        case ICAP_BITDEPTH:     g_cur.depth = d.depth; if (g_cur.pixeltype == TWPT_BW) g_cur.depth = 1; break;
        case ICAP_UNITS:        g_cur.units = d.units; break;
        case ICAP_XFERMECH:     g_cur.xfermech = d.xfermech; break;
        case ICAP_XRESOLUTION:
        case ICAP_YRESOLUTION:  g_cur.dpi = d.dpi; break;
        case ICAP_FRAMES:       g_cur.left = d.left; g_cur.top = d.top; g_cur.right = d.right; g_cur.bottom = d.bottom; break;
        case ICAP_BRIGHTNESS:   g_cur.brightness = 0; break;
        case ICAP_CONTRAST:     g_cur.contrast = 0; break;
        case ICAP_GAMMA:        g_cur.gamma = 1.0; break;
        default: break;
        }
        r = cap_build(c->Cap, 1, &g_cur, c);
        if (r < 0) return fail(TWCC_CAPUNSUPPORTED);
        if (!r) return fail(TWCC_LOWMEMORY);
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    }
    }
    return fail(TWCC_BADPROTOCOL);
}

/* ------------------------------------------------------------------ */
/* scan worker                                                         */
/* ------------------------------------------------------------------ */

static void free_image(void)
{
    if (g_raw) { VirtualFree(g_raw, 0, MEM_RELEASE); g_raw = NULL; }
    g_raw_size = 0;
    if (g_dib) { GlobalFree(g_dib); g_dib = NULL; }
    g_img_valid = 0;
}

static void fill_params(lide20_params_t *p)
{
    memset(p, 0, sizeof(*p));
    p->mode  = g_cur.pixeltype == TWPT_BW ? LIDE20_MODE_LINEART :
               g_cur.pixeltype == TWPT_GRAY ? LIDE20_MODE_GRAY : LIDE20_MODE_COLOR;
    p->depth = g_cur.depth;
    p->dpi   = g_cur.dpi;
    p->left_mm   = g_cur.left * MM_PER_INCH;
    p->top_mm    = g_cur.top * MM_PER_INCH;
    p->width_mm  = (g_cur.right - g_cur.left) * MM_PER_INCH;
    p->height_mm = (g_cur.bottom - g_cur.top) * MM_PER_INCH;
    if (p->left_mm + p->width_mm  > g_caps.max_width_mm)  p->width_mm  = g_caps.max_width_mm  - p->left_mm;
    if (p->top_mm  + p->height_mm > g_caps.max_height_mm) p->height_mm = g_caps.max_height_mm - p->top_mm;
    p->brightness = g_cur.brightness / 10;
    p->contrast   = g_cur.contrast / 10;
    p->gamma      = g_cur.gamma;
}

/* native transfers: build the DIB header, palette, and compute the stride */
static int dib_prepare(void)
{
    BITMAPINFOHEADER *bih;
    unsigned char *p;
    int w = g_img.pixels_per_line, h = g_img.lines;
    int bpp, pal = 0, i;
    size_t total;

    if (g_img.channels == 3) bpp = 24;
    else if (g_img.depth == 1) { bpp = 1; pal = 2; }
    else { bpp = 8; pal = 256; }

    g_dib_stride = (((size_t)w * bpp + 31) / 32) * 4;
    g_dib_hdr    = sizeof(BITMAPINFOHEADER) + sizeof(RGBQUAD) * (size_t)pal;
    total        = g_dib_hdr + g_dib_stride * (size_t)h;
    if (total > MAX_NATIVE_DPI_BYTES) return 0;

    g_dib = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
    if (!g_dib) return 0;
    p = (unsigned char *)GlobalLock(g_dib);
    if (!p) { GlobalFree(g_dib); g_dib = NULL; return 0; }

    bih = (BITMAPINFOHEADER *)p;
    bih->biSize = sizeof(*bih);
    bih->biWidth = w; bih->biHeight = h;
    bih->biPlanes = 1; bih->biBitCount = (WORD)bpp;
    bih->biCompression = BI_RGB;
    bih->biSizeImage = (DWORD)(g_dib_stride * (size_t)h);
    bih->biXPelsPerMeter = bih->biYPelsPerMeter = (LONG)(g_cur.dpi / 0.0254 + 0.5);
    bih->biClrUsed = pal; bih->biClrImportant = pal;

    if (pal == 2) {
        RGBQUAD *q = (RGBQUAD *)(p + sizeof(*bih));
#if LINEART_ONE_IS_WHITE
        q[0].rgbBlue = q[0].rgbGreen = q[0].rgbRed = 0;
        q[1].rgbBlue = q[1].rgbGreen = q[1].rgbRed = 255;
#else
        q[0].rgbBlue = q[0].rgbGreen = q[0].rgbRed = 255;
        q[1].rgbBlue = q[1].rgbGreen = q[1].rgbRed = 0;
#endif
    } else if (pal == 256) {
        RGBQUAD *q = (RGBQUAD *)(p + sizeof(*bih));
        for (i = 0; i < 256; i++) q[i].rgbBlue = q[i].rgbGreen = q[i].rgbRed = (BYTE)i;
    }
    GlobalUnlock(g_dib);
    return 1;
}

/* store one scan line into the native DIB (bottom-up, BGR, 8 bit) */
static void dib_store_line(unsigned char *dibbase, int y, const unsigned char *src)
{
    int w = g_img.pixels_per_line, x;
    unsigned char *dst = dibbase + g_dib_hdr + g_dib_stride * (size_t)(g_img.lines - 1 - y);

    if (g_img.channels == 3) {
        if (g_img.depth > 8) {
            const unsigned short *s16 = (const unsigned short *)src;
            for (x = 0; x < w; x++) {
                dst[x * 3 + 0] = (unsigned char)(s16[x * 3 + 2] >> 8);
                dst[x * 3 + 1] = (unsigned char)(s16[x * 3 + 1] >> 8);
                dst[x * 3 + 2] = (unsigned char)(s16[x * 3 + 0] >> 8);
            }
        } else {
            for (x = 0; x < w; x++) {
                dst[x * 3 + 0] = src[x * 3 + 2];
                dst[x * 3 + 1] = src[x * 3 + 1];
                dst[x * 3 + 2] = src[x * 3 + 0];
            }
        }
    } else if (g_img.depth == 1) {
        memcpy(dst, src, ((size_t)w + 7) / 8);
    } else if (g_img.depth > 8) {
        const unsigned short *s16 = (const unsigned short *)src;
        for (x = 0; x < w; x++) dst[x] = (unsigned char)(s16[x] >> 8);
    } else {
        memcpy(dst, src, (size_t)w);
    }
}

static DWORD WINAPI scan_thread(LPVOID unused)
{
    unsigned char *line = NULL;
    unsigned char *dibp = NULL;
    int r, y, native = (g_cur.xfermech == TWSX_NATIVE);
    (void)unused;

    r = lide20_scan_begin(g_scanner, &g_params, &g_img);
    if (r == LIDE20_OK) {
        line = (unsigned char *)malloc((size_t)g_img.bytes_per_line + 64);
        if (!line) r = LIDE20_E_NOMEM;
    }
    if (r == LIDE20_OK) {
        if (native) {
            if (!dib_prepare()) r = LIDE20_E_NOMEM;
            else dibp = (unsigned char *)GlobalLock(g_dib);
        } else {
            g_raw_size = (size_t)g_img.bytes_per_line * (size_t)g_img.lines;
            g_raw = (unsigned char *)VirtualAlloc(NULL, g_raw_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!g_raw) r = LIDE20_E_NOMEM;
        }
    }
    if (r != LIDE20_OK) {
        g_scan_error = r;
        InterlockedExchange(&g_scan_finished, 1);
        if (line) free(line);
        if (g_scanner) lide20_scan_end(g_scanner);
        if (g_hwnd) PostMessage(g_hwnd, WM_DS_READY, 0, (LPARAM)r);
        return 0;
    }

    g_img_valid = 1;
    InterlockedExchange(&g_lines_done, 0);
    if (g_hwnd) PostMessage(g_hwnd, WM_DS_READY, 1, 0);

    for (y = 0; y < g_img.lines; y++) {
        r = lide20_scan_read_line(g_scanner, line);
        if (r != LIDE20_OK) { g_scan_error = r; break; }
        if (native) dib_store_line(dibp, y, line);
        else memcpy(g_raw + (size_t)y * (size_t)g_img.bytes_per_line, line, (size_t)g_img.bytes_per_line);
        InterlockedIncrement(&g_lines_done);
    }
    if (dibp) GlobalUnlock(g_dib);
    free(line);
    lide20_scan_end(g_scanner);
    InterlockedExchange(&g_scan_finished, 1);
    return 0;
}

static void stop_scan(int wait)
{
    if (g_thread) {
        if (!g_scan_finished && g_scanner) lide20_cancel(g_scanner);
        if (wait) WaitForSingleObject(g_thread, 30000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}

static int wait_lines(int need)
{
    while (g_lines_done < need) {
        if (g_scan_error) return 0;
        if (g_scan_finished && g_lines_done < need) return 0;
        Sleep(5);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* notification window                                                 */
/* ------------------------------------------------------------------ */

static DSMENTRYPROC dsm_entry(void)
{
    if (g_have_ep && g_ep.DSM_Entry) return g_ep.DSM_Entry;
    if (!g_dsm) {
        HMODULE m = GetModuleHandleA("twaindsm.dll");
        if (!m) m = GetModuleHandleA("twain_32.dll");
        if (m) g_dsm = (DSMENTRYPROC)GetProcAddress(m, "DSM_Entry");
    }
    return g_dsm;
}

static void notify_app(TW_UINT16 msg)
{
    DSMENTRYPROC f = dsm_entry();
    if (f && g_have_app)
        f(&g_ds, &g_app, DG_CONTROL, DAT_NULL, msg, NULL);
}

static LRESULT CALLBACK ds_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DS_READY) {
        if (w && g_state == 5) {
            g_state = 6;
            g_pending = 1;
            g_mem_rows_sent = 0;
            notify_app(MSG_XFERREADY);
        } else if (!w) {
            notify_app(MSG_CLOSEDSREQ);
        }
        return 0;
    }
    if (m == WM_DS_CLOSEREQ) {
        notify_app(MSG_CLOSEDSREQ);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static int create_window(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ds_wndproc;
    wc.hInstance = g_inst;
    wc.lpszClassName = "LiDE20DSNotify";
    RegisterClassA(&wc);
    g_hwnd = CreateWindowA("LiDE20DSNotify", "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_inst, NULL);
    return g_hwnd != NULL;
}

/* ------------------------------------------------------------------ */
/* settings dialog                                                     */
/* ------------------------------------------------------------------ */

static const char *k_area_names[] = { "Full bed", "A5 (148 x 210 mm)", "10 x 15 cm", "Custom" };

static void dlg_set_mm(HWND d, int id, double mm)
{
    char b[32];
    sprintf(b, "%.1f", mm);
    SetDlgItemTextA(d, id, b);
}
static double dlg_get_mm(HWND d, int id)
{
    char b[32];
    GetDlgItemTextA(d, id, b, sizeof(b));
    return atof(b);
}

static void dlg_update_depth(HWND d)
{
    int mode = (int)SendDlgItemMessageA(d, IDC_MODE, CB_GETCURSEL, 0, 0);
    HWND cb = GetDlgItem(d, IDC_DEPTH);
    SendMessageA(cb, CB_RESETCONTENT, 0, 0);
    if (mode == 2) {
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"1 bit");
        SendMessageA(cb, CB_SETCURSEL, 0, 0);
        EnableWindow(cb, FALSE);
    } else {
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"8 bit");
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)"16 bit");
        SendMessageA(cb, CB_SETCURSEL, g_cur.depth == 16 ? 1 : 0, 0);
        EnableWindow(cb, TRUE);
    }
}

static void dlg_area_changed(HWND d)
{
    int a = (int)SendDlgItemMessageA(d, IDC_AREA, CB_GETCURSEL, 0, 0);
    BOOL custom = (a == 3);
    double w = g_caps.max_width_mm, h = g_caps.max_height_mm;
    if (a == 1) { w = 148; h = 210; }
    if (a == 2) { w = 100; h = 150; }
    if (w > g_caps.max_width_mm) w = g_caps.max_width_mm;
    if (h > g_caps.max_height_mm) h = g_caps.max_height_mm;
    if (!custom) {
        dlg_set_mm(d, IDC_LEFT, 0); dlg_set_mm(d, IDC_TOP, 0);
        dlg_set_mm(d, IDC_WIDTH, w); dlg_set_mm(d, IDC_HEIGHT, h);
    }
    EnableWindow(GetDlgItem(d, IDC_LEFT), custom);
    EnableWindow(GetDlgItem(d, IDC_TOP), custom);
    EnableWindow(GetDlgItem(d, IDC_WIDTH), custom);
    EnableWindow(GetDlgItem(d, IDC_HEIGHT), custom);
}

static INT_PTR CALLBACK dlg_proc(HWND d, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_INITDIALOG: {
        int i, cur = 0;
        char b[16];
        SendDlgItemMessageA(d, IDC_MODE, CB_ADDSTRING, 0, (LPARAM)"Color");
        SendDlgItemMessageA(d, IDC_MODE, CB_ADDSTRING, 0, (LPARAM)"Grayscale");
        SendDlgItemMessageA(d, IDC_MODE, CB_ADDSTRING, 0, (LPARAM)"Black & white (lineart)");
        SendDlgItemMessageA(d, IDC_MODE, CB_SETCURSEL,
            g_cur.pixeltype == TWPT_RGB ? 0 : g_cur.pixeltype == TWPT_GRAY ? 1 : 2, 0);
        for (i = 0; i < N_DPIS; i++) {
            if ((int)k_dpis[i] > max_dpi()) continue;
            sprintf(b, "%u", k_dpis[i]);
            SendDlgItemMessageA(d, IDC_DPI, CB_ADDSTRING, 0, (LPARAM)b);
            if ((int)k_dpis[i] == g_cur.dpi) cur = i;
        }
        SendDlgItemMessageA(d, IDC_DPI, CB_SETCURSEL, cur, 0);
        dlg_update_depth(d);
        for (i = 0; i < 4; i++)
            SendDlgItemMessageA(d, IDC_AREA, CB_ADDSTRING, 0, (LPARAM)k_area_names[i]);
        SendDlgItemMessageA(d, IDC_AREA, CB_SETCURSEL, 3, 0);
        dlg_set_mm(d, IDC_LEFT, g_cur.left * MM_PER_INCH);
        dlg_set_mm(d, IDC_TOP, g_cur.top * MM_PER_INCH);
        dlg_set_mm(d, IDC_WIDTH, (g_cur.right - g_cur.left) * MM_PER_INCH);
        dlg_set_mm(d, IDC_HEIGHT, (g_cur.bottom - g_cur.top) * MM_PER_INCH);
        sprintf(b, "%d", g_cur.brightness / 10); SetDlgItemTextA(d, IDC_BRIGHT, b);
        sprintf(b, "%d", g_cur.contrast / 10);   SetDlgItemTextA(d, IDC_CONTRAST, b);
        {
            char t[128];
            sprintf(t, "%s - bed %.0f x %.0f mm, up to %d dpi.\nWarm-up and calibration can take up to a minute.",
                    g_caps.model, g_caps.max_width_mm, g_caps.max_height_mm, g_caps.max_dpi);
            SetDlgItemTextA(d, IDC_STATUS, t);
        }
        dlg_area_changed(d);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDC_MODE && HIWORD(w) == CBN_SELCHANGE) { dlg_update_depth(d); return TRUE; }
        if (LOWORD(w) == IDC_AREA && HIWORD(w) == CBN_SELCHANGE) { dlg_area_changed(d); return TRUE; }
        if (LOWORD(w) == IDOK) {
            int mode = (int)SendDlgItemMessageA(d, IDC_MODE, CB_GETCURSEL, 0, 0);
            int di = (int)SendDlgItemMessageA(d, IDC_DPI, CB_GETCURSEL, 0, 0);
            int k = 0, i;
            double l_ = dlg_get_mm(d, IDC_LEFT), t_ = dlg_get_mm(d, IDC_TOP);
            double w_ = dlg_get_mm(d, IDC_WIDTH), h_ = dlg_get_mm(d, IDC_HEIGHT);
            if (l_ < 0) l_ = 0;
            if (t_ < 0) t_ = 0;
            if (l_ + w_ > g_caps.max_width_mm) w_ = g_caps.max_width_mm - l_;
            if (t_ + h_ > g_caps.max_height_mm) h_ = g_caps.max_height_mm - t_;
            if (w_ < 2 || h_ < 2) {
                MessageBoxA(d, "The scan area is too small or outside the scan bed.", "LiDE 20", MB_ICONWARNING);
                return TRUE;
            }
            set_pixeltype(mode == 0 ? TWPT_RGB : mode == 1 ? TWPT_GRAY : TWPT_BW);
            if (mode != 2)
                g_cur.depth = (int)SendDlgItemMessageA(d, IDC_DEPTH, CB_GETCURSEL, 0, 0) == 1 ? 16 : 8;
            for (i = 0; i < N_DPIS; i++) {
                if ((int)k_dpis[i] > max_dpi()) continue;
                if (k == di) { g_cur.dpi = (int)k_dpis[i]; break; }
                k++;
            }
            g_cur.left = l_ / MM_PER_INCH;  g_cur.top = t_ / MM_PER_INCH;
            g_cur.right = (l_ + w_) / MM_PER_INCH;  g_cur.bottom = (t_ + h_) / MM_PER_INCH;
            g_cur.brightness = (int)dlg_get_mm(d, IDC_BRIGHT) * 10;
            g_cur.contrast   = (int)dlg_get_mm(d, IDC_CONTRAST) * 10;
            if (g_cur.brightness < -1000) g_cur.brightness = -1000;
            if (g_cur.brightness > 1000)  g_cur.brightness = 1000;
            if (g_cur.contrast < -1000) g_cur.contrast = -1000;
            if (g_cur.contrast > 1000)  g_cur.contrast = 1000;
            EndDialog(d, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(d, IDCANCEL); return TRUE; }
        break;
    case WM_CLOSE:
        EndDialog(d, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* triplet handlers                                                    */
/* ------------------------------------------------------------------ */

static void fill_identity(TW_IDENTITY *id)
{
    id->Version.MajorNum = 0;
    id->Version.MinorNum = 1;
    id->Version.Language = TWLG_ENGLISH_USA;
    id->Version.Country  = TWCY_USA;
    strcpy(id->Version.Info, "0.1.0 (user-space WinUSB)");
    id->ProtocolMajor = TWON_PROTOCOLMAJOR;
    id->ProtocolMinor = TWON_PROTOCOLMINOR;
    id->SupportedGroups = DG_CONTROL | DG_IMAGE | DF_DS2;
    strcpy(id->Manufacturer, "RetroFlatbed");
    strcpy(id->ProductFamily, "CanoScan");
    strcpy(id->ProductName, "Canon CanoScan LiDE 20");
}

static void close_device(void)
{
    stop_scan(1);
    free_image();
    if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
    if (g_scanner) { lide20_close(g_scanner); g_scanner = NULL; }
    g_have_app = 0;
}

static TW_UINT16 do_identity(TW_IDENTITY *origin, TW_UINT16 msg, TW_IDENTITY *id)
{
    switch (msg) {
    case MSG_GET:
        if (!id) return fail(TWCC_BADVALUE);
        {
            TW_UINT32 keep = id->Id;
            fill_identity(id);
            id->Id = keep;
        }
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;

    case MSG_OPENDS: {
        int r;
        if (g_state != 3) return fail(TWCC_SEQERROR);
        if (!id) return fail(TWCC_BADVALUE);
        r = lide20_open(0, &g_scanner);
        if (r != LIDE20_OK) {
            g_scanner = NULL;
            return fail(r == LIDE20_E_NO_DEVICE ? TWCC_NODS : TWCC_OPERATIONERROR);
        }
        g_caps = *lide20_get_caps(g_scanner);
        g_ds = *id;
        if (origin) { g_app = *origin; g_have_app = 1; }
        reset_settings();
        g_pending = 0;
        g_state = 4;
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    }
    case MSG_CLOSEDS:
        if (g_state != 4) return fail(TWCC_SEQERROR);
        close_device();
        g_state = 3;
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    }
    return fail(TWCC_BADPROTOCOL);
}

static void begin_acquire(void)
{
    DWORD tid;
    free_image();
    g_scan_error = 0;
    g_scan_finished = 0;
    g_lines_done = 0;
    g_pending = 0;
    fill_params(&g_params);
    g_thread = CreateThread(NULL, 0, scan_thread, NULL, 0, &tid);
    if (!g_thread) {
        g_scan_error = LIDE20_E_NOMEM;
        PostMessage(g_hwnd, WM_DS_READY, 0, 0);
    }
}

static TW_UINT16 do_userinterface(TW_UINT16 msg, TW_USERINTERFACE *ui)
{
    if (msg == MSG_ENABLEDS || msg == MSG_ENABLEDSUIONLY) {
        if (g_state != 4) return fail(TWCC_SEQERROR);
        if (!ui) return fail(TWCC_BADVALUE);
        if (!create_window()) return fail(TWCC_LOWMEMORY);

        if (ui->ShowUI || msg == MSG_ENABLEDSUIONLY) {
            INT_PTR r = DialogBoxParamA(g_inst, MAKEINTRESOURCEA(IDD_SCAN),
                                        (HWND)ui->hParent, dlg_proc, 0);
            ui->ModalUI = TRUE;
            g_state = 5;
            if (r != IDOK || msg == MSG_ENABLEDSUIONLY) {
                PostMessage(g_hwnd, WM_DS_CLOSEREQ, 0, 0);
                g_cc = TWCC_SUCCESS;
                return TWRC_SUCCESS;
            }
        } else {
            g_state = 5;
        }
        begin_acquire();
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    }
    if (msg == MSG_DISABLEDS) {
        if (g_state < 5) return fail(TWCC_SEQERROR);
        if (g_state == 7) return fail(TWCC_SEQERROR);
        stop_scan(1);
        free_image();
        if (g_hwnd) { DestroyWindow(g_hwnd); g_hwnd = NULL; }
        g_pending = 0;
        g_state = 4;
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    }
    return fail(TWCC_BADPROTOCOL);
}

static TW_UINT16 do_pendingxfers(TW_UINT16 msg, TW_PENDINGXFERS *px)
{
    if (!px) return fail(TWCC_BADVALUE);
    switch (msg) {
    case MSG_GET:
        if (g_state < 4 || g_state > 7) return fail(TWCC_SEQERROR);
        px->Count = (TW_UINT16)(g_state >= 6 ? g_pending : 0);
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    case MSG_ENDXFER:
        if (g_state != 6 && g_state != 7) return fail(TWCC_SEQERROR);
        stop_scan(1);
        free_image();
        g_pending = 0;
        px->Count = 0;
        px->EOJ = 0;
        g_state = 5;
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    case MSG_RESET:
        if (g_state != 6) return fail(TWCC_SEQERROR);
        stop_scan(1);
        free_image();
        g_pending = 0;
        px->Count = 0;
        g_state = 5;
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    }
    return fail(TWCC_BADPROTOCOL);
}

static TW_UINT16 do_imageinfo(TW_IMAGEINFO *ii)
{
    int i;
    if (g_state != 6 && g_state != 7) return fail(TWCC_SEQERROR);
    if (!ii) return fail(TWCC_BADVALUE);
    if (!g_img_valid) return fail(TWCC_SEQERROR);

    memset(ii, 0, sizeof(*ii));
    ii->XResolution = to_fix(g_cur.dpi);
    ii->YResolution = to_fix(g_cur.dpi);
    ii->ImageWidth  = g_img.pixels_per_line;
    ii->ImageLength = g_img.lines;
    ii->SamplesPerPixel = (TW_INT16)g_img.channels;
    for (i = 0; i < 8; i++) ii->BitsPerSample[i] = (i < g_img.channels) ? (TW_INT16)g_img.depth : 0;
    ii->BitsPerPixel = (TW_INT16)(g_img.depth * g_img.channels);
    ii->Planar = FALSE;
    ii->PixelType = (TW_INT16)g_cur.pixeltype;
    ii->Compression = TWCP_NONE;
    g_cc = TWCC_SUCCESS;
    return TWRC_SUCCESS;
}

static TW_UINT16 do_imagelayout(TW_UINT16 msg, TW_IMAGELAYOUT *il)
{
    if (!il) return fail(TWCC_BADVALUE);
    switch (msg) {
    case MSG_GET:
    case MSG_GETCURRENT:
        get_frame(&il->Frame);
        il->DocumentNumber = 1; il->PageNumber = 1; il->FrameNumber = 1;
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    case MSG_GETDEFAULT:
        default_frame(&il->Frame);
        il->DocumentNumber = 1; il->PageNumber = 1; il->FrameNumber = 1;
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    case MSG_SET: {
        int r;
        if (g_state != 4) return fail(TWCC_SEQERROR);
        r = set_frame(&il->Frame);
        if (r < 0) return fail(TWCC_BADVALUE);
        g_cc = TWCC_SUCCESS;
        return r ? TWRC_CHECKSTATUS : TWRC_SUCCESS;
    }
    case MSG_RESET:
        if (g_state != 4) return fail(TWCC_SEQERROR);
        g_cur.left = g_def.left; g_cur.top = g_def.top;
        g_cur.right = g_def.right; g_cur.bottom = g_def.bottom;
        get_frame(&il->Frame);
        g_cc = TWCC_SUCCESS;
        return TWRC_SUCCESS;
    }
    return fail(TWCC_BADPROTOCOL);
}

static TW_UINT16 do_nativexfer(TW_MEMREF pData)
{
    if (g_state != 6) return fail(TWCC_SEQERROR);
    if (!pData) return fail(TWCC_BADVALUE);
    if (g_cur.xfermech != TWSX_NATIVE || !g_dib) {
        if (!g_img_valid) return fail(TWCC_OPERATIONERROR);
        return fail(TWCC_SEQERROR);
    }
    g_state = 7;
    if (!wait_lines(g_img.lines)) {
        g_state = 6;
        return fail(g_scan_error == LIDE20_E_CANCELLED ? TWCC_SEQERROR : TWCC_OPERATIONERROR);
    }
    *(TW_HANDLE *)pData = (TW_HANDLE)g_dib;
    g_dib = NULL;                 /* ownership passes to the application */
    g_pending = 0;
    g_cc = TWCC_SUCCESS;
    return TWRC_XFERDONE;
}

static TW_UINT16 do_setupmemxfer(TW_SETUPMEMXFER *s)
{
    if (!s) return fail(TWCC_BADVALUE);
    if (g_state < 4 || g_state > 6) return fail(TWCC_SEQERROR);
    s->MinBufSize = 4096;
    s->MaxBufSize = 1024 * 1024;
    s->Preferred  = 64 * 1024;
    g_cc = TWCC_SUCCESS;
    return TWRC_SUCCESS;
}

static TW_UINT16 do_memxfer(TW_IMAGEMEMXFER *m)
{
    size_t bpr, avail_rows, rows, out_bpr;
    unsigned char *dst;
    int y, x, remaining;

    if (g_state != 6 && g_state != 7) return fail(TWCC_SEQERROR);
    if (!m) return fail(TWCC_BADVALUE);
    if (g_cur.xfermech != TWSX_MEMORY || !g_raw) return fail(TWCC_SEQERROR);
    if (m->Memory.Flags & TWMF_HANDLE) {
        dst = (unsigned char *)h_lock((TW_HANDLE)m->Memory.TheMem);
    } else {
        dst = (unsigned char *)m->Memory.TheMem;
    }
    if (!dst) return fail(TWCC_BADVALUE);

    /* output uses 8-bit samples for 16-bit data only when the app asked for 8, so stay native */
    out_bpr = (size_t)g_img.bytes_per_line;
    bpr = out_bpr;
    avail_rows = m->Memory.Length / bpr;
    if (avail_rows < 1) {
        if (m->Memory.Flags & TWMF_HANDLE) h_unlock((TW_HANDLE)m->Memory.TheMem);
        return fail(TWCC_BADVALUE);
    }
    g_state = 7;
    remaining = g_img.lines - g_mem_rows_sent;
    rows = avail_rows < (size_t)remaining ? avail_rows : (size_t)remaining;

    if (!wait_lines(g_mem_rows_sent + (int)rows)) {
        if (m->Memory.Flags & TWMF_HANDLE) h_unlock((TW_HANDLE)m->Memory.TheMem);
        g_state = 6;
        return fail(TWCC_OPERATIONERROR);
    }

    for (y = 0; y < (int)rows; y++) {
        unsigned char *src = g_raw + (size_t)(g_mem_rows_sent + y) * bpr;
        memcpy(dst + (size_t)y * bpr, src, bpr);
        if (g_img.depth == 1 && !LINEART_ONE_IS_WHITE)
            for (x = 0; x < (int)bpr; x++) dst[(size_t)y * bpr + x] = (unsigned char)~dst[(size_t)y * bpr + x];
    }

    m->Compression  = TWCP_NONE;
    m->BytesPerRow  = (TW_UINT32)bpr;
    m->Columns      = g_img.pixels_per_line;
    m->Rows         = (TW_UINT32)rows;
    m->XOffset      = 0;
    m->YOffset      = g_mem_rows_sent;
    m->BytesWritten = (TW_UINT32)(rows * bpr);
    g_mem_rows_sent += (int)rows;

    if (m->Memory.Flags & TWMF_HANDLE) h_unlock((TW_HANDLE)m->Memory.TheMem);

    g_cc = TWCC_SUCCESS;
    if (g_mem_rows_sent >= g_img.lines) {
        g_pending = 0;
        return TWRC_XFERDONE;
    }
    return TWRC_SUCCESS;
}

static TW_UINT16 do_status(TW_STATUS *s)
{
    if (!s) return fail(TWCC_BADVALUE);
    s->ConditionCode = g_cc;
    s->Data = 0;
    g_cc = TWCC_SUCCESS;
    return TWRC_SUCCESS;
}

static TW_UINT16 do_xfergroup(TW_UINT16 msg, TW_UINT32 *grp)
{
    if (!grp) return fail(TWCC_BADVALUE);
    if (msg == MSG_GET) { *grp = DG_IMAGE; g_cc = TWCC_SUCCESS; return TWRC_SUCCESS; }
    if (msg == MSG_SET) { g_cc = TWCC_SUCCESS; return TWRC_SUCCESS; }
    return fail(TWCC_BADPROTOCOL);
}

/* ------------------------------------------------------------------ */
/* entry points                                                        */
/* ------------------------------------------------------------------ */

TW_UINT16 FAR PASCAL DS_Entry(pTW_IDENTITY pOrigin, TW_UINT32 DG, TW_UINT16 DAT,
                              TW_UINT16 MSG, TW_MEMREF pData)
{
    TW_UINT16 rc;

    EnterCriticalSection(&g_cs);

    if (DG == DG_CONTROL) {
        switch (DAT) {
        case DAT_IDENTITY:
            rc = do_identity(pOrigin, MSG, (TW_IDENTITY *)pData);
            break;
        case DAT_CAPABILITY:
            rc = (g_state < 4) ? fail(TWCC_SEQERROR) : do_capability(MSG, (TW_CAPABILITY *)pData);
            break;
        case DAT_USERINTERFACE:
            rc = do_userinterface(MSG, (TW_USERINTERFACE *)pData);
            break;
        case DAT_EVENT:
            if (MSG == MSG_PROCESSEVENT && pData) {
                ((TW_EVENT *)pData)->TWMessage = MSG_NULL;
                rc = TWRC_NOTDSEVENT;
            } else rc = fail(TWCC_BADPROTOCOL);
            break;
        case DAT_PENDINGXFERS:
            rc = do_pendingxfers(MSG, (TW_PENDINGXFERS *)pData);
            break;
        case DAT_SETUPMEMXFER:
            rc = (MSG == MSG_GET) ? do_setupmemxfer((TW_SETUPMEMXFER *)pData) : fail(TWCC_BADPROTOCOL);
            break;
        case DAT_STATUS:
            rc = (MSG == MSG_GET) ? do_status((TW_STATUS *)pData) : fail(TWCC_BADPROTOCOL);
            break;
        case DAT_XFERGROUP:
            rc = do_xfergroup(MSG, (TW_UINT32 *)pData);
            break;
        case DAT_ENTRYPOINT:
            if (MSG == MSG_SET && pData) {
                g_ep = *(TW_ENTRYPOINT *)pData;
                g_have_ep = (g_ep.Size >= offsetof(TW_ENTRYPOINT, DSM_MemUnlock) + sizeof(void *));
                g_cc = TWCC_SUCCESS;
                rc = TWRC_SUCCESS;
            } else rc = fail(TWCC_BADPROTOCOL);
            break;
        case DAT_CUSTOMDSDATA:
            rc = fail(TWCC_BADPROTOCOL);
            break;
        default:
            rc = fail(TWCC_BADPROTOCOL);
        }
    } else if (DG == DG_IMAGE) {
        switch (DAT) {
        case DAT_IMAGEINFO:
            rc = (MSG == MSG_GET) ? do_imageinfo((TW_IMAGEINFO *)pData) : fail(TWCC_BADPROTOCOL);
            break;
        case DAT_IMAGELAYOUT:
            rc = do_imagelayout(MSG, (TW_IMAGELAYOUT *)pData);
            break;
        case DAT_IMAGENATIVEXFER:
            rc = (MSG == MSG_GET) ? do_nativexfer(pData) : fail(TWCC_BADPROTOCOL);
            break;
        case DAT_IMAGEMEMXFER:
            rc = (MSG == MSG_GET) ? do_memxfer((TW_IMAGEMEMXFER *)pData) : fail(TWCC_BADPROTOCOL);
            break;
        default:
            rc = fail(TWCC_BADPROTOCOL);
        }
    } else {
        rc = fail(TWCC_BADPROTOCOL);
    }

    LeaveCriticalSection(&g_cs);
    return rc;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        g_cs_init = 1;
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_cs_init) DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}
