/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Binary protocol between the radio and a computer: framing, session and
 * dispatch.  Portable C, no platform dependency; the transport that gives it a
 * UART lives in platform/targets/c62/pc_proto_c62.c and the handlers that
 * touch the stores in pc_proto_ops.c.
 *
 * The frame and the rules are in core/pc_proto.h and docs/PC_PROTO.md.
 *
 * One thread answers every frame, so anything that can block in it can silence
 * the radio completely: no reply, no log line, no fault.  The rule the whole
 * file is written to is therefore that no handler waits for a lock the rest of
 * the firmware owns, and that the status snapshot takes none at all.  Where a
 * wait is unavoidable (a write that has to see a consistent state.channel) the
 * platform uses state_tryLock() and answers PC_ERR_BUSY rather than parking.
 * The transport counts its own iterations and the supervisor reports a stall
 * instead of leaving the operator with a cable that does nothing.
 */

#include <string.h>

#include "core/pc_proto.h"
#include "pc_proto_internal.h"
#include "core/crc.h"
#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/settings.h"

/* Bytes of header before the payload and of CRC after it, see the frame
 * layout in pc_proto.h. */
#define BODY_HDR 8
#define BODY_TRAILER 2

/*
 * The frame CRC: CRC-16/CCITT-FALSE, polynomial 0x1021 with the register
 * starting at 0xFFFF.
 *
 * core/crc.h's crc_ccitt() is the same polynomial but starts at zero (what the
 * repository calls crc_ccitt is CRC-16/XMODEM numerically), so the seed is
 * passed explicitly here instead of relying on the name.  CCITT-FALSE is the
 * one the baseband dump and its reader already use, so a client carries one
 * CRC routine for the whole cable.
 */
static uint16_t pcCrc(const void *data, size_t len)
{
    return crc_ccitt_update(0xFFFF, data, len);
}

/*
 * ----------------------------------------------------------------------------
 * TLV
 * ----------------------------------------------------------------------------
 */

static size_t tlvHead(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                      uint16_t len)
{
    if (off == 0 && buf == NULL)
        return 0;
    if (off + 4 + len > cap)
        return 0;
    buf[off++] = (uint8_t)(tag & 0xFF);
    buf[off++] = (uint8_t)(tag >> 8);
    buf[off++] = (uint8_t)(len & 0xFF);
    buf[off++] = (uint8_t)(len >> 8);
    return off;
}

size_t pc_tlv_u8(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                 uint8_t value)
{
    off = tlvHead(buf, cap, off, tag, 1);
    if (off == 0)
        return 0;
    buf[off++] = value;
    return off;
}

size_t pc_tlv_i8(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                 int8_t value)
{
    return pc_tlv_u8(buf, cap, off, tag, (uint8_t)value);
}

size_t pc_tlv_u16(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                  uint16_t value)
{
    off = tlvHead(buf, cap, off, tag, 2);
    if (off == 0)
        return 0;
    buf[off++] = (uint8_t)(value & 0xFF);
    buf[off++] = (uint8_t)(value >> 8);
    return off;
}

size_t pc_tlv_i16(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                  int16_t value)
{
    return pc_tlv_u16(buf, cap, off, tag, (uint16_t)value);
}

size_t pc_tlv_u32(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                  uint32_t value)
{
    off = tlvHead(buf, cap, off, tag, 4);
    if (off == 0)
        return 0;
    buf[off++] = (uint8_t)(value & 0xFF);
    buf[off++] = (uint8_t)((value >> 8) & 0xFF);
    buf[off++] = (uint8_t)((value >> 16) & 0xFF);
    buf[off++] = (uint8_t)(value >> 24);
    return off;
}

size_t pc_tlv_str(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                  const char *str)
{
    size_t len = (str == NULL) ? 0 : strlen(str);
    off = tlvHead(buf, cap, off, tag, (uint16_t)len);
    if (off == 0)
        return 0;
    if (len > 0)
        memcpy(buf + off, str, len);
    return off + len;
}

size_t pc_tlv_bytes(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                    const void *data, size_t len)
{
    off = tlvHead(buf, cap, off, tag, (uint16_t)len);
    if (off == 0)
        return 0;
    if (len > 0)
        memcpy(buf + off, data, len);
    return off + len;
}

size_t pc_tlv_open(uint8_t *buf, size_t cap, size_t off, uint16_t tag)
{
    return tlvHead(buf, cap, off, tag, 0);
}

size_t pc_tlv_close(uint8_t *buf, size_t off, size_t start)
{
    if (buf == NULL || start < 4 || off < start)
        return 0;
    size_t len = off - start;
    if (len > 0xFFFF)
        return 0;
    buf[start - 2] = (uint8_t)(len & 0xFF);
    buf[start - 1] = (uint8_t)(len >> 8);
    return off;
}

bool pc_tlv_find(const uint8_t *tlv, size_t len, uint16_t tag,
                 const uint8_t **value, uint16_t *valueLen)
{
    size_t off = 0;

    while (off + 4 <= len) {
        uint16_t t = (uint16_t)(tlv[off] | (tlv[off + 1] << 8));
        uint16_t l = (uint16_t)(tlv[off + 2] | (tlv[off + 3] << 8));
        off += 4;
        if (off + l > len)
            return false; /* truncated TLV: the whole payload is suspect */
        if (t == tag) {
            if (value != NULL)
                *value = tlv + off;
            if (valueLen != NULL)
                *valueLen = l;
            return true;
        }
        off += l;
    }
    return false;
}

