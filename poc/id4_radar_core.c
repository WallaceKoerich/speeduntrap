/*
 * Radar proximity + WalControl overlay.
 * Does not touch HmiMain3D logic. When popup is off, never calls WalControl
 * except to restore HMI if an alert was already on screen.
 *
 * Pack: /fs/sda0/radar/points.txt  (also /fs/usb0 and /mnt/umass0)
 * GPS:  OEM PosReal.DNAVIPos 19.0 fused DR (same as map cursor).
 *       Fallback: radar/fix.txt only if DSI is not attached.
 * Speed fallback: /fs/sda0/radar/speed.txt
 * Popup persist: /fs/sda0/radar_popup.on  ("1" or "0")
 * Range persist: /fs/sda0/radar_range.m
 *
 * ZBE (observe-only, same CCS path as id4_aa_x86):
 *   MENU     — hide overlay, restore HmiMain3D (alert stays armed)
 *   OPTION×3 — show overlay again while still in range
 * Auto show/hide on range is unchanged.
 */

#include "id4_radar.h"
#include "id4_aa_am.h"
#include "id4_aa_cm.h"
#include "id4_aa_display.h"
#include "id4_aa_zbe.h"
#include "id4_radar_chime.h"
#ifdef SPEEDUNTRAP
#include "speeduntrap_ui.h"
#endif

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define W 1280
#define H 480
#define PASS_M 80.0
#define MAX_POINTS 20000
#define RANGE_DEFAULT 500
#define OPT_WINDOW_MS 2000

static const int k_ranges[] = {
    50, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000
};
#define N_RANGES ((int)(sizeof(k_ranges) / sizeof(k_ranges[0])))

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) >> 3) & 0x1F)))

static RadarPoint *g_pts;
static int g_npts;
static int g_popup = 1;
static int g_wal_ready;
static int g_showing;

int radar_overlay_showing(void)
{
    return g_showing;
}
static int g_alert;
static int g_user_min; /* MENU hid overlay; still in-range until pass */
static int g_demo;     /* OPTION x3: show overlay even outside Range */
static int g_hit = -1;
static int g_range_i = 4; /* 500 m */
static double g_enter_m = RANGE_DEFAULT;
static double g_closest = 1e9;
static double g_prev_dist = -1;
static double g_lat, g_lon, g_hdg, g_speed = -1;
static double g_coslat = 1.0;
static int g_have_gps;
static int g_have_hdg;
static char g_status[80] = "Ready";
static char g_range_line[32] = "Range 500 m";
static aa_display g_disp;
static uint16_t g_fb[W * H];
static char g_last_frame[160];
static int g_verbose;
static int g_test_mode;
static volatile int g_stop_run;
static char g_fix_src[96];
static int g_fix_tokens;
static int g_test_in_pack = -1;
static int g_opt_n;
static long g_opt_ms;
static int g_chimed_hit = -1;

static const char *k_pack[] = {
    "/fs/sda0/radar/points.txt",
    "/fs/usb0/radar/points.txt",
    "/mnt/umass0/radar/points.txt",
    "/mnt/umass1/radar/points.txt",
    NULL
};
static const char *k_fix[] = {
    "/fs/sda0/radar/fix.txt",
    "/fs/usb0/radar/fix.txt",
    "/dev/shmem/radar_fix.txt",
    NULL
};
static const char *k_speed[] = {
    "/fs/sda0/radar/speed.txt",
    "/fs/usb0/radar/speed.txt",
    "/dev/shmem/radar_speed.txt",
    NULL
};
static const char *k_persist[] = {
    "/fs/sda0/radar_popup.on",
    "/dev/shmem/radar_popup.on",
    NULL
};
static const char *k_range[] = {
    "/fs/sda0/radar_range.m",
    "/dev/shmem/radar_range.m",
    NULL
};

/* 5x7, bit0 = top of column. ASCII 32..90. */
static const unsigned char k_f5[59][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
};

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static void set_status(const char *s)
{
    snprintf(g_status, sizeof(g_status), "%s", s);
}

