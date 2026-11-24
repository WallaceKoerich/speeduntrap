/*
 * Range-enter chime via WPRing.DMMWavePlayer + AM source 0x16.
 *
 * Live HU (id4_chime_probe wavepath):
 *   AM requestFocus/makeAudible src=0x16 sink=1
 *   sampleConfiguration type 2 CHBString (FUN_08091261) + WAV path
 *   executeCommand(0) → state 5. Stereo 44.1 kHz 16-bit PCM only.
 *   Player loops until STOP — cut after one 0.89 s play.
 *   Mono WAV goes 4→0 (silent). Do not send a path without the type-2 tag.
 */
#define _QNX_SOURCE
#include "id4_radar_chime.h"
#include "id4_aa_am.h"
#include "id4_zbe.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/netmgr.h>
#include <sys/iomsg.h>

extern int SBOpen(const char *path);
extern int SBClose(int fd);
extern int SBAttachInterface(int fd, const char *name, unsigned short maj,
                             unsigned short min, void *info);
extern int SBDetachInterface(int fd, int id0, int id1);

#define WAVE_IFACE     "WPRing.DMMWavePlayer"
#define WAVE_HDR       4
#define WAVE_PATH      "/fs/sda0/radar/chime.wav"
#define RING_MS        900

struct DsiBuf {
    uint8_t p[128];
    uint32_t n;
};

static uint32_t g_seq = 1;
static uint32_t g_party[4];
static int g_coid = -1;
static int g_chid = -1;
static int g_sfd = -1;
static int g_mchid;
static int g_schid;
static uint8_t g_hdr = WAVE_HDR;
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_rx;
static volatile int g_ok;
static volatile int g_stop;
static volatile int g_rx_stop;
static volatile int g_busy;
static volatile int g_cfg;
static volatile uint32_t g_state;

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void db_align(struct DsiBuf *b, unsigned a)
{
    while (b->n & (a - 1))
        b->p[b->n++] = 0;
}

static void db_u32(struct DsiBuf *b, uint32_t v)
{
    db_align(b, 4);
    wr_u32(b->p + b->n, v);
    b->n += 4;
}

static void db_u16(struct DsiBuf *b, uint16_t v)
{
    db_align(b, 2);
    b->p[b->n++] = (uint8_t)(v & 0xFF);
    b->p[b->n++] = (uint8_t)(v >> 8);
}

/* CHBDSIOStream::write(CHBString): uint32 (strlen+1) then bytes + NUL. */
static void db_chb(struct DsiBuf *b, const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;

    db_u32(b, n);
    if (b->n + n > sizeof(b->p))
        n = (uint32_t)(sizeof(b->p) - b->n);
    memcpy(b->p + b->n, s, n);
    b->n += n;
}

static void fill_hdr(uint8_t *h, uint32_t command, uint32_t payload_n)
{
    memset(h, 0, DSI_MSGHDR_SIZE);
    wr_u32(h + 0, DSI_RESULT_OK);
    h[4] = g_hdr;
    memcpy(h + 8, g_party, 16);
    wr_u32(h + 0x18, command);
    wr_u32(h + 0x20, payload_n);
}

static int dsi_send(uint32_t reqtype, uint32_t extra2, const struct DsiBuf *args)
{
    uint8_t hdr[DSI_MSGHDR_SIZE];
    uint32_t info[4];
    uint8_t rbuf[64];
    iov_t siov[3], riov[2];
    uint32_t rhdr[10];
    uint32_t payn;
    int n, niov;

    if (g_coid < 0)
        return -1;
    payn = args ? args->n : 0;
    memset(info, 0, sizeof(info));
    info[1] = reqtype;
    info[2] = extra2;
    fill_hdr(hdr, DSI_CMD_DATA_REQUEST, DSI_REQINFO_SIZE + payn);
    SETIOV(&siov[0], hdr, DSI_MSGHDR_SIZE);
    SETIOV(&siov[1], info, DSI_REQINFO_SIZE);
    niov = 2;
    if (payn) {
        SETIOV(&siov[2], args->p, payn);
        niov = 3;
    }
    memset(rhdr, 0, sizeof(rhdr));
    memset(rbuf, 0, sizeof(rbuf));
    SETIOV(&riov[0], rhdr, sizeof(rhdr));
    SETIOV(&riov[1], rbuf, sizeof(rbuf));
    pthread_mutex_lock(&g_lk);
    info[3] = g_seq;
    n = MsgSendv(g_coid, siov, niov, riov, 2);
    if (n >= 0)
        g_seq++;
    pthread_mutex_unlock(&g_lk);
    if (n < 0) {
        printf("chime: WAVE MsgSendv extra2=0x%X errno=%d (%s)\n",
               extra2, errno, strerror(errno));
        return -1;
    }
    return 0;
}