bool pc_tlv_get_u8(const uint8_t *tlv, size_t len, uint16_t tag, uint8_t *out)
{
    const uint8_t *v;
    uint16_t l;

    if (!pc_tlv_find(tlv, len, tag, &v, &l) || l != 1)
        return false;
    *out = v[0];
    return true;
}

bool pc_tlv_get_i8(const uint8_t *tlv, size_t len, uint16_t tag, int8_t *out)
{
    uint8_t v;

    if (!pc_tlv_get_u8(tlv, len, tag, &v))
        return false;
    *out = (int8_t)v;
    return true;
}

bool pc_tlv_get_u16(const uint8_t *tlv, size_t len, uint16_t tag,
                    uint16_t *out)
{
    const uint8_t *v;
    uint16_t l;

    if (!pc_tlv_find(tlv, len, tag, &v, &l) || l != 2)
        return false;
    *out = (uint16_t)(v[0] | (v[1] << 8));
    return true;
}

bool pc_tlv_get_i16(const uint8_t *tlv, size_t len, uint16_t tag, int16_t *out)
{
    uint16_t v;

    if (!pc_tlv_get_u16(tlv, len, tag, &v))
        return false;
    *out = (int16_t)v;
    return true;
}

bool pc_tlv_get_u32(const uint8_t *tlv, size_t len, uint16_t tag,
                    uint32_t *out)
{
    const uint8_t *v;
    uint16_t l;

    if (!pc_tlv_find(tlv, len, tag, &v, &l) || l != 4)
        return false;
    *out = (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) |
           ((uint32_t)v[3] << 24);
    return true;
}

/*
 * ----------------------------------------------------------------------------
 * Framing
 * ----------------------------------------------------------------------------
 */

struct slipWriter
{
    uint8_t *out;
    size_t   cap;
    size_t   n;
    bool     ok;
    uint16_t crc;
};

static void slipEsc(struct slipWriter *w, uint8_t b)
{
    if (!w->ok)
        return;
    if (w->n + 2 > w->cap) {
        w->ok = false;
        return;
    }
    w->out[w->n++] = PC_PROTO_ESC;
    w->out[w->n++] = (b == PC_PROTO_END) ? PC_PROTO_ESC_END : PC_PROTO_ESC_ESC;
}

/* A byte of the body: escaped on the wire, folded into the CRC. */
static void slipBody(struct slipWriter *w, uint8_t b)
{
    if (!w->ok)
        return;
    w->crc = crc_ccitt_update(w->crc, &b, 1);
    if (b == PC_PROTO_END || b == PC_PROTO_ESC)
        slipEsc(w, b);
    else if (w->n + 1 > w->cap)
        w->ok = false;
    else
        w->out[w->n++] = b;
}

/* Raw byte: only the delimiters that open and close the frame. */
static void slipPlain(struct slipWriter *w, uint8_t b)
{
    if (!w->ok)
        return;
    if (w->n + 1 > w->cap)
        w->ok = false;
    else
        w->out[w->n++] = b;
}

/*
 * A byte of the CRC: escaped like the rest of the body, but not folded into
 * the CRC.  Escaping it is not optional: the CRC can be 0xC0 or 0xDB, and an
 * unescaped one is read back as a delimiter or as the start of an escape pair,
 * which costs the last byte of the frame.  (That bug turned every frame whose
 * CRC began with 0xDB into garbage.)
 */
static void slipCrc(struct slipWriter *w, uint8_t b)
{
    if (b == PC_PROTO_END || b == PC_PROTO_ESC)
        slipEsc(w, b);
    else
        slipPlain(w, b);
}

size_t pc_proto_encode(uint8_t type, uint8_t seq, uint16_t id, uint8_t flags,
                       const uint8_t *payload, size_t len, uint8_t *out,
                       size_t cap)
{
    if (out == NULL || cap < 4 || len > PC_PROTO_MAX_PAYLOAD)
        return 0;

    struct slipWriter w = { .out = out, .cap = cap, .n = 0, .ok = true,
                            .crc = 0xFFFF };

    slipPlain(&w, PC_PROTO_END);
    slipBody(&w, PC_PROTO_FRAME_VERSION);
    slipBody(&w, type);
    slipBody(&w, seq);
    slipBody(&w, (uint8_t)(id & 0xFF));
    slipBody(&w, (uint8_t)(id >> 8));
    slipBody(&w, flags);
    slipBody(&w, (uint8_t)(len & 0xFF));
    slipBody(&w, (uint8_t)(len >> 8));
    for (size_t i = 0; i < len; i++)
        slipBody(&w, payload[i]);
    slipCrc(&w, (uint8_t)(w.crc & 0xFF));
    slipCrc(&w, (uint8_t)(w.crc >> 8));
    slipPlain(&w, PC_PROTO_END);

    return w.ok ? w.n : 0;
}

/*
 * ----------------------------------------------------------------------------
 * Transmit
 * ----------------------------------------------------------------------------
 */

static bool sendFrame(pcProto_t *p, uint8_t type, uint8_t seq, uint16_t id,
                      uint8_t flags, const uint8_t *payload, size_t len)
{
    if (p->port == NULL || p->port->write == NULL)
        return false;

    size_t n = pc_proto_encode(type, seq, id, flags, payload, len, p->txBody,
                               sizeof(p->txBody));
    if (n == 0) {
        p->stats.txDropped++;
        return false;
    }
    if (p->port->write(p->port->ctx, p->txBody, n) < 0) {
        p->stats.txDropped++;
        return false;
    }
    p->stats.txFrames++;
    return true;
}