static int range_index_for(int m)
{
    int i, best = 4, bd = 10000;
    for (i = 0; i < N_RANGES; i++) {
        int d = k_ranges[i] - m;
        if (d < 0)
            d = -d;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

static void apply_range_i(int idx)
{
    if (idx < 0)
        idx = 0;
    if (idx >= N_RANGES)
        idx = N_RANGES - 1;
    g_range_i = idx;
    g_enter_m = (double)k_ranges[idx];
    snprintf(g_range_line, sizeof(g_range_line), "Range %d m", k_ranges[idx]);
}

static void persist_range(void)
{
    FILE *f = fopen(k_range[0], "w");
    if (!f)
        f = fopen(k_range[1], "w");
    if (!f)
        return;
    fprintf(f, "%d\n", k_ranges[g_range_i]);
    fclose(f);
}

static void load_range(void)
{
    int i, m = RANGE_DEFAULT;
    for (i = 0; k_range[i]; i++) {
        FILE *f = fopen(k_range[i], "r");
        if (!f)
            continue;
        if (fscanf(f, "%d", &m) != 1)
            m = RANGE_DEFAULT;
        fclose(f);
        break;
    }
    apply_range_i(range_index_for(m));
}

static int read_flag_file(const char *const *paths, int *out)
{
    int i;
    for (i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "r");
        char buf[16];
        if (!f)
            continue;
        buf[0] = 0;
        if (fgets(buf, sizeof(buf), f)) {
            *out = (buf[0] != '0');
            fclose(f);
            return 0;
        }
        fclose(f);
    }
    return -1;
}

static void persist_popup(int on)
{
    FILE *f = fopen(k_persist[0], "w");
    if (!f)
        f = fopen(k_persist[1], "w");
    if (!f)
        return;
    fprintf(f, "%d\n", on ? 1 : 0);
    fclose(f);
}

static void load_popup(void)
{
    int v = 1;
    if (read_flag_file(k_persist, &v) == 0)
        g_popup = v ? 1 : 0;
}

static void clip_copy(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!src)
        src = "";
    n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int load_pack_path(const char *path)
{
    FILE *f;
    char line[256];
    int n = 0;

    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!g_pts) {
        g_pts = (RadarPoint *)calloc(MAX_POINTS, sizeof(RadarPoint));
        if (!g_pts) {
            fclose(f);
            return -1;
        }
    }
    while (fgets(line, sizeof(line), f) && n < MAX_POINTS) {
        RadarPoint p;
        char via[64], fab[64], model[64];
        int hd;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        memset(&p, 0, sizeof(p));
        via[0] = fab[0] = model[0] = 0;
        hd = -1;
        if (sscanf(line, "%lf %lf %d %d %d %63s %63s %63s",
                   &p.lat, &p.lon, &p.leves, &p.pesados, &hd,
                   via, fab, model) < 4)
            continue;
        p.heading = hd;
        clip_copy(p.via, sizeof(p.via), via);
        clip_copy(p.fab, sizeof(p.fab), fab);
        clip_copy(p.model, sizeof(p.model), model);
        g_pts[n++] = p;
    }
    fclose(f);
    g_npts = n;
    printf("radar pack %s: %d points\n", path, n);
    {
        int j;
        g_test_in_pack = -1;
        for (j = 0; j < n; j++) {
            if (fabs(g_pts[j].lat + 26.962599) < 0.00005 &&
                fabs(g_pts[j].lon + 50.421094) < 0.00005) {
                g_test_in_pack = j;
                printf("radar pack: TEST control at index %d (%.6f %.6f)\n",
                       j, g_pts[j].lat, g_pts[j].lon);
                break;
            }
        }
        if (g_test_in_pack < 0)
            printf("radar pack: TEST control NOT in this file\n");
    }
    return n > 0 ? 0 : -1;
}

static void load_pack(void)
{
    int i;
    for (i = 0; k_pack[i]; i++) {
        if (load_pack_path(k_pack[i]) == 0)
            return;
    }
    /* Built-in TEST point so the overlay can be proven without a USB pack. */
    g_pts = (RadarPoint *)calloc(1, sizeof(RadarPoint));
    if (!g_pts)
        return;
    g_pts[0].lat = -26.962599;
    g_pts[0].lon = -50.421094;
    g_pts[0].leves = 30;
    g_pts[0].pesados = 20;
    g_pts[0].heading = -1;
    clip_copy(g_pts[0].via, sizeof(g_pts[0].via), "TEST");
    clip_copy(g_pts[0].fab, sizeof(g_pts[0].fab), "TEST");
    clip_copy(g_pts[0].model, sizeof(g_pts[0].model), "TEST");
    g_npts = 1;
    g_test_in_pack = 0;
    printf("radar pack: built-in TEST point only\n");
}

