/*
 * Seek-only map pack (radar-tiles-v1 binary). Never opens a PBF.
 * Loads a ring of ~2 km tiles around the car, same idea as the PC emulator.
 */

#include "speeduntrap_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define LOAD_RING 2
#define DROP_RING 3
#define CACHE_MAX 32
#define TILE_LEN_MAX (512u * 1024u)

typedef struct {
    int32_t tx;
    int32_t ty;
    uint32_t off;
    uint32_t len;
} MapIdx;

typedef struct {
    int tx, ty;
    uint32_t len;
    uint8_t *data;
} MapTile;

static FILE *g_fp;
static MapIdx *g_idx;
static uint32_t g_ntiles;
static uint32_t g_deg_e6 = 20000;
static MapTile g_cache[CACHE_MAX];
static int g_ncache;
static int g_cx = 0x7fffffff, g_cy = 0x7fffffff;
static const char *k_map[] = {
    "/fs/sda0/radar/map.bin",
    "/fs/usb0/radar/map.bin",
    "/mnt/umass0/radar/map.bin",
    "radar/map.bin",
    NULL
};

int speeduntrap_map_ntiles(void) { return (int)g_ntiles; }
uint32_t speeduntrap_map_deg_e6(void) { return g_deg_e6; }

int speeduntrap_cache_n(void) { return g_ncache; }

int speeduntrap_cache_slot(int i, int *tx, int *ty,
                           const uint8_t **data, uint32_t *len)
{
    if (i < 0 || i >= g_ncache)
        return 0;
    if (tx) *tx = g_cache[i].tx;
    if (ty) *ty = g_cache[i].ty;
    if (data) *data = g_cache[i].data;
    if (len) *len = g_cache[i].len;
    return 1;
}

static int g_loads;

static long map_now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static int find_idx(int tx, int ty)
{
    int lo = 0, hi = (int)g_ntiles - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_idx[mid].tx < tx || (g_idx[mid].tx == tx && g_idx[mid].ty < ty))
            lo = mid + 1;
        else if (g_idx[mid].tx > tx || (g_idx[mid].tx == tx && g_idx[mid].ty > ty))
            hi = mid - 1;
        else
            return mid;
    }
    return -1;
}

static MapTile *cache_find(int tx, int ty)
{
    int i;

    for (i = 0; i < g_ncache; i++) {
        if (g_cache[i].tx == tx && g_cache[i].ty == ty)
            return &g_cache[i];
    }
    return NULL;
}

static MapTile *cache_get(int tx, int ty)
{
    int slot, id;
    MapTile *t;
    long t0;

    t = cache_find(tx, ty);
    if (t)
        return t;
    id = find_idx(tx, ty);
    if (g_fp == NULL)
        return NULL;
    if (g_ncache < CACHE_MAX)
        slot = g_ncache++;
    else {
        slot = 0;
        free(g_cache[0].data);
        memmove(&g_cache[0], &g_cache[1], (CACHE_MAX - 1) * sizeof(MapTile));
        slot = CACHE_MAX - 1;
        g_ncache = CACHE_MAX;
    }
    t = &g_cache[slot];
    memset(t, 0, sizeof(*t));
    t->tx = tx;
    t->ty = ty;
    if (id < 0) {
        t->len = 0;
        t->data = NULL;
        return t;
    }
    t->len = g_idx[id].len;
    if (t->len == 0 || t->len > TILE_LEN_MAX) {
        printf("map: skip %d,%d len=%u\n", tx, ty, t->len);
        return t;
    }
    t->data = (uint8_t *)malloc(t->len);
    if (!t->data)
        return NULL;
    g_loads++;
    if (g_loads <= 12)
        printf("map: seek %d,%d off=%u len=%u\n", tx, ty, g_idx[id].off, t->len);
    t0 = map_now_ms();
    if (fseek(g_fp, (long)g_idx[id].off, SEEK_SET) != 0 ||
        fread(t->data, 1, t->len, g_fp) != t->len) {
        printf("map: read fail %d,%d off=%u\n", tx, ty, g_idx[id].off);
        free(t->data);
        t->data = NULL;
        return NULL;
    }
    if (g_loads <= 12)
        printf("map: got %d,%d in %ld ms\n", tx, ty, map_now_ms() - t0);
    return t;
}

