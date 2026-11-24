/* CID 1280x480 map + HUD. Palette from Snazzy Maps "Orange" (style 145431):
 * land/water black+lightness, highways #f26c4f, HUD accent the same. */

#include "speeduntrap_ui.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) >> 3) & 0x1F)))

static const unsigned char k_f5[59][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};

static uint16_t *g_fb;
static int g_w, g_h;
static double g_lat, g_lon, g_hdg;
static int g_have_hdg;
static double g_meters = 1600.0;
static double g_meters_tgt = 1600.0;
static int g_hairline;
#define METERS_MIN 400.0
#define METERS_MAX 8000.0
#define ATLAS_W 2560
#define ATLAS_H 960
#define ATLAS_PAINT_MAX 32
#define FAR_MPP 3.125
#define NEAR_MPP 0.8

typedef struct {
    uint16_t *px;
    int w, h;
    double mpp, lat, lon;
    int hairline;
    int npaint;
    int ptx[ATLAS_PAINT_MAX];
    int pty[ATLAS_PAINT_MAX];
} Atlas;

static Atlas g_far, g_near;
static int g_zoom_lock, g_zoom_ox, g_zoom_oy;
static Atlas *g_zoom_from;

static const uint16_t COL_LAND = RGB565(40, 40, 40);
static const uint16_t COL_PANEL = RGB565(16, 12, 10);
static const uint16_t COL_WATER = RGB565(22, 22, 22);
static const uint16_t COL_LO = RGB565(74, 74, 74);
static const uint16_t COL_AR = RGB565(96, 96, 96);
static const uint16_t COL_HW = RGB565(242, 108, 79);
static const uint16_t COL_CASE = RGB565(0, 0, 0);
static const uint16_t COL_HOT = RGB565(242, 108, 79);
static const uint16_t COL_GREY = RGB565(176, 176, 176);
static const uint16_t COL_CAR = RGB565(242, 108, 79);
static const uint16_t COL_CAR_EDGE = RGB565(0, 0, 0);
static const uint16_t COL_PIN = RGB565(200, 88, 68);
static const uint16_t COL_RING = RGB565(64, 64, 64);

static void plot(int x, int y, uint16_t c)
{
    if ((unsigned)x >= (unsigned)g_w || (unsigned)y >= (unsigned)g_h)
        return;
    g_fb[y * g_w + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    int i, j;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0)
        return;
    for (j = 0; j < h; j++) {
        uint16_t *row = g_fb + (y + j) * g_w + x;
        for (i = 0; i < w; i++)
            row[i] = c;
    }
}

static int seg_skip(int x0, int y0, int x1, int y1)
{
    int xmin = x0 < x1 ? x0 : x1;
    int xmax = x0 > x1 ? x0 : x1;
    int ymin = y0 < y1 ? y0 : y1;
    int ymax = y0 > y1 ? y0 : y1;

    if (xmax < -4 || xmin > g_w + 4 || ymax < -4 || ymin > g_h + 4)
        return 1;
    if (xmax - xmin > g_w * 2 || ymax - ymin > g_h * 2)
        return 1;
    return 0;
}

static int clip_t(double *t0, double *t1, double p, double q)
{
    double r;
    if (p == 0.0)
        return q >= 0.0;
    r = q / p;
    if (p < 0.0) {
        if (r > *t1)
            return 0;
        if (r > *t0)
            *t0 = r;
    } else {
        if (r < *t0)
            return 0;
        if (r < *t1)
            *t1 = r;
    }
    return 1;
}

static int clip_line(int *x0, int *y0, int *x1, int *y1)
{
    double dx = (double)(*x1 - *x0);
    double dy = (double)(*y1 - *y0);
    double t0 = 0.0, t1 = 1.0;

    if (!clip_t(&t0, &t1, -dx, (double)*x0))
        return 0;
    if (!clip_t(&t0, &t1, dx, (double)(g_w - 1 - *x0)))
        return 0;
    if (!clip_t(&t0, &t1, -dy, (double)*y0))
        return 0;
    if (!clip_t(&t0, &t1, dy, (double)(g_h - 1 - *y0)))
        return 0;
    if (t1 < 1.0) {
        *x1 = *x0 + (int)(t1 * dx);
        *y1 = *y0 + (int)(t1 * dy);
    }
    if (t0 > 0.0) {
        *x0 = *x0 + (int)(t0 * dx);
        *y0 = *y0 + (int)(t0 * dy);
    }
    return 1;
}