static void refresh_coslat(void)
{
    g_coslat = cos(g_lat * M_PI / 180.0);
    if (g_coslat < 0.2)
        g_coslat = 0.2;
}

static double haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * 111320.0;
    double dlon = (lon2 - lon1) * 111320.0 * g_coslat;
    return sqrt(dlat * dlat + dlon * dlon);
}

static int heading_ok(const RadarPoint *p)
{
    double d;
    if (p->heading < 0 || !g_have_hdg)
        return 1;
    d = fabs(g_hdg - (double)p->heading);
    while (d > 180.0)
        d = 360.0 - d;
    return d <= 90.0;
}

/*
 * fix.txt is "lat lon [heading]" only.
 * Pack rows are "lat lon leves pesados heading via fab model" (8 tokens).
 * A pack row pasted into fix.txt used to become the car's GPS from the
 * first numbers — or fail entirely if line 1 was a '#' comment.
 */
static int parse_fix_line(char *line, double *lat, double *lon, double *hdg,
                          int *have_hdg, int *ntok)
{
    char *tok[16];
    int n = 0;
    char *p;

    *have_hdg = 0;
    *ntok = 0;
    while (*line == ' ' || *line == '\t' || *line == '\r')
        line++;
    if (*line == 0 || *line == '#' || *line == '\n')
        return -1;
    for (p = strtok(line, " \t,\r\n"); p && n < 16; p = strtok(NULL, " \t,\r\n"))
        tok[n++] = p;
    *ntok = n;
    if (n < 2)
        return -1;
    *lat = atof(tok[0]);
    *lon = atof(tok[1]);
    if (n == 3) {
        *hdg = atof(tok[2]);
        *have_hdg = 1;
    }
    return 0;
}

static void apply_fix(const char *path, int line_no, int ntok,
                      double lat, double lon, double hdg, int have_hdg)
{
    g_lat = lat;
    g_lon = lon;
    refresh_coslat();
    g_have_gps = 1;
    g_fix_tokens = ntok;
    snprintf(g_fix_src, sizeof(g_fix_src), "%s:%d", path, line_no);
    if (have_hdg) {
        g_hdg = hdg;
        g_have_hdg = 1;
    } else {
        g_have_hdg = 0;
    }
    if (g_verbose)
        printf("fix %s tokens=%d lat=%.6f lon=%.6f heading=%s%s\n",
               g_fix_src, ntok, lat, lon,
               have_hdg ? "set" : "none",
               ntok >= 4 ? " (looks like points.txt, heading ignored)" : "");
}

static void read_fix_files(void)
{
    int i;

    for (i = 0; k_fix[i]; i++) {
        FILE *f = fopen(k_fix[i], "r");
        char line[256], copy[256];
        double lat = 0, lon = 0, hdg = 0, fb_lat = 0, fb_lon = 0, fb_hdg = 0;
        int have_hdg, ntok, line_no = 0;
        int fb_ok = 0, fb_line = 0, fb_ntok = 0, fb_have = 0;
        if (!f) {
            if (g_verbose)
                printf("fix miss %s (%s)\n", k_fix[i], strerror(errno));
            continue;
        }
        if (g_verbose)
            printf("fix open %s\n", k_fix[i]);
        while (fgets(line, sizeof(line), f)) {
            line_no++;
            snprintf(copy, sizeof(copy), "%s", line);
            if (parse_fix_line(copy, &lat, &lon, &hdg, &have_hdg, &ntok) != 0)
                continue;
            /* Real GPS is 2 or 3 fields. Pack rows have 4+. Prefer a real fix;
             * if the file is only pack rows, keep the LAST one (TEST was appended). */
            if (ntok == 2 || ntok == 3) {
                fclose(f);
                apply_fix(k_fix[i], line_no, ntok, lat, lon, hdg, have_hdg);
                return;
            }
            fb_ok = 1;
            fb_lat = lat;
            fb_lon = lon;
            fb_hdg = hdg;
            fb_have = have_hdg;
            fb_line = line_no;
            fb_ntok = ntok;
        }
        fclose(f);
        if (fb_ok) {
            apply_fix(k_fix[i], fb_line, fb_ntok, fb_lat, fb_lon, fb_hdg, fb_have);
            return;
        }
    }
    if (g_verbose && !g_have_gps)
        printf("fix: no lat/lon in any known path\n");
}

