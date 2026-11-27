// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 OpenRTX Contributors (C62 port)
//
// DMR text messages (see dmr_sms.h). The packet builders, the TMS and ETSI
// text formats and their decoders follow RadioFoundry's core/dmr/dmrData.c
// (dmrDataBuildPreambles, dmrDataBuildPacket, ipHeader, encodeUDP, tmsText,
// tmsAck, dmrDataBuildEtsiText, dmrDataIPTextUDP, dmrDataBuildResponse,
// dmrDataDecodeTMS, dmrDataDecodeEtsiText, dmrDataDecodeIPText); MIT, see
// trunk/LICENSE.radiofoundry.
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "dmr_sms.h"

#include "dmr_port.h"

#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#define SMS_PRINT printk
#else
#define SMS_PRINT printf
#endif

/* Messages and the packet builders' buffers: PSRAM (dmr_port.h) */
#define SMS_PSRAM EXT_RAM_BSS_ATTR

/* Air interface constants (ETSI TS 102 361-1, -3) */
#define DT_CSBK 3u
#define DT_DATA_HEADER 6u
#define DT_RATE_12 7u
#define CSBKO_PREAMBLE 0x3Du
#define DPF_RESPONSE 0x01u
#define DPF_UNCONFIRMED 0x02u
#define DPF_CONFIRMED 0x03u
#define DPF_SHORT_DEFINED 0x0Du
#define SAP_IP 0x04u
#define SAP_UDPIP_COMP 0x03u
#define SAP_SHORT_DATA 0x0Au
#define RESPONSE_ACK(ns) (0x08u | ((ns) & 0x07u)) /* class 0, type 1 */
#define PORT_TMS 4007u
#define PORT_ETSI_TEXT 5016u
#define UDPC_ETSI_TEXT 1u   /* compressed header port IDs (Tables 7.17/18) */
#define UDPC_MOTOROLA_TMS 98u
#define CAI_INDIVIDUAL 0x0Cu /* 12.x.y.z */
#define CAI_GROUP 0xE1u      /* 225.x.y.z */
#define DD_7BIT 0x02u
#define DD_ISO8859_1 0x03u
#define DD_ISO8859_LAST 0x11u
#define DD_UTF8 0x12u
#define DD_UTF16 0x13u
#define DD_UTF16BE 0x14u
#define DD_UTF16LE 0x15u

#define PREAMBLES 16    /* before a message or a TMS ACK (dmr-data.md 12.6.4) */
#define BURSTS_MAX 48   /* the longest list: 140 characters of TMS, 44 */
#define LISTS 3         /* waiting lists: ours and two answers */
#define LIST_WAIT_MS 15000u /* not started by then: given up */
#define DUP_MS 60000u   /* the same text from the same radio again: a retry */
#define BLOCK 12        /* rate 1/2 block octets */

enum { K_OURS, K_REPLY };