static int connect_one(uint32_t nd, pid_t pid, int chid)
{
    uint8_t hdr[DSI_MSGHDR_SIZE];
    int32_t cinfo[4];
    int32_t reply[4];
    iov_t siov[2], riov[1];
    int coid, n, rnd;
    uint64_t to = 200000000ULL;

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
    TimerTimeout(CLOCK_REALTIME, _NTO_TIMEOUT_SEND, NULL, &to, NULL);
    n = MsgSendv(coid, siov, 2, riov, 1);
    if (n < 0) {
        ConnectDetach(coid);
        return -1;
    }
    printf("chime: WAVE ConnectRequest nd=%u pid=%d chid=%d ok reply=%d %d %d %d\n",
           nd, (int)pid, chid, reply[0], reply[1], reply[2], reply[3]);
    return coid;
}

static void *rx_thread(void *arg)
{
    (void)arg;
    while (!g_stop && !g_rx_stop) {
        uint8_t buf[256];
        struct _msg_info mi;
        int rcvid;
        uint64_t to = 250000000ULL;

        TimerTimeout(CLOCK_REALTIME, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
        rcvid = MsgReceive(g_chid, buf, sizeof(buf), &mi);
        if (rcvid < 0) {
            if (errno == ETIMEDOUT || errno == EINTR)
                continue;
            printf("chime: WAVE MsgReceive errno=%d (%s)\n",
                   errno, strerror(errno));
            break;
        }
        if (rcvid == 0)
            continue;
        if ((uint32_t)mi.msglen >= DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE) {
            uint32_t extra2 = rd_u32(buf + DSI_MSGHDR_SIZE + 8);
            uint32_t argsn = (uint32_t)mi.msglen -
                             (DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE);
            const uint8_t *args = buf + DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE;

            if (extra2 == 0xC0000000u && argsn >= 4) {
                g_state = rd_u32(args);
                printf("chime: WAVE state %u\n", g_state);
            } else if (extra2 == 0x80000001u && argsn >= 4) {
                g_cfg = 1;
                if (rd_u32(args) == 2)
                    printf("chime: WAVE type2 path configured\n");
                else
                    printf("chime: WAVE sample 0x%02X configured\n",
                           argsn >= 5 ? args[4] : 0);
            } else if (extra2 == 0x80000000u && argsn == 0) {
                printf("chime: WAVE execute void-OK\n");
            } else if ((extra2 == 0x80000000u || extra2 == 0x80000001u) &&
                       argsn >= 4) {
                printf("chime: WAVE extra2=0x%08X result=%u\n",
                       extra2, rd_u32(args));
            } else {
                printf("chime: WAVE rx extra2=0x%08X n=%u\n", extra2, argsn);
            }
        }
        MsgReply(rcvid, 0, NULL, 0);
    }
    return NULL;
}

static int wave_open(void)
{
    SConnectionInfo info;
    const char *path = DSI_SB_PATH;
    char *env;
    static const uint32_t kAttrs[] = {
        0xC0000000u, 0xC0000001u, 0xC0000002u, 0xC0000003u,
        0x80000000u, 0x80000001u, 0x80000002u, 0x80000003u
    };
    unsigned i;
    int coid = -1;

    if (g_ok)
        return 0;
    g_rx_stop = 0;
    g_cfg = 0;
    g_state = 0;
    env = getenv("SB_PATH");
    if (env && env[0])
        path = env;
    g_sfd = SBOpen(path);
    if (g_sfd < 0) {
        printf("chime: WAVE SBOpen errno=%d (%s)\n", errno, strerror(errno));
        return -1;
    }
    memset(&info, 0, sizeof(info));
    if (SBAttachInterface(g_sfd, WAVE_IFACE, 1, 0, &info) != 0) {
        printf("chime: WAVE SBAttach errno=%d (%s)\n", errno, strerror(errno));
        SBClose(g_sfd);
        g_sfd = -1;
        return -1;
    }
    g_mchid = info.mchid;
    g_schid = info.schid;
    g_party[0] = (uint32_t)info.extra[0];
    g_party[1] = (uint32_t)info.extra[1];
    g_party[2] = (uint32_t)info.extra[2];
    g_party[3] = (uint32_t)info.extra[3];
    g_chid = ChannelCreate(0);
    if (g_chid < 0) {
        SBDetachInterface(g_sfd, g_mchid, g_schid);
        SBClose(g_sfd);
        g_sfd = -1;
        return -1;
    }
    /* pid=0 is intel WPRing 274518. Do not fall through to jacinto 4106. */
    if (info.pid != 0)
        coid = connect_one((uint32_t)info.pid, (pid_t)info.mchid, info.schid);
    if (coid < 0)
        coid = connect_one(0, (pid_t)info.mchid, info.schid);
    if (coid < 0)
        coid = connect_one((uint32_t)info.nd, (pid_t)info.mchid, info.schid);
    if (coid < 0) {
        ChannelDestroy(g_chid);
        g_chid = -1;
        SBDetachInterface(g_sfd, g_mchid, g_schid);
        SBClose(g_sfd);
        g_sfd = -1;
        return -1;
    }
    g_coid = coid;
    if (pthread_create(&g_rx, NULL, rx_thread, NULL) != 0) {
        printf("chime: WAVE rx thread failed\n");
        return -1;
    }
    pthread_detach(g_rx);
    usleep(50000);
    for (i = 0; i < sizeof(kAttrs) / sizeof(kAttrs[0]); i++)
        dsi_send(DSI_REQTYPE_NOTIFY, kAttrs[i], NULL);
    g_ok = 1;
    printf("chime: WAVE %s 1.0 ready party=%u\n", WAVE_IFACE, g_party[0]);
    return 0;
}

static void wave_close(void)
{
    g_rx_stop = 1;
    if (g_coid >= 0) {
        ConnectDetach(g_coid);
        g_coid = -1;
    }
    if (g_chid >= 0) {
        ChannelDestroy(g_chid);
        g_chid = -1;
    }
    if (g_sfd >= 0) {
        if (g_mchid || g_schid)
            SBDetachInterface(g_sfd, g_mchid, g_schid);
        SBClose(g_sfd);
        g_sfd = -1;
    }
    g_ok = 0;
    g_cfg = 0;
    g_state = 0;
}

static int wait_state(uint32_t want, int ms)
{
    int i, n = (ms + 49) / 50;

    for (i = 0; i < n && !g_stop; i++) {
        if (g_state == want)
            return 0;
        usleep(50000);
    }
    return -1;
}

static void wave_cfg(void)
{
    struct DsiBuf a;

    memset(&a, 0, sizeof(a));
    db_u32(&a, 2);
    db_chb(&a, WAVE_PATH);
    db_u32(&a, 1);
    db_u16(&a, 0xFFFF);
    db_u16(&a, 1000);
    printf("chime: WAVE type2 '%s' (%u bytes)\n", WAVE_PATH, a.n);
    dsi_send(DSI_REQTYPE_REQUEST, 1, &a);
}

static void wave_cmd(uint32_t cmd)
{
    struct DsiBuf a;

    memset(&a, 0, sizeof(a));
    db_u32(&a, cmd);
    dsi_send(DSI_REQTYPE_REQUEST, 0, &a);
}

static void *chime_thread(void *arg)
{
    int i;

    (void)arg;
    /* Probe waveam: AM 0x16 held first, then WPRing attach → state 2.
     * Attaching WavePlayer at boot left state 1; START never reached 5. */
    if (aa_am_request_focus() != 0) {
        printf("chime: AM 0x16 focus failed\n");
        goto done;
    }
    if (wave_open() != 0) {
        printf("chime: WPRing attach failed\n");
        aa_am_release_focus();
        goto done;
    }
    if (wait_state(2, 1000) != 0)
        printf("chime: WAVE state %u after attach (want 2)\n", g_state);
    wave_cfg();
    for (i = 0; i < 20 && !g_cfg && !g_stop; i++)
        usleep(50000);
    if (!g_cfg) {
        printf("chime: WAVE type2 configure failed — no START\n");
        wave_close();
        aa_am_release_focus();
        goto done;
    }
    if (wait_state(3, 1000) != 0)
        printf("chime: WAVE state %u after cfg (want 3)\n", g_state);
    printf("chime: WAVE '%s' START (state %u)\n", WAVE_PATH, g_state);
    wave_cmd(0);
    if (wait_state(5, 1000) != 0) {
        printf("chime: WAVE START did not reach playing (state %u)\n",
               g_state);
    } else {
        for (i = 0; i < RING_MS / 50 && !g_stop; i++)
            usleep(50000);
    }
    wave_cmd(1);
    wait_state(3, 500);
    printf("chime: WAVE STOP (state %u)\n", g_state);
    wave_close();
    aa_am_release_focus();

done:
    pthread_mutex_lock(&g_lk);
    g_busy = 0;
    pthread_mutex_unlock(&g_lk);
    return NULL;
}

void radar_chime_log_info(void)
{
    printf("radar: chime AM 0x16 + %s (~%u ms, one play)\n",
           WAVE_PATH, RING_MS);
    if (access(WAVE_PATH, R_OK) != 0)
        printf("radar: missing %s — copy poc/radar/chime.wav\n", WAVE_PATH);
}

void radar_chime_play(void)
{
    pthread_t th;

    pthread_mutex_lock(&g_lk);
    if (g_busy || g_stop) {
        pthread_mutex_unlock(&g_lk);
        return;
    }
    g_busy = 1;
    pthread_mutex_unlock(&g_lk);

    if (pthread_create(&th, NULL, chime_thread, NULL) != 0) {
        printf("chime: pthread %s\n", strerror(errno));
        pthread_mutex_lock(&g_lk);
        g_busy = 0;
        pthread_mutex_unlock(&g_lk);
        return;
    }
    pthread_detach(th);
}

void radar_chime_shutdown(void)
{
    int i;

    g_stop = 1;
    for (i = 0; i < 40 && g_busy; i++)
        usleep(50000);
    if (g_ok)
        wave_cmd(1);
    wave_close();
    aa_am_release_focus();
}