static void read_speed_file(void)
{
    int i;
    if (g_speed >= 0)
        return;
    for (i = 0; k_speed[i]; i++) {
        FILE *f = fopen(k_speed[i], "r");
        double v;
        if (!f)
            continue;
        if (fscanf(f, "%lf", &v) == 1)
            g_speed = v;
        fclose(f);
        if (g_speed >= 0)
            return;
    }
}

static uint16_t *pix(int x, int y)
{
    if (x < 0 || y < 0 || x >= W || y >= H)
        return NULL;
    return &g_fb[y * W + x];
}

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            g_fb[(y + j) * W + x + i] = c;
}

static void draw_char(int x, int y, char ch, uint16_t c, int s)
{
    const unsigned char *g;
    int col, row, dx, dy;
    unsigned idx;
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 32);
    if (ch < 32 || ch > 90)
        ch = ' ';
    idx = (unsigned)(ch - 32);
    g = k_f5[idx];
    for (col = 0; col < 5; col++) {
        unsigned char bits = g[col];
        for (row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                for (dy = 0; dy < s; dy++)
                    for (dx = 0; dx < s; dx++) {
                        uint16_t *p = pix(x + col * s + dx, y + row * s + dy);
                        if (p)
                            *p = c;
                    }
            }
        }
    }
}

static void draw_text(int x, int y, const char *s, uint16_t c, int scale)
{
    int cx = x;
    for (; s && *s; s++) {
        if (*s == '\n') {
            y += 8 * scale + 4;
            cx = x;
            continue;
        }
        draw_char(cx, y, *s, c, scale);
        cx += 6 * scale;
    }
}

static int dist_bucket(double dist)
{
    int cap = k_ranges[g_range_i];
    int d = (int)dist;
    if (d >= cap)
        return cap;
    d = (d / 50) * 50;
    if (d < 0)
        d = 0;
    return d;
}

static void render_alert(const RadarPoint *p, double dist)
{
#ifdef SPEEDUNTRAP
    speeduntrap_render(g_fb, W, H, g_lat, g_lon, g_hdg, g_have_hdg, g_speed,
                       p, dist, g_pts, g_npts, g_hit, 1);
    return;
#else
    char line[96];
    uint16_t bg = RGB565(12, 16, 28);
    uint16_t white = RGB565(240, 240, 240);
    uint16_t hot = RGB565(255, 30, 0);
    uint16_t grey = RGB565(160, 170, 180);

    fill_rect(0, 0, W, H, bg);
    fill_rect(0, 0, W, 8, hot);
    fill_rect(0, H - 8, W, 8, hot);
    draw_text(48, 28, "SPEED TRAP", hot, 6);
    if (dist >= 0)
        snprintf(line, sizeof(line), "%.0f m", dist);
    else
        snprintf(line, sizeof(line), "-- m");
    draw_text(48, 110, line, white, 10);
    draw_text(48, 250, "SPEED LIMIT", hot, 5);
    snprintf(line, sizeof(line), "%d km/h", p->leves);
    draw_text(48, 310, line, hot, 5);
    draw_text(640, 110, "SPEED", hot, 6);
    if (g_speed >= 0)
        snprintf(line, sizeof(line), "%.0f km/h", g_speed);
    else
        snprintf(line, sizeof(line), "-- km/h");
    draw_text(640, 170, line, hot, 6);
    snprintf(line, sizeof(line), "%s", p->via[0] ? p->via : "-");
    draw_text(640, 270, line, grey, 4);
    snprintf(line, sizeof(line), "%s  %s", p->fab, p->model);
    draw_text(640, 330, line, grey, 3);
    draw_text(48, 400, "MENU hide     OPTION x3 show", grey, 2);
#endif
}

static void overlay_hide(void)
{
    if (!g_showing)
        return;
    aa_hmi_restore();
    if (g_wal_ready)
        aa_display_set_visible(&g_disp, 0);
    g_showing = 0;
    g_last_frame[0] = 0;
    printf("radar: restored HmiMain3D\n");
}

