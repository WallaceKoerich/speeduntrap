/*
 * id4_radar.c
 *
 * ConnectedDrive "Radar" — Popup on/off checkbox.
 * Same SAS/RHMI knock as Light Control. Overlay is WalControl + KD/EGL
 * (id4_aa_display.c), not HmiMain3D Check Control.
 *
 * Build (QNX 6.5 ntox86-gcc):
 *   ntox86-gcc -O2 -Wall -o id4_radar_x86 \
 *     id4_radar.c id4_radar_core.c id4_radar_gps.c \
 *     id4_aa_display.c id4_aa_zbe.c \
 *     -lsocket -lm -lEGL -lGLESv2 -lKD
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <dlfcn.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "qnx_cert.h"
#include "hello_assets.h"
#include "radar_assets.h"
#include "id4_radar.h"
#ifdef SPEEDUNTRAP
#include "speeduntrap_ui.h"
#include <pthread.h>
#endif

#define ETCH_VERSION 3

/* Etch tagged typecodes */
#define TC_NULL         0x80
#define TC_NONE         0x81
#define TC_FALSE        0x82
#define TC_TRUE         0x83
#define TC_BYTE         0x84
#define TC_SHORT        0x85
#define TC_INT          0x86
#define TC_LONG         0x87
#define TC_FLOAT        0x88
#define TC_DOUBLE       0x89
#define TC_EMPTY_STRING 0x8A
#define TC_BYTES        0x8B
#define TC_STRING       0x93
#define TC_CUSTOM       0x95

/* Known field/message hashes (validated against server captures) */
#define H_MESSAGE_ID      0x6306b468
#define H_RESULT          0x8104fdc2
#define H_IN_REPLY_TO     0xeda8c9a6
#define H_DATA            0x144a776f
#define H_HANDLE          0xa29699ed
#define H_TYPE            0x0884f67f

#define H_SAS_CERTIFICATE 0x304ba8bb
#define H_SAS_LOGIN       0x7e002a4d
#define H_RHMI_CREATE     0x722a4d67
#define H_RHMI_SET_RESOURCE 0x1998b6e5
#define H_RHMI_ADD_ACTION_EVENT_HANDLER 0x7e8fab7c
#define H_RHMI_INITIALIZE 0x9364779b
#define H_RHMI_SET_DATA   0x5485cd21
#define H_ACTION_ID       0x235ca6d6 /* etch_hash("actionId") — not 0x7a30ced7 */
#define H_IDENT           0x4d9eccab /* etch_hash("ident") — not 0x00247bd0 */ /* etch_hash("ident") */

#define H_CDS_CREATE      0x3376152b
#define H_CDS_ADD_PROP    0x9f806ebd
#define H_CDS_ON_PROP     0xae0d3d69
#define H_CDS_ON_PROP_RES 0x0bae3b2c

#define RADAR_APP_ID         "me.wallace.rhmi.radar"
#define RADAR_ACTION_ENTRY   11
#define RADAR_ACTION_POPUP   13
#define RADAR_ACTION_RANGE_DN 14
#define RADAR_ACTION_RANGE_UP 15
#define RADAR_ACTION_STOP    16
#define RADAR_MODEL_STATUS   3
#define RADAR_MODEL_CHECKBOX 4
#define RADAR_MODEL_RANGE    6
#define RADAR_MODEL_RANGE_DN 9
#define RADAR_MODEL_RANGE_UP 10
#define RADAR_MODEL_STOP     11

#define SAS_PORT          4004
#define HANDSHAKE_TIMEOUT_SEC 10
#define MAX_WAIT_FRAMES   25

static struct timespec g_t0;
static int g_sock = -1;

/* Diagnostic verdicts: -1 failed, 0 not reached, 1 passed */
static int v_cert  = 0;
static int v_login = 0;
static int v_rhmi  = 0;
static long g_handle = -1;
static uint32_t g_msg_id;
static int g_pending_action = -1;
static long g_cds = -1;
static int handle_cds_frame(const unsigned char *rx, uint32_t rx_len);

static long elapsed_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - g_t0.tv_sec) * 1000L + (now.tv_nsec - g_t0.tv_nsec) / 1000000L;
}

