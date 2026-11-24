/*
 * Observe-only DSI client of MOSTSubSystemIntel.DSYSMOSTServer.
 * Subscribe informationProcessMessage (0x80000001) + registerDevCtrls,
 * never GET/SET, never requestRegisterDevices, never clrNotification.
 */

#define _QNX_SOURCE
#include "id4_aa_zbe.h"
#include "id4_aa_cm.h"

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

#define ZBE_Q 64

struct DsiBuf {
    uint8_t p[512];
    uint32_t n;
};

static uint32_t g_seq = 1;
static uint32_t g_clientId;
static uint16_t g_devId;
static int g_haveClient;
static int g_coid = -1;
static int g_chid = -1;
static uint32_t g_party[4];
static uint8_t g_hdr_major = (uint8_t)DSI_MOST_MAJOR;
static pthread_mutex_t g_dsi = PTHREAD_MUTEX_INITIALIZER;
static uint16_t g_cm_dest;
static uint8_t g_cm_inst = CM_INST_ID;
static int g_cm_have_dest;
static int g_cm_held;
static volatile int g_cm_want;
static volatile int g_cm_up;
static volatile int g_cm_seen;
static volatile int g_cm_inflight;
static uint16_t g_cm_conn = CM_CONN_SPEECH;
static uint8_t g_cm_last_op;
static uint8_t g_cm_state;
static int g_cm_dsi;
static int g_cm_am;
static uint32_t g_cm_last_build_ms;
#define CM_KEEPALIVE_MS 60u

static Id4CcsDec g_dec;
static Id4CcsEvent g_q[ZBE_Q];
static int g_qh, g_qt, g_qn;
static pthread_mutex_t g_qm = PTHREAD_MUTEX_INITIALIZER;
volatile int g_zbe_pending;

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void db_align(struct DsiBuf *b, unsigned a)
{
    while (b->n % a && b->n < sizeof(b->p))
        b->p[b->n++] = 0;
}

static void db_u8(struct DsiBuf *b, uint8_t v)
{
    if (b->n < sizeof(b->p))
        b->p[b->n++] = v;
}

static void db_u16(struct DsiBuf *b, uint16_t v)
{
    db_align(b, 2);
    db_u8(b, (uint8_t)(v & 0xFF));
    db_u8(b, (uint8_t)(v >> 8));
}

static void db_u32(struct DsiBuf *b, uint32_t v)
{
    db_align(b, 4);
    db_u8(b, (uint8_t)(v & 0xFF));
    db_u8(b, (uint8_t)((v >> 8) & 0xFF));
    db_u8(b, (uint8_t)((v >> 16) & 0xFF));
    db_u8(b, (uint8_t)((v >> 24) & 0xFF));
}

static void db_bytes(struct DsiBuf *b, const void *s, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)s;

    while (n-- && b->n < sizeof(b->p))
        b->p[b->n++] = *p++;
}