static void overlay_blit(void)
{
    aa_display_upload_rows(&g_disp, 0, H, g_fb);
    aa_display_present(&g_disp);
}

static void overlay_raise(void)
{
    /* zorder was set at aa_display_open. Do not run WalControl info/zorder
     * again with the overlay visible — that is what wedged the last run. */
    aa_display_set_visible(&g_disp, 1);
    aa_hmi_hide();
    g_showing = 1;
    g_last_frame[0] = 0;
    printf("radar: hid HmiMain3D, overlay on wal=%d\n", g_disp.wal_id);
}

static void overlay_show(const RadarPoint *p, double dist)
{
    char key[160];
    if (!g_wal_ready || g_stop_run)
        return;
#ifdef SPEEDUNTRAP
    snprintf(key, sizeof(key), "%.5f|%.5f|%.1f|%.0f|%d|%d",
             g_lat, g_lon, g_hdg, dist, p->leves, radar_range_m());
#else
    snprintf(key, sizeof(key), "%.0f|%.0f|%d|%d",
             dist, g_speed, p->leves, p->pesados);
#endif
#ifndef SPEEDUNTRAP
    if (!g_showing) {
        overlay_raise();
    }
    if (g_showing && strcmp(key, g_last_frame) == 0)
        return;
    render_alert(p, dist);
    overlay_blit();
#else
    {
        int pumped = 0, zooming = 0;
        static int zframes;
        long t0;

        if (!g_showing) {
            printf("radar: overlay compose dist=%.0f lat=%.5f lon=%.5f hdg=%.1f%s\n",
                   dist, g_lat, g_lon, g_hdg, g_have_hdg ? "" : " (no yaw)");
            speeduntrap_render(g_fb, W, H, g_lat, g_lon, g_hdg, g_have_hdg,
                               g_speed, p, dist, g_pts, g_npts, g_hit, 0);
            overlay_blit();
            overlay_raise();
            printf("radar: HUD up — one map tile per tick\n");
        }
        if (!speeduntrap_zoom_busy())
            pumped = speeduntrap_map_pump(g_lat, g_lon);
        zooming = speeduntrap_zoom_tick();
        if (g_stop_run)
            return;
        if (g_showing && strcmp(key, g_last_frame) == 0 && !pumped && !zooming)
            return;
        t0 = now_ms();
        speeduntrap_render(g_fb, W, H, g_lat, g_lon, g_hdg, g_have_hdg, g_speed,
                           p, dist, g_pts, g_npts, g_hit, 1);
        overlay_blit();
        t0 = now_ms() - t0;
        zframes++;
        if (t0 >= 50 || (zooming && (zframes % 20) == 1))
            printf("map: frame %ld ms (~%d fps)%s\n", t0,
                   t0 > 0 ? (int)(1000 / t0) : 0,
                   zooming ? " zoom" : "");
    }
#endif
    snprintf(g_last_frame, sizeof(g_last_frame), "%s", key);
}

static void end_alert(const char *why)
{
    (void)why;
    overlay_hide();
    g_alert = 0;
    g_user_min = 0;
    g_demo = 0;
    g_hit = -1;
    g_closest = 1e9;
    g_prev_dist = -1;
}

static void zbe_handle(void)
{
    Id4CcsEvent ev;

    while (aa_zbe_pop(&ev) == 1) {
#ifdef SPEEDUNTRAP
        if (ev.kind == 2 && ev.what && strcmp(ev.what, "ROTARY") == 0) {
            if (ev.delta) {
                speeduntrap_zoom(ev.delta);
                g_last_frame[0] = 0;
            }
            continue;
        }
#endif
        if (ev.kind != 1 || !ev.down || !ev.what)
            continue;
        if (strcmp(ev.what, "MENU") == 0) {
            if (g_showing) {
                overlay_hide();
                g_user_min = 1;
                g_demo = 0;
                printf("radar: MENU — overlay hidden, HMI back\n");
            }
        } else if (strcmp(ev.what, "OPTIONS") == 0) {
            long t = now_ms();
            if (g_opt_ms == 0 || t - g_opt_ms > OPT_WINDOW_MS)
                g_opt_n = 0;
            g_opt_ms = t;
            g_opt_n++;
            if (g_opt_n >= 3) {
                g_opt_n = 0;
                if (g_popup) {
                    g_user_min = 0;
                    g_demo = 1;
                    g_last_frame[0] = 0;
                    printf("radar: OPTION x3 — overlay on (ignore range)\n");
                }
            }
        }
    }
}