void pc_proto_sendResponse(pcProto_t *p, uint8_t seq, uint16_t id,
                           uint8_t flags, const uint8_t *payload, size_t len)
{
    sendFrame(p, PC_TYPE_RESPONSE, seq, id, flags, payload, len);
}

void pc_proto_sendError(pcProto_t *p, uint8_t seq, uint16_t id, int err)
{
    uint8_t buf[16];
    size_t off = pc_tlv_u8(buf, sizeof(buf), 0, PC_TAG_ERROR,
                           (uint8_t)(err < 0 ? -err : err));
    if (off == 0)
        return;
    off = pc_tlv_u16(buf, sizeof(buf), off, PC_TAG_MSG_ID, id);
    if (off == 0)
        return;
    p->stats.lastError = (uint16_t)(err < 0 ? -err : err);
    p->stats.lastErrorMsg = id;
    pc_proto_sendResponse(p, seq, PC_MSG_ERROR, 0, buf, off);
}

void pc_proto_sendEvent(pcProto_t *p, uint16_t id, const uint8_t *payload,
                        size_t len)
{
    if (!p->linked || !p->events)
        return;
    if (sendFrame(p, PC_TYPE_EVENT, 0, id, 0, payload, len))
        p->stats.events++;
}

/*
 * ----------------------------------------------------------------------------
 * Platform hooks
 * ----------------------------------------------------------------------------
 */

__attribute__((weak)) int pc_proto_platformStatus(pcProtoStatus_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->channelIdx = 0xFFFF;
    }
    return -1;
}

/**
 * Platform work that has to happen after the answer is on the wire.
 *
 * The bench PTT is the case: the handler must not key the PA itself, because
 * the keying is what puts the RTX thread to work, and a radio that transmits
 * (or, worse, one whose transmit path stalls) must not be able to swallow the
 * answer to the frame that asked for it.  The handler arms the press, the
 * frame is written, and this hook -- called from the transport loop, after the
 * dispatch -- applies it.
 */
__attribute__((weak)) void pc_proto_platformTick(uint32_t nowMs)
{
    (void)nowMs;
}

__attribute__((weak)) const char *pc_proto_platformModel(void)
{
    return "OpenRTX";
}

__attribute__((weak)) const char *pc_proto_platformFirmware(void)
{
    return "unknown";
}

__attribute__((weak)) const char *pc_proto_platformHardware(void)
{
    return "unknown";
}

int pc_proto_status(pcProtoStatus_t *out)
{
    memset(out, 0, sizeof(*out));
    out->channelIdx = 0xFFFF;
    return pc_proto_platformStatus(out);
}

size_t pc_proto_statusTlv(const pcProtoStatus_t *s, uint8_t *buf, size_t cap,
                          size_t off)
{
    off = pc_tlv_u8(buf, cap, off, PC_TAG_TX_STATE, s->txState);
    off = pc_tlv_u32(buf, cap, off, PC_TAG_RX_FREQ, s->rxFreq);
    off = pc_tlv_u32(buf, cap, off, PC_TAG_TX_FREQ, s->txFreq);
    off = pc_tlv_u8(buf, cap, off, PC_TAG_MODE, s->mode);
    off = pc_tlv_i16(buf, cap, off, PC_TAG_RSSI, s->rssi);
    off = pc_tlv_u16(buf, cap, off, PC_TAG_CHANNEL_IDX, s->channelIdx);
    off = pc_tlv_u16(buf, cap, off, PC_TAG_BATTERY_MV, s->batteryMv);
    off = pc_tlv_u8(buf, cap, off, PC_TAG_SQUELCH, s->squelchOpen);
    off = pc_tlv_str(buf, cap, off, PC_TAG_CHANNEL_NAME, s->channelName);
    /*
     * The power the radio would key with, and why it would not.  Both travel
     * with every status, so a PTT that does nothing is diagnosable from the
     * PC without opening the radio (docs/TX_SAFETY.md).
     */
    off = pc_tlv_u16(buf, cap, off, PC_TAG_TX_POWER, s->txPower);
    off = pc_tlv_str(buf, cap, off, PC_TAG_TX_REASON,
                     (s->txReason[0] != '\0') ? s->txReason : "none");
    /*
     * The accessory port travels with the status too: with the speaker-mic PTT
     * sharing the pad with the serial line, "my requests stop being answered
     * while I key" is explained here and nowhere else (docs/ACCESSORY.md).
     */
    off = pc_tlv_u8(buf, cap, off, PC_TAG_PORT_MODE, s->portMode);
    off = pc_tlv_u32(buf, cap, off, PC_TAG_PORT_LOST, s->portLost);
    return off;
}

/*
 * ----------------------------------------------------------------------------
 * Capabilities
 * ----------------------------------------------------------------------------
 */

