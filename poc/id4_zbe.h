/*
 * id4_zbe.h
 *
 * NBT ID4 CCS MOST / DSI constants recovered from HmiMain3D.unzipped
 * (CPxyCentralControlSystem::registerFunctions @ 0x08B7DBFA) and
 * libimp_sys_most_base_nbt.so.11 (DSYSMOSTTypes / CDSYSMOSTServerProxy).
 *
 * Do not send clrNotification. Do not register as HMI_HU / HMI_RSE_*.
 */

#ifndef ID4_ZBE_H
#define ID4_ZBE_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * CCS FBlock — factory create pushes 0xE5, then registerFunctions:
 *
 *   push 0 / 1 / 2 / 0xC06 / 0xC10 / 0xC20 / 0xC30 / 0xC31 /
 *        0xC41 / 0xC42 / 0xC43 / 0xC44 / 0xC45
 *   call addFunction (0x8AF75C8)
 *
 * Member offsets on the FBlock object; HMI proxy is those + 0x50.
 * ------------------------------------------------------------------------- */
#define CCS_FBLOCK_ID                 0xE5u
#define CCS_INST_ID                   0x01u   /* default HU instance */

#define CCS_FKT_FKTIDS                0x000u  /* +0x06C */
#define CCS_FKT_NOTIFICATION          0x001u  /* +0x100 */
#define CCS_FKT_NOTIFICATION_CHECK    0x002u  /* +0x198 */
#define CCS_FKT_STATE                 0xC06u  /* +0x6CC CccsState */
#define CCS_FKT_ROTARY_ENCODER        0xC10u  /* +0x758 CccsRotaryEncoder */
#define CCS_FKT_PUSH_BUTTON           0xC20u  /* +0x7EC CccsPushButton */
#define CCS_FKT_ROTARY_ENCODER_CFG    0xC30u  /* +0x884 CccsRotaryEncoderConfig */
#define CCS_FKT_DISPLAY_CONFIG        0xC31u  /* +0x910 CccsDisplayConfig */
#define CCS_FKT_TOUCH_CONTROL         0xC41u  /* +0x980 CccsTouchControl */
#define CCS_FKT_GESTURE               0xC42u  /* +0x9F0 */
#define CCS_FKT_ABS_COORD             0xC43u  /* +0xA78 */
#define CCS_FKT_CHAR_RECOG            0xC44u  /* +0xB0C */
#define CCS_FKT_CHAR_RECOG2           0xC45u  /* +0xBA0 */

/*
 * Amplifier ConnectionMaster — CPxyConnectionMaster::FUN_08af6160(..., 3, 0)
 * and registerFunctions (HmiMain3D 0x08B8361C):
 *   +0x6CC Fkt 0x53 BuildHighSyncConnection(SH, connId)
 *   +0x764 Fkt 0x54 RemoveHighSyncConnection(SH, connId)
 * Speech / iSpeech uses EAudioSource 0x60 and connection ID 0x36
 * (DevConnectionMaster: "Building Highsyncconnection (ID 0x36)").
 * Payload uint16s are MOST big-endian (FUN_089b043e).
 * Do not requestRegisterDevices for 0x03 — HMI owns the FBlock.
 */
#define CM_FBLOCK_ID                  0x03u
#define CM_INST_ID                    0x00u
#define CM_FKT_BUILD_HIGHSYNC         0x053u
#define CM_FKT_REMOVE_HIGHSYNC        0x054u
#define CM_FKT_CONN_CTRL              0xC05u  /* HighSyncConnectionControl */
#define CM_FKT_CONN_TABLE             0xC03u
#define CM_CONN_SPEECH                0x0036u  /* EAudioSource 0x60 iSpeech; HMI 0x54's foreign builders */
#define CM_CONN_ANN_ALT               0x0032u  /* src 0x5F, same amp group 5 as 0x36 (ACID DAT_09879224) */
#define CM_CONN_NAV                   0x0033u  /* EAudioSource 0x56 NAVI Prompter_1; ACID group 11 */
#define CM_CONN_G5_30                 0x0030u  /* src 0x5D group 5 */
#define CM_CONN_G5_31                 0x0031u  /* src 0x5E group 5 */
#define CM_CONN_G5_3A                 0x003Au  /* src 0x5C group 5 */
#define CM_SENDER_HANDLE              0x1D40u  /* 0x53 owner SH */
#define CM_SENDER_HANDLE_CTRL         0x1D41u  /* C05 uses a different FBlock handle counter */
#define CM_CTRL_UNMUTE                1u      /* muteAudioSource: bMute^1 */

