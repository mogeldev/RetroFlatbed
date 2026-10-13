/* Canon CanoScan LiDE 20 (04a9:220d) user-space scanner core.
 *
 * Thin, SANE-free API on top of the plustek backend hardware layer
 * (LM9832 ASIC), talking to the device through WinUSB.
 *
 * Copyright: derived from the SANE plustek backend (GPL-2.0-or-later with
 * the SANE linking exception). See LICENSE and NOTICE.md.
 */
#ifndef LIDE20_CORE_H
#define LIDE20_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef LIDE20_BUILD_DLL
#  define LIDE20_API __declspec(dllexport)
#else
#  define LIDE20_API
#endif

typedef struct lide20 lide20_t;

typedef enum {
    LIDE20_MODE_LINEART = 0,   /* 1 bit  */
    LIDE20_MODE_GRAY    = 1,   /* 8 / 16 bit */
    LIDE20_MODE_COLOR   = 2    /* 24 / 48 bit */
} lide20_mode_t;

enum {
    LIDE20_OK            =  0,
    LIDE20_E_NO_DEVICE   = -1,   /* no scanner with a WinUSB driver found   */
    LIDE20_E_OPEN        = -2,   /* device present but could not be opened  */
    LIDE20_E_IO          = -3,
    LIDE20_E_INVALID     = -4,
    LIDE20_E_NOMEM       = -5,
    LIDE20_E_CANCELLED   = -6,
    LIDE20_E_STATE       = -7
};

typedef struct {
    char   model[64];
    int    optic_dpi;        /* native optical resolution (600)             */
    int    max_dpi;          /* highest resolution offered (1200)           */
    double max_width_mm;
    double max_height_mm;
} lide20_caps_t;

typedef struct {
    lide20_mode_t mode;
    int    depth;            /* bits per channel: 1 (lineart), 8 or 16      */
    int    dpi;              /* 50 .. max_dpi                               */
    double left_mm, top_mm;  /* scan window origin                          */
    double width_mm, height_mm;
    int    brightness;       /* -100 .. 100                                 */
    int    contrast;         /* -100 .. 100                                 */
    double gamma;            /* 0 = default (1.0)                           */
} lide20_params_t;

typedef struct {
    int pixels_per_line;
    int bytes_per_line;
    int lines;
    int depth;               /* bits per sample                              */
    int channels;            /* 1 or 3                                       */
} lide20_image_t;

/* Number of supported scanners that are currently present with WinUSB bound. */
LIDE20_API int  lide20_device_count(void);
/* True if the USB device is attached but no WinUSB driver exposes it. */
LIDE20_API int  lide20_device_present_without_driver(void);

LIDE20_API int  lide20_open(int index, lide20_t **out);
LIDE20_API void lide20_close(lide20_t *s);
LIDE20_API const lide20_caps_t *lide20_get_caps(const lide20_t *s);

/* Computes the resulting image geometry without touching the hardware. */
LIDE20_API int  lide20_get_image_info(lide20_t *s, const lide20_params_t *p, lide20_image_t *info);

/* Starts a scan (lamp warm-up + calibration happen here, may take a while). */
LIDE20_API int  lide20_scan_begin(lide20_t *s, const lide20_params_t *p, lide20_image_t *info);
/* Reads the next scan line into dst (info.bytes_per_line bytes).
 * Returns LIDE20_OK, or LIDE20_E_STATE once all lines have been delivered. */
LIDE20_API int  lide20_scan_read_line(lide20_t *s, unsigned char *dst);
LIDE20_API int  lide20_scan_end(lide20_t *s);
/* Thread-safe: requests abort of a running calibration/scan. */
LIDE20_API void lide20_cancel(lide20_t *s);

LIDE20_API const char *lide20_strerror(int err);
LIDE20_API void lide20_set_debug(int level);

#ifdef __cplusplus
}
#endif
#endif