uint32_t pc_proto_caps(void)
{
    uint32_t caps = PC_CAP_CHANNELS | PC_CAP_CONTACTS | PC_CAP_MESSAGES |
                    PC_CAP_SETTINGS | PC_CAP_ZONES | PC_CAP_EVENTS |
                    PC_CAP_CHAN_BLOCK | PC_CAP_MSG_SEND |
                    PC_CAP_SETTINGS_FIELDS | PC_CAP_ZONE_IMPORT;

#ifdef CONFIG_C62_MODE_STATS
    caps |= PC_CAP_STATS;
#endif
#ifdef CONFIG_C62_PACKET_MODEM
    caps |= PC_CAP_PACKET_MODEM;
#endif
#ifdef CONFIG_C62_ANALYZER
    caps |= PC_CAP_ANALYZER;
#endif
#ifdef CONFIG_C62_DEBUG_PROTO
    /* Off in a production build: no log streaming, no register or memory
     * access, not even the advertisement that they exist. */
    caps |= PC_CAP_DEBUG;
#endif
#ifdef CONFIG_C62_BENCH_PROTO
    /* The bench namespace moves the radio and keys the PA, so it is never in
     * a production build either.  It shares the session token with the debug
     * namespace: a build with either of them hands one out. */
    caps |= PC_CAP_BENCH;
#endif
    return caps;
}

#if defined(CONFIG_C62_DEBUG_PROTO) || defined(CONFIG_C62_BENCH_PROTO)
/** Rotate the session token.  Not a secret: see the note in pc_proto.h. */
static void tokenRotate(pcProto_t *p)
{
    /* No RNG on this radio: uptime, the address of the instance and a
     * counter.  The token tells a stale client from the one that did HELLO;
     * it is not a defence against someone holding the cable, and the
     * documentation says so. */
    p->dbgNonce += 0x9E3779B9u ^ (uint32_t)(uintptr_t)p;
    p->dbgNonce ^= p->nowMs + 0x85EBCA6Bu;
    p->dbgNonce *= 0xC2B2AE35u;
    p->dbgToken = p->dbgNonce;
    if (p->dbgToken == 0)
        p->dbgToken = 1;
}
#endif

bool pc_proto_tokenOk(const pcProto_t *p, const uint8_t *tlv, size_t len)
{
    uint32_t token = 0;
    if (p->dbgToken == 0)
        return false;
    if (!pc_tlv_get_u32(tlv, len, PC_TAG_TOKEN, &token))
        return false;
    return token == p->dbgToken;
}

/*
 * ----------------------------------------------------------------------------
 * Core handlers
 * ----------------------------------------------------------------------------
 */

int pc_core_ping(struct pcReq *r)
{
    size_t off = pc_tlv_u32(r->out, r->outCap, 0, PC_TAG_UPTIME_MS,
                            r->p->nowMs);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_STATUS, PC_OK);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_core_sessionClose(struct pcReq *r)
{
    uint8_t buf[4];
    size_t off = pc_tlv_u8(buf, sizeof(buf), 0, PC_TAG_STATUS, PC_OK);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    /* Answer first, then drop the session: the transport sees linked == false
     * and gives the UART back to the log.  A positive return tells the
     * dispatcher the response has already been sent. */
    pc_proto_sendResponse(r->p, r->seq, r->id, 0, buf, off);
    r->p->linked = false;
    r->p->events = false;
    r->p->subPeriodMs = 0;
    return 1;
}

int pc_core_counters(struct pcReq *r)
{
    size_t off = pc_tlv_bytes(r->out, r->outCap, 0, PC_TAG_STATS,
                              &r->p->stats, sizeof(pcProtoStats_t));
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

static int pc_core_hello(struct pcReq *r)
{
    uint16_t version = 0;
    uint32_t caps = 0;
    const char *m = pc_proto_platformModel();
    const char *fw = pc_proto_platformFirmware();
    const char *hw = pc_proto_platformHardware();
    int lastUsed = chstore_lastUsed();

    if (!pc_tlv_get_u16(r->in, r->inLen, PC_TAG_VERSION, &version)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    pc_tlv_get_u32(r->in, r->inLen, PC_TAG_CAPS, &caps);

    bool ok = version >= PC_PROTO_VERSION_MIN;
    r->p->clientVersion = version;
    r->p->clientCaps = caps;
    r->p->linked = ok;
    r->p->events = ok && ((caps & PC_CAP_EVENTS) != 0);
    r->p->subPeriodMs = 0;

    /* A version below the minimum is not a failure of the frame: the client
     * still gets the identity and the numbers, so it can say why it will not
     * go on instead of showing a timeout.  Degrading means answering, not
     * closing. */
    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_STATUS,
                           (uint8_t)(ok ? PC_OK : PC_ERR_VERSION));
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_VERSION,
                     PC_PROTO_VERSION);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_VERSION_MIN,
                     PC_PROTO_VERSION_MIN);
    off = pc_tlv_str(r->out, r->outCap, off, PC_TAG_MODEL,
                     m != NULL ? m : "OpenRTX");
    off = pc_tlv_str(r->out, r->outCap, off, PC_TAG_FIRMWARE, fw ? fw : "");
    off = pc_tlv_str(r->out, r->outCap, off, PC_TAG_HARDWARE, hw ? hw : "");
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_CAPS, pc_proto_caps());
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_CHCOUNT,
                     (uint16_t)chstore_count());
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_CTCOUNT,
                     (uint16_t)cstore_count());
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_SETTINGS_VER,
                     SETTINGS_VERSION);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_SETTINGS_SIZE,
                     (uint16_t)sizeof(settings_t));
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_CHSTORE_VER,
                     CHSTORE_VERSION);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_LASTUSED,
                     (uint16_t)(lastUsed < 0 ? 0xFFFF : lastUsed));
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_UPTIME_MS, r->p->nowMs);
#if defined(CONFIG_C62_DEBUG_PROTO) || defined(CONFIG_C62_BENCH_PROTO)
    /* Every HELLO rotates the token, so a second client on the cable
     * invalidates the first one's write permission instead of sharing it. */
    tokenRotate(r->p);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_TOKEN, r->p->dbgToken);