static void line_inner(int x0, int y0, int x1, int y1, uint16_t c, int raw)
{
    int dx, sx, dy, sy, err, e2, n = 0, lim;

    dx = x1 > x0 ? x1 - x0 : x0 - x1;
    sx = x0 < x1 ? 1 : -1;
    dy = y1 > y0 ? y1 - y0 : y0 - y1;
    sy = y0 < y1 ? 1 : -1;
    err = dx - dy;
    lim = g_w + g_h + 16;
    for (;;) {
        if (raw)
            g_fb[y0 * g_w + x0] = c;
        else
            plot(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        if (++n > lim)
            break;
        e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    if ((unsigned)x0 < (unsigned)g_w && (unsigned)y0 < (unsigned)g_h &&
        (unsigned)x1 < (unsigned)g_w && (unsigned)y1 < (unsigned)g_h) {
        line_inner(x0, y0, x1, y1, c, 1);
        return;
    }
    if (!clip_line(&x0, &y0, &x1, &y1))
        return;
    line_inner(x0, y0, x1, y1, c, 0);
}

static void line_w(int x0, int y0, int x1, int y1, uint16_t c, int w)
{
    int k;
    line(x0, y0, x1, y1, c);
    for (k = 1; k <= w / 2; k++) {
        line(x0, y0 - k, x1, y1 - k, c);
        line(x0, y0 + k, x1, y1 + k, c);
        line(x0 - k, y0, x1 - k, y1, c);
        line(x0 + k, y0, x1 + k, y1, c);
    }
}

static void merc(double lat, double lon, double *x, double *y)
{
    double s = sin(lat * M_PI / 180.0);
    *x = (lon + 180.0) / 360.0;
    *y = 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI);
}

static void to_screen(double lat, double lon, int *px, int *py)
{
    double cx, cy, x, y, mpp, mperx, latr;

    merc(g_lat, g_lon, &cx, &cy);
    merc(lat, lon, &x, &y);
    mpp = g_meters / (double)g_w;
    latr = g_lat * M_PI / 180.0;
    mperx = 40075016.686 * cos(latr);
    *px = (int)(g_w / 2.0 + ((x - cx) * mperx) / mpp);
    *py = (int)(g_h / 2.0 + ((y - cy) * 40075016.686) / mpp);
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
                    for (dx = 0; dx < s; dx++)
                        plot(x + col * s + dx, y + row * s + dy, c);
            }
        }
    }
}

static void draw_text(int x, int y, const char *s, uint16_t c, int scale)
{
    int cx = x;
    for (; s && *s; s++) {
        draw_char(cx, y, *s, c, scale);
        cx += 6 * scale;
    }
}

typedef struct {
    int dummy;
} DrawCtx;

static uint32_t rd_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static int32_t rd_i32(const uint8_t *p)
{
    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)v;
}

static void draw_tile(const uint8_t *data, uint32_t len, void *ctx)
{
    uint32_t n, i, o = 2;
    int pass;

    (void)ctx;
    if (len < 2)
        return;
    n = rd_u16(data);
    /* No black casing — it baked a contour that survived every zoom. */
    for (pass = 1; pass < 2; pass++) {
        o = 2;
        for (i = 0; i < n && o + 3 <= len; i++) {
            uint8_t kind = data[o];
            uint32_t npts = rd_u16(data + o + 1);
            uint32_t need = 3 + npts * 8;
            uint32_t p, k;
            int x0, y0, x1, y1;
            uint16_t col;
            int wid;

            if (o + need > len)
                break;
            if (kind <= 2) {
                col = kind == 2 ? COL_HW : (kind == 1 ? COL_AR : COL_LO);
                if (g_hairline)
                    wid = kind >= 1 ? 2 : 1;
                else
                    wid = kind == 2 ? 3 : (kind == 1 ? 2 : 1);
            } else {
                col = COL_WATER;
                wid = 1;
            }
            if (pass == 0 && kind <= 2) {
                col = COL_CASE;
                wid += 1;
            } else if (pass == 0) {
                o += need;
                continue;
            }
            for (k = 1; k < npts; k++) {
                p = o + 3 + (k - 1) * 8;
                to_screen(rd_i32(data + p + 4) / 1e6, rd_i32(data + p) / 1e6, &x0, &y0);
                to_screen(rd_i32(data + p + 12) / 1e6, rd_i32(data + p + 8) / 1e6, &x1, &y1);
                if (seg_skip(x0, y0, x1, y1))
                    continue;
                if (wid <= 1)
                    line(x0, y0, x1, y1, col);
                else
                    line_w(x0, y0, x1, y1, col, wid);
            }
            o += need;
        }
    }
}

