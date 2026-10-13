/* retroflatbed-scan - command line scanner for the Canon CanoScan LiDE 20.
 * Writes PNM (P4/P5/P6, 16-bit big-endian per the format). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "lide20_core.h"

static lide20_t *g_scanner;

static void on_sigint(int sig)
{
    (void)sig;
    if (g_scanner) lide20_cancel(g_scanner);
}

static void usage(void)
{
    puts("usage: retroflatbed-scan [options] -o out.pnm\n"
         "  --list                 list detected scanners\n"
         "  -m lineart|gray|color  scan mode (default color)\n"
         "  -d 8|16                bit depth (default 8)\n"
         "  -r DPI                 resolution (default 300, 50..1200)\n"
         "  -x MM -y MM            top-left corner in mm (default 0,0)\n"
         "  -w MM -h MM            size in mm (default: full bed)\n"
         "  -b N -c N              brightness / contrast, -100..100\n"
         "  -g GAMMA               gamma (default 1.0)\n"
         "  -v                     verbose debug output\n");
}

int main(int argc, char **argv)
{
    lide20_params_t p;
    lide20_image_t  img;
    lide20_t       *s = NULL;
    const char     *out = NULL;
    int             list = 0, i, r, full_w = 1, full_h = 1;
    FILE           *f;
    unsigned char  *line;

    memset(&p, 0, sizeof(p));
    p.mode = LIDE20_MODE_COLOR;
    p.depth = 8;
    p.dpi = 300;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--list")) list = 1;
        else if (!strcmp(a, "-v")) lide20_set_debug(10);
        else if (i + 1 < argc && !strcmp(a, "-o")) out = argv[++i];
        else if (i + 1 < argc && !strcmp(a, "-m")) {
            const char *m = argv[++i];
            p.mode = !strcmp(m, "lineart") ? LIDE20_MODE_LINEART :
                     !strcmp(m, "gray")    ? LIDE20_MODE_GRAY : LIDE20_MODE_COLOR;
        }
        else if (i + 1 < argc && !strcmp(a, "-d")) p.depth = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(a, "-r")) p.dpi = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(a, "-x")) p.left_mm = atof(argv[++i]);
        else if (i + 1 < argc && !strcmp(a, "-y")) p.top_mm = atof(argv[++i]);
        else if (i + 1 < argc && !strcmp(a, "-w")) { p.width_mm = atof(argv[++i]); full_w = 0; }
        else if (i + 1 < argc && !strcmp(a, "-h")) { p.height_mm = atof(argv[++i]); full_h = 0; }
        else if (i + 1 < argc && !strcmp(a, "-b")) p.brightness = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(a, "-c")) p.contrast = atoi(argv[++i]);
        else if (i + 1 < argc && !strcmp(a, "-g")) p.gamma = atof(argv[++i]);
        else { usage(); return 2; }
    }

    if (list) {
        int n = lide20_device_count();
        printf("%d usable CanoScan LiDE 20 device(s)\n", n);
        if (!n && lide20_device_present_without_driver())
            puts("A LiDE 20 (04a9:220d) is attached but no WinUSB driver is bound.\n"
                 "Run driver\\install.ps1 as administrator (or use Zadig -> WinUSB).");
        return n ? 0 : 1;
    }
    if (!out) { usage(); return 2; }

    r = lide20_open(0, &s);
    if (r != LIDE20_OK) {
        fprintf(stderr, "open: %s\n", lide20_strerror(r));
        if (r == LIDE20_E_NO_DEVICE && lide20_device_present_without_driver())
            fputs("The scanner is attached but has no WinUSB driver. "
                  "Run driver\\install.ps1 as administrator.\n", stderr);
        return 1;
    }
    g_scanner = s;
    signal(SIGINT, on_sigint);

    {
        const lide20_caps_t *c = lide20_get_caps(s);
        printf("%s: optical %d dpi, max %d dpi, bed %.1f x %.1f mm\n",
               c->model, c->optic_dpi, c->max_dpi, c->max_width_mm, c->max_height_mm);
        if (full_w) p.width_mm  = c->max_width_mm  - p.left_mm;
        if (full_h) p.height_mm = c->max_height_mm - p.top_mm;
    }

    printf("scanning (warm-up and calibration may take up to a minute)...\n");
    r = lide20_scan_begin(s, &p, &img);
    if (r != LIDE20_OK) {
        fprintf(stderr, "scan_begin: %s\n", lide20_strerror(r));
        lide20_close(s);
        return 1;
    }
    printf("image: %d x %d, %d bit x %d ch\n", img.pixels_per_line, img.lines, img.depth, img.channels);

    f = fopen(out, "wb");
    line = (unsigned char *)malloc((size_t)img.bytes_per_line);
    if (!f || !line) { fprintf(stderr, "cannot open output/alloc\n"); lide20_close(s); return 1; }

    if (img.depth == 1)
        fprintf(f, "P4\n%d %d\n", img.pixels_per_line, img.lines);
    else
        fprintf(f, "P%d\n%d %d\n%d\n", img.channels == 3 ? 6 : 5,
                img.pixels_per_line, img.lines, img.depth > 8 ? 65535 : 255);

    for (i = 0; i < img.lines; i++) {
        r = lide20_scan_read_line(s, line);
        if (r != LIDE20_OK) {
            fprintf(stderr, "\nread_line %d: %s\n", i, lide20_strerror(r));
            break;
        }
        /* 1-bit data: 1 = black, same as PBM, no conversion needed */
        if (img.depth > 8) {
            int b;   /* PNM 16-bit is big-endian, host data is little-endian */
            for (b = 0; b + 1 < img.bytes_per_line; b += 2) {
                unsigned char t = line[b]; line[b] = line[b + 1]; line[b + 1] = t;
            }
        }
        fwrite(line, 1, (size_t)img.bytes_per_line, f);
        if ((i & 31) == 0) { printf("\rline %d/%d", i, img.lines); fflush(stdout); }
    }
    printf("\rline %d/%d\n", i, img.lines);

    fclose(f);
    free(line);
    lide20_scan_end(s);
    lide20_close(s);
    printf("%s %s\n", r == LIDE20_OK ? "wrote" : "partial image written to", out);
    return r == LIDE20_OK ? 0 : 1;
}
