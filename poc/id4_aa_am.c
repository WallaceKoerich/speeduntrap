/*
 * AudioManagement DSI — HMI-owned iSpeech mix for the radar chime.
 *
 * Evidence:
 *   jacinto servicebroker.cfg [FORWARD]:
 *     AudioManagement.DMMAudioManagement
 *     AudioManagement.DMMAudioManagementNBT
 *   NbtDiag CHBVersion(1,5,0) for DMMAudioManagementNBT
 *   Connectivity CDMMAudioManagementProxy 1.0 (CHBVersion 1,0,0):
 *     FUN_08840cae method 2 requestFocus(source, sink)  args=8
 *     FUN_08840bbe method 3 releaseFocus(source, sink)  args=8
 *     CPTelAudioDeviceHandler_NBT: sink 1 = speakers
 *   ACID DAT_09879224: EAudioSource 0x56 → HighSync 0x33 (amp group 11)
 *
 * DSI ConnectionMaster builds 0x33 in C03 state 4 (muted). OEM then
 * CHmiAudioSink::muteAudioSource(false) / makeAudible so it goes to 0.
 *
 * DSI MessageHeader version is 4 (same as PosReal), not the iface 1.5.
 */

#define _QNX_SOURCE
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

#define AM_IFACE_NBT      "AudioManagement.DMMAudioManagementNBT"
#define AM_IFACE          "AudioManagement.DMMAudioManagement"
#define AM_MAJOR          1
#define AM_MINOR_NBT      5
#define AM_MINOR          0
#define AM_HDR_VER        4
#define AM_REQ_FOCUS      2u   /* Connectivity FUN_08840cae requestFocus */
#define AM_REQ_RELEASE    3u   /* FUN_08840bbe releaseFocus */
#define AM_REQ_AUDIBLE    4u   /* FUN_08840c36 makeAudible — builds the mix */
#define AM_SRC_SPEECH     0x16u  /* WPRing persistency; live HU 0x80000003 then sample 0x1A */
#define AM_SNK_SPEAKERS   1u

struct DsiBuf {
    uint8_t p[64];
    uint32_t n;
};

static uint32_t g_seq = 1;
static int g_coid = -1;
static int g_chid = -1;
static uint32_t g_party[4];
static uint8_t g_hdr_major = AM_HDR_VER;
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_ok;
static volatile int g_done;
static volatile int g_focus;
static volatile uint32_t g_note;
static volatile uint32_t g_note_src;
static char g_iface[64];

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

static void db_u32(struct DsiBuf *b, uint32_t v)
{
    if (b->n + 4 > sizeof(b->p))
        return;
    wr_u32(b->p + b->n, v);
    b->n += 4;
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
                       uint32_t extra3, const struct DsiBuf *args)
{
    uint8_t hdr[DSI_MSGHDR_SIZE];
    uint32_t info[4];
    uint8_t rbuf[256];
    iov_t siov[3], riov[2];
    uint32_t rhdr[10];
    uint32_t payn;
    int n, niov;

    if (coid < 0)
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
    info[3] = extra3 ? extra3 : g_seq;
    n = MsgSendv(coid, siov, niov, riov, 2);
    if (n >= 0)
        g_seq++;
    pthread_mutex_unlock(&g_lk);
    if (n < 0) {
        printf("chime: AM MsgSendv a2=0x%X errno=%d (%s)\n",
               extra2, errno, strerror(errno));
        return -1;
    }
    if (reqtype == DSI_REQTYPE_REQUEST) {
        unsigned i, show;

        printf("chime: AM reply method=%u n=%d res=0x%x pay",
               extra2, n, rhdr[0]);
        show = 16;
        if (n > 0 && (unsigned)n < show)
            show = (unsigned)n;
        for (i = 0; i < show; i++)
            printf(" %02X", rbuf[i]);
        printf("\n");
    }
    return 0;
}