struct list {
    uint8_t kind, n, pos;
    uint32_t queued;
    bool queuedSet;
    uint8_t dt[BURSTS_MAX];
    uint8_t b[BURSTS_MAX][12];
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static SMS_PSRAM struct list lists[LISTS];
static unsigned qHead, qCount;
static SMS_PSRAM struct dmrSmsMsg inbox[DMR_SMS_INBOX];
static unsigned inCount;
static uint32_t serial;
static uint32_t ownId;
static uint8_t txState;
static uint32_t txDst;
static bool txGroup;
static uint8_t txSeq;      /* our TMS sequence number */
static uint8_t txTmsSeq;   /* the one of our message waiting for its ACK */
static uint16_t ipId;

/* ---- Octets -------------------------------------------------------------- */

static void putId(uint8_t *p, uint32_t id)
{
    p[0] = (uint8_t)(id >> 16);
    p[1] = (uint8_t)(id >> 8);
    p[2] = (uint8_t)id;
}

static void putU16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void putU32(uint8_t *p, uint32_t v)
{
    putU16(p, (uint16_t)(v >> 16));
    putU16(p + 2, (uint16_t)v);
}

static uint32_t idAddress(bool group, uint32_t id)
{
    return ((uint32_t)(group ? CAI_GROUP : CAI_INDIVIDUAL) << 24)
           | (id & 0xFFFFFFu);
}

/* CRC-CCITT of 10 octets (B.3.7), inverted, masked, MSB first after them */
static void addCrc16(uint8_t *buf, uint8_t mask)
{
    uint16_t crc = 0;
    for (int i = 0; i < 10; i++) {
        crc ^= (uint16_t)(buf[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
    }
    crc = (uint16_t)(~crc ^ (uint16_t)((mask << 8) | mask));
    buf[10] = (uint8_t)(crc >> 8);
    buf[11] = (uint8_t)crc;
}

static uint16_t onesSum(uint32_t sum, const uint8_t *d, int n)
{
    for (int i = 0; i < n; i += 2)
        sum += (uint32_t)((d[i] << 8) | ((i + 1) < n ? d[i + 1] : 0));
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)sum;
}

/* ---- Bursts -------------------------------------------------------------- */

struct out {
    uint8_t *dt;
    uint8_t (*b)[12];
    int n, max;
};

static bool put(struct out *o, uint8_t dt, const uint8_t b[12])
{
    if (o->n >= o->max)
        return false;
    o->dt[o->n] = dt;
    memcpy(o->b[o->n], b, 12);
    o->n++;
    return true;
}

/* Preamble CSBKs: each with the bursts still to follow after it */
static bool preambles(struct out *o, bool group, uint32_t dst, uint32_t src,
                      int count, int follow)
{
    uint8_t buf[12];
    for (int i = 0; i < count; i++) {
        const int cbf = count - 1 - i + follow;
        buf[0] = 0x80u | CSBKO_PREAMBLE; /* last block */
        buf[1] = 0x00;                   /* FID */
        buf[2] = (uint8_t)(0x80u | (group ? 0x40u : 0x00u)); /* data, G/I */
        buf[3] = (uint8_t)(cbf > 255 ? 255 : cbf);
        putId(&buf[4], dst);
        putId(&buf[7], src);
        addCrc16(buf, 0xA5);
        if (!put(o, DT_CSBK, buf))
            return false;
    }
    return true;
}

/* The user octets padded, the CRC-32 (least significant octet first, B.8C),
 * in rate 1/2 blocks */
static bool blocks(struct out *o, const uint8_t *data, int length)
{
    static SMS_PSRAM uint8_t pkt[BURSTS_MAX * BLOCK];
    /*
     * The block count is derived here, not passed in: every caller computed
     * it with this formula, so a stale or wrong one -- the parameter was
     * redundant with `length` -- made `pad` negative and turned the memset
     * below into a write of (size_t)pad bytes over a 576 byte PSRAM static.
     */
    const int nb = (length + 4 + BLOCK - 1) / BLOCK;
    const int pad = nb * BLOCK - 4 - length;

    if ((length < 0) || (nb < 1) || (nb > BURSTS_MAX)
        || (length + pad + 4 > (int)sizeof(pkt)))
        return false;

    memcpy(pkt, data, (size_t)length);
    memset(pkt + length, 0, (size_t)pad);
    const uint32_t crc = dmr_crc32(pkt, length + pad);
    uint8_t *c = pkt + length + pad;
    c[0] = (uint8_t)crc;
    c[1] = (uint8_t)(crc >> 8);
    c[2] = (uint8_t)(crc >> 16);
    c[3] = (uint8_t)(crc >> 24);
    for (int i = 0; i < nb; i++)
        if (!put(o, DT_RATE_12, pkt + i * BLOCK))
            return false;
    return true;
}

/* Unconfirmed packet (figure 8.3 header) after its preambles */
static bool packet(struct out *o, uint8_t sap, bool group, uint32_t dst,
                   uint32_t src, const uint8_t *data, int length, int pre)
{
    const int nb = (length + 4 + BLOCK - 1) / BLOCK;
    const int pad = nb * BLOCK - 4 - length;
    if (length <= 0 || pad > 31 || pre + 1 + nb > o->max - o->n)
        return false;
    if (!preambles(o, group, dst, src, pre, 1 + nb))
        return false;
    uint8_t h[12];
    h[0] = (uint8_t)((group ? 0x80u : 0x00u) | ((pad & 0x10) ? 0x10u : 0u)
                     | DPF_UNCONFIRMED);
    h[1] = (uint8_t)((sap << 4) | (pad & 0x0F));
    putId(&h[2], dst);
    putId(&h[5], src);
    h[8] = (uint8_t)(0x80u | nb); /* full message */
    h[9] = 0x00;                  /* FSN */
    addCrc16(h, 0xCC);
    return put(o, DT_DATA_HEADER, h) && blocks(o, data, length);
}

/* IPv4 / UDP around a payload (full header, SAP 4), with both checksums */
static int udp(uint8_t *ip, bool group, uint32_t dst, uint32_t src,
               uint16_t port, const uint8_t *payload, int length)
{
    const int ulen = 8 + length;
    const int total = 20 + ulen;
    memset(ip, 0, 28);
    memcpy(ip + 28, payload, (size_t)length);
    putU16(&ip[20], port);
    putU16(&ip[22], port);
    putU16(&ip[24], (uint16_t)ulen);
    ip[0] = 0x45;
    putU16(&ip[2], (uint16_t)total);
    putU16(&ip[4], ipId++);
    ip[8] = group ? 1 : 64; /* TTL: the air is a group's one hop */
    ip[9] = 17;             /* UDP */
    putU32(&ip[12], idAddress(false, src));
    putU32(&ip[16], idAddress(group, dst));
    putU16(&ip[10], (uint16_t)~onesSum(0, ip, 20));
    uint16_t s = onesSum(17u + (uint32_t)ulen, &ip[12], 8);
    s = (uint16_t)~onesSum(s, &ip[20], ulen);
    putU16(&ip[26], s == 0 ? 0xFFFFu : s);
    return total;
}

/* Motorola TMS simple text: length, header 0xA0 (0xE0: ACK requested), no
 * address, sequence number, encoding 4, the text in UTF-16LE */
static int tmsText(const char *text, uint8_t seq, bool ack, uint8_t *t)
{
    int n = (int)strlen(text);
    if (n > DMR_SMS_TEXT_MAX)
        n = DMR_SMS_TEXT_MAX;
    const int length = 6 + 2 * n;
    putU16(&t[0], (uint16_t)(length - 2));
    t[2] = ack ? 0xE0 : 0xA0;
    t[3] = 0x00;
    t[4] = (uint8_t)(0x80u | (seq & 0x1Fu));
    t[5] = 0x04;
    for (int i = 0; i < n; i++) {
        t[6 + 2 * i] = (uint8_t)text[i];
        t[7 + 2 * i] = 0x00;
    }
    return length;
}

/* The TMS ACK: header 0xBF, no address, the sequence number (00 03 BF 00
 * <s/n>; past 31 a second octet) */
static int tmsAck(uint8_t seq, uint8_t a[6])
{
    a[0] = 0x00;
    a[1] = 0x03;
    a[2] = 0xBF;
    a[3] = 0x00;
    a[4] = seq & 0x1Fu;
    seq &= 0x7Fu;
    if (seq > 0x1Fu) {
        a[1] = 0x04;
        a[4] |= 0x80u;
        a[5] = seq & 0x60u;
        return 6;
    }
    return 5;
}

int dmrSms_build(uint32_t dst, bool group, uint8_t format, const char *text,
                 uint32_t src, uint8_t dt[], uint8_t b[][12], int max)
{
    static SMS_PSRAM uint8_t pl[6 + 2 * DMR_SMS_TEXT_MAX];
    static SMS_PSRAM uint8_t ip[28 + 6 + 2 * DMR_SMS_TEXT_MAX];
    struct out o = { dt, b, 0, max };
    int n = (int)strlen(text);
    if (n == 0 || n > DMR_SMS_TEXT_MAX)
        return 0;
    switch (format) {
        case DMR_SMS_TMS: {
            /* An individual message asks for the TMS ACK */
            const int len = tmsText(text, txSeq, !group, pl);
            const int total = udp(ip, group, dst, src, PORT_TMS, pl, len);
            if (!packet(&o, SAP_IP, group, dst, src, ip, total, PREAMBLES))
                return 0;
            break;
        }
        case DMR_SMS_ETSI_IP: {
            for (int i = 0; i < n; i++) {
                pl[2 * i] = 0x00;
                pl[2 * i + 1] = (uint8_t)text[i];
            }
            const int total = udp(ip, group, dst, src, PORT_ETSI_TEXT, pl,
                                  2 * n);
            if (!packet(&o, SAP_IP, group, dst, src, ip, total, PREAMBLES))
                return 0;
            break;
        }
        case DMR_SMS_ETSI_DD: {
            /* DD_HEAD (DPF 13, SAP 10): ISO 8859-1, F 1; the bit padding
             * of the octets after the text */
            const int nb = (n + 4 + BLOCK - 1) / BLOCK;
            const int pad = nb * BLOCK - 4 - n;
            if (nb > 63 || PREAMBLES + 1 + nb > max)
                return 0;
            if (!preambles(&o, group, dst, src, PREAMBLES, 1 + nb))
                return 0;
            uint8_t h[12];
            h[0] = (uint8_t)((group ? 0x80u : 0x00u) | (group ? 0u : 0x40u)
                             | (((nb >> 4) & 0x03) << 4) | DPF_SHORT_DEFINED);
            h[1] = (uint8_t)((SAP_SHORT_DATA << 4) | (nb & 0x0F));
            putId(&h[2], dst);
            putId(&h[5], src);
            h[8] = (uint8_t)((DD_ISO8859_1 << 2) | 0x01u);
            h[9] = (uint8_t)(pad * 8);
            addCrc16(h, 0xCC);
            if (!put(&o, DT_DATA_HEADER, h)
                || !blocks(&o, (const uint8_t *)text, n))
                return 0;
            break;
        }
        default:
            return 0;
    }
    return o.n;
}

/* ---- The queue ----------------------------------------------------------- */

/* A free list, or NULL; with the lock held */
static struct list *queueSlot(void)
{
    if (qCount >= LISTS)
        return NULL;
    struct list *l = &lists[(qHead + qCount) % LISTS];
    memset(l, 0, sizeof(*l));
    return l;
}

static void queueCommit(void)
{
    qCount++;
}

bool dmrSms_send(uint32_t dst, bool group, uint8_t format, const char *text,
                 uint32_t src)
{
    bool ok = false;
    pthread_mutex_lock(&lock);
    if (txState != DMR_SMS_TX_QUEUED) {
        struct list *l = queueSlot();
        if (l != NULL) {
            txSeq = (uint8_t)((txSeq + 1) & 0x1Fu);
            const int n = dmrSms_build(dst, group, format, text, src, l->dt,
                                       l->b, BURSTS_MAX);
            if (n > 0) {
                l->kind = K_OURS;
                l->n = (uint8_t)n;
                queueCommit();
                txState = DMR_SMS_TX_QUEUED;
                txDst = dst;
                txGroup = group;
                txTmsSeq = format == DMR_SMS_TMS && !group ? txSeq : 0xFFu;
                serial++;
                ok = true;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    if (ok)
        SMS_PRINT("[DMR SMS] mensaje en cola: %s %lu, formato %u, %u "
                  "caracteres\n", group ? "grupo" : "ID",
                  (unsigned long)dst, (unsigned)format,
                  (unsigned)strlen(text));
    return ok;
}

uint8_t dmrSms_txState(uint32_t *dst, bool *group)
{
    pthread_mutex_lock(&lock);
    const uint8_t s = txState;
    if (dst)
        *dst = txDst;
    if (group)
        *group = txGroup;
    pthread_mutex_unlock(&lock);
    return s;
}

bool dmrSms_pending(void)
{
    pthread_mutex_lock(&lock);
    const bool p = qCount > 0;
    pthread_mutex_unlock(&lock);
    return p;
}

bool dmrSms_txNext(uint8_t *dt, uint8_t b[12])
{
    bool ok = false;
    pthread_mutex_lock(&lock);
    if (qCount > 0) {
        struct list *l = &lists[qHead];
        if (l->pos < l->n) {
            *dt = l->dt[l->pos];
            memcpy(b, l->b[l->pos], 12);
            l->pos++;
            ok = true;
        }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

static void finishHead(bool ok)
{
    struct list *l = &lists[qHead];
    if (l->kind == K_OURS) {
        txState = ok ? DMR_SMS_TX_SENT : DMR_SMS_TX_FAILED;
        serial++;
    }
    qHead = (qHead + 1) % LISTS;
    qCount--;
}

void dmrSms_txDone(bool ok)
{
    pthread_mutex_lock(&lock);
    if (qCount > 0) {
        const bool ours = lists[qHead].kind == K_OURS;
        /* Started and cut short: it did not go out whole */
        const bool whole = lists[qHead].pos >= lists[qHead].n;
        finishHead(ok && whole);
        pthread_mutex_unlock(&lock);
        SMS_PRINT("[DMR SMS] %s %s\n", ours ? "mensaje" : "respuesta",
                  ok && whole ? "enviado" : "no enviado");
        return;
    }
    pthread_mutex_unlock(&lock);
}

void dmrSms_tick(uint32_t now_ms)
{
    pthread_mutex_lock(&lock);
    if (qCount > 0) {
        struct list *l = &lists[qHead];
        if (!l->queuedSet) {
            l->queuedSet = true;
            l->queued = now_ms;
        } else if (l->pos == 0 && (uint32_t)(now_ms - l->queued)
                                      > LIST_WAIT_MS) {
            const bool ours = l->kind == K_OURS;
            finishHead(false);
            pthread_mutex_unlock(&lock);
            SMS_PRINT("[DMR SMS] %s descartado: el canal no se libero en "
                      "15 s\n", ours ? "mensaje" : "respuesta");
            return;
        }
    }
    pthread_mutex_unlock(&lock);
}

/* An answer to queue (with the lock held): dropped if the queue is full */
static void queueReply(const uint8_t *dts, uint8_t (*bs)[12], int n)
{
    struct list *l = queueSlot();
    if (l == NULL || n <= 0) {
        SMS_PRINT("[DMR SMS] respuesta descartada: cola llena\n");
        return;
    }
    l->kind = K_REPLY;
    l->n = (uint8_t)n;
    memcpy(l->dt, dts, (size_t)n);
    memcpy(l->b, bs, (size_t)n * 12);
    queueCommit();
}

/* ---- Receive ------------------------------------------------------------- */

/* A Latin-1 character as ASCII for the screen: accents taken off, '?'
 * outside, control characters dropped (0) */
static char ascii(uint32_t cp)
{
    static const char hi[] =
        "AAAAAAACEEEEIIII"  /* C0 */
        "DNOOOOOxOUUUUYTs"  /* D0 */
        "aaaaaaaceeeeiiii"  /* E0 */
        "dnooooo/ouuuuyty"; /* F0 */
    if (cp == '\r' || cp == '\n' || cp == '\t')
        return ' ';
    if (cp < 0x20)
        return 0;
    if (cp < 0x7F)
        return (char)cp;
    if (cp >= 0xC0 && cp <= 0xFF)
        return hi[cp - 0xC0];
    return '?';
}

static void textPut(char *t, int *n, uint32_t cp)
{
    const char c = ascii(cp);
    if (c == ' ' && *n == 0)
        return; /* some radios start with CR LF */
    if (c != 0 && *n < DMR_SMS_TEXT_MAX)
        t[(*n)++] = c;
}

static void utf16(const uint8_t *d, int len, bool be, char *t, int *n)
{
    for (int i = 0; i + 1 < len; i += 2) {
        const uint16_t u = be ? (uint16_t)((d[i] << 8) | d[i + 1])
                              : (uint16_t)((d[i + 1] << 8) | d[i]);
        if (u == 0)
            break;
        if (u >= 0xD800u && u <= 0xDBFFu) {
            i += 2;
            textPut(t, n, '?');
            continue;
        }
        textPut(t, n, u);
    }
}

static void utf8(const uint8_t *d, int len, char *t, int *n)
{
    for (int i = 0; i < len; i++) {
        const uint8_t c = d[i];
        if (c == 0)
            break;
        if (c < 0x80) {
            textPut(t, n, c);
        } else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            textPut(t, n, ((uint32_t)(c & 0x1F) << 6) | (d[i + 1] & 0x3F));
            i++;
        } else if ((c & 0xC0) != 0x80) {
            textPut(t, n, '?');
            i += (c & 0xF0) == 0xE0 ? 2 : 3;
        }
    }
}

static void bits7(const uint8_t *d, int bits, char *t, int *n)
{
    for (int pos = 0; pos + 7 <= bits; pos += 7) {
        uint8_t c = 0;
        for (int b = 0; b < 7; b++) {
            const int bit = pos + b;
            c = (uint8_t)((c << 1) | ((d[bit >> 3] >> (7 - (bit & 7))) & 1));
        }
        if (c == 0)
            break;
        textPut(t, n, c);
    }
}

/* TMS: false if not a text or an ACK */
static bool tmsDecode(const uint8_t *p, int len, bool *isAck, bool *wantAck,
                      uint8_t *seq, char *t, int *n)
{
    if (len < 4)
        return false;
    const uint8_t h = p[2];
    int end = ((p[0] << 8) | p[1]) + 2;
    int idx = 4 + p[3]; /* skip the address */
    if (end > len)
        end = len;
    *wantAck = (h & 0x40u) != 0;
    *isAck = (h & 0x1Fu) == 0x1Fu;
    *seq = 0;
    if ((h & 0x80u) && idx < end) {
        *seq = p[idx] & 0x1Fu;
        if ((p[idx] & 0x80u) && idx + 1 < end)
            *seq |= p[idx + 1] & 0x60u;
    }
    if (*isAck)
        return true;
    if ((h & 0x10u) || (h & 0x0Fu))
        return false; /* a control PDU, not a simple text */
    if (h & 0x80u)
        while (idx < end)
            if ((p[idx++] & 0x80u) == 0)
                break;
    for (; idx + 1 < end; idx += 2) {
        const uint16_t c = (uint16_t)(p[idx] | (p[idx + 1] << 8));
        if (c == 0)
            break;
        textPut(t, n, c);
    }
    return true;
}

/* The answers a message to us asks for */
/*
 * The answering scratch.  A confirmed response is one burst, a TMS ACK is
 * a preamble chain, but the arrays were sized for the longest of either and
 * reserved on every call -- 624 bytes of stack, in the rtx thread, for an
 * ACK that is usually one burst, on a radio whose DTCM is at 95.9 %.
 * answer() runs with `lock` held and is its only user, so one copy of the
 * scratch, in PSRAM with the rest of this file's state, serves every call.
 */
static SMS_PSRAM uint8_t replyDt[BURSTS_MAX];
static SMS_PSRAM uint8_t replyBs[BURSTS_MAX][12];

static void answer(const dmr_packet_t *pk, bool tmsWantsAck, uint8_t tmsSeq)
{
    const dmr_data_header_t *h = &pk->hdr;
    uint8_t *dts = replyDt;
    uint8_t (*bs)[12] = replyBs;
    if (h->group || h->dst != ownId || ownId == 0)
        return;
    /* Confirmed data, or short data with the response bit: the ETSI
     * response, ACK with its N(S), alone */
    if (h->dpf == DPF_CONFIRMED
        || (h->dpf == DPF_SHORT_DEFINED && h->resp_req)) {
        uint8_t r[12];
        r[0] = DPF_RESPONSE;
        r[1] = (uint8_t)(h->sap << 4);
        putId(&r[2], h->src);
        putId(&r[5], ownId);
        r[8] = 0;
        r[9] = RESPONSE_ACK(h->dpf == DPF_CONFIRMED ? h->ns : 0);
        addCrc16(r, 0xCC);
        dts[0] = DT_DATA_HEADER;
        memcpy(bs[0], r, 12);
        queueReply(dts, bs, 1);
        SMS_PRINT("[DMR SMS] respuesta ETSI (ACK) a %lu en cola\n",
                  (unsigned long)h->src);
    }
    if (tmsWantsAck) {
        static uint8_t ip[28 + 6];
        uint8_t a[6];
        const int al = tmsAck(tmsSeq, a);
        const int total = udp(ip, false, h->src, ownId, PORT_TMS, a, al);
        struct out o = { dts, bs, 0, BURSTS_MAX };
        if (packet(&o, SAP_IP, false, h->src, ownId, ip, total, PREAMBLES)) {
            queueReply(dts, bs, o.n);
            SMS_PRINT("[DMR SMS] ACK TMS (s/n %u) a %lu en cola\n",
                      (unsigned)tmsSeq, (unsigned long)h->src);
        }
    }
}

static void keep(const dmr_packet_t *pk, uint8_t format, const char *text,
                 uint32_t t_ms)
{
    const dmr_data_header_t *h = &pk->hdr;
    /* A retry of the last one: once */
    for (unsigned i = 0; i < inCount; i++)
        if (inbox[i].src == h->src && inbox[i].dst == h->dst
            && strcmp(inbox[i].text, text) == 0
            && (uint32_t)(t_ms - inbox[i].t_ms) < DUP_MS)
            return;
    if (inCount < DMR_SMS_INBOX)
        inCount++;
    memmove(&inbox[1], &inbox[0], (inCount - 1) * sizeof(inbox[0]));
    struct dmrSmsMsg *m = &inbox[0];
    memset(m, 0, sizeof(*m));
    m->src = h->src;
    m->dst = h->dst;
    m->group = h->group;
    m->format = format;
    m->unread = true;
    m->t_ms = t_ms;
    snprintf(m->text, sizeof(m->text), "%s", text);
    serial++;
}

bool dmrSms_rxPacket(const dmr_packet_t *pk, uint32_t t_ms)
{
    const dmr_data_header_t *h = &pk->hdr;
    char text[DMR_SMS_TEXT_MAX + 1];
    int n = 0;
    uint8_t format;
    bool isAck = false, wantAck = false;
    uint8_t seq = 0;
    if (pk->data == NULL)
        return false;

    if (h->dpf == DPF_SHORT_DEFINED) {
        const uint8_t *d = pk->data;
        int len = pk->len - (h->dd_pad >> 3);
        const uint8_t f = h->dd_format;
        if (len < 0)
            len = 0;
        if (f == DD_7BIT) {
            bits7(d, pk->len * 8 - h->dd_pad, text, &n);
        } else if (f >= DD_ISO8859_1 && f <= DD_ISO8859_LAST) {
            for (int i = 0; i < len && d[i] != 0; i++)
                textPut(text, &n, (d[i] < 0x80 || f == DD_ISO8859_1) ? d[i]
                                                                    : '?');
        } else if (f == DD_UTF8) {
            utf8(d, len, text, &n);
        } else if (f == DD_UTF16 || f == DD_UTF16BE || f == DD_UTF16LE) {
            bool be = f != DD_UTF16LE;
            if (f == DD_UTF16 && len >= 2
                && ((d[0] == 0xFF && d[1] == 0xFE)
                    || (d[0] == 0xFE && d[1] == 0xFF))) {
                be = d[0] == 0xFE;
                d += 2;
                len -= 2;
            }
            utf16(d, len, be, text, &n);
        } else {
            return false; /* binary, BCD: not text */
        }
        format = DMR_SMS_ETSI_DD;
    } else if (pk->udp && pk->udp_len > 0) {
        uint16_t port = pk->dst_port;
        if (port == 0)
            port = pk->dst_port_id == UDPC_ETSI_TEXT ? PORT_ETSI_TEXT
                 : pk->dst_port_id == UDPC_MOTOROLA_TMS ? PORT_TMS : 0;
        const uint8_t *p = pk->data + pk->udp_off;
        const int len = pk->udp_len;
        if (port == PORT_TMS) {
            if (!tmsDecode(p, len, &isAck, &wantAck, &seq, text, &n))
                return false;
            format = DMR_SMS_TMS;
        } else if (port == PORT_ETSI_TEXT) {
            bool be = true;
            int l = len;
            if (l >= 2 && ((p[0] == 0xFE && p[1] == 0xFF)
                           || (p[0] == 0xFF && p[1] == 0xFE))) {
                be = p[0] == 0xFE;
                p += 2;
                l -= 2;
            }
            utf16(p, l, be, text, &n);
            format = DMR_SMS_ETSI_IP;
        } else {
            return false;
        }
    } else {
        return false;
    }
    text[n] = 0;

    pthread_mutex_lock(&lock);
    if (isAck) {
        /* The TMS ACK of our message */
        const bool ours = h->dst == ownId && txTmsSeq != 0xFFu
                          && (seq & 0x1Fu) == txTmsSeq;
        if (ours && txState == DMR_SMS_TX_SENT) {
            txState = DMR_SMS_TX_ACKED;
            serial++;
        }
        pthread_mutex_unlock(&lock);
        SMS_PRINT("[DMR SMS] ACK TMS de %lu a %lu (s/n %u)%s\n",
                  (unsigned long)h->src, (unsigned long)h->dst,
                  (unsigned)seq, ours ? ": nuestro mensaje entregado" : "");
        return true;
    }
    keep(pk, format, text, t_ms);
    answer(pk, format == DMR_SMS_TMS && wantAck, seq);
    pthread_mutex_unlock(&lock);
    static const char *const names[DMR_SMS_FORMATS] = {
        "TMS", "ETSI IP", "ETSI corto"
    };
    SMS_PRINT("[DMR SMS] %s de %lu a %s%lu: %s\n", names[format],
              (unsigned long)h->src, h->group ? "TG " : "ID ",
              (unsigned long)h->dst, text);
    return true;
}

/* ---- Inbox --------------------------------------------------------------- */

void dmrSms_setOwnId(uint32_t id)
{
    ownId = id;
}

unsigned dmrSms_count(void)
{
    return inCount;
}

unsigned dmrSms_unread(void)
{
    unsigned n = 0;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < inCount; i++)
        n += inbox[i].unread;
    pthread_mutex_unlock(&lock);
    return n;
}

bool dmrSms_get(unsigned i, struct dmrSmsMsg *out)
{
    pthread_mutex_lock(&lock);
    const bool ok = i < inCount;
    if (ok)
        *out = inbox[i];
    pthread_mutex_unlock(&lock);
    return ok;
}

void dmrSms_markRead(unsigned i)
{
    pthread_mutex_lock(&lock);
    if (i < inCount && inbox[i].unread) {
        inbox[i].unread = false;
        serial++;
    }
    pthread_mutex_unlock(&lock);
}

void dmrSms_delete(unsigned i)
{
    pthread_mutex_lock(&lock);
    if (i < inCount) {
        memmove(&inbox[i], &inbox[i + 1],
                (inCount - i - 1) * sizeof(inbox[0]));
        inCount--;
        serial++;
    }
    pthread_mutex_unlock(&lock);
}

uint32_t dmrSms_serial(void)
{
    return serial;
}