#endif
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }

    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * Dispatch
 * ----------------------------------------------------------------------------
 */

static const struct pcProtoEntry pc_table[] =
{
    { PC_MSG_HELLO,           0,                       pc_core_hello },
    { PC_MSG_PING,            0,                       pc_core_ping },
    { PC_MSG_SESSION_CLOSE,   0,                       pc_core_sessionClose },
    { PC_MSG_COUNTERS,        0,                       pc_core_counters },

    { PC_MSG_CHAN_COUNT,      PC_CAP_CHANNELS,         pc_ops_chanCount },
    { PC_MSG_CHAN_GET,        PC_CAP_CHANNELS,         pc_ops_chanGet },
    { PC_MSG_CHAN_SET,        PC_CAP_CHANNELS,         pc_ops_chanSet },
    { PC_MSG_CHAN_ADD,        PC_CAP_CHANNELS,         pc_ops_chanAdd },
    { PC_MSG_CHAN_DELETE,     PC_CAP_CHANNELS,         pc_ops_chanDelete },
    { PC_MSG_CHAN_LIST,       PC_CAP_CHANNELS,         pc_ops_chanList },
    { PC_MSG_CHAN_WRITE_BLOCK,PC_CAP_CHAN_BLOCK,       pc_ops_chanWriteBlock },
    { PC_MSG_CHAN_LASTUSED,   PC_CAP_CHANNELS,         pc_ops_chanLastUsed },

    { PC_MSG_CONTACT_COUNT,   PC_CAP_CONTACTS,         pc_ops_contactCount },
    { PC_MSG_CONTACT_GET,     PC_CAP_CONTACTS,         pc_ops_contactGet },
    { PC_MSG_CONTACT_SET,     PC_CAP_CONTACTS,         pc_ops_contactSet },
    { PC_MSG_CONTACT_ADD,     PC_CAP_CONTACTS,         pc_ops_contactAdd },
    { PC_MSG_CONTACT_DELETE,  PC_CAP_CONTACTS,         pc_ops_contactDelete },
    { PC_MSG_CONTACT_LIST,    PC_CAP_CONTACTS,         pc_ops_contactList },

    { PC_MSG_ZONE_COUNT,      PC_CAP_ZONES,            pc_ops_zoneCount },
    { PC_MSG_ZONE_GET,        PC_CAP_ZONES,            pc_ops_zoneGet },
    { PC_MSG_ZONE_CHANNEL,    PC_CAP_ZONES,            pc_ops_zoneChannel },
    { PC_MSG_ZONE_IMPORT,     PC_CAP_ZONE_IMPORT,      pc_ops_zoneImport },

    { PC_MSG_SETTINGS_GET,    PC_CAP_SETTINGS,         pc_ops_settingsGet },
    { PC_MSG_SETTINGS_SET,    PC_CAP_SETTINGS,         pc_ops_settingsSet },
    { PC_MSG_SETTINGS_INFO,   PC_CAP_SETTINGS,         pc_ops_settingsInfo },

    { PC_MSG_STATUS_GET,      0,                       pc_ops_statusGet },
    { PC_MSG_STATUS_SUBSCRIBE,PC_CAP_EVENTS,           pc_ops_statusSubscribe },

    { PC_MSG_STATS_GET,       PC_CAP_STATS,            pc_ops_statsGet },

    { PC_MSG_MSG_COUNT,       PC_CAP_MESSAGES,         pc_ops_msgCount },
    { PC_MSG_MSG_INBOX,       PC_CAP_MESSAGES,         pc_ops_msgInbox },
    { PC_MSG_MSG_OUTBOX,      PC_CAP_MESSAGES,         pc_ops_msgOutbox },
    { PC_MSG_MSG_SEND,        PC_CAP_MSG_SEND,         pc_ops_msgSend },
    { PC_MSG_MSG_DELETE,      PC_CAP_MESSAGES,         pc_ops_msgDelete },
    { PC_MSG_MSG_MARK_READ,   PC_CAP_MESSAGES,         pc_ops_msgMarkRead },

    /* Reserved blocks: the row exists, so a client gets NOT_SUPPORTED and
     * not UNKNOWN_MSG while the capability bit stays clear. */
    { PC_MSG_PM_TX,           PC_CAP_PACKET_MODEM,     pc_ops_reserved },
    { PC_MSG_PM_STATUS,       PC_CAP_PACKET_MODEM,     pc_ops_reserved },
    { PC_MSG_ANA_SPECTRUM,    PC_CAP_ANALYZER,         pc_ops_reserved },
    { PC_MSG_ANA_STATS,       PC_CAP_ANALYZER,         pc_ops_reserved },

    /* Debug namespace: present in the table always, served only when the
     * build has CONFIG_C62_DEBUG_PROTO; otherwise the capability check above
     * answers NOT_ALLOWED. */
    { PC_MSG_DBG_INFO,        PC_CAP_DEBUG,            pc_dbg_info },
    { PC_MSG_DBG_LOG_SUB,     PC_CAP_DEBUG,            pc_dbg_logSub },
    { PC_MSG_DBG_LOG_DRAIN,   PC_CAP_DEBUG,            pc_dbg_logDrain },
    { PC_MSG_DBG_REG_READ,    PC_CAP_DEBUG,            pc_dbg_regRead },
    { PC_MSG_DBG_REG_WRITE,   PC_CAP_DEBUG,            pc_dbg_regWrite },
    { PC_MSG_DBG_AGC,         PC_CAP_DEBUG,            pc_dbg_agc },
    { PC_MSG_DBG_MEM_READ,    PC_CAP_DEBUG,            pc_dbg_memRead },
    { PC_MSG_DBG_CAP_START,   PC_CAP_DEBUG,            pc_dbg_capStart },
    { PC_MSG_DBG_CAP_STOP,    PC_CAP_DEBUG,            pc_dbg_capStop },
    { PC_MSG_DBG_CAP_READ,    PC_CAP_DEBUG,            pc_dbg_capRead },
    { PC_MSG_DBG_BB,          PC_CAP_DEBUG,            pc_dbg_bb },
    { PC_MSG_DBG_TX_TRACE,    PC_CAP_DEBUG,            pc_dbg_txTrace },
    { PC_MSG_DBG_RX_COUNTERS, PC_CAP_DEBUG,            pc_dbg_rxCounters },

    /* Bench namespace: same rule, refused on PC_CAP_BENCH when the build has
     * no CONFIG_C62_BENCH_PROTO. */
    { PC_MSG_BENCH_ARM,       PC_CAP_BENCH,            pc_bench_arm },
    { PC_MSG_BENCH_TUNE,      PC_CAP_BENCH,            pc_bench_tune },
    { PC_MSG_BENCH_PTT,       PC_CAP_BENCH,            pc_bench_ptt },
    { PC_MSG_BENCH_CAPTURE,   PC_CAP_BENCH,            pc_bench_capture },
    { PC_MSG_BENCH_RSSI,      PC_CAP_BENCH,            pc_bench_rssi },
    { PC_MSG_BENCH_SWEEP,     PC_CAP_BENCH,            pc_bench_sweep },
    { PC_MSG_BENCH_REPORT,    PC_CAP_BENCH,            pc_bench_report },
};