static int probe_target(uint32_t nd, pid_t pid, int chid, const char *tag)
{
    uint8_t hdr[DSI_MSGHDR_SIZE];
    int32_t cinfo[4];
    int32_t reply[4];
    iov_t siov[2], riov[1];
    int coid, n, rnd, saved;
    uint64_t to = 200000000ULL;

    printf("chime: AM probe %s nd=%u pid=%d chid=%d hdr=%u\n",
           tag, nd, (int)pid, chid, (unsigned)g_hdr_major);
    coid = ConnectAttach(nd, pid, chid, _NTO_SIDE_CHANNEL, _NTO_COF_CLOEXEC);
    if (coid < 0) {
        printf("chime: AM ConnectAttach errno=%d (%s)\n",
               errno, strerror(errno));
        return -1;
    }
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
        saved = errno;
        printf("chime: AM ConnectRequest errno=%d (%s) rnd=%d\n",
               saved, strerror(saved), rnd);
        ConnectDetach(coid);
        return -1;
    }
    printf("chime: AM ConnectRequest ok coid=%d n=%d reply=%d %d %d %d\n",
           coid, n, reply[0], reply[1], reply[2], reply[3]);
    return coid;
}

static void dump_info(const SConnectionInfo *info)
{
    const int32_t *w = (const int32_t *)info;
    char nd0[64], nd1[64];
    int i;

    printf("chime: AM SConnectionInfo");
    for (i = 0; i < 8; i++)
        printf(" [%d]=%d/0x%x", i, (int)w[i], (unsigned)w[i]);
    printf("\n");
    memset(nd0, 0, sizeof(nd0));
    memset(nd1, 0, sizeof(nd1));
    if (netmgr_ndtostr(ND2S_DIR_SHOW, info->nd, nd0, sizeof(nd0)) >= 0)
        printf("chime: AM ndtostr[0]='%s'\n", nd0);
    if (netmgr_ndtostr(ND2S_DIR_SHOW, info->pid, nd1, sizeof(nd1)) >= 0)
        printf("chime: AM ndtostr[1]='%s'\n", nd1);
}

static int connect_all(const SConnectionInfo *info)
{
    int coid = -1;
    uint8_t saved_maj = g_hdr_major;
    int pass;

    /* sniff: attachEx -> ConnectAttach(nd=info[1], pid=info[2], chid=info[3]).
     * GPS/MOST local uses (0, mchid, schid). AM is remote (nd=0x50001). */
    for (pass = 0; pass < 2 && coid < 0; pass++) {
        g_hdr_major = (pass == 0) ? AM_HDR_VER : AM_MAJOR;
        if (pass)
            printf("chime: AM retry ConnectRequest with hdr major %u\n",
                   (unsigned)g_hdr_major);
        /* Live HU: attachEx nd=info[1]=1 pid=4106 chid=7 is the jacinto AM. */
        coid = probe_target((uint32_t)info->pid, (pid_t)info->mchid,
                            info->schid, "attachEx nd=info[1]");
        if (coid < 0)
            coid = probe_target(0, (pid_t)info->mchid, info->schid,
                                "local mchid/schid");
        if (coid < 0)
            coid = probe_target((uint32_t)info->nd, (pid_t)info->mchid,
                                info->schid, "nd[0] mchid/schid");
        if (coid < 0)
            coid = probe_target((uint32_t)info->nd, (pid_t)info->pid,
                                info->mchid, "OEM nd/pid/mchid");
        if (coid < 0)
            coid = probe_target((uint32_t)info->nd, (pid_t)info->pid,
                                info->schid, "OEM nd/pid/schid");
        if (coid < 0 && info->schid != 7)
            coid = probe_target((uint32_t)info->nd, (pid_t)info->mchid, 7,
                                "nd mchid chid=7");
        if (coid < 0)
            coid = probe_target(0, (pid_t)info->pid, info->schid,
                                "local pid/schid");
    }
    if (coid < 0)
        g_hdr_major = saved_maj;
    return coid;
}