static void logline(const char *tag, const char *fmt, ...) {
    va_list ap;
    printf("[+%6ld ms] [%-4s] ", elapsed_ms(), tag);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

#define LOGI(...)  logline("info", __VA_ARGS__)
#define LOGOK(...) logline("OK",  __VA_ARGS__)
#define LOGE(...)  logline("FAIL", __VA_ARGS__)
#define LOGN(...)  logline("net",  __VA_ARGS__)

static void hexdump(const unsigned char *data, size_t size) {
    char ascii[17];
    size_t i, j;
    ascii[16] = '\0';
    for (i = 0; i < size; ++i) {
        printf("%02X ", data[i]);
        ascii[i % 16] = (data[i] >= ' ' && data[i] <= '~') ? data[i] : '.';
        if (((i + 1) % 8 == 0) || i + 1 == size) {
            printf(" ");
            if ((i + 1) % 16 == 0) {
                printf("| %s\n", ascii);
            } else if (i + 1 == size) {
                ascii[(i + 1) % 16] = '\0';
                if ((i + 1) % 16 <= 8) printf(" ");
                for (j = (i + 1) % 16; j < 16; ++j) printf("   ");
                printf("| %s\n", ascii);
            }
        }
    }
}

/* Prints printable string runs found inside a frame. Etch exceptions carry
 * their reason as embedded strings, so this surfaces e.g.
 * "authentication failed" straight from the server's reply. */
static void dump_frame_strings(const unsigned char *p, uint32_t len) {
    uint32_t i = 0, start;
    int found = 0;
    while (i < len) {
        if (p[i] >= 0x20 && p[i] <= 0x7E) {
            start = i;
            while (i < len && p[i] >= 0x20 && p[i] <= 0x7E) i++;
            if (i - start >= 4) {
                printf("           string: \"%.*s\"\n", (int)(i - start), p + start);
                found = 1;
            }
        } else {
            i++;
        }
    }
    if (!found) printf("           (no readable strings in frame)\n");
}

/* ---------------- Etch writer helpers ---------------- */

typedef struct {
    unsigned char *buf;
    size_t size;
    size_t cap;
} BWriter;

static BWriter *g_msg_id_val;
static BWriter *g_h_val;
static BWriter *g_cds_h_val;

static BWriter *bwriter_new(size_t cap) {
    BWriter *bw = malloc(sizeof(BWriter));
    bw->buf = malloc(cap);
    bw->size = 0;
    bw->cap = cap;
    return bw;
}

static void bwriter_free(BWriter *bw) {
    if (bw) { free(bw->buf); free(bw); }
}

static void bwriter_add(BWriter *bw, const unsigned char *data, size_t len) {
    if (bw->size + len > bw->cap) {
        bw->cap = (bw->size + len) * 2;
        bw->buf = realloc(bw->buf, bw->cap);
    }
    memcpy(bw->buf + bw->size, data, len);
    bw->size += len;
}

static void bwriter_add_u8(BWriter *bw, uint8_t v) { bwriter_add(bw, &v, 1); }

static void bwriter_add_int(BWriter *bw, int64_t v) {
    if (v >= -64 && v <= 127) {
        bwriter_add_u8(bw, (uint8_t)v);
    } else if (v >= -128 && v <= 127) {
        bwriter_add_u8(bw, TC_BYTE);
        bwriter_add_u8(bw, (uint8_t)v);
    } else if (v >= -32768 && v <= 32767) {
        bwriter_add_u8(bw, TC_SHORT);
        bwriter_add_u8(bw, (uint8_t)((v >> 8) & 0xFF));
        bwriter_add_u8(bw, (uint8_t)(v & 0xFF));
    } else if (v >= -2147483648LL && v <= 2147483647LL) {
        bwriter_add_u8(bw, TC_INT);
        bwriter_add_u8(bw, (uint8_t)((v >> 24) & 0xFF));
        bwriter_add_u8(bw, (uint8_t)((v >> 16) & 0xFF));
        bwriter_add_u8(bw, (uint8_t)((v >> 8) & 0xFF));
        bwriter_add_u8(bw, (uint8_t)(v & 0xFF));
    } else {
        bwriter_add_u8(bw, TC_LONG);
        int s;
        for (s = 56; s >= 0; s -= 8) bwriter_add_u8(bw, (uint8_t)((v >> s) & 0xFF));
    }
}

static void bwriter_add_bool(BWriter *bw, int v) { bwriter_add_u8(bw, v ? TC_TRUE : TC_FALSE); }

static void bwriter_add_bytes(BWriter *bw, const unsigned char *data, size_t len) {
    bwriter_add_u8(bw, TC_BYTES);
    bwriter_add_int(bw, (int64_t)len);
    bwriter_add(bw, data, len);
}

static void bwriter_add_string(BWriter *bw, const char *str) {
    if (!str || str[0] == '\0') { bwriter_add_u8(bw, TC_EMPTY_STRING); return; }
    bwriter_add_u8(bw, TC_STRING);
    bwriter_add_int(bw, (int64_t)strlen(str));
    bwriter_add(bw, (const unsigned char *)str, strlen(str));
}

static void bwriter_add_null(BWriter *bw) { bwriter_add_u8(bw, TC_NULL); }
static void bwriter_add_none(BWriter *bw) { bwriter_add_u8(bw, TC_NONE); }

static uint32_t etch_hash(const char *name) {
    uint64_t h = 0x1505;
    while (*name) {
        h = ((h * 0x1003F) + (unsigned char)(*name)) & 0xFFFFFFFF;
        name++;
    }
    if (h >= 0x80000000ULL) h -= 0x100000000ULL;
    return (uint32_t)h;
}

typedef struct { uint32_t key; BWriter *val; } DictEntry;

static BWriter *build_message(uint32_t type_hash, DictEntry *fields, size_t field_count) {
    BWriter *bw = bwriter_new(256);
    size_t i;
    bwriter_add_u8(bw, ETCH_VERSION);
    bwriter_add_int(bw, (int64_t)(int32_t)type_hash);
    bwriter_add_int(bw, (int64_t)field_count);
    for (i = 0; i < field_count; i++) {
        bwriter_add_int(bw, (int64_t)(int32_t)fields[i].key);
        bwriter_add(bw, fields[i].val->buf, fields[i].val->size);
    }
    bwriter_add_none(bw);
    return bw;
}

static BWriter *custom_type(const char *type_name, DictEntry *fields, size_t field_count) {
    BWriter *bw = bwriter_new(256);
    size_t i;
    bwriter_add_u8(bw, TC_CUSTOM);
    bwriter_add_int(bw, (int64_t)(int32_t)etch_hash(type_name));
    bwriter_add_int(bw, (int64_t)field_count);
    for (i = 0; i < field_count; i++) {
        bwriter_add_int(bw, (int64_t)(int32_t)fields[i].key);
        bwriter_add(bw, fields[i].val->buf, fields[i].val->size);
    }
    bwriter_add_none(bw);
    return bw;
}

/* ---------------- socket I/O ---------------- */

static int write_all(int fd, const unsigned char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int send_frame(int fd, BWriter *payload) {
    uint32_t len = (uint32_t)payload->size;
    unsigned char header[8] = {
        0xDE, 0xAD, 0xBE, 0xEF,
        (unsigned char)((len >> 24) & 0xFF), (unsigned char)((len >> 16) & 0xFF),
        (unsigned char)((len >> 8) & 0xFF), (unsigned char)(len & 0xFF)
    };
    LOGN(">>> sending frame, payload %u bytes", len);
    if (write_all(fd, header, 8) < 0 || write_all(fd, payload->buf, payload->size) < 0) {
        LOGE("socket write failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/* 0 = ok, 1 = timeout, 2 = closed, 3 = bad magic */
static int recv_frame(int fd, unsigned char **out, uint32_t *out_len) {
    unsigned char header[8];
    size_t total = 0;
    while (total < 8) {
        ssize_t n = read(fd, header + total, 8 - total);
        if (n == 0) return 2;
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 1 : 2;
        total += (size_t)n;
    }
    if (memcmp(header, "\xDE\xAD\xBE\xEF", 4) != 0) return 3;
    uint32_t len = ((uint32_t)header[4] << 24) | ((uint32_t)header[5] << 16) |
                   ((uint32_t)header[6] << 8) | (uint32_t)header[7];
    if (len > 16 * 1024 * 1024) return 3;
    unsigned char *payload = malloc(len);
    total = 0;
    while (total < len) {
        ssize_t n = read(fd, payload + total, len - total);
        if (n == 0) { free(payload); return 2; }
        if (n < 0) { int e = errno; free(payload); return (e == EAGAIN || e == EWOULDBLOCK) ? 1 : 2; }
        total += (size_t)n;
    }
    LOGN("<<< received frame, payload %u bytes (etch version 0x%02x)", len, payload[0]);
    *out = payload;
    *out_len = len;
    return 0;
}

/* ---------------- naive Etch value decoding ---------------- */

/* Locates a field by its hash (hashes are always wire-encoded as TC_INT)
 * and returns the offset of the field's value, or -1. */
static long find_field(const unsigned char *p, uint32_t len, uint32_t hash) {
    unsigned char sig[5] = {
        TC_INT,
        (unsigned char)((hash >> 24) & 0xFF), (unsigned char)((hash >> 16) & 0xFF),
        (unsigned char)((hash >> 8) & 0xFF), (unsigned char)(hash & 0xFF)
    };
    uint32_t i;
    for (i = 0; i + 5 <= len; i++) {
        if (memcmp(p + i, sig, 5) == 0) return (long)(i + 5);
    }
    return -1;
}

static int decode_int(const unsigned char *p, uint32_t len, uint32_t off, int64_t *out, uint32_t *next) {
    if (off >= len) return -1;
    uint8_t b = p[off];
    if (b <= 0x7F) { *out = b; *next = off + 1; return 0; }
    if (b >= 0xC0) { *out = (int8_t)b; *next = off + 1; return 0; }
    if (b == TC_BYTE && off + 1 < len) { *out = (int8_t)p[off + 1]; *next = off + 2; return 0; }
    if (b == TC_SHORT && off + 2 < len) {
        *out = (int16_t)((p[off + 1] << 8) | p[off + 2]); *next = off + 3; return 0;
    }
    if (b == TC_INT && off + 4 < len) {
        *out = (int32_t)(((uint32_t)p[off + 1] << 24) | ((uint32_t)p[off + 2] << 16) |
                         ((uint32_t)p[off + 3] << 8) | (uint32_t)p[off + 4]);
        *next = off + 5; return 0;
    }
    if (b == TC_LONG && off + 8 < len) {
        uint64_t u = 0;
        int s;
        for (s = 1; s <= 8; s++) u = (u << 8) | p[off + (uint32_t)s];
        *out = (int64_t)u;
        *next = off + 9; return 0;
    }
    return -1;
}

/* Extracts the result byte[] (the SAS challenge). Returns length or -1. */
static int extract_result_bytes(const unsigned char *p, uint32_t len, unsigned char *out, size_t out_cap) {
    long off = find_field(p, len, H_RESULT);
    int64_t blen;
    uint32_t cur;
    if (off < 0 || p[off] != TC_BYTES) return -1;
    if (decode_int(p, len, (uint32_t)off + 1, &blen, &cur) < 0) return -1;
    if (blen <= 0 || (size_t)blen > out_cap || cur + blen > len) return -1;
    memcpy(out, p + cur, (size_t)blen);
    return (int)blen;
}

static int frame_is_error(const unsigned char *p, uint32_t len) {
    long off = find_field(p, len, H_RESULT);
    if (off < 0) return 0;
    return p[off] == TC_CUSTOM; /* exception struct instead of a plain value */
}

static int frame_has_result(const unsigned char *p, uint32_t len) {
    return find_field(p, len, H_RESULT) >= 0;
}

static long frame_reply_to(const unsigned char *p, uint32_t len) {
    long off = find_field(p, len, H_IN_REPLY_TO);
    int64_t v;
    uint32_t next;
    if (off < 0 || decode_int(p, len, (uint32_t)off, &v, &next) < 0) return -1;
    return (long)v;
}

/* Waits for the reply belonging to msg_id. Returns:
 *   1 = success (void ack with matching in_reply_to, or a non-exception H_RESULT)
 *   0 = server answered with an Etch exception (error struct)
 *  -1 = timeout / connection lost
 *
 * iDrive 4 sas_login is a void Etch method: success is in_reply_to == msg_id
 * with NO H_RESULT (28-byte frame). Requiring H_RESULT treated that as async.
 */
static int wait_reply(int fd, uint32_t msg_id, int64_t *int_result, int verbose) {
    int tries;
    for (tries = 0; tries < MAX_WAIT_FRAMES; tries++) {
        unsigned char *rx = NULL;
        uint32_t rx_len = 0;
        int rc = recv_frame(fd, &rx, &rx_len);
        if (rc == 1) { LOGE("timeout waiting for reply to msg #%u", msg_id); return -1; }
        if (rc != 0) { LOGE("connection lost waiting for reply to msg #%u", msg_id); return -1; }

        long reply_to = frame_reply_to(rx, rx_len);
        int has_res = frame_has_result(rx, rx_len);
        int is_reply = (reply_to == (long)msg_id) || (has_res && reply_to == -1);

        if (is_reply) {
            if (frame_is_error(rx, rx_len)) {
                LOGE("server returned an EXCEPTION for msg #%u:", msg_id);
                hexdump(rx, rx_len > 256 ? 256 : rx_len);
                dump_frame_strings(rx, rx_len);
                free(rx);
                return 0;
            }
            if (int_result) {
                if (has_res) {
                    long off = find_field(rx, rx_len, H_RESULT);
                    uint32_t next;
                    if (decode_int(rx, rx_len, (uint32_t)off, int_result, &next) < 0) {
                        *int_result = -9999; /* non-int result, e.g. bytes/null */
                    }
                } else {
                    *int_result = 0; /* void success */
                }
            }
            if (!has_res)
                LOGOK("void Etch ack for msg #%u (in_reply_to matches, no H_RESULT)", msg_id);
            if (verbose) { LOGN("reply frame for msg #%u:", msg_id); hexdump(rx, rx_len > 128 ? 128 : rx_len); }
            free(rx);
            return 1;
        }
        {
            int64_t th = -1, aid = -1;
            uint32_t next = 0;
            long off;
            if (decode_int(rx, rx_len, 1, &th, &next) == 0 &&
                (uint32_t)th == etch_hash("de.bmw.idrive.BMWRemoting.rhmi_onActionEvent")) {
                off = find_field(rx, rx_len, etch_hash("actionId"));
                if (off >= 0 && decode_int(rx, rx_len, (uint32_t)off, &aid, &next) == 0)
                    g_pending_action = (int)aid;
                LOGI("queued raAction %d while waiting for msg #%u", g_pending_action, msg_id);
                free(rx);
                continue;
            }
            if (handle_cds_frame(rx, rx_len)) {
                free(rx);
                continue;
            }
        }
        LOGI("(async frame while waiting for msg #%u, type hash dump follows)", msg_id);
        hexdump(rx, rx_len > 128 ? 128 : rx_len);
        free(rx);
    }
    LOGE("gave up after %d frames waiting for msg #%u", MAX_WAIT_FRAMES, msg_id);
    return -1;
}

static int rhmi_set_bool(int model, int on) {
    int rc;
    BWriter *m_val, *b_val, *m;
    DictEntry f[4];

    g_msg_id++;
    g_msg_id_val->size = 0;
    bwriter_add_int(g_msg_id_val, g_msg_id);
    m_val = bwriter_new(16); bwriter_add_int(m_val, model);
    b_val = bwriter_new(16); bwriter_add_bool(b_val, on);
    f[0].key = H_MESSAGE_ID; f[0].val = g_msg_id_val;
    f[1].key = H_HANDLE;     f[1].val = g_h_val;
    f[2].key = etch_hash("modelId"); f[2].val = m_val;
    f[3].key = etch_hash("value");   f[3].val = b_val;
    m = build_message(H_RHMI_SET_DATA, f, 4);
    rc = send_frame(g_sock, m);
    bwriter_free(m);
    bwriter_free(m_val);
    bwriter_free(b_val);
    if (rc < 0) return -1;
    return wait_reply(g_sock, g_msg_id, NULL, 0) == 1 ? 0 : -1;
}

static int rhmi_set_string(int model, const char *s) {
    int rc;
    BWriter *m_val, *s_val, *m;
    DictEntry f[4];

    g_msg_id++;
    g_msg_id_val->size = 0;
    bwriter_add_int(g_msg_id_val, g_msg_id);
    m_val = bwriter_new(16); bwriter_add_int(m_val, model);
    s_val = bwriter_new(64); bwriter_add_string(s_val, s);
    f[0].key = H_MESSAGE_ID; f[0].val = g_msg_id_val;
    f[1].key = H_HANDLE;     f[1].val = g_h_val;
    f[2].key = etch_hash("modelId"); f[2].val = m_val;
    f[3].key = etch_hash("value");   f[3].val = s_val;
    m = build_message(H_RHMI_SET_DATA, f, 4);
    rc = send_frame(g_sock, m);
    bwriter_free(m);
    bwriter_free(m_val);
    bwriter_free(s_val);
    if (rc < 0) return -1;
    return wait_reply(g_sock, g_msg_id, NULL, 0) == 1 ? 0 : -1;
}

static int rhmi_add_action_handler(int action_id) {
    int rc;
    BWriter *a_val, *i_val, *m;
    DictEntry f[4];

    g_msg_id++;
    g_msg_id_val->size = 0;
    bwriter_add_int(g_msg_id_val, g_msg_id);
    a_val = bwriter_new(16); bwriter_add_int(a_val, action_id);
    i_val = bwriter_new(64); bwriter_add_string(i_val, RADAR_APP_ID);
    f[0].key = H_MESSAGE_ID; f[0].val = g_msg_id_val;
    f[1].key = H_HANDLE;     f[1].val = g_h_val;
    f[2].key = etch_hash("actionId"); f[2].val = a_val;
    f[3].key = etch_hash("ident");    f[3].val = i_val;
    m = build_message(H_RHMI_ADD_ACTION_EVENT_HANDLER, f, 4);
    rc = send_frame(g_sock, m);
    bwriter_free(m);
    bwriter_free(a_val);
    bwriter_free(i_val);
    if (rc < 0) return -1;
    rc = wait_reply(g_sock, g_msg_id, NULL, 0);
    if (rc == 0) {
        LOGE("rhmi_addActionEventHandler(action=%d) exception", action_id);
        return -1;
    }
    if (rc == 1)
        LOGOK("listening for raAction %d", action_id);
    else
        LOGI("raAction %d handler sent (no ack — treated as one-way)", action_id);
    return 0;
}


static int push_settings(void) {
    if (rhmi_set_bool(RADAR_MODEL_CHECKBOX, radar_popup_enabled()) != 0)
        return -1;
    if (rhmi_set_string(RADAR_MODEL_STATUS, radar_status_line()) != 0)
        return -1;
    if (rhmi_set_string(RADAR_MODEL_RANGE, radar_range_line()) != 0)
        return -1;
    (void)rhmi_set_bool(RADAR_MODEL_RANGE_DN, 0);
    (void)rhmi_set_bool(RADAR_MODEL_RANGE_UP, 0);
    return 0;
}

static int popup_set(int on) {
    radar_set_popup(on);
    push_settings();
    LOGOK("popup %s", radar_popup_enabled() ? "ON" : "OFF");
    return 0;
}

static void popup_keepalive(void) {
    static char last[80];
    static char last_r[32];
    const char *st;
    const char *rl;
    radar_tick();
    st = radar_status_line();
    rl = radar_range_line();
    if (strcmp(st, last) != 0 || strcmp(rl, last_r) != 0) {
        snprintf(last, sizeof(last), "%s", st);
        snprintf(last_r, sizeof(last_r), "%s", rl);
        rhmi_set_string(RADAR_MODEL_STATUS, st);
        rhmi_set_string(RADAR_MODEL_RANGE, rl);
        rhmi_set_bool(RADAR_MODEL_CHECKBOX, radar_popup_enabled());
    }
}

static int event_action_id(const unsigned char *rx, uint32_t rx_len) {
    uint32_t next;
    int64_t type_hash = -1;
    int64_t action_id = -1;
    long off;

    if (decode_int(rx, rx_len, 1, &type_hash, &next) != 0) return -1;
    if ((uint32_t)type_hash != etch_hash("de.bmw.idrive.BMWRemoting.rhmi_onActionEvent"))
        return -1;
    off = find_field(rx, rx_len, etch_hash("actionId"));
    if (off < 0) return -1;
    if (decode_int(rx, rx_len, (uint32_t)off, &action_id, &next) != 0) return -1;
    return (int)action_id;
}

static void handle_ra_action(int aid) {
    if (aid == RADAR_ACTION_POPUP) {
        LOGI("checkbox raAction %d — toggle popup (currently %s)",
             aid, radar_popup_enabled() ? "ON" : "OFF");
        popup_set(!radar_popup_enabled());
    } else if (aid == RADAR_ACTION_RANGE_DN) {
        LOGI("range - (raAction %d)", aid);
        radar_range_step(-1);
        push_settings();
    } else if (aid == RADAR_ACTION_RANGE_UP) {
        LOGI("range + (raAction %d)", aid);
        radar_range_step(1);
        push_settings();
    } else if (aid == RADAR_ACTION_ENTRY) {
        LOGI("speeduntrap opened (raAction %d)", aid);
        push_settings();
    }
}

static void drain_pending_actions(void) {
    while (g_pending_action >= 0) {
        int aid = g_pending_action;
        g_pending_action = -1;
        handle_ra_action(aid);
    }
}


static int g_net_quiet;

static int decode_string(const unsigned char *p, uint32_t len, uint32_t off,
                         char *out, int cap, uint32_t *next) {
    int64_t slen;
    uint32_t cur;
    if (!out || cap < 1 || off >= len)
        return -1;
    out[0] = '\0';
    if (p[off] == TC_EMPTY_STRING || p[off] == TC_NULL) {
        *next = off + 1;
        return p[off] == TC_EMPTY_STRING ? 0 : -1;
    }
    if (p[off] != TC_STRING)
        return -1;
    if (decode_int(p, len, off + 1, &slen, &cur) < 0)
        return -1;
    if (slen < 0 || cur + (uint32_t)slen > len)
        return -1;
    if (slen >= cap)
        slen = cap - 1;
    memcpy(out, p + cur, (size_t)slen);
    out[slen] = '\0';
    *next = cur + (uint32_t)slen;
    return 0;
}

static int cds_json_num(const char *json, const char *key, double *out) {
    char pat[48];
    const char *p;
    char *end;
    if (!json || !key || !out)
        return -1;
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(json, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '"')
        return -1;
    *out = strtod(p, &end);
    if (end == p)
        return -1;
    return 0;
}

/* HmiMain3D sends cds_onPropertyChangedEvent as a two-way Etch call.
 * libetch auto-replies; we must send a void _result or the session dies. */
static int send_void_reply(uint32_t result_hash, int64_t in_reply_to) {
    BWriter *irt, *mid, *m;
    DictEntry f[2];
    int rc, old_q;
    g_msg_id++;
    irt = bwriter_new(16);
    bwriter_add_int(irt, in_reply_to);
    mid = bwriter_new(16);
    bwriter_add_int(mid, g_msg_id);
    f[0].key = H_IN_REPLY_TO;
    f[0].val = irt;
    f[1].key = H_MESSAGE_ID;
    f[1].val = mid;
    m = build_message(result_hash, f, 2);
    old_q = g_net_quiet;
    g_net_quiet = 1;
    rc = send_frame(g_sock, m);
    g_net_quiet = old_q;
    bwriter_free(m);
    bwriter_free(irt);
    bwriter_free(mid);
    return rc;
}

static void cds_ack_if_call(const unsigned char *rx, uint32_t rx_len) {
    int64_t mid = -1;
    uint32_t next = 0;
    long off;
    if (frame_reply_to(rx, rx_len) >= 0)
        return;
    off = find_field(rx, rx_len, H_MESSAGE_ID);
    if (off < 0 || decode_int(rx, rx_len, (uint32_t)off, &mid, &next) != 0 || mid < 0)
        return;
    if (send_void_reply(H_CDS_ON_PROP_RES, mid) != 0)
        LOGE("CDS void _result for msg %lld failed", (long long)mid);
}

static int handle_cds_frame(const unsigned char *rx, uint32_t rx_len) {
    int64_t th = -1;
    uint32_t next = 0;
    long off;
    char name[80], ident[64], val[256];
    double n;
    if (decode_int(rx, rx_len, 1, &th, &next) != 0)
        return 0;
    if ((uint32_t)th != H_CDS_ON_PROP)
        return 0;
    cds_ack_if_call(rx, rx_len);
    name[0] = ident[0] = val[0] = '\0';
    off = find_field(rx, rx_len, etch_hash("propertyName"));
    if (off >= 0)
        (void)decode_string(rx, rx_len, (uint32_t)off, name, (int)sizeof(name), &next);
    off = find_field(rx, rx_len, H_IDENT);
    if (off >= 0)
        (void)decode_string(rx, rx_len, (uint32_t)off, ident, (int)sizeof(ident), &next);
    off = find_field(rx, rx_len, etch_hash("propertyValue"));
    if (off >= 0)
        (void)decode_string(rx, rx_len, (uint32_t)off, val, (int)sizeof(val), &next);
    if (strstr(val, "\"error\""))
        return 1;
    if (strcmp(ident, "spd") == 0 || strcmp(name, "driving.speedActual") == 0) {
        if (cds_json_num(val, "speedActual", &n) == 0)
            radar_set_speed_kmh(n);
        return 1;
    }
    return 1;
}

static int cds_subscribe(const char *prop, const char *ident, int interval_ms) {
    int rc;
    BWriter *pn, *id, *iv, *m;
    DictEntry f[5];
    if (g_cds < 0 || !g_cds_h_val)
        return -1;
    g_msg_id++;
    g_msg_id_val->size = 0;
    bwriter_add_int(g_msg_id_val, g_msg_id);
    pn = bwriter_new(80);  bwriter_add_string(pn, prop);
    id = bwriter_new(16);  bwriter_add_string(id, ident);
    iv = bwriter_new(16);  bwriter_add_int(iv, interval_ms);
    f[0].key = H_MESSAGE_ID;                    f[0].val = g_msg_id_val;
    f[1].key = H_HANDLE;                        f[1].val = g_cds_h_val;
    f[2].key = etch_hash("propertyName");       f[2].val = pn;
    f[3].key = H_IDENT;                         f[3].val = id;
    f[4].key = etch_hash("intervalLimit");      f[4].val = iv;
    m = build_message(H_CDS_ADD_PROP, f, 5);
    rc = send_frame(g_sock, m);
    bwriter_free(m);
    bwriter_free(pn);
    bwriter_free(id);
    bwriter_free(iv);
    if (rc < 0)
        return -1;
    rc = wait_reply(g_sock, g_msg_id, NULL, 0);
    if (rc == 1) {
        LOGOK("CDS subscribe %s (%d ms)", prop, interval_ms);
        return 0;
    }
    LOGE("CDS subscribe %s failed (rc=%d)", prop, rc);
    return -1;
}

static int cds_start(void) {
    int64_t h = -1;
    int rc;
    BWriter *uuid, *m;
    DictEntry f[2];
    g_msg_id++;
    g_msg_id_val->size = 0;
    bwriter_add_int(g_msg_id_val, g_msg_id);
    uuid = bwriter_new(80);
    bwriter_add_string(uuid, RADAR_APP_ID);
    f[0].key = H_MESSAGE_ID;
    f[0].val = g_msg_id_val;
    f[1].key = etch_hash("appUuid");
    f[1].val = uuid;
    m = build_message(H_CDS_CREATE, f, 2);
    rc = send_frame(g_sock, m);
    bwriter_free(m);
    bwriter_free(uuid);
    if (rc < 0)
        return -1;
    rc = wait_reply(g_sock, g_msg_id, &h, 1);
    if (rc != 1 || h < 0 || h == -9999) {
        LOGE("cds_create failed (rc=%d handle=%lld) — speed from radar/speed.txt",
             rc, (long long)h);
        return -1;
    }
    g_cds = (long)h;
    if (g_cds_h_val)
        bwriter_free(g_cds_h_val);
    g_cds_h_val = bwriter_new(16);
    bwriter_add_int(g_cds_h_val, g_cds);
    LOGOK("cds_create handle=%ld", g_cds);
    (void)cds_subscribe("driving.speedActual", "spd", 0);
    return 0;
}

/* ---------------- RSA-MD5 challenge signing ---------------- */
/*
 * Real OpenSSL lives in /fs/sda0/lib, not /usr/lib.
 *   libcrypto.so.1  = SSLeay / OpenSSL 0.9.8–1.0  (EVP_SignInit, RSA_sign)
 *   libcrypto.so.2  = later ABI (EVP_DigestSign*) — do not use for SAS
 * SAS verify is RSA PKCS#1 v1.5 over MD5 (same as iDrive 6 CAuthVerification).
 * Prefer so.1 RSA_sign; fall back to the built-in schoolbook path.
 */

#define BN_LIMBS 64  /* 2048-bit, little-endian uint32 limbs */

static void md5_block(uint32_t h[4], const unsigned char buf[64])
{
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const unsigned char S[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5,9,14,20, 5,9,14,20, 5,9,14,20, 5,9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    uint32_t w[16], a=h[0], b=h[1], c=h[2], d=h[3], f, x, t;
    unsigned j, g;
    for (j = 0; j < 16; j++)
        w[j] = (uint32_t)buf[j*4] | ((uint32_t)buf[j*4+1]<<8) |
               ((uint32_t)buf[j*4+2]<<16) | ((uint32_t)buf[j*4+3]<<24);
    for (j = 0; j < 64; j++) {
        if (j < 16)      { f = (b & c) | ((~b) & d); g = j; }
        else if (j < 32) { f = (d & b) | ((~d) & c); g = (5*j + 1) & 15; }
        else if (j < 48) { f = b ^ c ^ d;            g = (3*j + 5) & 15; }
        else             { f = c ^ (b | (~d));       g = (7*j) & 15; }
        x = a + f + K[j] + w[g];
        t = d; d = c; c = b;
        b = b + ((x << S[j]) | (x >> (32 - S[j])));
        a = t;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
}

static void md5(const unsigned char *data, size_t len, unsigned char out[16])
{
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    unsigned char buf[64];
    uint64_t bitlen = (uint64_t)len * 8;
    size_t off = 0, rem, i;

    while (len - off >= 64) {
        md5_block(h, data + off);
        off += 64;
    }
    rem = len - off;
    memset(buf, 0, 64);
    memcpy(buf, data + off, rem);
    buf[rem] = 0x80;
    if (rem >= 56) {
        md5_block(h, buf);
        memset(buf, 0, 64);
    }
    buf[56] = (unsigned char)bitlen;
    buf[57] = (unsigned char)(bitlen >> 8);
    buf[58] = (unsigned char)(bitlen >> 16);
    buf[59] = (unsigned char)(bitlen >> 24);
    buf[60] = (unsigned char)(bitlen >> 32);
    buf[61] = (unsigned char)(bitlen >> 40);
    buf[62] = (unsigned char)(bitlen >> 48);
    buf[63] = (unsigned char)(bitlen >> 56);
    md5_block(h, buf);
    for (i = 0; i < 4; i++) {
        out[i*4]   = (unsigned char)h[i];
        out[i*4+1] = (unsigned char)(h[i] >> 8);
        out[i*4+2] = (unsigned char)(h[i] >> 16);
        out[i*4+3] = (unsigned char)(h[i] >> 24);
    }
}

static int der_int(const unsigned char *p, size_t n, size_t *off,
                   unsigned char *out, size_t out_max, size_t *out_len)
{
    size_t i = *off, len, j;
    if (i >= n || p[i++] != 0x02) return -1;
    if (i >= n) return -1;
    if (p[i] & 0x80) {
        int nbytes = p[i++] & 0x7f;
        len = 0;
        while (nbytes--) {
            if (i >= n) return -1;
            len = (len << 8) | p[i++];
        }
    } else {
        len = p[i++];
    }
    if (i + len > n) return -1;
    /* skip leading 0x00 sign byte */
    j = 0;
    if (len > 0 && p[i] == 0x00) { i++; len--; }
    if (len > out_max) return -1;
    memcpy(out, p + i, len);
    *out_len = len;
    *off = i + len;
    (void)j;
    return 0;
}

static void bn_from_be(uint32_t o[BN_LIMBS], const unsigned char *be, size_t n)
{
    size_t i;
    memset(o, 0, BN_LIMBS * sizeof(uint32_t));
    for (i = 0; i < n; i++) {
        size_t bit = (n - 1 - i) * 8;
        o[bit / 32] |= (uint32_t)be[i] << (bit % 32);
    }
}

static void bn_to_be(unsigned char *be, size_t n, const uint32_t v[BN_LIMBS])
{
    size_t i;
    for (i = 0; i < n; i++) {
        size_t bit = (n - 1 - i) * 8;
        be[i] = (unsigned char)(v[bit / 32] >> (bit % 32));
    }
}

static int bn_cmp(const uint32_t a[BN_LIMBS], const uint32_t b[BN_LIMBS])
{
    int i;
    for (i = BN_LIMBS - 1; i >= 0; i--) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

static int bn_sub(uint32_t a[BN_LIMBS], const uint32_t b[BN_LIMBS])
{
    uint64_t br = 0;
    int i;
    for (i = 0; i < BN_LIMBS; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - br;
        a[i] = (uint32_t)t;
        br = (t >> 32) & 1;
    }
    return (int)br;
}

static void bn_shl1_add_mod(uint32_t r[BN_LIMBS], unsigned bit, const uint32_t m[BN_LIMBS])
{
    uint32_t c = bit;
    int i;
    for (i = 0; i < BN_LIMBS; i++) {
        uint32_t n = r[i] >> 31;
        r[i] = (r[i] << 1) | c;
        c = n;
    }
    /* leftover carry means r overflowed 2048 bits → definitely >= m */
    if (c || bn_cmp(r, m) >= 0)
        bn_sub(r, m);
}

static void bn_modexp(uint32_t out[BN_LIMBS], const uint32_t base[BN_LIMBS],
                      const uint32_t exp[BN_LIMBS], const uint32_t mod[BN_LIMBS])
{
    uint32_t r[BN_LIMBS], b[BN_LIMBS];
    int limb, bit;
    memset(r, 0, sizeof(r));
    r[0] = 1;
    memcpy(b, base, sizeof(b));
    /* process exp LSB first: square-and-multiply */
    for (limb = 0; limb < BN_LIMBS; limb++) {
        uint32_t e = exp[limb];
        for (bit = 0; bit < 32; bit++) {
            if (e & 1) {
                /* r = r * b mod m  via 2048-bit schoolbook into 4096 then reduce */
                uint32_t acc[BN_LIMBS * 2];
                int i, j;
                memset(acc, 0, sizeof(acc));
                for (i = 0; i < BN_LIMBS; i++) {
                    uint64_t c = 0;
                    for (j = 0; j < BN_LIMBS; j++) {
                        uint64_t t = (uint64_t)acc[i+j] + (uint64_t)r[i] * b[j] + c;
                        acc[i+j] = (uint32_t)t;
                        c = t >> 32;
                    }
                    acc[i + BN_LIMBS] += (uint32_t)c;
                }
                /* reduce acc (4096-bit) mod m by shifting in bits */
                {
                    uint32_t red[BN_LIMBS];
                    int k;
                    memset(red, 0, sizeof(red));
                    for (k = BN_LIMBS * 64 - 1; k >= 0; k--) {
                        unsigned bitv = (acc[k / 32] >> (k % 32)) & 1;
                        bn_shl1_add_mod(red, bitv, mod);
                    }
                    memcpy(r, red, sizeof(r));
                }
            }
            /* b = b^2 mod m */
            {
                uint32_t acc[BN_LIMBS * 2];
                int i, j, k;
                memset(acc, 0, sizeof(acc));
                for (i = 0; i < BN_LIMBS; i++) {
                    uint64_t c = 0;
                    for (j = 0; j < BN_LIMBS; j++) {
                        uint64_t t = (uint64_t)acc[i+j] + (uint64_t)b[i] * b[j] + c;
                        acc[i+j] = (uint32_t)t;
                        c = t >> 32;
                    }
                    acc[i + BN_LIMBS] += (uint32_t)c;
                }
                {
                    uint32_t red[BN_LIMBS];
                    memset(red, 0, sizeof(red));
                    for (k = BN_LIMBS * 64 - 1; k >= 0; k--) {
                        unsigned bitv = (acc[k / 32] >> (k % 32)) & 1;
                        bn_shl1_add_mod(red, bitv, mod);
                    }
                    memcpy(b, red, sizeof(b));
                }
            }
            e >>= 1;
        }
    }
    memcpy(out, r, sizeof(r));
}

static int parse_rsa_nd(const unsigned char *der, size_t der_len,
                        unsigned char n[256], size_t *nlen,
                        unsigned char d[256], size_t *dlen)
{
    size_t off = 0, seq_len, ign;
    unsigned char tmp[260];
    if (der_len < 2 || der[0] != 0x30) return -1;
    off = 1;
    if (der[off] & 0x80) {
        int nb = der[off++] & 0x7f;
        seq_len = 0;
        while (nb--) seq_len = (seq_len << 8) | der[off++];
    } else {
        seq_len = der[off++];
    }
    (void)seq_len;
    /* version */
    if (der_int(der, der_len, &off, tmp, sizeof(tmp), &ign) != 0) return -1;
    if (der_int(der, der_len, &off, n, 256, nlen) != 0) return -1; /* n */
    if (der_int(der, der_len, &off, tmp, sizeof(tmp), &ign) != 0) return -1; /* e */
    if (der_int(der, der_len, &off, d, 256, dlen) != 0) return -1; /* d */
    return 0;
}

static int sign_challenge_builtin(const unsigned char *challenge, size_t chal_len,
                          unsigned char *out_sig, size_t *out_sig_len) {
    /* DigestInfo for MD5 */
    static const unsigned char di_prefix[18] = {
        0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
        0x02, 0x05, 0x05, 0x00, 0x04, 0x10
    };
    unsigned char md[16], n[256], d[256], em[256];
    size_t nlen = 0, dlen = 0, k, i;
    uint32_t bn_n[BN_LIMBS], bn_d[BN_LIMBS], bn_m[BN_LIMBS], bn_s[BN_LIMBS];

    if (parse_rsa_nd(embedded_privkey, embedded_privkey_len, n, &nlen, d, &dlen) != 0) {
        LOGE("failed to parse embedded RSA private key");
        return -1;
    }
    if (nlen != 256) {
        LOGE("unexpected RSA modulus size %u (want 256)", (unsigned)nlen);
        return -1;
    }
    LOGI("signing with built-in RSA-MD5 (n=%u d=%u bytes, no libcrypto)",
         (unsigned)nlen, (unsigned)dlen);

    md5(challenge, chal_len, md);

    /* PKCS#1 v1.5: 00 01 FF..FF 00 || DigestInfo */
    memset(em, 0xff, 256);
    em[0] = 0x00;
    em[1] = 0x01;
    k = 256 - 1 - 18 - 16; /* index of 0x00 before DigestInfo */
    em[k] = 0x00;
    memcpy(em + k + 1, di_prefix, 18);
    memcpy(em + k + 1 + 18, md, 16);

    bn_from_be(bn_n, n, 256);
    bn_from_be(bn_d, d, dlen);
    bn_from_be(bn_m, em, 256);
    bn_modexp(bn_s, bn_m, bn_d, bn_n);
    bn_to_be(out_sig, 256, bn_s);
    *out_sig_len = 256;
    (void)i;
    return 0;
}

/* OpenSSL 0.9.8m NIDs / padding (objects.h, rsa.h) */
#define OPENSSL_NID_MD5     4
#define OPENSSL_NID_SHA1    64
#define OPENSSL_NID_SHA256  672
#define RSA_PKCS1_PADDING   1

typedef enum {
    SIGN_RSA_MD5 = 0,
    SIGN_RSA_SHA1,
    SIGN_RSA_SHA256,
    SIGN_RAW_PKCS1,
    SIGN_MD5_XOR0,
    SIGN_MD5_XORV2,
    SIGN_MODE_COUNT
} SignMode;

static const char *sign_mode_name(SignMode m) {
    switch (m) {
    case SIGN_RSA_MD5:    return "RSA-MD5 DigestInfo of raw challenge";
    case SIGN_RSA_SHA1:   return "RSA-SHA1 DigestInfo of raw challenge";
    case SIGN_RSA_SHA256: return "RSA-SHA256 DigestInfo of raw challenge";
    case SIGN_RAW_PKCS1:  return "RSA_private_encrypt PKCS#1 of raw 16-byte challenge";
    case SIGN_MD5_XOR0:   return "RSA-MD5 of (challenge XOR MD5(uint32le 0))";
    case SIGN_MD5_XORV2:  return "RSA-MD5 of (challenge XOR MD5(uint32le 2))";
    default:              return "?";
    }
}

static void *dlopen_libcrypto_ssleay(void) {
    static const char *paths[] = {
        "/fs/sda0/lib/libcrypto.so.1",
        "libcrypto.so.1",
        "/lib/libcrypto.so.1",
        NULL
    };
    const char *old;
    char ldp[512];
    int i;
    void *h;

    old = getenv("LD_LIBRARY_PATH");
    if (!old || strstr(old, "/fs/sda0/lib") == NULL) {
        snprintf(ldp, sizeof(ldp), "/fs/sda0/lib%s%s",
                 (old && old[0]) ? ":" : "",
                 (old && old[0]) ? old : "");
        setenv("LD_LIBRARY_PATH", ldp, 1);
    }

    /* Preload zlib so libcrypto's NEEDED can resolve from /fs/sda0/lib. */
    dlopen("/fs/sda0/lib/libz.so.2", RTLD_NOW | RTLD_GLOBAL);

    for (i = 0; paths[i]; i++) {
        h = dlopen(paths[i], RTLD_NOW);
        if (h) {
            LOGI("dlopen %s OK", paths[i]);
            return h;
        }
        LOGI("dlopen %s failed: %s", paths[i], dlerror());
    }
    return NULL;
}

static int sign_challenge_openssl(SignMode mode,
                                  const unsigned char *challenge, size_t chal_len,
                                  unsigned char *out_sig, size_t *out_sig_len) {
    typedef void *(*fn_d2i)(void **, const unsigned char **, long);
    typedef int (*fn_RSA_sign)(int, const unsigned char *, unsigned int,
                               unsigned char *, unsigned int *, void *);
    typedef int (*fn_RSA_size)(const void *);
    typedef void (*fn_RSA_free)(void *);
    typedef int (*fn_RSA_private_encrypt)(int, const unsigned char *,
                                          unsigned char *, void *, int);
    typedef char *(*fn_SSLeay_version)(int);
    typedef unsigned char *(*fn_hash)(const unsigned char *, unsigned long, unsigned char *);
    typedef void (*fn_void)(void);

    static void *h;
    static int logged_ver;
    fn_d2i d2i_RSAPrivateKey;
    fn_RSA_sign RSA_sign;
    fn_RSA_size RSA_size;
    fn_RSA_free RSA_free;
    fn_RSA_private_encrypt RSA_private_encrypt;
    fn_SSLeay_version SSLeay_version;
    fn_hash MD5, SHA1, SHA256;
    fn_void add_all;
    const unsigned char *p;
    unsigned char md[64], work[256], le[4];
    unsigned int siglen, mdlen;
    int n, rc, nid;
    size_t i;
    void *rsa;

    if (!h) {
        h = dlopen_libcrypto_ssleay();
        if (!h) return -1;
    }

    d2i_RSAPrivateKey = (fn_d2i)dlsym(h, "d2i_RSAPrivateKey");
    RSA_sign = (fn_RSA_sign)dlsym(h, "RSA_sign");
    RSA_size = (fn_RSA_size)dlsym(h, "RSA_size");
    RSA_free = (fn_RSA_free)dlsym(h, "RSA_free");
    RSA_private_encrypt = (fn_RSA_private_encrypt)dlsym(h, "RSA_private_encrypt");
    SSLeay_version = (fn_SSLeay_version)dlsym(h, "SSLeay_version");
    MD5 = (fn_hash)dlsym(h, "MD5");
    SHA1 = (fn_hash)dlsym(h, "SHA1");
    SHA256 = (fn_hash)dlsym(h, "SHA256");
    add_all = (fn_void)dlsym(h, "OPENSSL_add_all_algorithms_noconf");
    if (!add_all)
        add_all = (fn_void)dlsym(h, "OpenSSL_add_all_algorithms");

    if (!d2i_RSAPrivateKey || !RSA_sign || !RSA_size || !RSA_free || !MD5) {
        LOGE("libcrypto.so.1 missing RSA_sign/d2i_RSAPrivateKey/MD5");
        return -1;
    }
    if (SSLeay_version && !logged_ver) {
        LOGI("OpenSSL: %s", SSLeay_version(0));
        logged_ver = 1;
    }
    if (add_all)
        add_all();

    p = embedded_privkey;
    rsa = d2i_RSAPrivateKey(NULL, &p, (long)embedded_privkey_len);
    if (!rsa) {
        LOGE("d2i_RSAPrivateKey failed (embedded key is not PKCS#1 RSA?)");
        return -1;
    }

    n = RSA_size(rsa);
    if (n <= 0 || (size_t)n > *out_sig_len) {
        LOGE("RSA_size %d does not fit output buffer %u", n, (unsigned)*out_sig_len);
        RSA_free(rsa);
        return -1;
    }

    if (mode == SIGN_RAW_PKCS1) {
        if (!RSA_private_encrypt) {
            LOGE("RSA_private_encrypt missing");
            RSA_free(rsa);
            return -1;
        }
        rc = RSA_private_encrypt((int)chal_len, challenge, out_sig, rsa, RSA_PKCS1_PADDING);
        RSA_free(rsa);
        if (rc <= 0) {
            LOGE("RSA_private_encrypt returned %d", rc);
            return -1;
        }
        *out_sig_len = (size_t)rc;
        LOGI("signed with RSA_private_encrypt PKCS#1 (%u bytes)", (unsigned)*out_sig_len);
        return 0;
    }

    if (chal_len > sizeof(work)) {
        RSA_free(rsa);
        return -1;
    }
    memcpy(work, challenge, chal_len);

    if (mode == SIGN_MD5_XOR0 || mode == SIGN_MD5_XORV2) {
        uint32_t v = (mode == SIGN_MD5_XORV2) ? 2u : 0u;
        le[0] = (unsigned char)(v);
        le[1] = (unsigned char)(v >> 8);
        le[2] = (unsigned char)(v >> 16);
        le[3] = (unsigned char)(v >> 24);
        if (!MD5(le, 4, md)) {
            RSA_free(rsa);
            return -1;
        }
        for (i = 0; i < chal_len && i < 16; i++)
            work[i] ^= md[i];
        LOGI("XOR mask MD5(uint32le %u) applied to challenge", (unsigned)v);
    }

    nid = OPENSSL_NID_MD5;
    mdlen = 16;
    if (mode == SIGN_RSA_SHA1) {
        if (!SHA1) { RSA_free(rsa); return -1; }
        nid = OPENSSL_NID_SHA1;
        mdlen = 20;
        if (!SHA1(work, (unsigned long)chal_len, md)) { RSA_free(rsa); return -1; }
    } else if (mode == SIGN_RSA_SHA256) {
        if (!SHA256) { RSA_free(rsa); return -1; }
        nid = OPENSSL_NID_SHA256;
        mdlen = 32;
        if (!SHA256(work, (unsigned long)chal_len, md)) { RSA_free(rsa); return -1; }
    } else {
        if (!MD5(work, (unsigned long)chal_len, md)) { RSA_free(rsa); return -1; }
    }

    siglen = (unsigned int)n;
    rc = RSA_sign(nid, md, mdlen, out_sig, &siglen, rsa);
    RSA_free(rsa);
    if (rc != 1) {
        LOGE("RSA_sign(nid=%d) returned %d", nid, rc);
        return -1;
    }
    *out_sig_len = siglen;
    LOGI("signed with libcrypto.so.1 RSA_sign nid=%d (%u bytes)", nid, (unsigned)siglen);
    return 0;
}

static int sign_challenge(SignMode mode,
                          const unsigned char *challenge, size_t chal_len,
                          unsigned char *out_sig, size_t *out_sig_len) {
    size_t cap = *out_sig_len;
    if (sign_challenge_openssl(mode, challenge, chal_len, out_sig, out_sig_len) == 0)
        return 0;
    if (mode != SIGN_RSA_MD5) {
        LOGI("OpenSSL cannot produce mode %s", sign_mode_name(mode));
        return -1;
    }
    LOGI("OpenSSL path failed, falling back to built-in RSA-MD5");
    *out_sig_len = cap;
    return sign_challenge_builtin(challenge, chal_len, out_sig, out_sig_len);
}

/* ---------------- verdict summary ---------------- */

static const char *verdict_str(int v) {
    return v == 1 ? "PASSED" : (v == -1 ? "FAILED" : "not reached");
}

static void print_summary(void) {
    printf("\n");
    printf("==========================================================\n");
    printf("  DIAGNOSTIC SUMMARY\n");
    printf("==========================================================\n");
    printf("  CERT CHECK (sas_certificate) : %s\n", verdict_str(v_cert));
    printf("  SAS LOGIN (signed challenge) : %s\n", verdict_str(v_login));
    printf("  RHMI REGISTRATION            : %s\n", verdict_str(v_rhmi));
    if (g_handle >= 0) printf("  RHMI handle                  : %ld\n", g_handle);
    printf("----------------------------------------------------------\n");
    if (v_cert == -1) {
        printf("  -> Certificate was REJECTED by IPCEServer.\n");
        printf("     The X509_verify_cert patch is NOT active.\n");
        printf("     Re-apply: /tmp/ipce_mem_patch_x86 <IPCEServer_PID>\n");
        printf("     (or replace the binary with IPCEServerPatched)\n");
    } else if (v_cert == 1 && v_login == -1) {
        printf("  -> Cert accepted but every signature mode was refused.\n");
        printf("     Embedded key matches the PKCS#7 cert; OpenSSL 0.9.8m RSA-MD5\n");
        printf("     is the iDrive-6 algorithm. Likely the token has a NULL pubkey\n");
        printf("     because v10 never finished X509_get_pubkey. Next HU patch is\n");
        printf("     login-accept at 0x8DBF2C3 (test ax,ax -> xor ax,ax).\n");
    } else if (v_login == 1 && v_rhmi == -1) {
        if (g_handle >= 0) {
            printf("  -> rhmi_create OK (handle %ld) but a later RHMI call failed.\n", g_handle);
            printf("     Check the FAIL line above (addActionEventHandler / setResource /\n");
            printf("     initialize). Hello World skips addActionEventHandler entirely.\n");
        } else {
            printf("  -> Login OK but rhmi_create was refused.\n");
            printf("     Exception 'RHMI is not permitted for this certificate' means\n");
            printf("     getRHMIPermissionValue returned 0 (token has no RHMI=true map\n");
            printf("     because v10 skipped parsePermissions). Next HU patch is v12:\n");
            printf("     0x8E3EFC0  push/mov... -> mov eax,1; ret.\n");
        }
    } else if (v_rhmi == 1) {
        printf("  -> All good! 'speeduntrap' is live in the\n");
        printf("     ConnectedDrive menu while this program runs.\n");
    }
    printf("==========================================================\n\n");
}

#ifdef SPEEDUNTRAP
static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void *stop_watchdog(void *arg)
{
    int i;

    (void)arg;
    while (!radar_should_stop())
        sleep_ms(100);
    printf("radar: watchdog restoring HMI now\n");
    radar_force_restore();
    /* Never _exit while SAS is registered — that reboots the HU.
     * --test may hang in USB; then _exit after 2s so Putty returns. */
    if (!radar_is_test())
        return NULL;
    for (i = 0; i < 20; i++)
        sleep_ms(100);
    printf("radar: test still blocked — leaving\n");
    _exit(0);
    return NULL;
}

static void start_stop_watchdog(void)
{
    pthread_t wd;

    if (pthread_create(&wd, NULL, stop_watchdog, NULL) == 0)
        pthread_detach(wd);
}
#endif

static void on_sigint(int sig) {
#ifdef SPEEDUNTRAP
    const char msg[] = "\n[+++] Ctrl+C — restoring HMI, then leaving.\n";
    (void)sig;
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    radar_request_stop();
#else
    const char msg[] = "\n[+++] signal: restoring HMI, leaving.\n";
    (void)sig;
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    radar_shutdown();
    _exit(0);
#endif
}

static int sas_connect(void) {
    struct timeval tv;
    struct sockaddr_in srv;

    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    LOGI("connect to SAS @ 127.0.0.1:%d", SAS_PORT);
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock < 0) {
        LOGE("socket(): %s", strerror(errno));
        return -1;
    }
    tv.tv_sec = HANDSHAKE_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_port = htons(SAS_PORT);
    srv.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(g_sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        LOGE("connect 127.0.0.1:%d failed: %s", SAS_PORT, strerror(errno));
        return -1;
    }
    LOGOK("connected to 127.0.0.1:%d", SAS_PORT);
    return 0;
}

/* ---------------- main flow ---------------- */

int main(int argc, char **argv) {
    int a;

    setvbuf(stdout, NULL, _IONBF, 0);
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

#ifdef SPEEDUNTRAP
    start_stop_watchdog();
#endif

#ifdef RADAR_TEST
    (void)argc;
    (void)argv;
    return radar_test_run();
#endif

    for (a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--test") == 0)
            return radar_test_run();
    }

    printf("\n");
    printf("==========================================================\n");
#ifdef SPEEDUNTRAP
    printf("  speeduntrap  ConnectedDrive RADAR settings + map overlay\n");
    printf("  SAS/RHMI @ 127.0.0.1:%d   Popup / Range / Stop\n", SAS_PORT);
    printf("  overlay = map tiles + HUD  1280x480\n");
#else
    printf("  iDrive 4 ConnectedDrive RADAR\n");
    printf("  SAS/RHMI @ 127.0.0.1:%d   WalControl overlay, no HmiMain3D patch\n",
           SAS_PORT);
    printf("  GPS: PosReal.DNAVIPos fused DR (no OBD)  overlay 1280x480\n");
#endif
    printf("  checkbox: Popup on/off   Range - / Range +\n");
    printf("  ZBE: MENU hide overlay   OPTION x3 restore\n");
    printf("==========================================================\n\n");

    uint32_t msg_id = 0;
    BWriter *msg_id_val = bwriter_new(16);
    BWriter *data_val = bwriter_new(3000);
    int rc;
    int mode;
    int logged_in = 0;
    unsigned char challenge[256];
    int chal_len = -1;
    struct timeval tv;

    /* One TCP session per signing mode (sas_login is one-shot). */
    for (mode = 0; mode < (int)SIGN_MODE_COUNT; mode++) {
        unsigned char signature[256];
        size_t sig_len = sizeof(signature);

        printf("\n---------- login attempt %d/%d: %s ----------\n",
               mode + 1, (int)SIGN_MODE_COUNT, sign_mode_name((SignMode)mode));

        if (sas_connect() != 0) goto fail;

        msg_id = 0;
        v_cert = 0;
        chal_len = -1;

        LOGI("STAGE 1/7 - sas_certificate (embedded PKCS#7, %u bytes)",
             (unsigned)embedded_pkcs7_len);
        msg_id++;
        msg_id_val->size = 0; bwriter_add_int(msg_id_val, msg_id);
        data_val->size = 0;   bwriter_add_bytes(data_val, embedded_pkcs7, embedded_pkcs7_len);
        {
            DictEntry f[] = { {H_MESSAGE_ID, msg_id_val}, {H_DATA, data_val} };
            BWriter *m = build_message(H_SAS_CERTIFICATE, f, 2);
            rc = send_frame(g_sock, m);
            bwriter_free(m);
        }
        if (rc < 0) goto fail;

        {
            int tries;
            for (tries = 0; tries < MAX_WAIT_FRAMES && chal_len < 0; tries++) {
                unsigned char *rx = NULL;
                uint32_t rx_len = 0;
                int rrc = recv_frame(g_sock, &rx, &rx_len);
                if (rrc == 1) { LOGE("timeout waiting for certificate challenge"); break; }
                if (rrc != 0) { LOGE("connection lost during certificate stage"); break; }
                if (frame_has_result(rx, rx_len) && frame_is_error(rx, rx_len)) {
                    LOGE("HmiMain3D REJECTED the certificate:");
                    hexdump(rx, rx_len > 256 ? 256 : rx_len);
                    dump_frame_strings(rx, rx_len);
                    free(rx);
                    v_cert = -1;
                    break;
                }
                chal_len = extract_result_bytes(rx, rx_len, challenge, sizeof(challenge));
                if (chal_len > 0) {
                    v_cert = 1;
                    if (mode == 0) {
                        printf("\n");
                        printf("  ##############################################\n");
                        printf("  #  CERT CHECK: PASSED                        #\n");
                        printf("  #  HmiMain3D accepted our certificate and    #\n");
                        printf("  #  sent a %d-byte login challenge.           #\n", chal_len);
                        printf("  #  -> v10 cert patch is WORKING.             #\n");
                        printf("  ##############################################\n\n");
                    }
                    LOGOK("challenge received (%d bytes):", chal_len);
                    hexdump(challenge, (size_t)chal_len);
                }
                free(rx);
            }
        }
        if (v_cert != 1) {
            v_cert = -1;
            LOGE("no challenge received - certificate stage FAILED");
            print_summary();
            goto fail;
        }

        LOGI("STAGE 2/7 - %s", sign_mode_name((SignMode)mode));
        if (sign_challenge((SignMode)mode, challenge, (size_t)chal_len, signature, &sig_len) != 0) {
            LOGE("signing failed for this mode, trying next");
            continue;
        }
        LOGOK("signature computed (%u bytes):", (unsigned)sig_len);
        hexdump(signature, sig_len > 64 ? 64 : sig_len);

        LOGI("STAGE 3/7 - sas_login");
        msg_id++;
        msg_id_val->size = 0; bwriter_add_int(msg_id_val, msg_id);
        data_val->size = 0;   bwriter_add_bytes(data_val, signature, sig_len);
        {
            DictEntry f[] = { {H_MESSAGE_ID, msg_id_val}, {H_DATA, data_val} };
            BWriter *m = build_message(H_SAS_LOGIN, f, 2);
            rc = send_frame(g_sock, m);
            bwriter_free(m);
        }
        if (rc < 0) goto fail;

        rc = wait_reply(g_sock, msg_id, NULL, 1);
        if (rc == 1) {
            logged_in = 1;
            v_login = 1;
            printf("\n");
            printf("  ##############################################\n");
            printf("  #  SAS LOGIN: PASSED — v11 void ack accepted     #\n");
            printf("  #  (%s)\n", sign_mode_name((SignMode)mode));
            printf("  ##############################################\n\n");
            break;
        }
        LOGE("sas_login REJECTED for this mode");
    }

    if (!logged_in) {
        v_login = -1;
        LOGE("all %d signing modes refused (cert still passed)", (int)SIGN_MODE_COUNT);
        print_summary();
        goto fail;
    }

    /* ---- STAGE 4: rhmi_create ---- */
    LOGI("STAGE 4/7 - rhmi_create (register app metadata)");
    msg_id++;
    msg_id_val->size = 0; bwriter_add_int(msg_id_val, msg_id);
    {
        BWriter *tok_val = bwriter_new(16); bwriter_add_null(tok_val);

        BWriter *vb[3];
        vb[0] = bwriter_new(16); bwriter_add_int(vb[0], 0);
        vb[1] = bwriter_new(16); bwriter_add_int(vb[1], 1);
        vb[2] = bwriter_new(16); bwriter_add_int(vb[2], 0);
        DictEntry vf[] = { {etch_hash("major"), vb[0]}, {etch_hash("minor"), vb[1]}, {etch_hash("revision"), vb[2]} };
        BWriter *vers = custom_type("de.bmw.idrive.BMWRemoting.VersionInfo", vf, 3);

        BWriter *mb[4];
        mb[0] = bwriter_new(64); bwriter_add_string(mb[0], RADAR_APP_ID);
        mb[2] = bwriter_new(64); bwriter_add_string(mb[2], "speeduntrap");
        mb[3] = bwriter_new(64); bwriter_add_string(mb[3], "wallace");
        DictEntry mf[] = { {etch_hash("id"), mb[0]}, {etch_hash("version"), vers},
                           {etch_hash("name"), mb[2]}, {etch_hash("vendor"), mb[3]} };
        BWriter *meta = custom_type("de.bmw.idrive.BMWRemoting.RHMIMetaData", mf, 4);

        DictEntry f[] = { {H_MESSAGE_ID, msg_id_val}, {etch_hash("token"), tok_val}, {etch_hash("metaData"), meta} };
        BWriter *m = build_message(H_RHMI_CREATE, f, 3);
        rc = send_frame(g_sock, m);
        bwriter_free(m);
    }
    if (rc < 0) goto fail;

    {
        int64_t handle_val = -1;
        rc = wait_reply(g_sock, msg_id, &handle_val, 1);
        if (rc == 1 && handle_val >= 0 && handle_val != -9999) {
            g_handle = (long)handle_val;
            LOGOK("rhmi_create accepted, RHMI handle = %ld", g_handle);
        } else {
            v_rhmi = -1;
            LOGE("rhmi_create failed (rc=%d)", rc);
            print_summary();
            goto fail;
        }
    }

    /* ---- STAGE 5: rhmi_setResource x3 (DESCRIPTION / TEXTDB / IMAGEDB) ---- */
    LOGI("STAGE 5/7 - rhmi_setResource (DESCRIPTION/TEXTDB/IMAGEDB)");
    BWriter *h_val = bwriter_new(16); bwriter_add_int(h_val, g_handle);
    BWriter *tr_val = bwriter_new(16); bwriter_add_bool(tr_val, 1);

    {
        static const struct { const char *name; const unsigned char *data; size_t len; } res[3] = {
            { "DESCRIPTION", NULL, 0 }, { "TEXTDB", NULL, 0 }, { "IMAGEDB", NULL, 0 }
        };
        const unsigned char *databuf;
        size_t datalen;
        int i;
        for (i = 0; i < 3; i++) {
            if (i == 0)      { databuf = radar_xml;         datalen = radar_xml_len; }
            else if (i == 1) { databuf = radar_texts_zip;   datalen = radar_texts_zip_len; }
            else             { databuf = images_zip;        datalen = images_zip_len; }

            LOGI("uploading %s (%u bytes)...", res[i].name, (unsigned)datalen);
            msg_id++;
            msg_id_val->size = 0; bwriter_add_int(msg_id_val, msg_id);
            BWriter *res_data = bwriter_new(datalen + 16);
            bwriter_add_bytes(res_data, databuf, datalen);
            DictEntry rf[] = { {etch_hash(res[i].name), tr_val} };
            BWriter *rtype = custom_type("de.bmw.idrive.BMWRemoting.RHMIResourceType", rf, 1);
            DictEntry f[] = { {H_MESSAGE_ID, msg_id_val}, {H_HANDLE, h_val}, {H_DATA, res_data}, {H_TYPE, rtype} };
            BWriter *m = build_message(H_RHMI_SET_RESOURCE, f, 4);
            rc = send_frame(g_sock, m);
            bwriter_free(m);
            if (rc < 0) goto fail;

            rc = wait_reply(g_sock, msg_id, NULL, 0);
            if (rc != 1) {
                v_rhmi = -1;
                LOGE("setResource(%s) failed (rc=%d)", res[i].name, rc);
                print_summary();
                goto fail;
            }
            LOGOK("%s acknowledged", res[i].name);
        }
    }

    /* Handlers must be registered before initialize (same order as etch_hello_world.py). */
    g_msg_id = msg_id;
    g_msg_id_val = msg_id_val;
    g_h_val = h_val;

    LOGI("STAGE 6/8 - rhmi_addActionEventHandler (entry + popup + range)");
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (rhmi_add_action_handler(RADAR_ACTION_ENTRY) != 0) {
        v_rhmi = -1;
        print_summary();
        goto fail;
    }
    if (rhmi_add_action_handler(RADAR_ACTION_POPUP) != 0) {
        v_rhmi = -1;
        print_summary();
        goto fail;
    }
    if (rhmi_add_action_handler(RADAR_ACTION_RANGE_DN) != 0) {
        v_rhmi = -1;
        print_summary();
        goto fail;
    }
    if (rhmi_add_action_handler(RADAR_ACTION_RANGE_UP) != 0) {
        v_rhmi = -1;
        print_summary();
        goto fail;
    }

    tv.tv_sec = HANDSHAKE_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* ---- STAGE 7: rhmi_initialize ---- */
    LOGI("STAGE 7/8 - rhmi_initialize");
    g_msg_id++;
    g_msg_id_val->size = 0; bwriter_add_int(g_msg_id_val, g_msg_id);
    {
        DictEntry f[] = { {H_MESSAGE_ID, g_msg_id_val}, {H_HANDLE, g_h_val} };
        BWriter *m = build_message(H_RHMI_INITIALIZE, f, 2);
        rc = send_frame(g_sock, m);
        bwriter_free(m);
    }
    if (rc < 0) goto fail;

    rc = wait_reply(g_sock, g_msg_id, NULL, 1);
    if (rc != 1) {
        v_rhmi = -1;
        LOGE("rhmi_initialize FAILED (rc=%d) - HMI rejected the app description", rc);
        print_summary();
        goto fail;
    }
    v_rhmi = 1;
    LOGOK("rhmi_initialize SUCCESS - speeduntrap registered in ConnectedDrive");

    LOGI("STAGE 8/9 - cds_create (speedActual, optional)");
    (void)cds_start();

    LOGI("STAGE 9/9 - overlay + checkbox from persist");
    radar_init();
    if (push_settings() != 0) goto fail;
    LOGOK("checkbox + range + status pushed");

    /* Blocking recv; select() provides the 2s keepalive tick. */
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("\n");
    printf("==========================================================\n");
    printf("  REGISTRATION COMPLETE\n");
    printf("  ConnectedDrive -> speeduntrap\n");
    printf("  Check Popup to swap CID at the Range setting (50..1000 m).\n");
    printf("  Uncheck: never touch WalControl / HmiMain3D visibility.\n");
    printf("  MENU hides overlay; OPTION x3 shows it (any distance).\n");
    printf("==========================================================\n\n");
    print_summary();

    for (;;) {
        fd_set rfds;
        struct timeval sel;
        unsigned char *rx = NULL;
        uint32_t rx_len = 0;
        int rrc, aid, nsel;

        FD_ZERO(&rfds);
        FD_SET(g_sock, &rfds);
        if (radar_should_stop())
            break;
        if (radar_popup_enabled()) {
            sel.tv_sec = 0;
#ifdef SPEEDUNTRAP
            sel.tv_usec = speeduntrap_zoom_busy() ? 1000 :
                          (radar_overlay_showing() ? 8000 : 200000);
#else
            sel.tv_usec = 200000;
#endif
        } else {
            sel.tv_sec = 1;
            sel.tv_usec = 0;
        }
        nsel = select(g_sock + 1, &rfds, NULL, NULL, &sel);
        if (nsel < 0) {
            if (errno == EINTR && radar_should_stop())
                break;
            LOGE("select(): %s", strerror(errno));
            print_summary();
            break;
        }
        if (nsel == 0) {
            popup_keepalive();
            drain_pending_actions();
            continue;
        }

        rrc = recv_frame(g_sock, &rx, &rx_len);
        if (rrc != 0) {
            LOGE("server closed the connection (rc=%d) - menu entry removed", rrc);
            print_summary();
            break;
        }

        aid = event_action_id(rx, rx_len);
        if (aid == RADAR_ACTION_POPUP || aid == RADAR_ACTION_ENTRY ||
            aid == RADAR_ACTION_RANGE_DN || aid == RADAR_ACTION_RANGE_UP) {
            handle_ra_action(aid);
        } else if (handle_cds_frame(rx, rx_len)) {
            popup_keepalive();
        } else {
            int64_t type_hash = -1;
            uint32_t next;
            if (decode_int(rx, rx_len, 1, &type_hash, &next) == 0) {
                LOGI("EVENT from headunit: type hash 0x%08lx (%u bytes)",
                     (unsigned long)(uint32_t)type_hash, rx_len);
            } else {
                LOGI("EVENT from headunit (%u bytes, unparseable type)", rx_len);
            }
            hexdump(rx, rx_len > 256 ? 256 : rx_len);
            dump_frame_strings(rx, rx_len);
        }
        free(rx);
        drain_pending_actions();
    }

    radar_shutdown();
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    bwriter_free(msg_id_val);
    bwriter_free(data_val);
    bwriter_free(h_val);
    bwriter_free(tr_val);
    return 0;

fail:
    LOGE("aborting.");
    radar_shutdown();
    print_summary();
    return 1;
}
