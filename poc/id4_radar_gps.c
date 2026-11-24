/*
 * Live GPS = OEM fused dead reckoning, same feed as the map cursor.
 *
 * Evidence (id4_pos_probe on HU):
 *   SBAttachInterface("PosReal.DNAVIPos", 19, 0)
 *   DSI MessageHeader version 4 (not 19)
 *   startNotify 0xC0000000 → TPositioning / TCarPosition
 *   NDS: lon then lat, deg = i32 * 360 / 2^32
 *   garage: -26.96139, -50.42229 (~180 m from TEST pin)
 *
 * No DID, no OBD, no /dev/nav/gps.
 */

#define _QNX_SOURCE
#include "id4_radar.h"
#include "id4_zbe.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/netmgr.h>
#include <sys/iomsg.h>
#include <time.h>

extern int SBOpen(const char *path);
extern int SBClose(int fd);
extern int SBAttachInterface(int fd, const char *name, unsigned short maj,
                             unsigned short min, void *info);
extern int SBDetachInterface(int fd, int id0, int id1);

#define POS_IFACE         "PosReal.DNAVIPos"
#define POS_MAJOR         19
#define POS_MINOR         0
#define POS_HDR_VER       4
#define POS_UPD_FUSED     0xC0000000u
#define GPS_STALE_MS      5000
#define GPS_RETRY_MS      3000

static uint32_t g_seq = 1;
static int g_coid = -1;
static int g_chid = -1;
static uint32_t g_party[4];
static uint8_t g_hdr_major = POS_HDR_VER;
static volatile int g_stop;
static volatile int g_on_bus;
static volatile int g_started;
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static double g_lat, g_lon, g_hdg;
static int g_have_hdg;
static long g_last_ok_ms;
static long g_last_print_ms;
static double g_print_lat, g_print_lon, g_print_hdg = -999.0;