static const struct pcProtoEntry *pc_lookup(uint16_t id)
{
    for (size_t i = 0; i < sizeof(pc_table) / sizeof(pc_table[0]); i++) {
        if (pc_table[i].id == id)
            return &pc_table[i];
    }
    return NULL;
}

static void pc_dispatch(pcProto_t *p, const uint8_t *body, size_t len,
                        size_t plen)
{
    uint8_t seq = body[2];
    uint8_t flags = body[5];
    uint16_t id = (uint16_t)(body[3] | (body[4] << 8));
    const uint8_t *payload = body + BODY_HDR;

    const struct pcProtoEntry *e = pc_lookup(id);
    if (e == NULL) {
        p->stats.unknown++;
        pc_proto_sendError(p, seq, id, PC_ERR_UNKNOWN_MSG);
        return;
    }
    if (e->caps != 0 && (e->caps & pc_proto_caps()) != e->caps) {
        p->stats.unknown++;
        /* The debug namespace answers "not allowed", not "not supported":
         * a production build has to look like a radio that refuses, never
         * like one that does not know the question. */
        pc_proto_sendError(p, seq, id,
                           (e->caps & (PC_CAP_DEBUG | PC_CAP_BENCH)) != 0
                               ? PC_ERR_NOT_ALLOWED : PC_ERR_NOT_SUPPORTED);
        return;
    }

    struct pcReq r = {
        .p = p, .seq = seq, .flags = flags, .id = id,
        .in = payload, .inLen = plen,
        .out = p->resp, .outCap = sizeof(p->resp), .outLen = 0, .err = 0
    };

    int rc = e->fn(&r);
    if (rc == 0)
        pc_proto_sendResponse(p, seq, id, r.flags, r.out, r.outLen);
    else if (rc < 0)
        pc_proto_sendError(p, seq, id, r.err != 0 ? r.err : -rc);
    /* rc > 0: the handler sent its own response already. */
}

/* Validate one assembled body and hand it over.  Returns 1 when it was good. */
static uint32_t pc_deliver(pcProto_t *p)
{
    if (p->rxOverflow) {
        p->stats.overflows++;
        p->stats.formErrors++;
        return 0;
    }
    if (p->rxLen < BODY_HDR + BODY_TRAILER) {
        p->stats.formErrors++;
        return 0;
    }
    uint8_t *body = p->rxBody;
    size_t len = p->rxLen;
    uint16_t plen = (uint16_t)(body[6] | (body[7] << 8));

    if (body[0] != PC_PROTO_FRAME_VERSION) {
        p->stats.formErrors++;
        return 0;
    }
    if ((size_t)plen != len - BODY_HDR - BODY_TRAILER) {
        p->stats.formErrors++;
        return 0;
    }
    uint16_t crc = (uint16_t)(body[len - 2] | (body[len - 1] << 8));
    if (pcCrc(body, len - 2) != crc) {
        p->stats.crcErrors++;
        return 0;
    }
    p->stats.frames++;

    if (body[1] == PC_TYPE_REQUEST) {
        p->stats.requests++;
        pc_dispatch(p, body, len, plen);
    }
    /* Responses and events are ignored: this side is the device.  Neither
     * gets a reply, so two radios on one cable cannot talk past each other. */
    return 1;
}