#define GEO_MAX 32

typedef struct {
    int tx, ty;
    int nseg;
    int32_t emin, emax, nmin, nmax;
    int32_t *xy;
    uint8_t *kind;
} GeoTile;

static GeoTile g_geo[GEO_MAX];
static int g_ngeo;
static int g_geo_set;
static int g_geo_logn = -1;
static double g_geo_olat, g_geo_olon, g_geo_coslat;

static void geo_ll_cm(double lat, double lon, int32_t *e, int32_t *n)
{
    *e = (int32_t)((lon - g_geo_olon) * (11132000.0 * g_geo_coslat));
    *n = (int32_t)((lat - g_geo_olat) * 11132000.0);
}

static void geo_clear_one(GeoTile *g)
{
    free(g->xy);
    free(g->kind);
    memset(g, 0, sizeof(*g));
}

static void geo_clear_all(void)
{
    int i;

    for (i = 0; i < GEO_MAX; i++)
        geo_clear_one(&g_geo[i]);
    g_ngeo = 0;
}

static void geo_origin(double lat, double lon)
{
    g_geo_olat = lat;
    g_geo_olon = lon;
    g_geo_coslat = cos(lat * M_PI / 180.0);
    g_geo_set = 1;
}

static int geo_bake(GeoTile *g, int tx, int ty, const uint8_t *data, uint32_t len)
{
    uint32_t n, i, o, npts, need, k, p;
    int nseg = 0, s;
    int32_t e0, n0, e1, n1;

    geo_clear_one(g);
    g->tx = tx;
    g->ty = ty;
    g->emin =  2147483647;
    g->emax = -2147483647;
    g->nmin =  2147483647;
    g->nmax = -2147483647;
    if (!data || len < 2)
        return 0;
    n = rd_u16(data);
    o = 2;
    for (i = 0; i < n && o + 3 <= len; i++) {
        npts = rd_u16(data + o + 1);
        need = 3 + npts * 8;
        if (o + need > len)
            break;
        if (npts >= 2) {
            if (nseg > 80000 - (int)(npts - 1))
                break;
            nseg += (int)(npts - 1);
        }
        o += need;
    }
    if (nseg <= 0)
        return 0;
    g->xy = (int32_t *)malloc((size_t)nseg * 4u * sizeof(int32_t));
    g->kind = (uint8_t *)malloc((size_t)nseg);
    if (!g->xy || !g->kind) {
        geo_clear_one(g);
        return 0;
    }
    o = 2;
    s = 0;
    for (i = 0; i < n && o + 3 <= len; i++) {
        uint8_t kind = data[o];
        npts = rd_u16(data + o + 1);
        need = 3 + npts * 8;
        if (o + need > len)
            break;
        for (k = 1; k < npts && s < nseg; k++) {
            p = o + 3 + (k - 1) * 8;
            geo_ll_cm(rd_i32(data + p + 4) / 1e6, rd_i32(data + p) / 1e6, &e0, &n0);
            geo_ll_cm(rd_i32(data + p + 12) / 1e6, rd_i32(data + p + 8) / 1e6, &e1, &n1);
            g->xy[s * 4 + 0] = e0;
            g->xy[s * 4 + 1] = n0;
            g->xy[s * 4 + 2] = e1;
            g->xy[s * 4 + 3] = n1;
            g->kind[s] = kind;
            if (e0 < g->emin) g->emin = e0;
            if (e1 < g->emin) g->emin = e1;
            if (e0 > g->emax) g->emax = e0;
            if (e1 > g->emax) g->emax = e1;
            if (n0 < g->nmin) g->nmin = n0;
            if (n1 < g->nmin) g->nmin = n1;
            if (n0 > g->nmax) g->nmax = n0;
            if (n1 > g->nmax) g->nmax = n1;
            s++;
        }
        o += need;
    }
    g->nseg = s;
    return 1;
}