static void find_nearest(int *best, double *bestd)
{
    int i;
    double best2 = 1e18;
    double k = 111320.0;
    double kl = 111320.0 * g_coslat;

    *best = -1;
    *bestd = 1e9;
    if (!g_have_gps || !g_pts)
        return;
    for (i = 0; i < g_npts; i++) {
        double dlat, dlon, d2;
        if (!heading_ok(&g_pts[i]))
            continue;
        dlat = (g_pts[i].lat - g_lat) * k;
        dlon = (g_pts[i].lon - g_lon) * kl;
        d2 = dlat * dlat + dlon * dlon;
        if (d2 < best2) {
            best2 = d2;
            *best = i;
        }
    }
    if (*best >= 0)
        *bestd = sqrt(best2);
}

static void maybe_chime(int best, double dist)
{
    if (!g_have_gps || best < 0)
        return;
    if (dist <= g_enter_m) {
        if (g_chimed_hit != best) {
            printf("radar: chime trap %d at %.0f m\n", best, dist);
            radar_chime_play();
            g_chimed_hit = best;
        }
    } else if (dist > g_enter_m + 150.0) {
        g_chimed_hit = -1;
    }
}

int radar_popup_enabled(void)
{
    return g_popup;
}

const char *radar_status_line(void)
{
    return g_status;
}

void radar_set_speed_kmh(double kmh)
{
    if (kmh >= 0 && kmh < 400)
        g_speed = kmh;
}

void radar_set_gps(double lat, double lon, double heading_deg, int heading_valid)
{
    g_lat = lat;
    g_lon = lon;
    refresh_coslat();
    g_have_gps = 1;
    g_fix_tokens = 2;
    snprintf(g_fix_src, sizeof(g_fix_src), "PosReal.DNAVIPos");
    if (heading_valid) {
        g_hdg = heading_deg;
        g_have_hdg = 1;
    }
}

void radar_request_stop(void)
{
    g_stop_run = 1;
}

int radar_should_stop(void)
{
    return g_stop_run;
}

int radar_is_test(void)
{
    return g_test_mode;
}

void radar_force_restore(void)
{
    if (g_wal_ready && g_disp.wal_id >= 0)
        aa_walcontrol(g_disp.wal_id, "visible 0");
    aa_hmi_force_restore();
    g_showing = 0;
    g_last_frame[0] = 0;
    printf("radar: forced HMI restore (overlay hidden)\n");
}

int radar_range_m(void)
{
    return k_ranges[g_range_i];
}

const char *radar_range_line(void)
{
    return g_range_line;
}

void radar_range_step(int dir)
{
    apply_range_i(g_range_i + (dir > 0 ? 1 : -1));
    persist_range();
    printf("radar: %s\n", g_range_line);
}

void radar_set_popup(int on)
{
    g_popup = on ? 1 : 0;
    persist_popup(g_popup);
    if (!g_popup) {
        end_alert("popup off");
        set_status("Popup off");
    } else {
        set_status("Popup on");
    }
}

void radar_emergency_stop(void)
{
    /* Do not _exit: HmiMain3D is on this plugin page. Killing the SAS
     * client there reboots the HU. Stay registered, just drop overlay/GPS. */
    overlay_hide();
    radar_set_popup(0);
    radar_gps_stop();
    aa_hmi_restore();
    set_status("Stopped");
    printf("radar: STOP — HMI restored, popup off, GPS off (process stays)\n");
}