/* MOST OPType (byte at MessageType + 0x14). Parsers accept 0x0C and 0x0F. */
#define MOST_OP_SET                   0x00u
#define MOST_OP_GET                   0x01u
#define MOST_OP_SETGET                0x02u
#define MOST_OP_STARTRESULT           0x06u  /* methods; CPxy StartOp param 6 */
#define MOST_OP_ERRORACK              0x09u  /* method ErrorAck; C05 paid 1D 40 42 */
#define MOST_OP_RESULT                0x0Au
#define MOST_OP_PROCESSING            0x0Bu
#define MOST_OP_STATUS                0x0Cu
#define MOST_OP_RESULTACK             0x0Du  /* method ResultAck; 0x53 paid 1D 40 */
#define MOST_OP_ERROR                 0x0Fu

/* -------------------------------------------------------------------------
 * DSYSMOSTTypes::MessageType  (DSIReadMessageType)
 *
 *   +0x00 u32  eTransmissionType
 *                0/4 = control (isCtrlMsg) — FBlock GET/SET/STATUS
 *                1/2/3 = async (isAsyncMsg)
 *              CHBFunctionBase defaults this to 0. Type 1 never reaches CCS.
 *   +0x04 i32  TTransmissionVariant.tag   (1=TXControl, 2=Info)
 *   +0x08 i32  TTransmissionVariant.value
 *   +0x0C MessageHeaderType (16 bytes)
 *   +0x1C CHBBuffer payload
 *
 * MessageHeaderType (DSIReadMessageHeaderType, CHBMOSTClientImpl copy):
 *   +0x00 u16 destAddr
 *   +0x02 u16 sourceAddr     (copied into AMS bytes [0..1] BE)
 *   +0x04 u8  fblockId       (log: "fblock 0x%02X")
 *   +0x05 u8  instId
 *   +0x06 u16 functionId     (12-bit FktID in low 12)
 *   +0x08 u8  opType
 *   +0x09 u8  pad[3]
 *   +0x0C u32 telId
 *
 * CHBBuffer as used at MessageType+0x1C:
 *   +0x00 u8*  data
 *   +0x04 u32  size
 *   +0x08 u32  cap
 * ------------------------------------------------------------------------- */
#define MOST_TXTYPE_CTRL              0   /* control channel — CCS C10/C20 */
#define MOST_TXTYPE_TX                1   /* async; do not use for FBlock GET/SET */
#define MOST_TXVAR_TXCONTROL          1
#define MOST_TXVAR_INFO               2
#define MOST_TI_SEGMENTED_BASE        0x13   /* TI 0x13..0x15 = segmented */

typedef struct {
    uint16_t destAddr;
    uint16_t sourceAddr;
    uint8_t  fblockId;
    uint8_t  instId;
    uint16_t functionId;
    uint8_t  opType;
    uint8_t  pad[3];
    uint32_t telId;
} DsysMostMessageHeader;

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t cap;
} DsysMostBuffer;

typedef struct {
    uint32_t              txType;
    int32_t               variantTag;
    int32_t               variantVal;
    DsysMostMessageHeader hdr;
    DsysMostBuffer        payload;
} DsysMostMessage;

