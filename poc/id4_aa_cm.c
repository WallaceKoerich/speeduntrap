/*
 * ConnectionMaster.DMMNBTConnectionMaster 0.3 — OEM HighSync owner.
 *
 * Evidence:
 *   jacinto servicebroker.cfg [FORWARD]: ConnectionMaster.DMMNBTConnectionMaster
 *   Connectivity SHBDSIImportEntry(..., 0, 3)  → major 0 minor 3
 *   CDMMNBTConnectionMasterProxy FUN_08842ba2 method 0
 *     requestBuildHighSyncConnection(uint16 connId)  args=2
 *   FUN_08842b2e method 1 requestRemoveHighSyncConnection(uint16)
 *   CDevSpeechPrompter FUN_080dc592: useCase!=0 → EAudioSource 0x56
 *   CDevConnectionMaster FUN_080d2a66: src 0x56 ↔ HighSync 0x33
 *   ACID DAT_09879224: src 0x56 sink0=0x33 group 11 (not iSpeech group 5)
 *   natp.cfg Prompter_1 = NAVI → /input_mono_0
 *
 * Foreign MOST 0x53 on 0x36 is 0x54'd by HMI. This DSI is the same
 * path Connectivity uses so jacinto owns the mix for the whole prompt.
 */

#define _QNX_SOURCE
#include "id4_aa_cm.h"
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

#define CM_IFACE          "ConnectionMaster.DMMNBTConnectionMaster"
#define CM_MAJOR          0
#define CM_MINOR          3
#define CM_MAJOR_ALT      3
#define CM_MINOR_ALT      0
#define CM_HDR_VER        4
#define CM_REQ_BUILD      0u   /* Connectivity FUN_08842ba2 */
#define CM_REQ_REMOVE     1u   /* FUN_08842b2e */

struct DsiBuf {
    uint8_t p[64];
    uint32_t n;
};