static void geo_sync(void)
{
    int i, n, tx, ty, gi, nused = 0, segs = 0;
    const uint8_t *data;
    uint32_t len;
    int32_t e, ncm;
    int used[GEO_MAX];

    if (!g_geo_set)
        geo_origin(g_lat, g_lon);
    else {
        geo_ll_cm(g_lat, g_lon, &e, &ncm);
        if (e > 2000000 || e < -2000000 || ncm > 2000000 || ncm < -2000000) {
            geo_clear_all();
            geo_origin(g_lat, g_lon);
        }
    }

    memset(used, 0, sizeof(used));
    n = speeduntrap_cache_n();
    for (i = 0; i < n; i++) {
        int have = 0, empty = -1;
        if (!speeduntrap_cache_slot(i, &tx, &ty, &data, &len) || !data)
            continue;
        for (gi = 0; gi < GEO_MAX; gi++) {
            if (g_geo[gi].xy && g_geo[gi].tx == tx && g_geo[gi].ty == ty) {
                used[gi] = 1;
                have = 1;
                break;
            }
            if (empty < 0 && !g_geo[gi].xy)
                empty = gi;
        }
        if (have)
            continue;
        if (empty < 0)
            continue;
        if (geo_bake(&g_geo[empty], tx, ty, data, len))
            used[empty] = 1;
    }
    for (gi = 0; gi < GEO_MAX; gi++) {
        if (g_geo[gi].xy && !used[gi])
            geo_clear_one(&g_geo[gi]);
        if (g_geo[gi].xy) {
            nused++;
            segs += g_geo[gi].nseg;
        }
    }
    g_ngeo = nused;
    if (nused != g_geo_logn) {
        g_geo_logn = nused;
        printf("map: vectors %d tiles %d segs (scale in RAM)\n", nused, segs);
    }
}

static void geo_draw(void)
{
    int i, s, x0, y0, x1, y1;
    int32_t ce, cn, he, hn;
    int64_t sc;
    uint8_t kind;
    uint16_t col;
    const int32_t *xy;
    GeoTile *g;

    geo_ll_cm(g_lat, g_lon, &ce, &cn);
    sc = (int64_t)((g_w * 65536.0) / (g_meters * 100.0));
    if (sc < 1)
        sc = 1;
    he = (int32_t)(g_meters * 70.0);
    hn = (int32_t)(g_meters * 70.0 * (double)g_h / (double)g_w);
    for (i = 0; i < GEO_MAX; i++) {
        g = &g_geo[i];
        xy = g->xy;
        if (!xy)
            continue;
        if (g->emax < ce - he || g->emin > ce + he ||
            g->nmax < cn - hn || g->nmin > cn + hn)
            continue;
        for (s = 0; s < g->nseg; s++) {
            kind = g->kind[s];
            x0 = g_w / 2 + (int)(((int64_t)(xy[s * 4 + 0] - ce) * sc) >> 16);
            y0 = g_h / 2 - (int)(((int64_t)(xy[s * 4 + 1] - cn) * sc) >> 16);
            x1 = g_w / 2 + (int)(((int64_t)(xy[s * 4 + 2] - ce) * sc) >> 16);
            y1 = g_h / 2 - (int)(((int64_t)(xy[s * 4 + 3] - cn) * sc) >> 16);
            if (seg_skip(x0, y0, x1, y1))
                continue;
            if (kind <= 2)
                col = kind == 2 ? COL_HW : (kind == 1 ? COL_AR : COL_LO);
            else
                col = COL_WATER;
            line(x0, y0, x1, y1, col);
        }
    }
}

static void shade_panel(void)
{
    int x, y, w = 420;
    uint16_t tint = (COL_PANEL & 0xF7DE) >> 1;

    if (w > g_w)
        w = g_w;
    for (y = 0; y < g_h; y++) {
        uint16_t *row = g_fb + y * g_w;
        for (x = 0; x < w; x++)
            row[x] = (uint16_t)(((row[x] & 0xF7DE) >> 1) + tint);
    }
}

static void fill_disk(int cx, int cy, int rad, uint16_t c)
{
    int x, y, r2 = rad * rad;

    for (y = -rad; y <= rad; y++) {
        for (x = -rad; x <= rad; x++) {
            if (x * x + y * y <= r2)
                plot(cx + x, cy + y, c);
        }
    }
}

