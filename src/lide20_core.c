/* Canon CanoScan LiDE 20 core: glue between the SANE plustek hardware layer
 * (unity-built below, exactly like backend/plustek.c does) and the
 * lide20_* API. All SANE frontend plumbing (options, fork/pipe reader,
 * signals) is replaced by direct, synchronous calls.
 */
#include <math.h>
#include <errno.h>
#include <setupapi.h>

#include "../third_party/sane/include/sane/sane.h"
#include "../third_party/sane/include/sane/sanei_debug.h"
#include "lide20_core.h"

#define MM_PER_INCH     25.4
#define SANE_I18N(text) text
#define STRINGIFY(x)    #x
typedef int SANE_Pid;

#define _DBG_FATAL      0
#define _DBG_ERROR      1
#define _DBG_WARNING    3
#define _DBG_INFO       5
#define _DBG_PROC       7
#define _DBG_SANE_INIT 10
#define _DBG_INFO2     15
#define _DBG_DREGS     20
#define _DBG_DCALDATA  22
#define _DBG_DPIC      25
#define _DBG_READ      30

#define BACKEND_NAME plustek
#include "../third_party/sane/include/sane/sanei_usb.h"
#include "../third_party/sane/include/sane/sanei_lm983x.h"
#include "../third_party/sane/include/sane/sanei_access.h"

#include "../third_party/sane/backend/plustek-usb.h"
#include "../third_party/sane/backend/plustek.h"

#define _DEFAULT_DEVICE "auto"

/* declared here, as the plustek hardware files use them */
static volatile SANE_Bool cancelRead;
static DevList           *usbDevs;

#include "../third_party/sane/backend/plustek-usbio.c"
#include "../third_party/sane/backend/plustek-usbdevs.c"
#include "../third_party/sane/backend/plustek-usbhw.c"
#include "../third_party/sane/backend/plustek-usbmap.c"
#include "../third_party/sane/backend/plustek-usbscan.c"
#include "../third_party/sane/backend/plustek-usbimg.c"
#include "../third_party/sane/backend/plustek-usbcalfile.c"
#include "../third_party/sane/backend/plustek-usbshading.c"
#include "../third_party/sane/backend/plustek-usbcal.c"
#include "../third_party/sane/backend/plustek-usb.c"

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

int lide20_debug_level = 0;

void lide20_dbg(int level, const char *fmt, ...)
{
    static FILE *logf;
    static int   inited;
    va_list ap;

    if (!inited) {
        const char *lv = getenv("RETROFLATBED_DEBUG");
        const char *lf = getenv("RETROFLATBED_LOG");
        if (!lv) lv = getenv("LIDE20_DEBUG");      /* former names still work */
        if (!lf) lf = getenv("LIDE20_LOG");
        if (lv) lide20_debug_level = atoi(lv);
        if (lf) logf = fopen(lf, "a");
        inited = 1;
    }
    if (level > lide20_debug_level)
        return;
    va_start(ap, fmt);
    vfprintf(logf ? logf : stderr, fmt, ap);
    va_end(ap);
    if (logf) fflush(logf);
}

void lide20_set_debug(int level) { lide20_debug_level = level; }

/* ------------------------------------------------------------------ */
/* device handling                                                     */
/* ------------------------------------------------------------------ */

enum { ST_CLOSED = 0, ST_IDLE, ST_SCANNING };

struct lide20 {
    int             sim;        /* LIDE20_SIMULATE=1: synthetic test image, no USB */
    int             sim_line;
    Plustek_Device *dev;
    lide20_caps_t   caps;
    int             state;
    lide20_image_t  img;
    int             lines_left;
    unsigned char  *linebuf;
    size_t          linebuf_size;
    SANE_Word       gamma[4][4096];
};

