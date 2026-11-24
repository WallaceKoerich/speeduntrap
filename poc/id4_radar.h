/*
 * Radar detector: pack + GPS + WalControl overlay.
 * RHMI settings live in id4_radar.c (ConnectedDrive checkbox).
 */
#ifndef ID4_RADAR_H
#define ID4_RADAR_H

typedef struct {
    double lat;
    double lon;
    int leves;
    int pesados;
    int heading;
    char via[24];
    char fab[24];
    char model[24];
} RadarPoint;

int radar_init(void);
int radar_overlay_showing(void);
void radar_shutdown(void);
int radar_popup_enabled(void);
void radar_set_popup(int on);
void radar_tick(void);
void radar_set_speed_kmh(double kmh);
void radar_set_gps(double lat, double lon, double heading_deg, int heading_valid);
const char *radar_status_line(void);
int radar_test_run(void);
void radar_request_stop(void);
int radar_should_stop(void);
void radar_force_restore(void);
int radar_is_test(void);

int radar_range_m(void);
void radar_range_step(int dir);
const char *radar_range_line(void);

int radar_gps_start(void);
void radar_gps_stop(void);
int radar_gps_poll(void);
int radar_gps_on_bus(void);
void radar_emergency_stop(void);

#endif