static void draw_hud(const RadarPoint *p, double dist, double speed)
{
    char line[96];
    const char *via = (p && p->via[0] && p->via[0] != '-') ? p->via : "-";
    const char *brand = (p && p->fab[0] && p->fab[0] != '-') ? p->fab : "-";
    const char *model = (p && p->model[0] && p->model[0] != '-') ? p->model : "-";
    int menu_y = 444;
    int lh = 14, title_h = 28, dist_h = 42, num_h = 28, txt_h = 21;
    int inn = 6, out = 16;
    int block = title_h + inn + dist_h + out +
                lh + inn + num_h + out +
                lh + inn + num_h + out +
                lh + inn + txt_h + out +
                lh + inn + txt_h + inn + txt_h;
    int y = (menu_y - block) / 2;
    int x = 20;
    uint16_t speed_col = (speed > (p ? p->leves : 9999)) ? COL_HOT : COL_GREY;

    draw_text(x, y, "SPEED TRAP", COL_HOT, 4); y += title_h + inn;
    if (dist >= 0)
        snprintf(line, sizeof(line), "%.0f m", dist);
    else
        snprintf(line, sizeof(line), "-- m");
    draw_text(x, y, line, COL_GREY, 6); y += dist_h + out;
    draw_text(x, y, "SPEED LIMIT", COL_GREY, 2); y += lh + inn;
    snprintf(line, sizeof(line), "%d km/h", p ? p->leves : 0);
    draw_text(x, y, line, COL_HOT, 4); y += num_h + out;
    draw_text(x, y, "SPEED", COL_GREY, 2); y += lh + inn;
    if (speed >= 0)
        snprintf(line, sizeof(line), "%.0f km/h", speed);
    else
        snprintf(line, sizeof(line), "-- km/h");
    draw_text(x, y, line, speed_col, 4); y += num_h + out;
    draw_text(x, y, "ROAD", COL_GREY, 2); y += lh + inn;
    draw_text(x, y, via, COL_GREY, 3); y += txt_h + out;
    draw_text(x, y, "RADAR", COL_GREY, 2); y += lh + inn;
    draw_text(x, y, brand, COL_GREY, 3); y += txt_h + inn;
    draw_text(x, y, model, COL_GREY, 3);
    draw_text(x, menu_y, "MENU hide   OPTION x3 show", COL_GREY, 2);
}

static void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
{
    int minx, maxx, miny, maxy, x, y, d;

    minx = x0; if (x1 < minx) minx = x1; if (x2 < minx) minx = x2;
    maxx = x0; if (x1 > maxx) maxx = x1; if (x2 > maxx) maxx = x2;
    miny = y0; if (y1 < miny) miny = y1; if (y2 < miny) miny = y2;
    maxy = y0; if (y1 > maxy) maxy = y1; if (y2 > maxy) maxy = y2;
    d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (d == 0)
        return;
    for (y = miny; y <= maxy; y++) {
        for (x = minx; x <= maxx; x++) {
            int w0 = (y1 - y2) * (x - x2) + (x2 - x1) * (y - y2);
            int w1 = (y2 - y0) * (x - x2) + (x0 - x2) * (y - y2);
            int w2 = d - w0 - w1;
            if (d > 0) {
                if (w0 >= 0 && w1 >= 0 && w2 >= 0)
                    plot(x, y, c);
            } else if (w0 <= 0 && w1 <= 0 && w2 <= 0) {
                plot(x, y, c);
            }
        }
    }
}

