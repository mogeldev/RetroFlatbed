/* twain_test - minimal TWAIN application + DSM stand-in.
 * Loads LiDE20.ds directly and drives the full state machine:
 *   open -> set caps -> enable (no UI) -> XFERREADY -> transfer -> close.
 *
 *   twain_test [-dsm [path\twaindsm.dll]]   (go through the real DSM and discover the source)
 *              [-ds path\RetroFlatbed.ds] [-x native|memory] [-r dpi] [-m color|gray|bw]
 *              [-d 8|16] [-w mm] [-h mm] [-ui] [-abort ms] -o out.(bmp|pnm)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "twain.h"

static TW_IDENTITY g_app, g_ds;
static volatile int g_xferready, g_closereq;

static TW_UINT16 FAR PASCAL dsm_entry(pTW_IDENTITY o, pTW_IDENTITY d, TW_UINT32 dg,
                                      TW_UINT16 dat, TW_UINT16 msg, TW_MEMREF data)
{
    (void)o; (void)d; (void)data;
    if (dg == DG_CONTROL && dat == DAT_NULL) {
        if (msg == MSG_XFERREADY)   g_xferready = 1;
        if (msg == MSG_CLOSEDSREQ)  g_closereq = 1;
    }
    return TWRC_SUCCESS;
}
static TW_HANDLE  FAR PASCAL m_alloc(TW_UINT32 n) { return (TW_HANDLE)GlobalAlloc(GHND, n); }
static void       FAR PASCAL m_free(TW_HANDLE h)  { GlobalFree((HGLOBAL)h); }
static TW_MEMREF  FAR PASCAL m_lock(TW_HANDLE h)  { return GlobalLock((HGLOBAL)h); }
static void       FAR PASCAL m_unlock(TW_HANDLE h){ GlobalUnlock((HGLOBAL)h); }

static DSENTRYPROC ds_entry;
static DSMENTRYPROC g_dsm;      /* -dsm mode: every call goes through the real DSM */
static HWND g_parent;

static TW_UINT16 ds_call(TW_UINT32 dg, TW_UINT16 dat, TW_UINT16 msg, TW_MEMREF p)
{
    if (g_dsm) return g_dsm(&g_app, &g_ds, dg, dat, msg, p);
    return ds_entry(&g_app, dg, dat, msg, p);
}
#define DS(dg, dat, msg, p) ds_call((dg), (dat), (msg), (TW_MEMREF)(p))

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static TW_UINT16 cc(void)
{
    TW_STATUS st;
    memset(&st, 0, sizeof(st));
    DS(DG_CONTROL, DAT_STATUS, MSG_GET, &st);
    return st.ConditionCode;
}

static TW_FIX32 fix(double f)
{
    TW_FIX32 r; LONG v = (LONG)floor(f * 65536.0 + 0.5);
    r.Whole = (TW_INT16)(v >> 16); r.Frac = (TW_UINT16)(v & 0xFFFF); return r;
}

static TW_UINT16 set_one(TW_UINT16 cap, TW_UINT16 type, TW_UINT32 item)
{
    TW_CAPABILITY c; TW_ONEVALUE *p; TW_UINT16 rc;
    c.Cap = cap; c.ConType = TWON_ONEVALUE;
    c.hContainer = GlobalAlloc(GHND, sizeof(TW_ONEVALUE));
    p = (TW_ONEVALUE *)GlobalLock(c.hContainer);
    p->ItemType = type; p->Item = item;
    GlobalUnlock(c.hContainer);
    rc = DS(DG_CONTROL, DAT_CAPABILITY, MSG_SET, &c);
    GlobalFree(c.hContainer);
    return rc;
}