static long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t rd_i32(const uint8_t *p)
{
    return (int32_t)rd_u32(p);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static float rd_f32(const uint8_t *p)
{
    float f;

    memcpy(&f, p, 4);
    return f;
}

static double nds_deg(int32_t v)
{
    return (double)v * (360.0 / 4294967296.0);
}

static void fill_hdr(uint8_t *h, uint32_t command, uint32_t payload_n)
{
    memset(h, 0, DSI_MSGHDR_SIZE);
    wr_u32(h + 0, DSI_RESULT_OK);
    h[4] = g_hdr_major;
    memcpy(h + 8, g_party, 16);
    wr_u32(h + 0x18, command);
    wr_u32(h + 0x20, payload_n);
}

static int dsi_send_on(int coid, uint32_t reqtype, uint32_t extra2,
                       uint32_t extra3)
{
    uint8_t hdr[DSI_MSGHDR_SIZE];
    uint32_t info[4];
    uint8_t rbuf[64];
    iov_t siov[2], riov[2];
    uint32_t rhdr[10];
    int n;

    if (coid < 0)
        return -1;
    memset(info, 0, sizeof(info));
    info[1] = reqtype;
    info[2] = extra2;
    info[3] = extra3;
    fill_hdr(hdr, DSI_CMD_DATA_REQUEST, DSI_REQINFO_SIZE);
    SETIOV(&siov[0], hdr, DSI_MSGHDR_SIZE);
    SETIOV(&siov[1], info, DSI_REQINFO_SIZE);
    memset(rhdr, 0, sizeof(rhdr));
    memset(rbuf, 0, sizeof(rbuf));
    SETIOV(&riov[0], rhdr, sizeof(rhdr));
    SETIOV(&riov[1], rbuf, sizeof(rbuf));
    n = MsgSendv(coid, siov, 2, riov, 2);
    if (n < 0)
        return -1;
    g_seq++;
    return 0;
}

static int probe_target(uint32_t nd, pid_t pid, int chid)
{
    uint8_t hdr[DSI_MSGHDR_SIZE];
    int32_t cinfo[4];
    int32_t reply[4];
    iov_t siov[2], riov[1];
    int coid, n, rnd;

    coid = ConnectAttach(nd, pid, chid, _NTO_SIDE_CHANNEL, _NTO_COF_CLOEXEC);
    if (coid < 0)
        return -1;
    rnd = netmgr_remote_nd((int)nd, 0);
    if (rnd < 0)
        rnd = 0;
    memset(cinfo, 0, sizeof(cinfo));
    cinfo[0] = g_chid;
    cinfo[1] = g_chid;
    cinfo[2] = (int32_t)getpid();
    cinfo[3] = rnd;
    fill_hdr(hdr, DSI_CMD_CONNECT, DSI_CONNECT_INFO_SIZE);
    SETIOV(&siov[0], hdr, DSI_MSGHDR_SIZE);
    SETIOV(&siov[1], cinfo, DSI_CONNECT_INFO_SIZE);
    memset(reply, 0, sizeof(reply));
    SETIOV(&riov[0], reply, sizeof(reply));
    n = MsgSendv(coid, siov, 2, riov, 1);
    if (n < 0) {
        ConnectDetach(coid);
        return -1;
    }
    return coid;
}

static int decode_fused(const uint8_t *p, uint32_t n,
                        double *lat, double *lon, double *hdg)
{
    int32_t nds_lon, nds_lat;
    double la, lo;
    float h;

    if (n < 48 || rd_u32(p + 8) < 1)
        return -1;
    nds_lon = rd_i32(p + 12);
    nds_lat = rd_i32(p + 16);
    lo = nds_deg(nds_lon);
    la = nds_deg(nds_lat);
    if (la < -90.0 || la > 90.0 || lo < -180.0 || lo > 180.0)
        return -1;
    /* TCarPosition third float (wire +44), not +32.
     * GPS-DR (NavigationPositioning.c): lat += cos(h)*ds/R, lon += sin(h)*ds/R
     * so h=0 is north, h=π/2 is east. FUN_080e852a: 0/90/180/270 = N/E/S/W.
     * Garage: +32=-18.36 (not a compass), +44=325.56 (OSM street ~327°). */
    h = rd_f32(p + 44);
    *lat = la;
    *lon = lo;
    *hdg = (double)h;
    return 0;
}

static void on_fused(const uint8_t *args, uint32_t argsn)
{
    double lat, lon, hdg, raw;
    long t;
    static int dumped;

    if (decode_fused(args, argsn, &lat, &lon, &hdg) != 0)
        return;
    if (!dumped && argsn >= 48) {
        dumped = 1;
        printf("gps: TCarPosition f32 +24=%.2f +28=%.2f +32=%.2f +36=%.2f +40=%.2f +44=%.2f\n",
               rd_f32(args + 24), rd_f32(args + 28), rd_f32(args + 32),
               rd_f32(args + 36), rd_f32(args + 40), rd_f32(args + 44));
    }
    t = now_ms();
    raw = hdg;
    pthread_mutex_lock(&g_lk);
    g_lat = lat;
    g_lon = lon;
    /* +44 is degrees clockwise from north (same frame as the DR step). */
    if (hdg == hdg && hdg >= -360.0 && hdg <= 360.0) {
        while (hdg < 0.0)
            hdg += 360.0;
        while (hdg >= 360.0)
            hdg -= 360.0;
        g_hdg = hdg;
        g_have_hdg = 1;
    } else {
        g_have_hdg = 0;
    }
    g_last_ok_ms = t;
    pthread_mutex_unlock(&g_lk);
    if (!g_last_print_ms || t - g_last_print_ms > 10000 ||
        fabs(lat - g_print_lat) > 0.00002 || fabs(lon - g_print_lon) > 0.00002 ||
        (g_have_hdg && fabs(g_hdg - g_print_hdg) > 2.0)) {
        printf("gps: PosReal fused lat=%.6f lon=%.6f hdg=%.1f%s\n",
               lat, lon, g_have_hdg ? g_hdg : raw,
               g_have_hdg ? " (icon yaw +44, draw h-90)" : " (no icon yaw)");
        g_print_lat = lat;
        g_print_lon = lon;
        g_print_hdg = g_hdg;
        g_last_print_ms = t;
    }
}

static void parse_dsi_rx(const uint8_t *p, uint32_t n)
{
    uint32_t extra2, argsn;
    const uint8_t *args;

    if (n < DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE)
        return;
    extra2 = rd_u32(p + DSI_MSGHDR_SIZE + 8);
    args = p + DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE;
    argsn = n - (DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE);
    if (extra2 == POS_UPD_FUSED && argsn)
        on_fused(args, argsn);
}

static int gps_attach_once(int *sfd_out, SConnectionInfo *info)
{
    int sfd, coid;
    const char *path = DSI_SB_PATH;
    char *env;

    env = getenv("SB_PATH");
    if (env && env[0])
        path = env;
    sfd = SBOpen(path);
    if (sfd < 0) {
        printf("gps: SBOpen errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    memset(info, 0, sizeof(*info));
    if (SBAttachInterface(sfd, POS_IFACE, POS_MAJOR, POS_MINOR, info) != 0) {
        printf("gps: SBAttach %s %u.%u rc=%d\n", POS_IFACE, POS_MAJOR, POS_MINOR,
               errno);
        SBClose(sfd);
        return -1;
    }
    g_hdr_major = POS_HDR_VER;
    g_party[0] = (uint32_t)info->extra[0];
    g_party[1] = (uint32_t)info->extra[1];
    g_party[2] = (uint32_t)info->extra[2];
    g_party[3] = (uint32_t)info->extra[3];
    g_chid = ChannelCreate(0);
    if (g_chid < 0) {
        SBDetachInterface(sfd, info->mchid, info->schid);
        SBClose(sfd);
        return -1;
    }
    coid = probe_target(0, (pid_t)info->mchid, info->schid);
    if (coid < 0 && info->schid != 7)
        coid = probe_target(0, (pid_t)info->mchid, 7);
    if (coid < 0)
        coid = probe_target((uint32_t)info->nd, (pid_t)info->mchid, info->schid);
    if (coid < 0) {
        ChannelDestroy(g_chid);
        g_chid = -1;
        SBDetachInterface(sfd, info->mchid, info->schid);
        SBClose(sfd);
        return -1;
    }
    g_coid = coid;
    dsi_send_on(g_coid, DSI_REQTYPE_NOTIFY, POS_UPD_FUSED, g_seq);
    g_on_bus = 1;
    *sfd_out = sfd;
    printf("gps: %s %u.%u fused DR (maps), no DID/OBD\n",
           POS_IFACE, POS_MAJOR, POS_MINOR);
    return 0;
}

static void gps_detach(int sfd, SConnectionInfo *info)
{
    if (g_coid >= 0) {
        ConnectDetach(g_coid);
        g_coid = -1;
    }
    if (g_chid >= 0) {
        ChannelDestroy(g_chid);
        g_chid = -1;
    }
    if (sfd >= 0) {
        SBDetachInterface(sfd, info->mchid, info->schid);
        SBClose(sfd);
    }
    g_on_bus = 0;
}

static void *gps_thread(void *arg)
{
    SConnectionInfo info;
    int sfd = -1;

    (void)arg;
    while (!g_stop) {
        if (sfd < 0) {
            if (gps_attach_once(&sfd, &info) != 0) {
                usleep((useconds_t)GPS_RETRY_MS * 1000);
                continue;
            }
        }
        {
            uint8_t buf[2048];
            struct _msg_info mi;
            int rcvid;
            uint64_t to = 500000000ULL;

            TimerTimeout(CLOCK_REALTIME, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
            rcvid = MsgReceive(g_chid, buf, sizeof(buf), &mi);
            if (rcvid < 0) {
                if (errno == ETIMEDOUT || errno == EINTR)
                    continue;
                printf("gps: MsgReceive errno=%d (%s) — reattach\n",
                       errno, strerror(errno));
                gps_detach(sfd, &info);
                sfd = -1;
                usleep((useconds_t)GPS_RETRY_MS * 1000);
                continue;
            }
            if (rcvid == 0)
                continue;
            parse_dsi_rx(buf, (uint32_t)mi.msglen);
            MsgReply(rcvid, 0, NULL, 0);
        }
    }
    gps_detach(sfd, &info);
    return NULL;
}

int radar_gps_start(void)
{
    pthread_t th;

    g_stop = 0;
    g_on_bus = 0;
    g_last_ok_ms = 0;
    g_last_print_ms = 0;
    g_seq = 1;
    if (g_started)
        return 0;
    if (pthread_create(&th, NULL, gps_thread, NULL) != 0) {
        printf("gps: thread failed (%s)\n", strerror(errno));
        return -1;
    }
    pthread_detach(th);
    g_started = 1;
    printf("gps: OEM PosReal.DNAVIPos 19.0 (Kalman DR, notify 0xC0000000)\n");
    return 0;
}

void radar_gps_stop(void)
{
    g_stop = 1;
    g_on_bus = 0;
    g_started = 0;
}

int radar_gps_on_bus(void)
{
    return g_on_bus;
}

int radar_gps_poll(void)
{
    double lat, lon, hdg;
    int have_hdg;
    long ok, t = now_ms();

    pthread_mutex_lock(&g_lk);
    lat = g_lat;
    lon = g_lon;
    hdg = g_hdg;
    have_hdg = g_have_hdg;
    ok = g_last_ok_ms;
    pthread_mutex_unlock(&g_lk);
    if (!ok || t - ok > GPS_STALE_MS)
        return 0;
    radar_set_gps(lat, lon, hdg, have_hdg);
    return 1;
}