uint32_t pc_proto_rx(pcProto_t *p, const uint8_t *data, size_t len)
{
    uint32_t frames = 0;

    if (p == NULL || data == NULL)
        return 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (b == PC_PROTO_END) {
            if (p->rxInFrame)
                frames += pc_deliver(p);
            p->rxLen = 0;
            p->rxInFrame = true;
            p->rxEsc = false;
            p->rxOverflow = false;
            continue;
        }
        if (!p->rxInFrame)
            continue; /* garbage before the first delimiter */

        uint8_t v;
        if (p->rxEsc) {
            p->rxEsc = false;
            if (b == PC_PROTO_ESC_END)
                v = PC_PROTO_END;
            else if (b == PC_PROTO_ESC_ESC)
                v = PC_PROTO_ESC;
            else
                v = b; /* not a pair we produce: taken literally, the CRC
                        * decides whether it was noise */
        } else if (b == PC_PROTO_ESC) {
            p->rxEsc = true;
            continue;
        } else {
            v = b;
        }

        if (p->rxLen >= sizeof(p->rxBody))
            p->rxOverflow = true;
        else
            p->rxBody[p->rxLen++] = v;
    }
    return frames;
}

/*
 * ----------------------------------------------------------------------------
 * Session
 * ----------------------------------------------------------------------------
 */

void pc_proto_init(pcProto_t *p, const struct pcProtoPort *port)
{
    if (p == NULL)
        return;
    memset(p, 0, sizeof(*p));
    p->port = port;
    p->events = false;
    p->logLevel = PC_LOG_INFO;
    p->logRate = 0;
    p->logLastMs = 0;
    /* The bench state lives in its own module and survives an instance being
     * re-initialised: a port that comes up must come up with nothing keyed. */
    pc_proto_benchReset();
}

void pc_proto_rxResync(pcProto_t *p)
{
    if (p == NULL)
        return;

    p->rxLen = 0;
    p->rxEsc = false;
    p->rxInFrame = false;
    p->rxOverflow = false;
}

void pc_proto_reset(pcProto_t *p)
{
    if (p == NULL)
        return;
    p->rxLen = 0;
    p->rxEsc = false;
    p->rxInFrame = false;
    p->rxOverflow = false;
    p->linked = false;
    p->events = false;
    p->clientVersion = 0;
    p->clientCaps = 0;
    p->subPeriodMs = 0;
    p->logRate = 0;
    p->logCount = 0;
    p->logHead = 0;
    p->dbgToken = 0;
    /* A session that goes away must not leave the transmitter keyed or a
     * capture armed: the bench state belongs to the session. */
    pc_proto_benchReset();
}

bool pc_proto_linked(const pcProto_t *p)
{
    return p != NULL && p->linked;
}

/**
 * Push one buffered log line out as an event.  The frame either leaves whole or
 * is dropped and counted: a log line never becomes raw bytes on the cable.
 */
static void logEmit(pcProto_t *p)
{
    if (p->logCount == 0)
        return;

    /* Oldest first: the ring is a FIFO whose head is the newest line. */
    uint8_t idx = (uint8_t)((p->logHead + PC_PROTO_LOG_LINES - p->logCount)
                            % PC_PROTO_LOG_LINES);
    pcProtoLogLine_t *l = &p->logRing[idx];

    size_t off = pc_tlv_u32(p->resp, sizeof(p->resp), 0, PC_TAG_LOG_SEQ,
                            l->seq);
    off = pc_tlv_u32(p->resp, sizeof(p->resp), off, PC_TAG_UPTIME_MS,
                     l->timeMs);
    off = pc_tlv_u8(p->resp, sizeof(p->resp), off, PC_TAG_LEVEL, l->level);
    off = pc_tlv_str(p->resp, sizeof(p->resp), off, PC_TAG_MODULE, l->module);
    off = pc_tlv_str(p->resp, sizeof(p->resp), off, PC_TAG_TEXT, l->text);
    off = pc_tlv_u32(p->resp, sizeof(p->resp), off, PC_TAG_DROPPED,
                     p->logDropped);
    if (off == 0) {
        p->logDropped++;
        p->logCount--;
        return;
    }

    size_t before = p->stats.txFrames;
    pc_proto_sendEvent(p, PC_EVT_LOG, p->resp, off);
    if (p->stats.txFrames == before)
        p->logDropped++; /* the port could not take it */

    p->logCount--;
}

/** Rate limit: a token bucket of milli-lines, 1000 per line emitted. */
static void logDrain(pcProto_t *p)
{
    if (p->logRate == 0)
        return;

    uint32_t dt = p->nowMs - p->logLastMs;
    p->logLastMs = p->nowMs;
    if (dt > 60000)
        dt = 60000; /* a long gap does not buy a burst of 60 s of lines */

    uint32_t add = (uint32_t)p->logRate * dt;
    uint32_t cap = 4000; /* four lines of credit, no more */
    if (add >= cap || p->logTokens >= cap - add)
        p->logTokens = cap;
    else
        p->logTokens += add;

    while (p->logCount > 0 && p->logTokens >= 1000) {
        p->logTokens -= 1000;
        logEmit(p);
    }
}