static void init_config(CnfDef *cnf)
{
    memset(cnf, 0, sizeof(*cnf));

    cnf->adj.warmup       = -1;
    cnf->adj.lampOff      = -1;
    cnf->adj.lampOffOnEnd = -1;
    cnf->adj.posShadingY  = -1;
    cnf->adj.tpaShadingY  = -1;
    cnf->adj.negShadingY  = -1;
    cnf->adj.rgain        = -1;
    cnf->adj.ggain        = -1;
    cnf->adj.bgain        = -1;
    cnf->adj.rofs         = -1;
    cnf->adj.gofs         = -1;
    cnf->adj.bofs         = -1;
    cnf->adj.rlampoff     = -1;
    cnf->adj.glampoff     = -1;
    cnf->adj.blampoff     = -1;
    cnf->adj.incDarkTgt   = 1;
    cnf->adj.graygamma    = 1.0;
    cnf->adj.rgamma       = 1.0;
    cnf->adj.ggamma       = 1.0;
    cnf->adj.bgamma       = 1.0;
}

static void free_devlist(void)
{
    while (usbDevs) {
        DevList *n = usbDevs->next;
        free(usbDevs);
        usbDevs = n;
    }
}

static int count_devices(void)
{
    DevList *d;
    int n = 0;
    free_devlist();
    sanei_usb_init();
    usbGetList(&usbDevs);
    for (d = usbDevs; d; d = d->next)
        if (d->vendor_id == 0x04A9 && d->device_id == 0x220D)
            n++;
    return n;
}

static int sim_enabled(void)
{
    const char *e = getenv("RETROFLATBED_SIMULATE");
    if (!e) e = getenv("LIDE20_SIMULATE");          /* former name still works */
    return e && e[0] && e[0] != '0';
}

int lide20_device_count(void)
{
    int n;
    if (sim_enabled()) return 1;
    n = count_devices();
    free_devlist();
    return n;
}