int radar_init(void)
{
    if (!g_test_mode)
        load_popup();
    else
        g_popup = 1;
    load_range();
    load_pack();
    memset(&g_disp, 0, sizeof(g_disp));
    g_disp.wal_id = -1;
    if (aa_display_open(&g_disp, W, H, GL_RGB, GL_UNSIGNED_SHORT_5_6_5) == 0) {
        aa_display_set_visible(&g_disp, 0);
        g_wal_ready = 1;
        printf("radar overlay WalServer window %d %dx%d (hidden)\n",
               g_disp.wal_id, W, H);
    } else {
        g_wal_ready = 0;
        printf("radar overlay open failed — settings still work, no CID swap\n");
    }
    if (g_stop_run)
        return 0;
    if (aa_zbe_start() != 0)
        printf("radar: ZBE observe-only failed — MENU/OPTION disabled\n");
    else
        printf("radar: ZBE observe-only (MENU hide, OPTION x3 restore)\n");
    if (aa_cm_start() != 0)
        printf("radar: ConnectionMaster DSI thread failed — MOST 0x53 fallback\n");
    if (aa_am_start() != 0)
        printf("radar: AudioManagement DSI thread failed — C05 unmute only\n");
    if (radar_gps_start() != 0)
        printf("radar: PosReal GPS thread failed — fix.txt is fallback\n");
#ifdef SPEEDUNTRAP
    if (speeduntrap_map_open() != 0)
        printf("map: running without map.bin (HUD only)\n");
#endif
    if (g_popup)
        set_status(g_npts ? "Popup on" : "Popup on, no pack");
    else
        set_status("Popup off");
    printf("radar: %s\n", g_range_line);
    radar_chime_log_info();
    return 0;
}

void radar_shutdown(void)
{
    overlay_hide();
#ifdef SPEEDUNTRAP
    speeduntrap_map_close();
    speeduntrap_atlas_free();
#endif
    radar_chime_shutdown();
    radar_gps_stop();
    if (g_wal_ready) {
        aa_display_close(&g_disp);
        g_wal_ready = 0;
    }
    aa_hmi_restore();
}

void radar_tick(void)
{
    int best;
    double dist;
    double leave_m;
    double rearm_m;
    char st[80];

    if (g_stop_run)
        return;

    zbe_handle();

    if (!radar_gps_poll()) {
        /* DSI attached — do not paint the TEST pin from fix.txt as 0 m. */
        if (!radar_gps_on_bus())
            read_fix_files();
    }
    read_speed_file();
#ifdef SPEEDUNTRAP
    {
        static int nearest_skip;
        if (g_showing && g_hit >= 0 &&
            (speeduntrap_zoom_busy() || nearest_skip < 6)) {
            best = g_hit;
            dist = haversine_m(g_lat, g_lon, g_pts[g_hit].lat, g_pts[g_hit].lon);
            nearest_skip++;
        } else {
            find_nearest(&best, &dist);
            nearest_skip = 0;
        }
    }
#else
    find_nearest(&best, &dist);
#endif
    maybe_chime(best, dist);

    if (!g_popup) {
        if (g_showing)
            overlay_hide();
        g_demo = 0;
        set_status("Popup off");
        return;
    }

    leave_m = g_enter_m + 100.0;
    rearm_m = g_enter_m + 150.0;

    if (g_demo && !g_user_min) {
        if (g_have_gps && best >= 0) {
            overlay_show(&g_pts[best], dist);
            snprintf(st, sizeof(st), "Demo %.0f m", dist);
            set_status(st);
        } else
            set_status("Demo, no GPS");
        return;
    }

    if (!g_have_gps) {
        set_status("No GPS");
        if (g_showing)
            overlay_hide();
        return;
    }
    if (best < 0) {
        set_status("No traps in heading");
        if (g_alert)
            end_alert("none");
        return;
    }

    if (!g_alert) {
        if (dist <= g_enter_m) {
            g_alert = 1;
            g_user_min = 0;
            g_hit = best;
            g_closest = dist;
            g_prev_dist = dist;
            overlay_show(&g_pts[best], dist);
        }
    } else {
        if (best != g_hit)
            dist = haversine_m(g_lat, g_lon, g_pts[g_hit].lat, g_pts[g_hit].lon);
        if (dist < g_closest)
            g_closest = dist;
        if (dist > PASS_M && dist > g_closest + 40.0 && g_closest < PASS_M) {
            end_alert("passed");
        } else if (dist > leave_m) {
            end_alert("left");
        } else if (!g_user_min) {
            overlay_show(&g_pts[g_hit], dist);
        }
        g_prev_dist = dist;
    }

    if (g_alert && g_hit >= 0) {
        if (g_user_min)
            snprintf(st, sizeof(st), "Trap %.0f m (hidden)", dist);
        else
            snprintf(st, sizeof(st), "Trap %.0f m", dist);
        set_status(st);
    } else if (dist < rearm_m) {
        snprintf(st, sizeof(st), "Near %.0f m", dist);
        set_status(st);
    } else {
        set_status("Popup on");
    }
}