static void db_str(struct DsiBuf *b, const char *s)
{
    uint32_t n = (uint32_t)strlen(s);

    db_u32(b, n);
    db_bytes(b, s, n);
    db_align(b, 4);
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

static void zbe_push(const Id4CcsEvent *e)
{
    pthread_mutex_lock(&g_qm);
    if (g_qn < ZBE_Q) {
        g_q[g_qh] = *e;
        g_qh = (g_qh + 1) % ZBE_Q;
        g_qn++;
        g_zbe_pending = 1;
        if (e->kind == 2)
            printf("zbe %s %+d\n", e->what ? e->what : "ROTARY", e->delta);
        else
            printf("zbe %s %s\n", e->what ? e->what : "KEY",
                   e->down ? "down" : "up");
    }
    pthread_mutex_unlock(&g_qm);
}

int aa_zbe_pop(Id4CcsEvent *ev)
{
    int ok = 0;

    if (!ev)
        return 0;
    pthread_mutex_lock(&g_qm);
    if (g_qn > 0) {
        *ev = g_q[g_qt];
        g_qt = (g_qt + 1) % ZBE_Q;
        g_qn--;
        ok = 1;
    }
    pthread_mutex_unlock(&g_qm);
    return ok;
}

static int dsi_send_on(int coid, uint32_t reqtype, uint32_t extra2,
                       uint32_t extra3, const struct DsiBuf *args)
{
    uint8_t hdr[DSI_MSGHDR_SIZE];
    uint32_t info[4];
    uint8_t rbuf[512];
    iov_t siov[3], riov[2];
    uint32_t rhdr[10];
    uint32_t payn;
    int n, niov;

    if (coid < 0)
        return -1;
    payn = args ? args->n : 0;
    memset(info, 0, sizeof(info));
    info[0] = (reqtype == DSI_REQTYPE_REQUEST && extra2 == DSI_REQ_PROCESS_MESSAGE)
                  ? 0x20u : 0u;
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
    pthread_mutex_lock(&g_dsi);
    info[3] = extra3 ? extra3 : g_seq;
    n = MsgSendv(coid, siov, niov, riov, 2);
    if (n >= 0)
        g_seq++;
    pthread_mutex_unlock(&g_dsi);
    if (n < 0) {
        printf("zbe MsgSendv req=0x%X a2=0x%X errno=%d (%s)\n",
               reqtype, extra2, errno, strerror(errno));
        return -1;
    }
    return 0;
}

static int dsi_send(int coid, uint32_t method, const struct DsiBuf *args)
{
    return dsi_send_on(coid, DSI_REQTYPE_REQUEST, method, 0, args);
}

static int send_notify(int coid, uint32_t updateId)
{
    return dsi_send_on(coid, DSI_REQTYPE_NOTIFY, updateId, g_seq, NULL);
}

static int send_register_client_ext(int coid)
{
    struct DsiBuf a;

    memset(&a, 0, sizeof(a));
    db_str(&a, DSI_CLIENT_NAME);
    printf("zbe registerMOSTClientExt name=%s\n", DSI_CLIENT_NAME);
    return dsi_send(coid, DSI_REQ_REGISTER_CLIENT_EXT, &a);
}

static void wr_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static int send_cm_pay(uint16_t dest, uint8_t inst, uint16_t fkt,
                       const uint8_t *pay, uint32_t pn)
{
    struct DsiBuf a;
    uint16_t src;

    if (g_coid < 0 || !g_haveClient)
        return -1;
    src = g_devId ? g_devId : 0x2085;
    memset(&a, 0, sizeof(a));
    db_u32(&a, g_clientId);
    db_u32(&a, MOST_TXTYPE_CTRL);
    db_u32(&a, MOST_TXVAR_TXCONTROL);
    db_u32(&a, 1);
    db_u16(&a, dest);
    db_u16(&a, src);
    db_u8(&a, CM_FBLOCK_ID);
    db_u8(&a, inst);
    db_u16(&a, fkt);
    db_u8(&a, MOST_OP_STARTRESULT);
    db_u8(&a, 0);
    db_u8(&a, 0);
    db_u8(&a, 0);
    db_u32(&a, 0);
    db_u32(&a, pn);
    db_bytes(&a, pay, pn);
    return dsi_send(g_coid, DSI_REQ_PROCESS_MESSAGE, &a);
}

static uint32_t now_ms(void)
{
    uint64_t ns = 0;

    ClockTime(CLOCK_REALTIME, NULL, &ns);
    return (uint32_t)(ns / 1000000ULL);
}

static int send_cm_build(void)
{
    uint8_t pay[4];
    uint16_t dest = g_cm_dest ? g_cm_dest : 0x2021;
    uint8_t inst = g_cm_inst;

    wr_be16(pay, CM_SENDER_HANDLE);
    wr_be16(pay + 2, g_cm_conn);
    printf("chime: MOST CM dest=0x%04X %02X.%02X fkt=0x%03X StartResult sh=0x%04X conn=0x%04X\n",
           dest, CM_FBLOCK_ID, inst, CM_FKT_BUILD_HIGHSYNC,
           CM_SENDER_HANDLE, g_cm_conn);
    g_cm_inflight = 1;
    return send_cm_pay(dest, inst, CM_FKT_BUILD_HIGHSYNC, pay, 4);
}

/* HMI SpeechPrompter 0x54s idle SDS 0x36 ~150 ms after it appears in C03.
 * One DSI rebuild every CM_KEEPALIVE_MS closes that gap. Extra builds
 * during teardown (empty extra2=0 replies) delayed the one that lands. */
static void cm_kick(const char *why)
{
    uint32_t t;

    if (!g_cm_want || !g_cm_held)
        return;
    t = now_ms();
    if (g_cm_last_build_ms && (uint32_t)(t - g_cm_last_build_ms) < CM_KEEPALIVE_MS)
        return;
    g_cm_last_build_ms = t ? t : 1;
    printf("chime: HighSync 0x%02X rebuild (%s)\n", g_cm_conn, why);
    if (g_cm_dsi) {
        aa_cm_build(g_cm_conn);
        return;
    }
    send_cm_build();
}

static int c03_scan(const uint8_t *pl, uint32_t pn, uint16_t want, uint8_t *st_out)
{
    uint32_t off;
    int has = 0;

    if (pn < 2)
        return 0;
    for (off = 2; off + 3 <= pn; off += 3) {
        uint16_t c = (uint16_t)((pl[off] << 8) | pl[off + 1]);
        uint8_t st = pl[off + 2];

        if (c == want) {
            has = 1;
            if (st_out)
                *st_out = st;
        }
    }
    return has;
}

static int send_cm_remove(void)
{
    uint8_t pay[4];
    uint16_t dest = g_cm_dest ? g_cm_dest : 0x2021;

    wr_be16(pay, CM_SENDER_HANDLE);
    wr_be16(pay + 2, g_cm_conn);
    printf("chime: MOST CM dest=0x%04X %02X.%02X fkt=0x%03X StartResult sh=0x%04X conn=0x%04X\n",
           dest, CM_FBLOCK_ID, g_cm_inst, CM_FKT_REMOVE_HIGHSYNC,
           CM_SENDER_HANDLE, g_cm_conn);
    return send_cm_pay(dest, g_cm_inst, CM_FKT_REMOVE_HIGHSYNC, pay, 4);
}

/* CHmiAudioSink::muteAudioSource(false) → C05 EConnectionControl = bMute^1. */
static int send_cm_unmute(void)
{
    uint8_t pay[5];
    uint16_t dest = g_cm_dest ? g_cm_dest : 0x100E;

    wr_be16(pay, CM_SENDER_HANDLE_CTRL);
    wr_be16(pay + 2, g_cm_conn);
    pay[4] = (uint8_t)CM_CTRL_UNMUTE;
    printf("chime: MOST CM dest=0x%04X %02X.%02X fkt=0x%03X C05 unmute sh=0x%04X conn=0x%04X\n",
           dest, CM_FBLOCK_ID, g_cm_inst, CM_FKT_CONN_CTRL,
           CM_SENDER_HANDLE_CTRL, g_cm_conn);
    return send_cm_pay(dest, g_cm_inst, CM_FKT_CONN_CTRL, pay, 5);
}

static void wait_cm_mix(int ms)
{
    int i;

    for (i = 0; i < ms / 10; i++) {
        if (g_cm_want)
            cm_kick("wait");
        usleep(10000);
    }
}

static void parse_registry(const uint8_t *p, uint32_t n)
{
    uint32_t count, i, off;
    static int dumped;

    if (n < 4)
        return;
    if (!dumped) {
        unsigned k, show = n > 64 ? 64 : n;

        printf("zbe informationRegistry %u bytes:", n);
        for (k = 0; k < show; k++)
            printf(" %02X", p[k]);
        printf("%s\n", n > show ? " ..." : "");
        dumped = 1;
    }
    count = rd_u32(p);
    off = 4;
    if (count > 256 && n >= 8) {
        count = rd_u32(p + 4);
        off = 8;
    }
    if (count > 256 || off + count * 4 > n)
        return;
    for (i = 0; i < count; i++) {
        uint16_t dest = rd_u16(p + off + i * 4);
        uint16_t key = rd_u16(p + off + i * 4 + 2);
        uint8_t fb = (uint8_t)(key >> 8);
        uint8_t inst = (uint8_t)(key & 0xFF);

        if (fb != CM_FBLOCK_ID)
            continue;
        if (!g_cm_have_dest || inst == CM_INST_ID) {
            g_cm_dest = dest;
            g_cm_inst = inst;
            g_cm_have_dest = 1;
            printf("zbe ConnectionMaster dest=0x%04X inst=0x%02X\n", dest, inst);
        }
    }
}

static int send_register_devctrls(int coid, uint32_t clientId)
{
    static const uint16_t kKeys[] = {
        0xE501, 0xE50F, 0xE502, 0xE503, 0xE505, 0xE510, 0xE512, 0xE514, 0x1502,
        0x0300, 0x0301
    };
    struct DsiBuf a;
    uint32_t i, n = (uint32_t)(sizeof(kKeys) / sizeof(kKeys[0]));

    memset(&a, 0, sizeof(a));
    db_u32(&a, clientId);
    db_u32(&a, n);
    for (i = 0; i < n; i++)
        db_u16(&a, kKeys[i]);
    db_align(&a, 4);
    printf("zbe requestRegisterDevCtrls client=%u (listen, not own)\n", clientId);
    return dsi_send(coid, DSI_REQ_REGISTER_DEVCTRLS, &a);
}

static void feed_most(const uint8_t *p, uint32_t n)
{
    uint16_t fkt;
    uint8_t fb, inst, op;
    uint32_t payn;
    const uint8_t *pay;
    Id4CcsEvent ev[8];
    int i, k;

    if (n < 32)
        return;
    fb = p[16];
    inst = p[17];
    fkt = rd_u16(p + 18) & 0x0FFF;
    op = p[20];
    if (fb == CM_FBLOCK_ID) {
        uint32_t i, pn = 0;
        const uint8_t *pl = p + 32;

        if (n >= 32)
            pn = rd_u32(p + 28);
        if (pn > n - 32)
            pn = n - 32;
        if (!(fkt == CM_FKT_CONN_TABLE && op == MOST_OP_STATUS)) {
            printf("chime: MOST CM rx %02X.%02X fkt=0x%03X op=0x%02X n=%u pay",
                   fb, inst, fkt, op, n);
            for (i = 0; i < pn && i < 16; i++)
                printf(" %02X", pl[i]);
            printf("\n");
        }
        if (fkt == CM_FKT_BUILD_HIGHSYNC || fkt == CM_FKT_REMOVE_HIGHSYNC)
            g_cm_last_op = op;
        if (fkt == CM_FKT_CONN_TABLE && op == MOST_OP_STATUS) {
            static uint8_t last[48];
            static uint32_t lastn;
            uint8_t st = 0;
            int has = c03_scan(pl, pn, g_cm_conn, &st);
            uint32_t copy = pn > sizeof(last) ? (uint32_t)sizeof(last) : pn;
            int changed = (copy != lastn) || memcmp(last, pl, copy);

            if (changed) {
                uint32_t off;

                printf("chime: C03 table");
                for (off = 2; off + 3 <= pn; off += 3)
                    printf(" 0x%02X/%u",
                           (unsigned)((pl[off] << 8) | pl[off + 1]),
                           (unsigned)pl[off + 2]);
                printf("\n");
                memset(last, 0, sizeof(last));
                memcpy(last, pl, copy);
                lastn = copy;
            }

            if (has && (!g_cm_up || g_cm_state != st))
                printf("chime: HighSync 0x%02X is in the amp table state=%u\n",
                       g_cm_conn, (unsigned)st);
            if (!has && g_cm_up)
                printf("chime: HighSync 0x%02X dropped from amp table\n",
                       g_cm_conn);
            g_cm_up = has ? 1 : 0;
            g_cm_state = has ? st : 0;
            if (has)
                g_cm_seen = 1;
            else if (g_cm_seen)
                cm_kick("C03 drop");
        }
        if (fkt == CM_FKT_BUILD_HIGHSYNC) {
            if (op == MOST_OP_RESULTACK) {
                g_cm_up = 1;
                g_cm_seen = 1;
                g_cm_inflight = 0;
            } else if (op == MOST_OP_ERRORACK) {
                g_cm_inflight = 0;
            }
        }
        if (fkt == CM_FKT_REMOVE_HIGHSYNC && op == MOST_OP_RESULTACK) {
            g_cm_up = 0;
            g_cm_state = 0;
            cm_kick("0x54 ack");
        }
        if (fkt == CM_FKT_CONN_CTRL) {
            printf("chime: C05 op=0x%02X (0x0D=unmute ack, 0x09=error)\n", op);
        }
        return;
    }
    if (fb != CCS_FBLOCK_ID)
        return;
    payn = rd_u32(p + 28);
    pay = p + 32;
    if (payn > n - 32)
        payn = n - 32;
    k = id4_ccs_feed(&g_dec, fb, inst, fkt, op, pay, payn, ev, 8);
    for (i = 0; i < k; i++)
        zbe_push(&ev[i]);
}

static int g_need_devctrl;

static void parse_dsi_rx(const uint8_t *p, uint32_t n)
{
    uint32_t extra2, argsn;
    const uint8_t *args;

    if (n < DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE)
        return;
    extra2 = rd_u32(p + DSI_MSGHDR_SIZE + 8);
    args = p + DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE;
    argsn = n - (DSI_MSGHDR_SIZE + DSI_REQINFO_SIZE);
    if (extra2 == DSI_NOTIF_RESPONSE_CLIENT && argsn >= 6) {
        g_clientId = rd_u32(args);
        g_haveClient = 1;
        g_need_devctrl = 1;
        g_devId = rd_u16(args + 4);
        printf("zbe responseMOSTClient clientId=%u devId=0x%04X\n",
               g_clientId, g_devId);
        return;
    }
    if (extra2 == 0xC0000001u)
        parse_registry(args, argsn);
    if (extra2 == DSI_NOTIF_PROCESS_MESSAGE)
        feed_most(args, argsn);
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

static void *zbe_thread(void *arg)
{
    SConnectionInfo info;
    int sfd = -1, i;
    const char *path = DSI_SB_PATH;
    char *env;
    static const uint32_t kAttrs[] = {
        0xC0000002u, 0xC0000003u, 0xC0000001u,
        0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u,
        0x80000005u, 0x80000006u, 0x80000007u, 0x80000000u
    };

    (void)arg;
    id4_ccs_init(&g_dec);
    env = getenv("SB_PATH");
    if (env && env[0])
        path = env;
    sfd = SBOpen(path);
    if (sfd < 0) {
        printf("zbe SBOpen failed errno=%d (%s)\n", errno, strerror(errno));
        return NULL;
    }
    memset(&info, 0, sizeof(info));
    if (SBAttachInterface(sfd, DSI_MOST_IFACE, (unsigned short)DSI_MOST_MAJOR,
                          (unsigned short)DSI_MOST_MINOR, &info) != 0) {
        printf("zbe SBAttachInterface errno=%d (%s)\n", errno, strerror(errno));
        SBClose(sfd);
        return NULL;
    }
    g_party[0] = (uint32_t)info.extra[0];
    g_party[1] = (uint32_t)info.extra[1];
    g_party[2] = (uint32_t)info.extra[2];
    g_party[3] = (uint32_t)info.extra[3];
    g_chid = ChannelCreate(0);
    if (g_chid < 0) {
        printf("zbe ChannelCreate errno=%d (%s)\n", errno, strerror(errno));
        SBDetachInterface(sfd, info.mchid, info.schid);
        SBClose(sfd);
        return NULL;
    }
    g_coid = probe_target(0, (pid_t)info.mchid, info.schid);
    if (g_coid < 0)
        g_coid = probe_target((uint32_t)info.nd, (pid_t)info.mchid, info.schid);
    if (g_coid < 0)
        g_coid = probe_target(0, (pid_t)info.mchid, info.mchid);
    if (g_coid < 0) {
        printf("zbe no live DSI channel\n");
        SBDetachInterface(sfd, info.mchid, info.schid);
        SBClose(sfd);
        return NULL;
    }
    for (i = 0; i < (int)(sizeof(kAttrs) / sizeof(kAttrs[0])); i++)
        send_notify(g_coid, kAttrs[i]);
    send_register_client_ext(g_coid);
    printf("zbe observe-only, waiting for E5.01 C10/C20\n");

    for (;;) {
        uint8_t buf[4096];
        struct _msg_info mi;
        int rcvid;
        uint64_t to = 500000000ULL;

        TimerTimeout(CLOCK_REALTIME, _NTO_TIMEOUT_RECEIVE, NULL, &to, NULL);
        rcvid = MsgReceive(g_chid, buf, sizeof(buf), &mi);
        if (rcvid < 0) {
            if (errno == ETIMEDOUT || errno == EINTR)
                continue;
            printf("zbe MsgReceive errno=%d (%s)\n", errno, strerror(errno));
            break;
        }
        if (rcvid == 0)
            continue;
        parse_dsi_rx(buf, (uint32_t)mi.msglen);
        MsgReply(rcvid, 0, NULL, 0);
        if (g_need_devctrl && g_haveClient) {
            g_need_devctrl = 0;
            send_notify(g_coid, 0xC0000000u);
            send_register_devctrls(g_coid, g_clientId);
        }
    }
    return NULL;
}

int aa_zbe_speech_hold(void)
{
    int i;

    if (g_cm_held)
        return 0;
    g_cm_last_op = 0;
    g_cm_inflight = 0;
    g_cm_dsi = 0;
    g_cm_am = 0;
    g_cm_want = 0;
    g_cm_up = 0;
    g_cm_seen = 0;
    g_cm_state = 0;
    g_cm_last_build_ms = 0;
    g_cm_conn = CM_CONN_SPEECH;
    /* 0x30/0x31/0x3A/0x32 DSI result=1, never in C03. Only 0x36 mixes.
     * SpeechPrompter 0x54s it ~150 ms later (SH 0x0Fxx). Keep-alive. */
    if (aa_cm_build(CM_CONN_SPEECH) == 0) {
        g_cm_dsi = 1;
        g_cm_want = 1;
        g_cm_held = 1;
        g_cm_last_build_ms = now_ms();
        if (!g_cm_last_build_ms)
            g_cm_last_build_ms = 1;
        printf("chime: HighSync 0x%02X DSI keep-alive %u ms\n",
               g_cm_conn, CM_KEEPALIVE_MS);
        wait_cm_mix(250);
        printf("chime: HighSync 0x%02X mix wait done up=%d state=%u\n",
               g_cm_conn, g_cm_up, (unsigned)g_cm_state);
        return 0;
    }
    for (i = 0; i < 16 && !g_haveClient; i++)
        usleep(50000);
    if (!g_haveClient) {
        printf("chime: MOST client not up, skip HighSync 0x%02X\n", g_cm_conn);
        return -1;
    }
    for (i = 0; i < 12 && !g_cm_have_dest; i++)
        usleep(50000);
    if (!g_cm_have_dest) {
        g_cm_dest = 0x2021;
        g_cm_inst = CM_INST_ID;
        printf("chime: no CM in registry, trying dest=0x%04X\n", g_cm_dest);
    }
    g_cm_want = 1;
    g_cm_held = 1;
    if (send_cm_build() != 0) {
        g_cm_held = 0;
        g_cm_want = 0;
        return -1;
    }
    printf("chime: HighSync 0x%02X StartResult fallback (PCM keeps writing)\n",
           g_cm_conn);
    return 0;
}

void aa_zbe_speech_pump(void)
{
    if (!g_cm_want || !g_cm_held)
        return;
    cm_kick("keep");
}

void aa_zbe_speech_release(void)
{
    if (!g_cm_held)
        return;
    g_cm_want = 0;
    if (g_cm_dsi)
        aa_cm_remove();
    else
        send_cm_remove();
    g_cm_am = 0;
    g_cm_dsi = 0;
    g_cm_held = 0;
    g_cm_up = 0;
    g_cm_state = 0;
    g_cm_inflight = 0;
    g_cm_last_build_ms = 0;
    printf("chime: HighSync 0x%02X released\n", g_cm_conn);
}

int aa_zbe_start(void)
{
    pthread_t th;

    if (pthread_create(&th, NULL, zbe_thread, NULL) != 0) {
        printf("zbe thread failed: %s\n", strerror(errno));
        return -1;
    }
    pthread_detach(th);
    return 0;
}