static int attach_one(int sfd, const char *name, unsigned short maj,
                      unsigned short min, SConnectionInfo *info)
{
    memset(info, 0, sizeof(*info));
    if (SBAttachInterface(sfd, name, maj, min, info) != 0) {
        printf("chime: AM SBAttach %s %u.%u errno=%d (%s)\n",
               name, maj, min, errno, strerror(errno));
        return -1;
    }
    printf("chime: AM attached %s %u.%u nd=%d pid=%d mchid=%d schid=%d\n",
           name, maj, min, info->nd, info->pid, info->mchid, info->schid);
    snprintf(g_iface, sizeof(g_iface), "%s %u.%u", name, maj, min);
    dump_info(info);
    return 0;
}

static void parse_am_rx(const uint8_t *p, uint32_t n)
{
    uint32_t extra2, argsn, i, show;
    const uint8_t *args;

    if (n < DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE)
        return;
    extra2 = rd_u32(p + DSI_MSGHDR_SIZE + 8);
    args = p + DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE;
    argsn = n - (DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE);
    printf("chime: AM rx extra2=0x%08X n=%u pay", extra2, argsn);
    show = argsn;
    if (show > 64)
        show = 64;
    for (i = 0; i < show; i++)
        printf(" %02X", args[i]);
    printf("%s\n", argsn > show ? " ..." : "");
    if (extra2 >= 0x80000003u && extra2 <= 0x80000007u && argsn >= 8) {
        g_note_src = rd_u32(args);
        g_note = extra2;
        printf("chime: AM note extra2=0x%08X src=0x%x sink=%u\n",
               extra2, g_note_src, rd_u32(args + 4));
    }
    if (extra2 == 0xC0000001u && argsn >= 4) {
        uint32_t nrec = rd_u32(args), r, off;

        printf("chime: AM sources n=%u", nrec);
        for (r = 0; r < nrec && 4 + (r + 1) * 12 <= argsn; r++) {
            off = 4 + r * 12;
            printf("  [%u]=0x%x", r, rd_u32(args + off));
        }
        printf("\n");
    }
}