static void radar_dump_diag(int tick)
{
    int best = -1;
    double dist = -1;
    const RadarPoint *p = NULL;

    if (g_have_gps)
        find_nearest(&best, &dist);
    if (best >= 0)
        p = &g_pts[best];
    printf("[tick %d] popup=%d gps=%d src=%s tokens=%d lat=%.6f lon=%.6f "
           "hdg=%s pack=%d test_idx=%d nearest=%.1fm range=%.0f alert=%d demo=%d "
           "min=%d show=%d wal=%d overlay=%s status=\"%s\"",
           tick, g_popup, g_have_gps,
           g_have_gps ? g_fix_src : "-",
           g_fix_tokens,
           g_have_gps ? g_lat : 0.0,
           g_have_gps ? g_lon : 0.0,
           g_have_hdg ? "yes" : "no",
           g_npts, g_test_in_pack,
           (g_have_gps && best >= 0) ? dist : -1.0,
           g_enter_m, g_alert, g_demo, g_user_min,
           g_showing, g_disp.wal_id,
           g_wal_ready ? "ready" : "FAILED",
           g_status);
    if (p)
        printf(" hit=%.6f %.6f %s %d/%d",
               p->lat, p->lon, p->via, p->leves, p->pesados);
    printf("\n");
    if (g_have_gps && best >= 0 && dist > g_enter_m)
        printf("  not in range: need <= %.0f m (you are %.0f m from nearest packed point)\n",
               g_enter_m, dist);
    if (g_have_gps && g_test_in_pack >= 0) {
        double td = haversine_m(g_lat, g_lon,
                                g_pts[g_test_in_pack].lat,
                                g_pts[g_test_in_pack].lon);
        printf("  distance to TEST control: %.1f m\n", td);
    }
}

int radar_test_run(void)
{
    int tick = 0;

    g_verbose = 1;
    g_test_mode = 1;
    g_popup = 1;
    printf("\n");
    printf("==========================================================\n");
#ifdef SPEEDUNTRAP
    printf("  speeduntrap  (Putty logs, no ConnectedDrive)\n");
    printf("  GPS      = PosReal.DNAVIPos fused DR (no OBD)\n");
    printf("  overlay  = map tiles + HUD  1280x480\n");
    printf("  map.bin  = /fs/sda0/radar/map.bin\n");
    printf("  slay id4_radar_x86 first if it owns WalControl\n");
#else
    printf("  RADAR TEST  (no ConnectedDrive, logs on this SSH)\n");
    printf("  GPS      = PosReal.DNAVIPos fused DR (no OBD); fix.txt fallback\n");
    printf("  overlay  = 1280x480  MENU hide  OPTION x3 restore\n");
    printf("  points.txt = lat lon leves pesados heading via fab model\n");
    printf("  TEST     = -26.962599 -50.421094\n");
    printf("  slay the boot daemon first:  slay id4_radar_x86\n");
#endif
    printf("==========================================================\n\n");

    radar_init();
    if (g_stop_run) {
        printf("radar: stop during init\n");
        radar_shutdown();
        return 0;
    }
    if (!radar_gps_poll() && !radar_gps_on_bus())
        read_fix_files();
    if (!g_have_gps)
        printf("NO GPS yet — waiting for PosReal.DNAVIPos, or write fix.txt as:\n"
               "  -26.962599 -50.421094\n");
    else if (g_fix_tokens >= 4)
        printf("WARNING: fix.txt line looks like a pack row. Prefer a 2-field GPS line.\n");

    for (;;) {
        if (g_stop_run)
            break;
        radar_tick();
        radar_dump_diag(++tick);
#ifdef SPEEDUNTRAP
        usleep(speeduntrap_zoom_busy() ? 1000 :
               (g_showing ? 8000 : 200000));
#else
        usleep(200000);
#endif
    }
    printf("radar: leaving, restoring HMI\n");
    radar_shutdown();
    return 0;
}