static uint32_t g_seq = 1;
static int g_coid = -1;
static int g_chid = -1;
static uint32_t g_party[4];
static uint8_t g_hdr_major = CM_HDR_VER;
static pthread_mutex_t g_lk = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_ok;
static volatile int g_done;
static volatile int g_held;
static uint16_t g_conn;
static char g_iface[80];

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
        printf("chime: CM MsgSendv method=%u errno=%d (%s)\n",
               extra2, errno, strerror(errno));
        return -1;
    }
    if (reqtype == DSI_REQTYPE_REQUEST) {
        unsigned i, show;

        printf("chime: CM reply method=%u n=%d res=0x%x pay",
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

    printf("chime: CM probe %s nd=%u pid=%d chid=%d hdr=%u\n",
           tag, nd, (int)pid, chid, (unsigned)g_hdr_major);
    coid = ConnectAttach(nd, pid, chid, _NTO_SIDE_CHANNEL, _NTO_COF_CLOEXEC);
    if (coid < 0) {
        printf("chime: CM ConnectAttach errno=%d (%s)\n",
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
        printf("chime: CM ConnectRequest errno=%d (%s) rnd=%d\n",
               saved, strerror(saved), rnd);
        ConnectDetach(coid);
        return -1;
    }
    printf("chime: CM ConnectRequest ok coid=%d n=%d reply=%d %d %d %d\n",
           coid, n, reply[0], reply[1], reply[2], reply[3]);
    return coid;
}

static void dump_info(const SConnectionInfo *info)
{
    const int32_t *w = (const int32_t *)info;
    int i;

    printf("chime: CM SConnectionInfo");
    for (i = 0; i < 8; i++)
        printf(" [%d]=%d/0x%x", i, (int)w[i], (unsigned)w[i]);
    printf("\n");
}

static int connect_all(const SConnectionInfo *info)
{
    int coid = -1;
    uint8_t saved_maj = g_hdr_major;
    int pass;

    for (pass = 0; pass < 2 && coid < 0; pass++) {
        g_hdr_major = (pass == 0) ? CM_HDR_VER : 0;
        if (pass)
            printf("chime: CM retry ConnectRequest with hdr major %u\n",
                   (unsigned)g_hdr_major);
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

static int attach_one(int sfd, unsigned short maj, unsigned short min,
                      SConnectionInfo *info)
{
    memset(info, 0, sizeof(*info));
    if (SBAttachInterface(sfd, CM_IFACE, maj, min, info) != 0) {
        printf("chime: CM SBAttach %s %u.%u errno=%d (%s)\n",
               CM_IFACE, maj, min, errno, strerror(errno));
        return -1;
    }
    printf("chime: CM attached %s %u.%u nd=%d pid=%d mchid=%d schid=%d\n",
           CM_IFACE, maj, min, info->nd, info->pid, info->mchid, info->schid);
    snprintf(g_iface, sizeof(g_iface), "%s %u.%u", CM_IFACE, maj, min);
    dump_info(info);
    return 0;
}

static void parse_cm_rx(const uint8_t *p, uint32_t n)
{
    uint32_t extra2, argsn, i, show;
    const uint8_t *args;

    if (n < DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE)
        return;
    extra2 = rd_u32(p + DSI_MSGHDR_SIZE + 8);
    args = p + DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE;
    argsn = n - (DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE);
    if (extra2 == 0 && argsn == 0)
        return;
    printf("chime: CM rx extra2=0x%08X n=%u pay", extra2, argsn);
    show = argsn > 16 ? 16 : argsn;
    for (i = 0; i < show; i++)
        printf(" %02X", args[i]);
    printf("%s\n", argsn > show ? " ..." : "");
}

static void *cm_thread(void *arg)
{
    SConnectionInfo info;
    int sfd = -1;
    const char *path = DSI_SB_PATH;
    char *env;

    (void)arg;
    env = getenv("SB_PATH");
    if (env && env[0])
        path = env;
    sfd = SBOpen(path);
    if (sfd < 0) {
        printf("chime: CM SBOpen errno=%d (%s)\n", errno, strerror(errno));
        g_done = 1;
        return NULL;
    }
    g_chid = ChannelCreate(0);
    if (g_chid < 0) {
        printf("chime: CM ChannelCreate errno=%d (%s)\n",
               errno, strerror(errno));
        SBClose(sfd);
        g_done = 1;
        return NULL;
    }
    if (attach_one(sfd, CM_MAJOR, CM_MINOR, &info) != 0 &&
        attach_one(sfd, CM_MAJOR_ALT, CM_MINOR_ALT, &info) != 0) {
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
        printf("chime: CM no live DSI channel\n");
        SBDetachInterface(sfd, info.mchid, info.schid);
        SBClose(sfd);
        g_done = 1;
        return NULL;
    }
    g_ok = 1;
    g_done = 1;
    printf("chime: CM DSI ready (%s)\n", g_iface);

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
            printf("chime: CM MsgReceive errno=%d (%s)\n",
                   errno, strerror(errno));
            break;
        }
        if (rcvid == 0)
            continue;
        parse_cm_rx(buf, (uint32_t)mi.msglen);
        MsgReply(rcvid, 0, NULL, 0);
    }
    g_ok = 0;
    g_done = 1;
    return NULL;
}

static int send_conn(uint32_t method, uint16_t conn)
{
    struct DsiBuf a;

    if (!g_ok || g_coid < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.p[0] = (uint8_t)(conn & 0xFF);
    a.p[1] = (uint8_t)((conn >> 8) & 0xFF);
    a.n = 2;
    printf("chime: CM %s conn=0x%02X via %s\n",
           method == CM_REQ_BUILD ? "requestBuildHighSyncConnection" :
           "requestRemoveHighSyncConnection",
           conn, g_iface[0] ? g_iface : "?");
    return dsi_send_on(g_coid, DSI_REQTYPE_REQUEST, method, 0, &a);
}

int aa_cm_start(void)
{
    pthread_t th;

    if (pthread_create(&th, NULL, cm_thread, NULL) != 0) {
        printf("chime: CM thread failed: %s\n", strerror(errno));
        return -1;
    }
    pthread_detach(th);
    return 0;
}

int aa_cm_ready(void)
{
    return g_ok;
}

int aa_cm_build(uint16_t conn)
{
    int i;

    for (i = 0; i < 160 && !g_ok && !g_done; i++)
        usleep(25000);
    if (!g_ok)
        return -1;
    if (send_conn(CM_REQ_BUILD, conn) != 0)
        return -1;
    g_conn = conn;
    g_held = 1;
    return 0;
}

void aa_cm_remove(void)
{
    if (!g_held)
        return;
    send_conn(CM_REQ_REMOVE, g_conn);
    g_held = 0;
    printf("chime: CM requestRemoveHighSyncConnection sent\n");
}