/* -------------------------------------------------------------------------
 * DSI interface
 *   SHBDSIImportEntry("MOSTSubSystemIntel.DSYSMOSTServer", id=9, ver 4.4)
 *   SBAttachInterface(fd, name, major=4, minor=4, &ifd)
 *   SBOpen(path)  path="/srv/servicebroker"  override env SB_PATH
 *
 * CDSYSMOSTServerProxy method IDs (createRequestEvent / toDSI switch):
 *   0x00 requestRegisterDevices(clientId, THBVector<uint16> fblocks)  — I IMPLEMENT
 *   0x01 requestRegisterDevCtrls(clientId, THBVector<uint16> fblocks) — I LISTEN
 *        MOST NAL: CONNECT ClientID to FBKey; without this, ctrl msgs are filtered
 *   0x02 requestProcessMessage(clientId, MessageType)     flags=0x20
 *   0x03 registerMOSTClient(UpdateId vector, consumer)
 *   0x04 requestUnregisterDevices
 *   0x05 requestUnregisterDevCtrls
 *   0x08 unregisterMOSTClient
 *   0x0B requestSetRegistryFilter(clientId, FBlockKey vector)
 *        copies matching FBlock traffic to this client (does not claim the FBlock)
 *   0x0F registerMOSTClientExt(UpdateId vector, consumer, CHBString name)
 *        flags=4, arg = CHBString at +0x48, returns sequence/clientId
 *   0x10 unregisterMOSTClientExt
 *
 * fromDSI notification IDs (0x80000000 | UpdateId):
 *   0x80000000 responseMOSTClient(u32 clientId, u16 devId)
 *              extra[3] is DSI sequence, not clientId. Args start after the
 *              16-byte extra. CHBMOSTClientImpl stores clientId at +0x94
 *              then requestRegisterDevices(clientId, fblock vector)
 *   0x80000001 informationProcessMessage(MessageType)
 *   0x80000003 DeviceInfo vector
 *   0x80000007 CleanUp
 *
 * Subscribe UpdateId 1 so informationProcessMessage is delivered.
 * 0xC0000000 EMOSTCommunicationState, 0xC0000001 informationRegistry
 * (vector of {u16 destAddr, u16 fblockKey=(FBlock<<8)|Inst}),
 * 0xC0000002 DeviceID (OEM sourceAddr, e.g. 0x2000). CCS E5.01 is dest 0x3002.
 * ------------------------------------------------------------------------- */
#define DSI_MOST_IFACE                "MOSTSubSystemIntel.DSYSMOSTServer"
#define DSI_MOST_MAJOR                4
#define DSI_MOST_MINOR                4
#define DSI_SB_PATH                   "/srv/servicebroker"
#define DSI_CLIENT_NAME               "ID4_AA"

#define DSI_REQ_REGISTER_DEVICES      0x00u
#define DSI_REQ_REGISTER_DEVCTRLS     0x01u
#define DSI_REQ_REGISTER_EXT_DEVICES  0x0Du
#define DSI_REQ_PROCESS_MESSAGE       0x02u
#define DSI_REQ_REGISTER_CLIENT       0x03u
#define DSI_REQ_UNREGISTER_DEVICES    0x04u
#define DSI_REQ_UNREGISTER_DEVCTRLS   0x05u
#define DSI_REQ_UNREGISTER_CLIENT     0x08u
#define DSI_REQ_SET_REGISTRY_FILTER   0x0Bu
#define DSI_REQ_REGISTER_CLIENT_EXT   0x0Fu
#define DSI_REQ_UNREGISTER_CLIENT_EXT 0x10u

#define DSI_NOTIF_BASE                0x80000000u
#define DSI_UPD_RESPONSE_MOST_CLIENT  0u
#define DSI_UPD_PROCESS_MESSAGE       1u
#define DSI_NOTIF_RESPONSE_CLIENT     (DSI_NOTIF_BASE | DSI_UPD_RESPONSE_MOST_CLIENT)
#define DSI_NOTIF_PROCESS_MESSAGE     (DSI_NOTIF_BASE | DSI_UPD_PROCESS_MESSAGE)

/* DSI::MessageHeader 40 bytes (MessageHeader::clear sets +0 = RESULT_OK 0x200).
 * Command lives at +0x18: 9=ConnectRequest, 7=DataRequest, 8=DataResponse.
 * DataRequest also sends a 16-byte RequestType block (iov[1]):
 *   +0 unused, +4 RequestType 0x100=REQUEST, +8 seq, +12 method/id. */