static void *am_thread(void *arg)
{
    SConnectionInfo info;
    int sfd = -1, i;
    const char *path = DSI_SB_PATH;
    char *env;
    static const uint32_t kAttrs[] = {
        0x80000000u, 0x80000001u, 0x80000002u, 0x80000003u,
        0x80000004u, 0x80000005u, 0x80000006u, 0x80000007u,
        0xC0000000u, 0xC0000001u, 0xC0000002u
    };

    (void)arg;
    env = getenv("SB_PATH");
    if (env && env[0])
        path = env;
    sfd = SBOpen(path);
    if (sfd < 0) {
        printf("chime: AM SBOpen errno=%d (%s)\n", errno, strerror(errno));
        g_done = 1;
        return NULL;
    }
    g_chid = ChannelCreate(0);
    if (g_chid < 0) {
        printf("chime: AM ChannelCreate errno=%d (%s)\n",
               errno, strerror(errno));
        SBClose(sfd);
        g_done = 1;
        return NULL;
    }
    /* 1.0 has proven requestFocus/makeAudible method IDs (Connectivity). */
    if (attach_one(sfd, AM_IFACE, AM_MAJOR, AM_MINOR, &info) != 0 &&
        attach_one(sfd, AM_IFACE_NBT, AM_MAJOR, AM_MINOR_NBT, &info) != 0) {
        SBClose(sfd);
        g_done = 1;
        return NULL;
    }
    g_party[0] = (uint32_t)info.extra[0];
    g_party[1] = (uint32_t)info.extra[1];
    g_party[2] = (uint32_t)info.extra[2];
    g_party[3] = (uint32_t)info.extra[3];
    g_coid = connect_all(&info);
    if (g_coid < 0) {
        SBDetachInterface(sfd, info.mchid, info.schid);
        if (attach_one(sfd, AM_IFACE_NBT, AM_MAJOR, AM_MINOR_NBT, &info) == 0) {
            g_party[0] = (uint32_t)info.extra[0];
            g_party[1] = (uint32_t)info.extra[1];
            g_party[2] = (uint32_t)info.extra[2];
            g_party[3] = (uint32_t)info.extra[3];
            g_coid = connect_all(&info);
        }
    }
    if (g_coid < 0) {
        printf("chime: AM no live DSI channel\n");
        SBDetachInterface(sfd, info.mchid, info.schid);
        SBClose(sfd);
        g_done = 1;
        return NULL;
    }
    for (i = 0; i < (int)(sizeof(kAttrs) / sizeof(kAttrs[0])); i++)
        dsi_send_on(g_coid, DSI_REQTYPE_NOTIFY, kAttrs[i], g_seq, NULL);
    g_ok = 1;
    g_done = 1;
    printf("chime: AM DSI ready (%s)\n", g_iface);

    for (;;) {
        uint8_t buf[1024];
        struct _msg_info mi;
        int rcvid;
        uint64_t to = 500000000ULL;

        TimerTimeout(CLOCK_REALTIME, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
        rcvid = MsgReceive(g_chid, buf, sizeof(buf), &mi);
        if (rcvid < 0) {
            if (errno == ETIMEDOUT || errno == EINTR)
                continue;
            printf("chime: AM MsgReceive errno=%d (%s)\n",
                   errno, strerror(errno));
            break;
        }
        if (rcvid == 0)
            continue;
        parse_am_rx(buf, (uint32_t)mi.msglen);
        MsgReply(rcvid, 0, NULL, 0);
    }
    g_ok = 0;
    g_done = 1;
    return NULL;
}

static int send_focus(uint32_t method)
{
    struct DsiBuf a;

    if (!g_ok || g_coid < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    db_u32(&a, AM_SRC_SPEECH);
    db_u32(&a, AM_SNK_SPEAKERS);
    printf("chime: AM %s src=0x%X sink=%u via %s\n",
           method == AM_REQ_FOCUS ? "requestFocus" :
           method == AM_REQ_RELEASE ? "releaseFocus" : "makeAudible",
           AM_SRC_SPEECH, AM_SNK_SPEAKERS, g_iface[0] ? g_iface : "?");
    return dsi_send_on(g_coid, DSI_REQTYPE_REQUEST, method, 0, &a);
}

int aa_am_start(void)
{
    pthread_t th;

    if (pthread_create(&th, NULL, am_thread, NULL) != 0) {
        printf("chime: AM thread failed: %s\n", strerror(errno));
        return -1;
    }
    pthread_detach(th);
    return 0;
}

int aa_am_ready(void)
{
    return g_ok;
}

static int wait_note(uint32_t extra2, uint32_t src, int ms)
{
    int i, n = (ms + 49) / 50;

    for (i = 0; i < n; i++) {
        if (g_note == extra2 && g_note_src == src)
            return 0;
        usleep(50000);
    }
    printf("chime: AM wait 0x%08X src=0x%x timed out (last 0x%08X src=0x%x)\n",
           extra2, src, g_note, g_note_src);
    return -1;
}

int aa_am_request_focus(void)
{
    int i;

    for (i = 0; i < 160 && !g_ok && !g_done; i++)
        usleep(25000);
    if (!g_ok)
        return -1;
    /* Probe waveam: requestFocus → 0x80000003 {0x16,1}, then makeAudible
     * → 0x80000004 {0x16,1}, then attach WavePlayer. Mix first. */
    g_note = 0;
    if (send_focus(AM_REQ_FOCUS) != 0)
        return -1;
    wait_note(0x80000003u, AM_SRC_SPEECH, 1000);
    usleep(100000);
    g_note = 0;
    send_focus(AM_REQ_AUDIBLE);
    wait_note(0x80000004u, AM_SRC_SPEECH, 1000);
    usleep(400000);
    g_focus = 1;
    return 0;
}

void aa_am_release_focus(void)
{
    if (!g_focus)
        return;
    send_focus(AM_REQ_RELEASE);
    g_focus = 0;
    printf("chime: AM releaseFocus sent\n");
}