static int get_cur(TW_UINT16 cap, TW_UINT32 *val)
{
    TW_CAPABILITY c; TW_ONEVALUE *p; TW_UINT16 rc;
    memset(&c, 0, sizeof(c));
    c.Cap = cap; c.ConType = TWON_DONTCARE16;
    rc = DS(DG_CONTROL, DAT_CAPABILITY, MSG_GETCURRENT, &c);
    if (rc != TWRC_SUCCESS) return 0;
    p = (TW_ONEVALUE *)GlobalLock(c.hContainer);
    *val = p->Item;
    GlobalUnlock(c.hContainer);
    GlobalFree(c.hContainer);
    return 1;
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG m;
    while (GetTickCount() < end && !g_xferready && !g_closereq) {
        while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) {
            if (g_dsm) {           /* a TWAIN app offers every message to the DSM first */
                TW_EVENT ev;
                ev.pEvent = (TW_MEMREF)&m;
                ev.TWMessage = MSG_NULL;
                if (g_dsm(&g_app, &g_ds, DG_CONTROL, DAT_EVENT, MSG_PROCESSEVENT, &ev) == TWRC_DSEVENT) {
                    if (ev.TWMessage == MSG_XFERREADY)  g_xferready = 1;
                    if (ev.TWMessage == MSG_CLOSEDSREQ) g_closereq = 1;
                    continue;
                }
            }
            TranslateMessage(&m); DispatchMessage(&m);
        }
        Sleep(10);
    }
}

static void write_bmp(const char *fn, HGLOBAL h)
{
    BITMAPINFOHEADER *bih = (BITMAPINFOHEADER *)GlobalLock(h);
    BITMAPFILEHEADER fh;
    size_t pal = bih->biClrUsed * 4, total = bih->biSize + pal + bih->biSizeImage;
    FILE *f = fopen(fn, "wb");
    fh.bfType = 0x4D42; fh.bfSize = (DWORD)(sizeof(fh) + total); fh.bfReserved1 = fh.bfReserved2 = 0;
    fh.bfOffBits = (DWORD)(sizeof(fh) + bih->biSize + pal);
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(bih, 1, total, f);
    fclose(f);
    GlobalUnlock(h);
}