#define DSI_RESULT_OK                 0x200u
#define DSI_CMD_CONNECT               9u
#define DSI_CMD_DATA_REQUEST          7u
#define DSI_CMD_DATA_RESPONSE         8u
#define DSI_CMD_DISCONNECT            10u
#define DSI_REQTYPE_REQUEST           0x100u
#define DSI_REQTYPE_NOTIFY            0x101u
#define DSI_CMD_REQUEST               DSI_CMD_DATA_REQUEST
#define DSI_CMD_RESPONSE              DSI_CMD_DATA_RESPONSE
#define DSI_CMD_NOTIFICATION          DSI_REQTYPE_NOTIFY
#define DSI_EVT_REQUEST               DSI_CMD_DATA_REQUEST
#define DSI_EVT_RESPONSE              DSI_CMD_DATA_RESPONSE
#define DSI_EVT_NOTIFICATION          DSI_REQTYPE_NOTIFY
#define DSI_MSGHDR_SIZE               40
#define DSI_CONNECT_INFO_SIZE         16
#define DSI_REQINFO_SIZE              16

/* SBAttachInterface copies 32 bytes (8 dwords) here. CDSIClient then
 * HBChannelAccess::attach -> ConnectAttach(nd, pid, mchid, _NTO_SIDE_CHANNEL, 1). */
typedef struct {
    int32_t nd;
    int32_t pid;
    int32_t mchid;
    int32_t schid;
    int32_t extra[4];
} SConnectionInfo;

/* -------------------------------------------------------------------------
 * Live ID4 CCS STATUS (id4_zbe_sniff.log, dest 0x2021 src 0x3002, 7 bytes).
 * Same firmware layout as ID6 CHmiCCSController (C10/C20, inst 0x01):
 *
 * C10 RotaryEncoder:
 *   [0] sender 0x01  [1] receiver 0x1D  [2] message counter (+1/msg, 0xFF drop)
 *   [3..4] BE16 absolute detent (higher = clockwise, wraps)
 *   [5] cluster 0x1E  [6] overflow (non-zero: re-base, no delta)
 *
 * C20 PushButton, one key per message:
 *   [0] sender  [1] receiver  [2] counter (0xFF drop)
 *   [3] status 0 released / 1 pressed / 2 held / 3 no-change
 *   [4] tilt 1 up 2 right 4 down 8 left (0 on release)
 *   [5] cluster 0x00 hard / 0x1D tilt / 0x1E knob (sensor 1 = centre push)
 *   [6] sensor: hard-key bit n = slot n
 *       slots: 0 MENU  1 BACK  2 OPTIONS  3 COM  4 MEDIA  5 NAV  6 MAP
 * ------------------------------------------------------------------------- */
#define ZBE_BTN_RELEASE               0
#define ZBE_BTN_PRESS                 1
#define ZBE_BTN_HOLD                  2

#define CCS_SLOTS                     7

#define AA_KEYCODE_HOME               3u
#define AA_KEYCODE_BACK               4u
#define AA_KEYCODE_CALL               5u
#define AA_KEYCODE_DPAD_UP            19u
#define AA_KEYCODE_DPAD_DOWN          20u
#define AA_KEYCODE_DPAD_LEFT          21u
#define AA_KEYCODE_DPAD_RIGHT         22u
#define AA_KEYCODE_DPAD_CENTER        23u
#define AA_KEYCODE_SEARCH             84u
#define AA_KEYCODE_ROTARY             65536u
#define AA_KEYCODE_MEDIA              65537u
#define AA_KEYCODE_NAV                65538u
#define AA_KEYCODE_TEL                65540u

typedef struct {
    int kind;            /* 1 = key (keycode, down), 2 = relative (keycode, delta) */
    uint32_t keycode;
    int down;
    int delta;
    const char *what;
} Id4CcsEvent;

typedef struct {
    int last_pos;
    uint32_t tilt;
    int push;
    int slot[CCS_SLOTS];
} Id4CcsDec;

static void id4_ccs_init(Id4CcsDec *d)
{
    int i;

    d->last_pos = -1;
    d->tilt = 0;
    d->push = 0;
    for (i = 0; i < CCS_SLOTS; i++)
        d->slot[i] = 0;
}

static const char *id4_ccs_slot_name(int s)
{
    static const char *const names[CCS_SLOTS] = {
        "MENU", "BACK", "OPTIONS", "COM", "MEDIA", "NAV", "MAP"
    };

    return (s >= 0 && s < CCS_SLOTS) ? names[s] : "?";
}

static uint32_t id4_ccs_slot_key(int s)
{
    static const uint32_t keys[CCS_SLOTS] = {
        AA_KEYCODE_HOME, AA_KEYCODE_BACK, 0, AA_KEYCODE_CALL,
        AA_KEYCODE_MEDIA, AA_KEYCODE_NAV, AA_KEYCODE_NAV
    };

    return (s >= 0 && s < CCS_SLOTS) ? keys[s] : 0;
}