void pc_proto_logPush(pcProto_t *p, uint8_t level, const char *module,
                      const char *text)
{
    if (p == NULL || text == NULL)
        return;

    p->logSeq++;

    /* A line below the level the client asked for is not a loss: it was never
     * going to be sent.  A line from another module is not a loss either. */
    if (level > p->logLevel)
        return;
    if (p->logModule[0] != '\0') {
        const char *m = module != NULL ? module : "";
        size_t i = 0;
        while (p->logModule[i] != '\0' && p->logModule[i] == m[i])
            i++;
        if (p->logModule[i] != '\0')
            return;
    }

    pcProtoLogLine_t *l = &p->logRing[p->logHead];
    memset(l, 0, sizeof(*l));
    l->seq = p->logSeq;
    l->timeMs = p->nowMs;
    l->level = level;
    if (module != NULL) {
        size_t n = strlen(module);
        if (n >= sizeof(l->module))
            n = sizeof(l->module) - 1;
        memcpy(l->module, module, n);
    }
    size_t n = strlen(text);
    if (n >= sizeof(l->text))
        n = sizeof(l->text) - 1;
    memcpy(l->text, text, n);

    p->logHead = (uint8_t)((p->logHead + 1) % PC_PROTO_LOG_LINES);
    if (p->logCount == PC_PROTO_LOG_LINES)
        p->logDropped++; /* the oldest line goes, and the client sees the gap */
    else
        p->logCount++;
}

uint32_t pc_proto_logDropped(const pcProto_t *p)
{
    return p != NULL ? p->logDropped : 0;
}

void pc_proto_tick(pcProto_t *p, uint32_t nowMs)
{
    if (p == NULL)
        return;
    p->nowMs = nowMs;

    /* The bench state machine runs whether or not a client is subscribed to
     * events: a capture or a sweep is a measurement in progress, not an
     * event stream. */
    pc_proto_benchTick(nowMs);

    /* Deferred platform work: the bench's PTT edge, which is applied only once
     * the frame that asked for it has been answered. */
    pc_proto_platformTick(nowMs);

    if (!p->linked || !p->events)
        return;

    logDrain(p);

    if (p->subPeriodMs == 0)
        return;
    if ((int32_t)(nowMs - p->subNextMs) < 0)
        return;
    p->subNextMs = nowMs + p->subPeriodMs;

    pcProtoStatus_t s;
    pc_proto_status(&s);
    size_t off = pc_proto_statusTlv(&s, p->resp, sizeof(p->resp), 0);
    off = pc_tlv_u32(p->resp, sizeof(p->resp), off, PC_TAG_UPTIME_MS, nowMs);
    if (off == 0)
        return;
    pc_proto_sendEvent(p, PC_EVT_STATUS, p->resp, off);
}

/*
 * ----------------------------------------------------------------------------
 * Events
 * ----------------------------------------------------------------------------
 */

void pc_proto_eventChannel(pcProto_t *p, uint16_t index, const char *name)
{
    if (p == NULL)
        return;
    pcProtoStatus_t s;
    pc_proto_status(&s);
    size_t off = pc_tlv_u16(p->resp, sizeof(p->resp), 0, PC_TAG_CHANNEL_IDX,
                            index);
    off = pc_tlv_str(p->resp, sizeof(p->resp), off, PC_TAG_CHANNEL_NAME,
                     name != NULL ? name : s.channelName);
    off = pc_proto_statusTlv(&s, p->resp, sizeof(p->resp), off);
    if (off == 0)
        return;
    pc_proto_sendEvent(p, PC_EVT_CHANNEL, p->resp, off);
}

void pc_proto_eventPtt(pcProto_t *p, uint8_t txState)
{
    if (p == NULL)
        return;
    pcProtoStatus_t s;
    pc_proto_status(&s);
    size_t off = pc_tlv_u8(p->resp, sizeof(p->resp), 0, PC_TAG_TX_STATE,
                           txState);
    off = pc_proto_statusTlv(&s, p->resp, sizeof(p->resp), off);
    if (off == 0)
        return;
    pc_proto_sendEvent(p, PC_EVT_PTT, p->resp, off);
}

void pc_proto_eventMessage(pcProto_t *p, uint8_t transport, uint32_t peer,
                           const char *text, uint8_t unread)
{
    if (p == NULL)
        return;
    /* The text is cut to what one frame holds: a message is at most 160 bytes,
     * so this only bites when the frame would have to be split, and then a
     * truncated text with the flag clear is better than a dropped event. */
    size_t off = pc_tlv_u8(p->resp, sizeof(p->resp), 0, PC_TAG_MSG_TRANSPORT,
                           transport);
    off = pc_tlv_u32(p->resp, sizeof(p->resp), off, PC_TAG_MSG_PEER_ID, peer);
    off = pc_tlv_str(p->resp, sizeof(p->resp), off, PC_TAG_MSG_TEXT,
                     text != NULL ? text : "");
    off = pc_tlv_u8(p->resp, sizeof(p->resp), off, PC_TAG_MSG_UNREAD, unread);
    if (off == 0)
        return;
    pc_proto_sendEvent(p, PC_EVT_MESSAGE, p->resp, off);
}

void pc_proto_eventSettings(pcProto_t *p, uint16_t settingsCrc)
{
    if (p == NULL)
        return;
    /* The CRC and the version are what a client needs to decide whether its
     * own copy of the settings is still valid. */
    size_t off = pc_tlv_u16(p->resp, sizeof(p->resp), 0, PC_TAG_SETTINGS_VER,
                            SETTINGS_VERSION);
    off = pc_tlv_u16(p->resp, sizeof(p->resp), off, PC_TAG_SETTINGS_CRC,
                     settingsCrc);
    if (off == 0)
        return;
    pc_proto_sendEvent(p, PC_EVT_SETTINGS, p->resp, off);
}
