#ifndef SPEEDUNTRAP_UI_H
#define SPEEDUNTRAP_UI_H

#include <stdint.h>
#include "id4_radar.h"

int speeduntrap_map_open(void);
void speeduntrap_map_close(void);
void speeduntrap_map_sync(double lat, double lon, double hdg);
int speeduntrap_map_pump(double lat, double lon);
int speeduntrap_cache_n(void);
int speeduntrap_cache_slot(int i, int *tx, int *ty,
                           const uint8_t **data, uint32_t *len);
int speeduntrap_atlas_pump(double lat, double lon);
void speeduntrap_atlas_free(void);
int speeduntrap_view_ring(void);
void speeduntrap_zoom(int delta);
int speeduntrap_zoom_tick(void);
int speeduntrap_zoom_busy(void);
void speeduntrap_tile_of(double lat, double lon, int *tx, int *ty);
void speeduntrap_map_foreach(int tx0, int ty0, int tx1, int ty1,
                             void (*fn)(const uint8_t *data, uint32_t len, void *ctx),
                             void *ctx);
uint32_t speeduntrap_map_deg_e6(void);

void speeduntrap_render(uint16_t *fb, int w, int h,
                        double lat, double lon, double hdg, int have_hdg,
                        double speed_kmh, const RadarPoint *hit, double dist,
                        const RadarPoint *pts, int npts, int hit_i,
                        int with_map);

#endif