static void draw_car(void)
{
    /* OEM map/guidance rotates with (heading - 90) before sin/cos
     * (NavigationGuidanceViewer_NV: FUN_084504a4(h - 90); Positioning
     * FUN_080e84d0(h - 90)). Compass 0=N becomes math 0=E. Same 90° CCW
     * on this north-up CID. */
    double hdg = g_hdg - 90.0;
    double rad = hdg * M_PI / 180.0;
    int cx = g_w / 2, cy = g_h / 2;
    int pts[4][2];
    int local[4][2] = {{0, 22}, {14, -16}, {0, -8}, {-14, -16}};
    int i;

    for (i = 0; i < 4; i++) {
        double lr = (double)local[i][0], lf = (double)local[i][1];
        pts[i][0] = cx + (int)(sin(rad) * lf + cos(rad) * lr);
        pts[i][1] = cy + (int)(-cos(rad) * lf + sin(rad) * lr);
    }
    fill_tri(pts[0][0], pts[0][1], pts[1][0], pts[1][1], pts[2][0], pts[2][1], COL_CAR);
    fill_tri(pts[0][0], pts[0][1], pts[2][0], pts[2][1], pts[3][0], pts[3][1], COL_CAR);
    line(pts[0][0], pts[0][1], pts[1][0], pts[1][1], COL_CAR_EDGE);
    line(pts[1][0], pts[1][1], pts[2][0], pts[2][1], COL_CAR_EDGE);
    line(pts[2][0], pts[2][1], pts[3][0], pts[3][1], COL_CAR_EDGE);
    line(pts[3][0], pts[3][1], pts[0][0], pts[0][1], COL_CAR_EDGE);
}

int speeduntrap_view_ring(void)
{
    int r = (int)(g_meters / 2200.0);

    if (r < 1)
        r = 1;
    if (r > 2)
        r = 2;
    return r;
}

void speeduntrap_zoom(int delta)
{
    double m = g_meters_tgt;
    int n = delta > 0 ? delta : -delta;

    if (n <= 0)
        return;
    while (n--) {
        if (delta > 0)
            m *= 0.92;
        else
            m *= 1.087;
    }
    if (m < METERS_MIN)
        m = METERS_MIN;
    if (m > METERS_MAX)
        m = METERS_MAX;
    if (m == g_meters_tgt)
        return;
    g_meters_tgt = m;
    printf("map: zoom %.0f m across CID\n", g_meters_tgt);
}

int speeduntrap_zoom_tick(void)
{
    double d = g_meters_tgt - g_meters;

    if (d > -0.8 && d < 0.8) {
        if (g_meters == g_meters_tgt)
            return 0;
        g_meters = g_meters_tgt;
        return 1;
    }
    g_meters += d * 0.35;
    return 1;
}

static double dist_m(double lat0, double lon0, double lat1, double lon1)
{
    double dlat = (lat1 - lat0) * 111320.0;
    double dlon = (lon1 - lon0) * 111320.0 * cos(lat0 * M_PI / 180.0);

    return sqrt(dlat * dlat + dlon * dlon);
}

static int atlas_has(const Atlas *a, int tx, int ty)
{
    int i;

    for (i = 0; i < a->npaint; i++) {
        if (a->ptx[i] == tx && a->pty[i] == ty)
            return 1;
    }
    return 0;
}

static void atlas_mark(Atlas *a, int tx, int ty)
{
    if (a->npaint >= ATLAS_PAINT_MAX)
        return;
    a->ptx[a->npaint] = tx;
    a->pty[a->npaint] = ty;
    a->npaint++;
}

static void atlas_clear_px(Atlas *a)
{
    int i, n;

    if (!a->px)
        return;
    n = a->w * a->h;
    for (i = 0; i < n; i++)
        a->px[i] = COL_LAND;
    a->npaint = 0;
}

static int atlas_alloc(Atlas *a, double mpp, int hairline)
{
    size_t bytes = (size_t)ATLAS_W * (size_t)ATLAS_H * sizeof(uint16_t);

    if (a->px)
        return 0;
    a->px = (uint16_t *)malloc(bytes);
    if (!a->px)
        return -1;
    a->w = ATLAS_W;
    a->h = ATLAS_H;
    a->mpp = mpp;
    a->hairline = hairline;
    a->lat = 0;
    a->lon = 0;
    atlas_clear_px(a);
    return 0;
}

static void atlas_to_px(const Atlas *a, double lat, double lon, int *px, int *py)
{
    double cx, cy, x, y, mperx, latr;

    merc(a->lat, a->lon, &cx, &cy);
    merc(lat, lon, &x, &y);
    latr = a->lat * M_PI / 180.0;
    mperx = 40075016.686 * cos(latr);
    *px = (int)(a->w / 2.0 + ((x - cx) * mperx) / a->mpp);
    *py = (int)(a->h / 2.0 + ((y - cy) * 40075016.686) / a->mpp);
}