int main(int argc, char **argv)
{
    const char *dsfile = "RetroFlatbed.ds", *out = NULL, *xm = "native", *mode = "color";
    int dpi = 100, depth = 8, ui = 0, i, abort_ms = 0, use_dsm = 0;
    const char *dsmfile = "twaindsm.dll";
    double wmm = 0, hmm = 0;
    HMODULE mod;
    TW_ENTRYPOINT ep;
    TW_UINT32 v;
    TW_UINT16 rc;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-ds") && i + 1 < argc) dsfile = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-x") && i + 1 < argc) xm = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) dpi = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) depth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc) wmm = atof(argv[++i]);
        else if (!strcmp(argv[i], "-h") && i + 1 < argc) hmm = atof(argv[++i]);
        else if (!strcmp(argv[i], "-dsm")) { use_dsm = 1; if (i + 1 < argc && argv[i + 1][0] != '-') dsmfile = argv[++i]; }
        else if (!strcmp(argv[i], "-ui")) ui = 1;
        else if (!strcmp(argv[i], "-abort") && i + 1 < argc) abort_ms = atoi(argv[++i]);
    }

    memset(&g_app, 0, sizeof(g_app));
    g_app.Id = 0; g_app.ProtocolMajor = 2; g_app.ProtocolMinor = 5;
    g_app.SupportedGroups = DG_CONTROL | DG_IMAGE | DF_APP2;
    strcpy(g_app.Manufacturer, "lide20"); strcpy(g_app.ProductFamily, "tests"); strcpy(g_app.ProductName, "twain_test");

    if (use_dsm) {
        /* behave like a real TWAIN application: open the DSM, discover and open the source by name */
        WNDCLASSA wc;
        HMODULE dm = LoadLibraryA(dsmfile);
        TW_IDENTITY found;
        TW_UINT16 r;
        int have = 0;

        if (!dm) { printf("cannot load DSM %s (%lu)\n", dsmfile, GetLastError()); return 2; }
        g_dsm = (DSMENTRYPROC)GetProcAddress(dm, "DSM_Entry");
        if (!g_dsm) { printf("DSM_Entry not exported\n"); return 2; }

        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "twain_test_parent";
        RegisterClassA(&wc);
        g_parent = CreateWindowA("twain_test_parent", "twain_test", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);

        r = g_dsm(&g_app, NULL, DG_CONTROL, DAT_PARENT, MSG_OPENDSM, &g_parent);
        if (r != TWRC_SUCCESS) { printf("OPENDSM failed rc=%u\n", r); return 1; }
        printf("DSM opened\n");

        memset(&found, 0, sizeof(found));
        for (r = g_dsm(&g_app, NULL, DG_CONTROL, DAT_IDENTITY, MSG_GETFIRST, &found); r == TWRC_SUCCESS;
             r = g_dsm(&g_app, NULL, DG_CONTROL, DAT_IDENTITY, MSG_GETNEXT, &found)) {
            printf("  source: %s\n", found.ProductName);
            if (strstr(found.ProductName, "LiDE 20")) { g_ds = found; have = 1; break; }
        }
        CHECK(have, "Canon CanoScan LiDE 20 not listed by the DSM");
        if (!have) return 1;
        r = g_dsm(&g_app, NULL, DG_CONTROL, DAT_IDENTITY, MSG_OPENDS, &g_ds);
        if (r != TWRC_SUCCESS) { printf("OPENDS via DSM failed rc=%u\n", r); return 1; }
        printf("DS opened through the DSM (state 4)\n");
    } else {
        mod = LoadLibraryA(dsfile);
        if (!mod) { printf("cannot load %s (%lu)\n", dsfile, GetLastError()); return 2; }
        ds_entry = (DSENTRYPROC)GetProcAddress(mod, "DS_Entry");
        if (!ds_entry) { printf("DS_Entry not exported\n"); return 2; }
        g_app.Id = 1;

        memset(&ep, 0, sizeof(ep));
        ep.Size = sizeof(ep); ep.DSM_Entry = dsm_entry;
        ep.DSM_MemAllocate = m_alloc; ep.DSM_MemFree = m_free;
        ep.DSM_MemLock = m_lock; ep.DSM_MemUnlock = m_unlock;
        rc = ds_entry(&g_app, DG_CONTROL, DAT_ENTRYPOINT, MSG_SET, &ep);
        CHECK(rc == TWRC_SUCCESS, "ENTRYPOINT set rc=%u", rc);

        memset(&g_ds, 0, sizeof(g_ds));
        g_ds.Id = 2;
        rc = DS(DG_CONTROL, DAT_IDENTITY, MSG_OPENDS, &g_ds);
        if (rc != TWRC_SUCCESS) { printf("OPENDS failed rc=%u cc=%u\n", rc, cc()); return 1; }
        printf("DS opened (state 4)\n");
    }

    /* capability negotiation */
    CHECK(set_one(ICAP_UNITS, TWTY_UINT16, TWUN_INCHES) == TWRC_SUCCESS, "units");
    CHECK(set_one(ICAP_PIXELTYPE, TWTY_UINT16, !strcmp(mode, "bw") ? TWPT_BW : !strcmp(mode, "gray") ? TWPT_GRAY : TWPT_RGB) == TWRC_SUCCESS, "pixeltype");
    if (strcmp(mode, "bw"))
        CHECK(set_one(ICAP_BITDEPTH, TWTY_UINT16, depth * (strcmp(mode, "gray") ? 3 : 1)) == TWRC_SUCCESS, "bitdepth");
    CHECK(set_one(ICAP_XFERMECH, TWTY_UINT16, !strcmp(xm, "memory") ? TWSX_MEMORY : TWSX_NATIVE) == TWRC_SUCCESS, "xfermech");
    {
        TW_FIX32 f = fix(dpi); TW_UINT32 item; memcpy(&item, &f, 4);
        CHECK(set_one(ICAP_XRESOLUTION, TWTY_FIX32, item) != TWRC_FAILURE, "xres %d", dpi);
        CHECK(set_one(ICAP_YRESOLUTION, TWTY_FIX32, item) != TWRC_FAILURE, "yres %d", dpi);
    }
    CHECK(get_cur(ICAP_PIXELTYPE, &v) && (int)v == (!strcmp(mode, "bw") ? 0 : !strcmp(mode, "gray") ? 1 : 2), "pixeltype readback");
    CHECK(set_one(ICAP_COMPRESSION, TWTY_UINT16, TWCP_NONE) == TWRC_SUCCESS, "compression");
    CHECK(set_one(ICAP_UNITS, TWTY_UINT16, 99) == TWRC_FAILURE && cc() == TWCC_BADVALUE, "bad units rejected");
    CHECK(set_one(0x7fff, TWTY_UINT16, 1) == TWRC_FAILURE && cc() == TWCC_CAPUNSUPPORTED, "unknown cap rejected");

    /* supported caps list */
    {
        TW_CAPABILITY c; memset(&c, 0, sizeof(c)); c.Cap = CAP_SUPPORTEDCAPS; c.ConType = TWON_DONTCARE16;
        rc = DS(DG_CONTROL, DAT_CAPABILITY, MSG_GET, &c);
        CHECK(rc == TWRC_SUCCESS && c.ConType == TWON_ARRAY, "CAP_SUPPORTEDCAPS");
        if (rc == TWRC_SUCCESS) {
            TW_ARRAY *a = (TW_ARRAY *)GlobalLock(c.hContainer);
            printf("supports %lu capabilities\n", (unsigned long)a->NumItems);
            GlobalUnlock(c.hContainer); GlobalFree(c.hContainer);
        }
    }

    /* scan area */
    {
        TW_IMAGELAYOUT il;
        memset(&il, 0, sizeof(il));
        CHECK(DS(DG_IMAGE, DAT_IMAGELAYOUT, MSG_GET, &il) == TWRC_SUCCESS, "layout get");
        printf("full frame: %.2f x %.2f in\n", il.Frame.Right.Whole + il.Frame.Right.Frac / 65536.0,
               il.Frame.Bottom.Whole + il.Frame.Bottom.Frac / 65536.0);
        if (wmm > 0 && hmm > 0) {
            il.Frame.Left = fix(0); il.Frame.Top = fix(0);
            il.Frame.Right = fix(wmm / 25.4); il.Frame.Bottom = fix(hmm / 25.4);
            rc = DS(DG_IMAGE, DAT_IMAGELAYOUT, MSG_SET, &il);
            CHECK(rc == TWRC_SUCCESS || rc == TWRC_CHECKSTATUS, "layout set rc=%u", rc);
        }
    }

    /* illegal in state 4 */
    {
        TW_IMAGEINFO ii;
        CHECK(DS(DG_IMAGE, DAT_IMAGEINFO, MSG_GET, &ii) == TWRC_FAILURE && cc() == TWCC_SEQERROR, "IMAGEINFO in state 4 -> SEQERROR");
    }

    /* enable */
    {
        TW_USERINTERFACE u; memset(&u, 0, sizeof(u));
        u.ShowUI = ui; u.hParent = g_parent;
        rc = DS(DG_CONTROL, DAT_USERINTERFACE, MSG_ENABLEDS, &u);
        CHECK(rc == TWRC_SUCCESS, "ENABLEDS rc=%u cc=%u", rc, cc());
        printf("enabled (state 5), waiting for XFERREADY...\n");
    }
    if (abort_ms > 0) {
        pump(abort_ms);
        printf("aborting after %d ms (xferready=%d)\n", abort_ms, g_xferready);
        goto disable;
    }
    pump(300000);
    if (g_closereq) { printf("DS requested close (cancelled or failed)\n"); goto close_ds; }
    CHECK(g_xferready, "no XFERREADY within timeout");
    if (!g_xferready) goto disable;
    printf("XFERREADY (state 6)\n");

    {
        TW_PENDINGXFERS px; TW_IMAGEINFO ii;
        memset(&px, 0, sizeof(px));
        CHECK(DS(DG_CONTROL, DAT_PENDINGXFERS, MSG_GET, &px) == TWRC_SUCCESS && px.Count == 1, "pending count %u", px.Count);
        memset(&ii, 0, sizeof(ii));
        rc = DS(DG_IMAGE, DAT_IMAGEINFO, MSG_GET, &ii);
        CHECK(rc == TWRC_SUCCESS, "IMAGEINFO");
        printf("image %ld x %ld, %d bpp, %d samples, pixeltype %d\n", (long)ii.ImageWidth, (long)ii.ImageLength,
               ii.BitsPerPixel, ii.SamplesPerPixel, ii.PixelType);

        if (!strcmp(xm, "native")) {
            TW_HANDLE h = NULL;
            rc = DS(DG_IMAGE, DAT_IMAGENATIVEXFER, MSG_GET, &h);
            CHECK(rc == TWRC_XFERDONE && h, "native xfer rc=%u cc=%u", rc, cc());
            if (rc == TWRC_XFERDONE && h) {
                BITMAPINFOHEADER *b = (BITMAPINFOHEADER *)GlobalLock((HGLOBAL)h);
                CHECK(b->biWidth == ii.ImageWidth && b->biHeight == ii.ImageLength, "DIB size mismatch");
                GlobalUnlock((HGLOBAL)h);
                if (out) write_bmp(out, (HGLOBAL)h);
                GlobalFree((HGLOBAL)h);
                printf("native transfer done%s%s\n", out ? ", wrote " : "", out ? out : "");
            }
        } else {
            TW_SETUPMEMXFER sm; TW_IMAGEMEMXFER mx; unsigned char *buf; FILE *f = NULL;
            long rows = 0;
            memset(&sm, 0, sizeof(sm));
            CHECK(DS(DG_CONTROL, DAT_SETUPMEMXFER, MSG_GET, &sm) == TWRC_SUCCESS, "setupmemxfer");
            buf = (unsigned char *)malloc(sm.Preferred);
            if (out) {
                f = fopen(out, "wb");
                if (ii.BitsPerPixel == 1) fprintf(f, "P4\n%ld %ld\n", (long)ii.ImageWidth, (long)ii.ImageLength);
                else fprintf(f, "P%d\n%ld %ld\n%d\n", ii.SamplesPerPixel == 3 ? 6 : 5, (long)ii.ImageWidth, (long)ii.ImageLength,
                             ii.BitsPerSample[0] > 8 ? 65535 : 255);
            }
            for (;;) {
                memset(&mx, 0, sizeof(mx));
                mx.Memory.Flags = TWMF_APPOWNS | TWMF_POINTER;
                mx.Memory.Length = sm.Preferred;
                mx.Memory.TheMem = buf;
                rc = DS(DG_IMAGE, DAT_IMAGEMEMXFER, MSG_GET, &mx);
                if (rc != TWRC_SUCCESS && rc != TWRC_XFERDONE) { CHECK(0, "memxfer rc=%u cc=%u", rc, cc()); break; }
                if (f) {
                    if (ii.BitsPerPixel == 1) {      /* TWAIN: 1 = white, PBM: 1 = black */
                        unsigned long k;
                        for (k = 0; k < mx.BytesWritten; k++) buf[k] = (unsigned char)~buf[k];
                    }
                    if (ii.BitsPerSample[0] > 8) {   /* PNM 16-bit is big endian */
                        unsigned long k;
                        for (k = 0; k + 1 < mx.BytesWritten; k += 2) { unsigned char t = buf[k]; buf[k] = buf[k + 1]; buf[k + 1] = t; }
                    }
                    fwrite(buf, 1, mx.BytesWritten, f);
                }
                rows += mx.Rows;
                if (rc == TWRC_XFERDONE) break;
            }
            CHECK(rows == ii.ImageLength, "rows %ld != %ld", rows, (long)ii.ImageLength);
            if (f) fclose(f);
            free(buf);
            printf("memory transfer done: %ld rows%s%s\n", rows, out ? ", wrote " : "", out ? out : "");
        }

        memset(&px, 0, sizeof(px));
        rc = DS(DG_CONTROL, DAT_PENDINGXFERS, MSG_ENDXFER, &px);
        CHECK(rc == TWRC_SUCCESS && px.Count == 0, "ENDXFER count=%u", px.Count);
    }

disable:
    {
        TW_USERINTERFACE u; memset(&u, 0, sizeof(u));
        CHECK(DS(DG_CONTROL, DAT_USERINTERFACE, MSG_DISABLEDS, &u) == TWRC_SUCCESS, "DISABLEDS");
    }
close_ds:
    if (g_closereq) {
        TW_USERINTERFACE u; memset(&u, 0, sizeof(u));
        DS(DG_CONTROL, DAT_USERINTERFACE, MSG_DISABLEDS, &u);
    }
    CHECK(DS(DG_CONTROL, DAT_IDENTITY, MSG_CLOSEDS, &g_ds) == TWRC_SUCCESS, "CLOSEDS");
    if (g_dsm) CHECK(g_dsm(&g_app, NULL, DG_CONTROL, DAT_PARENT, MSG_CLOSEDSM, &g_parent) == TWRC_SUCCESS, "CLOSEDSM");
    printf(failures ? "RESULT: %d FAILURE(S)\n" : "RESULT: all checks passed\n", failures);
    return failures ? 1 : 0;
}