int lide20_device_present_without_driver(void)
{
    HDEVINFO        set;
    SP_DEVINFO_DATA dd;
    DWORD           i;
    int             found = 0;

    set = SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
        return 0;
    dd.cbSize = sizeof(dd);
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &dd); i++) {
        char  ids[1024];
        DWORD need;
        if (SetupDiGetDeviceRegistryPropertyA(set, &dd, SPDRP_HARDWAREID, NULL,
                                              (PBYTE)ids, sizeof(ids) - 2, &need)) {
            ids[sizeof(ids) - 1] = ids[sizeof(ids) - 2] = 0;
            if (strstr(ids, "VID_04A9&PID_220D") || strstr(ids, "vid_04a9&pid_220d")) {
                found = 1;
                break;
            }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static void build_gamma(lide20_t *s, double g)
{
    int ch, j;
    if (g <= 0.0) g = 1.0;
    for (ch = 0; ch < 4; ch++)
        for (j = 0; j < 4096; j++) {
            int v = (int)(255.0 * pow((double)j / 4095.0, 1.0 / g));
            s->gamma[ch][j] = v > 255 ? 255 : v;
        }
}

int lide20_open(int index, lide20_t **out)
{
    lide20_t       *s;
    Plustek_Device *dev;
    CnfDef          cnf;
    DevList        *d;
    int             n, handle, res;

    *out = NULL;
    if (sim_enabled()) {
        s = (lide20_t *)calloc(1, sizeof(*s));
        if (!s) return LIDE20_E_NOMEM;
        s->sim = 1;
        s->state = ST_IDLE;
        snprintf(s->caps.model, sizeof(s->caps.model), "CanoScan LiDE 20 (simulated)");
        s->caps.optic_dpi     = 600;
        s->caps.max_dpi       = 1200;
        s->caps.max_width_mm  = 2550 * MM_PER_INCH / 300.0;
        s->caps.max_height_mm = 3508 * MM_PER_INCH / 300.0;
        *out = s;
        return LIDE20_OK;
    }
    n = count_devices();
    if (n <= 0 || index < 0 || index >= n) {
        free_devlist();
        return LIDE20_E_NO_DEVICE;
    }

    /* skip the first <index> devices by marking them as already attached */
    for (d = usbDevs; d && index > 0; d = d->next)
        if (d->vendor_id == 0x04A9 && d->device_id == 0x220D) {
            d->attached = SANE_TRUE;
            index--;
        }

    s   = (lide20_t *)calloc(1, sizeof(*s));
    dev = (Plustek_Device *)calloc(1, sizeof(*dev));
    if (!s || !dev) {
        free(s); free(dev); free_devlist();
        return LIDE20_E_NOMEM;
    }

    init_config(&cnf);
    dev->fd           = -1;
    dev->name         = strdup(_DEFAULT_DEVICE);
    dev->sane.name    = dev->name;
    dev->sane.vendor  = "Canon";
    dev->initialized  = -1;
    dev->transferRate = DEFAULT_RATE;
    memcpy(&dev->adj, &cnf.adj, sizeof(AdjDef));
    snprintf(dev->usbId, _MAX_ID_LEN, "0x04A9-0x220D");

    handle = usbDev_open(dev, usbDevs, SANE_FALSE);
    if (handle < 0) {
        DBG(_DBG_ERROR, "lide20_open: usbDev_open failed (%d)\n", handle);
        free(dev->name); free(dev); free(s);
        free_devlist();
        return LIDE20_E_OPEN;
    }
    dev->fd = handle;

    res = usbDev_getCaps(dev);
    if (res < 0) {
        usbDev_close(dev);
        free(dev->name); free(dev); free(s);
        free_devlist();
        return LIDE20_E_IO;
    }

    s->dev   = dev;
    s->state = ST_IDLE;

    snprintf(s->caps.model, sizeof(s->caps.model), "%s",
             dev->usbDev.ModelStr ? dev->usbDev.ModelStr : "CanoScan LiDE 20");
    s->caps.optic_dpi     = dev->usbDev.Caps.OpticDpi.x;
    s->caps.max_dpi       = dev->usbDev.Caps.OpticDpi.x * 2;
    s->caps.max_width_mm  = dev->caps.wMaxExtentX * MM_PER_INCH / _MEASURE_BASE;
    s->caps.max_height_mm = dev->caps.wMaxExtentY * MM_PER_INCH / _MEASURE_BASE;

    build_gamma(s, 1.0);
    *out = s;
    return LIDE20_OK;
}

void lide20_close(lide20_t *s)
{
    Plustek_Device *dev;
    if (!s) return;
    if (s->sim) { free(s); return; }
    dev = s->dev;
    if (s->state == ST_SCANNING)
        lide20_scan_end(s);
    if (dev) {
        if (dev->fd >= 0) {
            usb_IsScannerReady(dev);
            usb_LampOn(dev, SANE_FALSE, SANE_FALSE);
            usbDev_close(dev);
        }
        free(dev->calFile);
        free(dev->res_list);
        free(dev->name);
        free(dev);
    }
    free(s->linebuf);
    free_devlist();
    free(s);
}

const lide20_caps_t *lide20_get_caps(const lide20_t *s) { return &s->caps; }

/* ------------------------------------------------------------------ */
/* scanning                                                            */
/* ------------------------------------------------------------------ */

static int scanmode_of(const lide20_params_t *p, int *depth)
{
    switch (p->mode) {
    case LIDE20_MODE_LINEART:
        *depth = 1;
        return COLOR_BW;
    case LIDE20_MODE_GRAY:
        *depth = (p->depth > 8) ? 16 : 8;
        return (*depth == 8) ? COLOR_256GRAY : COLOR_GRAY16;
    case LIDE20_MODE_COLOR:
        *depth = (p->depth > 8) ? 16 : 8;
        return (*depth == 8) ? COLOR_TRUE24 : COLOR_TRUE48;
    }
    return -1;
}

static int to_300(double mm) { return (int)(mm / MM_PER_INCH * 300.0 + 0.5); }

static int prepare_crop(lide20_t *s, const lide20_params_t *p, CropInfo *crop, int *scanmode, int *depth)
{
    Plustek_Device *dev = s->dev;
    int mode = scanmode_of(p, depth);

    if (mode < 0 || p->dpi < _DEF_DPI || p->dpi > s->caps.max_dpi)
        return LIDE20_E_INVALID;
    if (p->width_mm <= 0 || p->height_mm <= 0 ||
        p->left_mm < 0 || p->top_mm < 0 ||
        p->left_mm + p->width_mm > s->caps.max_width_mm + 0.01 ||
        p->top_mm + p->height_mm > s->caps.max_height_mm + 0.01)
        return LIDE20_E_INVALID;

    memset(crop, 0, sizeof(*crop));
    if (s->sim) {
        int px = (int)(p->width_mm / MM_PER_INCH * p->dpi + 0.5);
        int ln = (int)(p->height_mm / MM_PER_INCH * p->dpi + 0.5);
        crop->dwPixelsPerLine = px;
        crop->dwLinesPerArea  = ln;
        crop->dwBytesPerLine  = (mode == COLOR_BW) ? (px + 7) / 8 :
                                (mode == COLOR_256GRAY) ? px :
                                (mode == COLOR_GRAY16) ? px * 2 :
                                (mode == COLOR_TRUE24) ? px * 3 : px * 6;
        *scanmode = mode;
        return LIDE20_OK;
    }
    crop->ImgDef.xyDpi.x   = p->dpi;
    crop->ImgDef.xyDpi.y   = p->dpi;
    crop->ImgDef.crArea.x  = to_300(p->left_mm);
    crop->ImgDef.crArea.y  = to_300(p->top_mm);
    crop->ImgDef.crArea.cx = to_300(p->width_mm);
    crop->ImgDef.crArea.cy = to_300(p->height_mm);
    crop->ImgDef.wDataType = mode;
    crop->ImgDef.dwFlag    = SCANDEF_QualityScan;
    *scanmode = mode;

    if (usbDev_getCropInfo(dev, crop) < 0)
        return LIDE20_E_IO;
    return LIDE20_OK;
}

static void fill_info(lide20_image_t *img, const CropInfo *crop, int mode, int depth)
{
    img->pixels_per_line = (int)crop->dwPixelsPerLine;
    img->bytes_per_line  = (int)crop->dwBytesPerLine;
    img->lines           = (int)crop->dwLinesPerArea;
    img->depth           = depth;
    img->channels        = (mode == COLOR_TRUE24 || mode == COLOR_TRUE48) ? 3 : 1;
}

int lide20_get_image_info(lide20_t *s, const lide20_params_t *p, lide20_image_t *info)
{
    CropInfo crop;
    int mode, depth, r;

    if (!s || s->state != ST_IDLE) return LIDE20_E_STATE;
    r = prepare_crop(s, p, &crop, &mode, &depth);
    if (r != LIDE20_OK) return r;
    fill_info(info, &crop, mode, depth);
    return LIDE20_OK;
}

int lide20_scan_begin(lide20_t *s, const lide20_params_t *p, lide20_image_t *info)
{
    Plustek_Device *dev;
    CropInfo  crop;
    ScanInfo  sinfo;
    int       mode, depth, r, status;
    size_t    need;

    if (!s || s->state != ST_IDLE) return LIDE20_E_STATE;
    dev = s->dev;

    r = prepare_crop(s, p, &crop, &mode, &depth);
    if (r != LIDE20_OK) return r;
    fill_info(&s->img, &crop, mode, depth);
    if (s->img.lines <= 0 || s->img.bytes_per_line <= 0)
        return LIDE20_E_INVALID;

    cancelRead = SANE_FALSE;

    if (s->sim) {
        s->sim_line   = 0;
        s->lines_left = s->img.lines;
        s->state      = ST_SCANNING;
        if (info) *info = s->img;
        return LIDE20_OK;
    }

    sinfo.siBrightness = (short)p->brightness;
    sinfo.siContrast   = (short)p->contrast;
    memcpy(&sinfo.ImgDef, &crop.ImgDef, sizeof(ImgDef));

    if (usbDev_setScanEnv(dev, &sinfo) < 0)
        return LIDE20_E_IO;

    build_gamma(s, p->gamma);
    if (mode <= COLOR_GRAY16) {
        usbDev_setMap(dev, s->gamma[0], 4096, _MAP_MASTER);
    } else {
        usbDev_setMap(dev, s->gamma[1], 4096, _MAP_RED);
        usbDev_setMap(dev, s->gamma[2], 4096, _MAP_GREEN);
        usbDev_setMap(dev, s->gamma[3], 4096, _MAP_BLUE);
    }

    if (usbDev_startScan(dev) < 0)
        return LIDE20_E_IO;

    need = (size_t)s->img.bytes_per_line + 4096;
    if (need > s->linebuf_size) {
        free(s->linebuf);
        s->linebuf = (unsigned char *)malloc(need);
        if (!s->linebuf) {
            s->linebuf_size = 0;
            usbDev_stopScan(dev);
            return LIDE20_E_NOMEM;
        }
        s->linebuf_size = need;
    }
    memset(s->linebuf, 0, s->linebuf_size);

    /* warm-up, calibration, motor start */
    status = usbDev_Prepare(dev, s->linebuf);
    if (status != 0) {
        DBG(_DBG_ERROR, "lide20_scan_begin: usbDev_Prepare -> %d\n", status);
        usbDev_stopScan(dev);
        return (status == _E_ABORT) ? LIDE20_E_CANCELLED : LIDE20_E_IO;
    }

    s->lines_left = s->img.lines;
    s->state      = ST_SCANNING;
    if (info) *info = s->img;
    return LIDE20_OK;
}

int lide20_scan_read_line(lide20_t *s, unsigned char *dst)
{
    Plustek_Device *dev;
    int status;

    if (!s || s->state != ST_SCANNING) return LIDE20_E_STATE;
    if (s->lines_left <= 0) return LIDE20_E_STATE;

    if (s->sim) {
        int x, y = s->sim_line++, w = s->img.pixels_per_line;
        if (cancelRead) return LIDE20_E_CANCELLED;
        memset(dst, 0, (size_t)s->img.bytes_per_line);
        for (x = 0; x < w; x++) {
            int grid = (x % 100 == 0) || (y % 100 == 0);
            int r = grid ? 0 : (x * 255 / (w > 1 ? w - 1 : 1));
            int g = grid ? 0 : (y * 255 / (s->img.lines > 1 ? s->img.lines - 1 : 1));
            int b = grid ? 0 : 128;
            if (s->img.depth == 1) {
                if (((x / 16) + (y / 16)) & 1) dst[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
            } else if (s->img.channels == 3 && s->img.depth == 8) {
                dst[x * 3] = (unsigned char)r; dst[x * 3 + 1] = (unsigned char)g; dst[x * 3 + 2] = (unsigned char)b;
            } else if (s->img.channels == 3) {
                unsigned short *q = (unsigned short *)dst;
                q[x * 3] = (unsigned short)(r * 257); q[x * 3 + 1] = (unsigned short)(g * 257); q[x * 3 + 2] = (unsigned short)(b * 257);
            } else if (s->img.depth == 8) {
                dst[x] = (unsigned char)r;
            } else {
                ((unsigned short *)dst)[x] = (unsigned short)(r * 257);
            }
        }
        s->lines_left--;
        return LIDE20_OK;
    }
    dev = s->dev;

    /* the hardware layer advances UserBuf every line: rewind it */
    dev->scanning.UserBuf.pb = s->linebuf;

    status = usbDev_ReadLine(dev);
    if (status < 0) {
        DBG(_DBG_ERROR, "lide20_scan_read_line: status %d\n", status);
        return (status == _E_ABORT) ? LIDE20_E_CANCELLED : LIDE20_E_IO;
    }
    memcpy(dst, s->linebuf, (size_t)s->img.bytes_per_line);
    s->lines_left--;
    return LIDE20_OK;
}

int lide20_scan_end(lide20_t *s)
{
    if (!s || s->state != ST_SCANNING) return LIDE20_E_STATE;
    if (!s->sim) usbDev_stopScan(s->dev);
    s->state = ST_IDLE;
    cancelRead = SANE_FALSE;
    return LIDE20_OK;
}

void lide20_cancel(lide20_t *s)
{
    (void)s;
    cancelRead = SANE_TRUE;
}

const char *lide20_strerror(int err)
{
    switch (err) {
    case LIDE20_OK:          return "ok";
    case LIDE20_E_NO_DEVICE: return "no CanoScan LiDE 20 with a WinUSB driver found";
    case LIDE20_E_OPEN:      return "scanner found but could not be opened/initialised";
    case LIDE20_E_IO:        return "USB I/O error";
    case LIDE20_E_INVALID:   return "invalid scan parameters";
    case LIDE20_E_NOMEM:     return "out of memory";
    case LIDE20_E_CANCELLED: return "cancelled";
    case LIDE20_E_STATE:     return "invalid state";
    }
    return "unknown error";
}