static void atlas_bind(Atlas *a)
{
    g_fb = a->px;
    g_w = a->w;
    g_h = a->h;
    g_lat = a->lat;
    g_lon = a->lon;
    g_meters = (double)a->w * a->mpp;
    g_hairline = a->hairline;
}

static void atlas_paint_tile(Atlas *a, const uint8_t *data, uint32_t len)
{
    uint16_t *old_fb = g_fb;
    int ow = g_w, oh = g_h, ohair = g_hairline;
    double olat = g_lat, olon = g_lon, om = g_meters;

    atlas_bind(a);
    draw_tile(data, len, NULL);
    g_fb = old_fb;
    g_w = ow;
    g_h = oh;
    g_lat = olat;
    g_lon = olon;
    g_meters = om;
    g_hairline = ohair;
}

static void atlas_recenter(Atlas *a, double lat, double lon, double lim_m)
{
    if (!a->px)
        return;
    if (a->npaint == 0) {
        a->lat = lat;
        a->lon = lon;
        return;
    }
    if (dist_m(a->lat, a->lon, lat, lon) < lim_m)
        return;
    a->lat = lat;
    a->lon = lon;
    atlas_clear_px(a);
    printf("map: atlas recenter %.5f %.5f  mpp=%.2f\n", lat, lon, a->mpp);
}

static uint16_t atlas_px(const Atlas *a, int x, int y)
{
    if ((unsigned)x >= (unsigned)a->w || (unsigned)y >= (unsigned)a->h)
        return COL_LAND;
    return a->px[y * a->w + x];
}

static Atlas *atlas_pick(void)
{
    double near_w = (double)ATLAS_W * NEAR_MPP;

    /* Use the sheet that already covers this zoom — switch while the
     * knob moves, not after it settles. */
    if (g_near.px && g_near.npaint > 0 && g_meters <= near_w * 0.95)
        return &g_near;
    if (g_far.px && g_far.npaint > 0)
        return &g_far;
    if (g_near.px && g_near.npaint > 0)
        return &g_near;
    return NULL;
}

static void blit_atlas(const Atlas *a)
{
    int x, y, ox, oy, k16, sx, row_sx, row_sy;
    double k;

    atlas_to_px(a, g_lat, g_lon, &ox, &oy);
    if (speeduntrap_zoom_busy() && g_zoom_from == a) {
        if (!g_zoom_lock) {
            g_zoom_ox = ox;
            g_zoom_oy = oy;
            g_zoom_lock = 1;
        }
        ox = g_zoom_ox;
        oy = g_zoom_oy;
    } else {
        g_zoom_from = (Atlas *)a;
        g_zoom_ox = ox;
        g_zoom_oy = oy;
        g_zoom_lock = speeduntrap_zoom_busy();
    }
    k = (g_meters / (double)g_w) / a->mpp;
    if (k < 0.02)
        k = 0.02;
    if (k > 32.0)
        k = 32.0;
    k16 = (int)(k * 65536.0);
    for (y = 0; y < g_h; y++) {
        row_sy = (oy << 16) + (y - g_h / 2) * k16;
        row_sx = (ox << 16) + (0 - g_w / 2) * k16;
        sx = row_sx;
        for (x = 0; x < g_w; x++) {
            g_fb[y * g_w + x] = atlas_px(a, sx >> 16, row_sy >> 16);
            sx += k16;
        }
    }
}

int speeduntrap_atlas_pump(double lat, double lon)
{
    int i, n, tx, ty, ctx, cty, dtx, dty;
    const uint8_t *data;
    uint32_t len;

    if (atlas_alloc(&g_far, FAR_MPP, 1) != 0)
        return 0;
    (void)atlas_alloc(&g_near, NEAR_MPP, 0);
    atlas_recenter(&g_far, lat, lon, 1800.0);
    if (g_near.px)
        atlas_recenter(&g_near, lat, lon, 400.0);

    n = speeduntrap_cache_n();
    for (i = 0; i < n; i++) {
        if (!speeduntrap_cache_slot(i, &tx, &ty, &data, &len) || !data)
            continue;
        if (atlas_has(&g_far, tx, ty))
            continue;
        atlas_paint_tile(&g_far, data, len);
        atlas_mark(&g_far, tx, ty);
        if (g_far.npaint == 1 || (g_far.npaint % 8) == 0)
            printf("map: atlas far %d tiles\n", g_far.npaint);
        return 1;
    }
    if (!g_near.px)
        return 0;
    speeduntrap_tile_of(lat, lon, &ctx, &cty);
    for (i = 0; i < n; i++) {
        if (!speeduntrap_cache_slot(i, &tx, &ty, &data, &len) || !data)
            continue;
        dtx = tx - ctx;
        dty = ty - cty;
        if (dtx < 0) dtx = -dtx;
        if (dty < 0) dty = -dty;
        if (dtx > 1 || dty > 1)
            continue;
        if (atlas_has(&g_near, tx, ty))
            continue;
        atlas_paint_tile(&g_near, data, len);
        atlas_mark(&g_near, tx, ty);
        if (g_near.npaint == 1 || g_near.npaint >= 9)
            printf("map: atlas near %d tiles\n", g_near.npaint);
        return 1;
    }
    return 0;
}