void speeduntrap_map_foreach(int tx0, int ty0, int tx1, int ty1,
                             void (*fn)(const uint8_t *data, uint32_t len, void *ctx),
                             void *ctx)
{
    int tx, ty;
    MapTile *t;

    for (ty = ty0; ty <= ty1; ty++) {
        for (tx = tx0; tx <= tx1; tx++) {
            t = cache_find(tx, ty);
            if (t && t->data)
                fn(t->data, t->len, ctx);
        }
    }
}

void speeduntrap_tile_of(double lat, double lon, int *tx, int *ty)
{
    double deg = g_deg_e6 / 1e6;
    double fx = lon / deg;
    double fy = lat / deg;

    *tx = (int)fx;
    *ty = (int)fy;
    if (fx < 0.0 && (double)*tx != fx)
        (*tx)--;
    if (fy < 0.0 && (double)*ty != fy)
        (*ty)--;
}

int speeduntrap_map_open(void)
{
    int i;
    char magic[8];
    uint32_t ver, index_off, dummy;

    for (i = 0; k_map[i]; i++) {
        g_fp = fopen(k_map[i], "rb");
        if (g_fp) {
            setvbuf(g_fp, NULL, _IOFBF, 64 * 1024);
            break;
        }
    }
    if (!g_fp) {
        printf("map: no map.bin (looked /fs/sda0/radar/map.bin)\n");
        return -1;
    }
    if (fread(magic, 1, 8, g_fp) != 8 || memcmp(magic, "RMAP1", 5) != 0) {
        printf("map: bad magic in %s\n", k_map[i]);
        fclose(g_fp);
        g_fp = NULL;
        return -1;
    }
    if (fread(&ver, 4, 1, g_fp) != 1 ||
        fread(&g_deg_e6, 4, 1, g_fp) != 1 ||
        fread(&g_ntiles, 4, 1, g_fp) != 1 ||
        fread(&index_off, 4, 1, g_fp) != 1 ||
        fread(&dummy, 4, 1, g_fp) != 1 ||
        fread(&dummy, 4, 1, g_fp) != 1) {
        printf("map: short header\n");
        fclose(g_fp);
        g_fp = NULL;
        return -1;
    }
    if (ver != 1 || g_ntiles == 0 || g_ntiles > 2000000) {
        printf("map: unsupported header ver=%u n=%u\n", ver, g_ntiles);
        fclose(g_fp);
        g_fp = NULL;
        return -1;
    }
    g_idx = (MapIdx *)malloc(g_ntiles * sizeof(MapIdx));
    if (!g_idx) {
        fclose(g_fp);
        g_fp = NULL;
        return -1;
    }
    if (fseek(g_fp, (long)index_off, SEEK_SET) != 0 ||
        fread(g_idx, sizeof(MapIdx), g_ntiles, g_fp) != g_ntiles) {
        printf("map: index read failed\n");
        free(g_idx);
        g_idx = NULL;
        fclose(g_fp);
        g_fp = NULL;
        return -1;
    }
    printf("map: %s  tiles=%u  cell=%.0f m\n",
           k_map[i], g_ntiles, g_deg_e6 / 1e6 * 111320.0);
    return 0;
}

void speeduntrap_map_close(void)
{
    int i;

    for (i = 0; i < g_ncache; i++)
        free(g_cache[i].data);
    g_ncache = 0;
    free(g_idx);
    g_idx = NULL;
    if (g_fp) {
        fclose(g_fp);
        g_fp = NULL;
    }
}

void speeduntrap_map_sync(double lat, double lon, double hdg)
{
    (void)hdg;
    speeduntrap_map_pump(lat, lon);
}

int speeduntrap_map_pump(double lat, double lon)
{
    int tx, ty, x, y, r, n = 0, need;

    speeduntrap_tile_of(lat, lon, &tx, &ty);
    g_cx = tx;
    g_cy = ty;
    /* Always the max-out neighbourhood (ring 2), not the current zoom. */
    need = (2 * LOAD_RING + 1) * (2 * LOAD_RING + 1);
    for (r = 0; r <= LOAD_RING; r++) {
        for (y = ty - r; y <= ty + r; y++) {
            for (x = tx - r; x <= tx + r; x++) {
                if (r && x > tx - r && x < tx + r && y > ty - r && y < ty + r)
                    continue;
                n++;
                if (cache_find(x, y))
                    continue;
                cache_get(x, y);
                printf("map: window %d,%d  cache %d/%d\n", tx, ty, n, need);
                return 1;
            }
        }
    }
    return 0;
}