static uint32_t id4_ccs_tilt_key(unsigned dir)
{
    switch (dir) {
    case 1: return AA_KEYCODE_DPAD_UP;
    case 2: return AA_KEYCODE_DPAD_RIGHT;
    case 4: return AA_KEYCODE_DPAD_DOWN;
    case 8: return AA_KEYCODE_DPAD_LEFT;
    default: return 0;
    }
}

static const char *id4_ccs_tilt_name(uint32_t key)
{
    switch (key) {
    case AA_KEYCODE_DPAD_UP: return "TILT_UP";
    case AA_KEYCODE_DPAD_DOWN: return "TILT_DOWN";
    case AA_KEYCODE_DPAD_LEFT: return "TILT_LEFT";
    case AA_KEYCODE_DPAD_RIGHT: return "TILT_RIGHT";
    default: return "TILT_?";
    }
}

static int id4_ccs_add(Id4CcsEvent *out, int max, int k, int kind, uint32_t key,
                       int down, int delta, const char *what)
{
    if (!out || k >= max)
        return k;
    out[k].kind = kind;
    out[k].keycode = key;
    out[k].down = down;
    out[k].delta = delta;
    out[k].what = what;
    return k + 1;
}

/* STATUS only, FBlock 0xE5 inst 0x01, C10/C20 7-byte firmware layout. */
static int id4_ccs_feed(Id4CcsDec *d, uint8_t fb, uint8_t inst, uint16_t fkt,
                        uint8_t op, const uint8_t *p, uint32_t n,
                        Id4CcsEvent *out, int max)
{
    int k = 0;

    if (!d || fb != CCS_FBLOCK_ID || inst != CCS_INST_ID || op != MOST_OP_STATUS || !p)
        return 0;
    if (fkt == CCS_FKT_ROTARY_ENCODER) {
        int pos;
        uint8_t clus, ovf;
        int delta;

        if (n < 7)
            return 0;
        pos = (p[3] << 8) | p[4];
        clus = p[5];
        ovf = p[6];
        if (p[2] == 0xFF || clus != 0x1E) {
            d->last_pos = -1;
            return 0;
        }
        if (ovf != 0 || d->last_pos < 0) {
            d->last_pos = pos;
            return 0;
        }
        delta = (int)(int16_t)(uint16_t)(pos - d->last_pos);
        d->last_pos = pos;
        if (delta != 0)
            k = id4_ccs_add(out, max, k, 2, AA_KEYCODE_ROTARY, 0, delta, "ROTARY");
        return k;
    }
    if (fkt == CCS_FKT_PUSH_BUTTON && n >= 7) {
        uint8_t st, dir, clus, sens;
        int down;
        int b;

        if (p[2] == 0xFF)
            return 0;
        st = p[3];
        dir = p[4];
        clus = p[5];
        sens = p[6];
        if (st > 2)
            return 0;
        down = st != 0;
        if (clus == 0x1E) {
            if (sens == 1 && down != d->push) {
                d->push = down;
                k = id4_ccs_add(out, max, k, 1, AA_KEYCODE_DPAD_CENTER, down, 0, "PUSH");
            }
        } else if (clus == 0x1D) {
            uint32_t tk = (down && dir) ? id4_ccs_tilt_key(dir) : 0;

            if (tk != d->tilt) {
                if (d->tilt)
                    k = id4_ccs_add(out, max, k, 1, d->tilt, 0, 0,
                                    id4_ccs_tilt_name(d->tilt));
                if (tk)
                    k = id4_ccs_add(out, max, k, 1, tk, 1, 0, id4_ccs_tilt_name(tk));
                d->tilt = tk;
            }
        } else if (clus == 0x00) {
            for (b = 0; b < CCS_SLOTS; b++) {
                if (sens != (uint8_t)(1u << b) || down == d->slot[b])
                    continue;
                d->slot[b] = down;
                k = id4_ccs_add(out, max, k, 1, id4_ccs_slot_key(b), down, 0,
                                id4_ccs_slot_name(b));
            }
        }
        return k;
    }
    return 0;
}

#endif /* ID4_ZBE_H */