void speeduntrap_atlas_free(void)
{
    free(g_far.px);
    if (g_near.px && g_near.px != g_far.px)
        free(g_near.px);
    memset(&g_far, 0, sizeof(g_far));
    memset(&g_near, 0, sizeof(g_near));
    g_zoom_lock = 0;
    g_zoom_from = NULL;
    geo_clear_all();
    g_geo_set = 0;
    g_geo_logn = -1;
}

static Atlas *atlas_for_view(void)
{
    return atlas_pick();
}

int speeduntrap_zoom_busy(void)
{
    double d = g_meters_tgt - g_meters;

    return (d > 0.8 || d < -0.8);
}

static void draw_range_ring(void)
{
    int cx = g_w / 2, cy = g_h / 2;
    int rx, ry, rpx, i, n = 72;
    double range_m = (double)radar_range_m();

    to_screen(g_lat + range_m / 111320.0, g_lon, &rx, &ry);
    rpx = ry > cy ? ry - cy : cy - ry;
    if (rpx < 8)
        rpx = 8;
    for (i = 0; i < n; i++) {
        double a0, a1;
        int x0, y0, x1, y1;

        if ((i / 2) & 1)
            continue;
        a0 = (double)i * (2.0 * M_PI / (double)n);
        a1 = (double)(i + 1) * (2.0 * M_PI / (double)n);
        x0 = cx + (int)(cos(a0) * rpx);
        y0 = cy + (int)(sin(a0) * rpx);
        x1 = cx + (int)(cos(a1) * rpx);
        y1 = cy + (int)(sin(a1) * rpx);
        line(x0, y0, x1, y1, COL_RING);
    }
}

void speeduntrap_render(uint16_t *fb, int w, int h,
                        double lat, double lon, double hdg, int have_hdg,
                        double speed_kmh, const RadarPoint *hit, double dist,
                        const RadarPoint *pts, int npts, int hit_i,
                        int with_map)
{
    int i, px, py;
    double vis_m, latm, lonm;

    g_fb = fb;
    g_w = w;
    g_h = h;
    g_lat = lat;
    g_lon = lon;
    g_have_hdg = have_hdg;
    g_hdg = have_hdg ? hdg : 0.0;
    g_hairline = 0;

    fill_rect(0, 0, w, h, COL_LAND);
    if (with_map) {
        geo_sync();
        geo_draw();
    }

    if (with_map) {
        draw_range_ring();
        vis_m = g_meters * 0.7;
        latm = vis_m / 111320.0;
        lonm = vis_m / (111320.0 * cos(lat * M_PI / 180.0) + 1e-9);
        for (i = 0; i < npts; i++) {
            double dlat, dlon;
            int radp;
            if (pts[i].lat > lat + latm || pts[i].lat < lat - latm)
                continue;
            if (pts[i].lon > lon + lonm || pts[i].lon < lon - lonm)
                continue;
            dlat = (pts[i].lat - lat) * 111320.0;
            dlon = (pts[i].lon - lon) * 111320.0 * cos(lat * M_PI / 180.0);
            if (dlat * dlat + dlon * dlon > vis_m * vis_m)
                continue;
            to_screen(pts[i].lat, pts[i].lon, &px, &py);
            radp = (i == hit_i) ? 7 : 4;
            fill_disk(px, py, radp, i == hit_i ? COL_HOT : COL_PIN);
        }
    }
    draw_car();
    shade_panel();
    if (hit)
        draw_hud(hit, dist, speed_kmh);
}
