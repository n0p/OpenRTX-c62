/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrSmsApp.c
 * @brief DMR short message payloads: text codecs and the seven application
 * formats, ported bit for bit from the host prototype in work/dmr-sms
 * (sms_app.cpp), which validated them against ETSI TS 102 361-3 and against
 * the layout of the Motorola, Hytera and AnyTone radios that send them.
 *
 *   1. ETSI defined data, DD_HEAD with the text in the DD format (UTF-16BE,
 *      UTF-8): TS 102 361-1 clause 8.2.1.7, table 9.50.
 *   2. ETSI text over IP, TS 102 361-3 clause 5.7.1: unconfirmed data, SAP 4,
 *      IPv4 + UDP port 5016, UTF-16BE, no terminator.
 *   3. The same on the compressed UDP/IPv4 header (SAP 3, DPID 1).
 *   4. AnyTone: UDP 5016 with 00 0d 00 0a before the UTF-16LE text.
 *   5. Motorola TMS: UDP 4007, length-prefixed record, UCS-2 LE text after
 *      CR LF, sequence extension; type 0x1F is the acknowledgement.
 *   6. Hytera H-SMS as AnyTone radios send it: DD_HEAD, DD = BCD, payload =
 *      2 octets + UTF-16LE + NUL.
 */

#include <string.h>

#include "protocols/DMR/DmrSms.h"
#include "DmrSmsApp.h"

/* ------------------------------------------------------- text conversion --- */

size_t dmrSms_utf8ToUnits(const char *utf8, uint16_t *units, size_t maxUnits)
{
    size_t n = 0;
    size_t i = 0;
    size_t len;

    if (utf8 == NULL)
        return 0;
    if (units == NULL)
        maxUnits = 0xFFFFFFFFu; /* counting pass: nothing is written */
    len = strlen(utf8);

    while (i < len) {
        uint32_t cp = (uint8_t)utf8[i];
        int extra = 0;

        if (cp >= 0xF0) {
            cp &= 0x07;
            extra = 3;
        } else if (cp >= 0xE0) {
            cp &= 0x0F;
            extra = 2;
        } else if (cp >= 0xC0) {
            cp &= 0x1F;
            extra = 1;
        }
        if (i + (size_t)extra >= len)
            extra = 0;
        for (int k = 1; k <= extra; k++)
            cp = (cp << 6) | ((uint8_t)utf8[i + (size_t)k] & 0x3Fu);
        i += (size_t)extra + 1;

        /* Surrogate pairs need two units: drop the pair if only one fits. */
        size_t need = (cp > 0xFFFF) ? 2 : 1;

        if (n + need > maxUnits)
            break;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            if (units != NULL) {
                units[n] = (uint16_t)(0xD800u | (cp >> 10));
                units[n + 1] = (uint16_t)(0xDC00u | (cp & 0x3FFu));
            }
            n += 2;
        } else {
            if (units != NULL)
                units[n] = (uint16_t)cp;
            n++;
        }
    }
    return n;
}

/** Append one code point to a UTF-8 buffer; returns bytes written or 0. */
static size_t putUtf8(char *out, size_t cap, size_t at, uint32_t cp)
{
    if (cp < 0x80) {
        if (at + 1 > cap)
            return 0;
        out[at] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        if (at + 2 > cap)
            return 0;
        out[at] = (char)(0xC0u | (cp >> 6));
        out[at + 1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000) {
        if (at + 3 > cap)
            return 0;
        out[at] = (char)(0xE0u | (cp >> 12));
        out[at + 1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[at + 2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (at + 4 > cap)
        return 0;
    out[at] = (char)(0xF0u | (cp >> 18));
    out[at + 1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[at + 2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[at + 3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

size_t dmrSms_utf8ToUtf16(const char *utf8, uint8_t *out, size_t cap, bool be,
                          bool *truncated)
{
    uint16_t units[DMR_SMS_TEXT_MAX + 2];
    size_t maxUnits = cap / 2;
    size_t n;
    size_t i;

    if (maxUnits > sizeof(units) / sizeof(units[0]))
        maxUnits = sizeof(units) / sizeof(units[0]);
    n = dmrSms_utf8ToUnits(utf8, units, maxUnits);

    if (truncated != NULL)
        *truncated = dmrSms_utf8ToUnits(utf8, NULL, 0) > n;

    for (i = 0; i < n; i++) {
        out[2 * i] = (uint8_t)(be ? (units[i] >> 8) : (units[i] & 0xFFu));
        out[2 * i + 1] = (uint8_t)(be ? (units[i] & 0xFFu) : (units[i] >> 8));
    }
    return n * 2;
}

size_t dmrSms_utf16ToUtf8(const uint8_t *p, size_t n, bool be, char *out,
                          size_t cap, bool *truncated)
{
    size_t i = 0;
    size_t at = 0;

    if (truncated != NULL)
        *truncated = false;

    while (i + 1 < n) {
        uint16_t c = be ? (uint16_t)((p[i] << 8) | p[i + 1])
                        : (uint16_t)((p[i + 1] << 8) | p[i]);

        i += 2;
        if (c == 0)
            break;
        if ((c >= 0xD800) && (c <= 0xDBFF) && (i + 1 < n)) {
            uint16_t lo = be ? (uint16_t)((p[i] << 8) | p[i + 1])
                             : (uint16_t)((p[i + 1] << 8) | p[i]);

            if ((lo >= 0xDC00) && (lo <= 0xDFFF)) {
                uint32_t cp = 0x10000u
                              + (((uint32_t)(c - 0xD800) << 10)
                                 | (uint32_t)(lo - 0xDC00));
                size_t w = putUtf8(out, cap, at, cp);

                if (w == 0) {
                    if (truncated != NULL)
                        *truncated = true;
                    break;
                }
                at += w;
                i += 2;
                continue;
            }
        }
        size_t w = putUtf8(out, cap, at, c);

        if (w == 0) {
            if (truncated != NULL)
                *truncated = true;
            break;
        }
        at += w;
    }
    return at;
}

size_t dmrSms_latin1ToUtf8(const uint8_t *p, size_t n, char *out, size_t cap,
                           bool *truncated)
{
    size_t at = 0;

    if (truncated != NULL)
        *truncated = false;
    for (size_t i = 0; i < n; i++) {
        size_t w = putUtf8(out, cap, at, p[i]);

        if (w == 0) {
            if (truncated != NULL)
                *truncated = true;
            break;
        }
        at += w;
    }
    return at;
}

/* ------------------------------------------------------------ IPv4/UDP --- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint16_t inetSum(const uint8_t *p, size_t n, uint32_t sum)
{
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    if ((n & 1u) != 0)
        sum += (uint32_t)(p[n - 1] << 8);
    while ((sum >> 16) != 0)
        sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)sum;
}

/* Radio ID to IPv4: network 12, then the 24-bit ID (MOTOTRBO convention). */
static void radioAddr(uint8_t out[4], uint32_t id)
{
    out[0] = 12;
    out[1] = (uint8_t)((id >> 16) & 0xFFu);
    out[2] = (uint8_t)((id >> 8) & 0xFFu);
    out[3] = (uint8_t)(id & 0xFFu);
}

static void groupAddr(uint8_t out[4], uint32_t id)
{
    out[0] = 225; /* 225.x.y.z group multicast, MOTOTRBO convention */
    out[1] = (uint8_t)((id >> 16) & 0xFFu);
    out[2] = (uint8_t)((id >> 8) & 0xFFu);
    out[3] = (uint8_t)(id & 0xFFu);
}

/**
 * IPv4 + UDP datagram around a payload.  @p cap is the room left in the
 * packet, the caller has already bounded the payload.
 */
static size_t buildUdpIpv4(uint8_t *out, size_t cap, uint32_t src, uint32_t dst,
                           bool group, uint16_t sport, uint16_t dport,
                           uint8_t ttl, const uint8_t *payload, size_t len)
{
    uint8_t sa[4];
    uint8_t da[4];
    uint8_t *ip = out;
    uint8_t *udp = out + 20;

    if (cap < 28 + len)
        return 0;

    /* Header fields that are computed last must start at zero. */
    memset(out, 0, 28);

    radioAddr(sa, src);
    if (group)
        groupAddr(da, dst);
    else
        radioAddr(da, dst);

    ip[0] = 0x45;                                /* v4, IHL 5 */
    ip[1] = 0x00;                                /* DSCP/ECN */
    put16(ip + 2, (uint16_t)(20 + 8 + len));
    put16(ip + 4, 0x0001);                       /* identification */
    put16(ip + 6, 0x4000);                       /* DF */
    ip[8] = ttl;
    ip[9] = 17;                                  /* UDP */
    memcpy(ip + 12, sa, 4);
    memcpy(ip + 16, da, 4);
    put16(ip + 10, (uint16_t)~inetSum(ip, 20, 0));

    put16(udp + 0, sport);
    put16(udp + 2, dport);
    put16(udp + 4, (uint16_t)(8 + len));
    memcpy(udp + 8, payload, len);

    /* UDP checksum over the pseudo header and the datagram */
    uint32_t sum = 0;

    sum += (uint32_t)((sa[0] << 8) | sa[1]);
    sum += (uint32_t)((sa[2] << 8) | sa[3]);
    sum += (uint32_t)((da[0] << 8) | da[1]);
    sum += (uint32_t)((da[2] << 8) | da[3]);
    sum += 17;
    sum += (uint32_t)(8 + len);
    sum = inetSum(udp, 8 + len, sum);
    uint16_t u = (uint16_t)~sum;

    put16(udp + 6, (u != 0) ? u : 0xFFFF);
    return 28 + len;
}

/* ------------------------------------------------------ payload builders --- */

/** Text in the DD format of a defined data header. */
static size_t textToDd(const char *text, int dd, uint8_t *out, size_t cap,
                       bool *truncated)
{
    size_t i = 0;

    if (truncated != NULL)
        *truncated = false;

    switch (dd) {
        case DMR_SMS_DD_UTF16BE:
            return dmrSms_utf8ToUtf16(text, out, cap, true, truncated);

        case DMR_SMS_DD_UTF16LE:
            return dmrSms_utf8ToUtf16(text, out, cap, false, truncated);

        case DMR_SMS_DD_UTF8:
        case DMR_SMS_DD_8859_1:
        case DMR_SMS_DD_7BIT:
            for (i = 0; text[i] != '\0'; i++) {
                uint8_t b = (uint8_t)text[i];

                if (i >= cap) {
                    if (truncated != NULL)
                        *truncated = true;
                    break;
                }
                out[i] = (b < 0x80) ? b : (uint8_t)'?';
            }
            return i;

        default:
            /* binary: raw UTF-8 octets */
            for (i = 0; text[i] != '\0'; i++) {
                if (i >= cap) {
                    if (truncated != NULL)
                        *truncated = true;
                    break;
                }
                out[i] = (uint8_t)text[i];
            }
            return i;
    }
}

int dmrSms_buildPayload(enum dmrSmsApp app, uint32_t src, uint32_t dst,
                        bool group, const char *text, uint8_t tmsSeq,
                        bool ackReq, bool tmsAck, uint8_t *out, size_t cap,
                        bool *truncated)
{
    uint8_t body[DMR_SMS_PAYLOAD_MAX];
    size_t n;
    bool cut = false;

    if (truncated != NULL)
        *truncated = false;
    if (cap > DMR_SMS_PAYLOAD_MAX)
        cap = DMR_SMS_PAYLOAD_MAX;

    switch (app) {
        case DMR_SMS_APP_DD_UTF16BE:
            return (int)textToDd(text, DMR_SMS_DD_UTF16BE, out, cap, truncated);

        case DMR_SMS_APP_DD_UTF8:
            return (int)textToDd(text, DMR_SMS_DD_UTF8, out, cap, truncated);

        case DMR_SMS_APP_IP:
            /* TS 102 361-3 5.7.1: UTF-16BE on UDP 5016, no prefix, no NUL */
            if (cap < 28)
                return -1;
            n = dmrSms_utf8ToUtf16(text, body, cap - 28, true, &cut);
            if (truncated != NULL)
                *truncated = cut;
            return (int)buildUdpIpv4(out, cap, src, dst, group, 5016, 5016, 64,
                                     body, n);

        case DMR_SMS_APP_IP_HC:
            /*
             * The five-octet compressed header a radio sends when the
             * UDP/IPv4 header is redundant: IPv4 identification, SAID/DAID
             * nibbles, source port index, destination port index.  Index 1 is
             * the text port (5016), so the ports do not follow.
             */
            if (cap < 6)
                return -1;
            out[0] = 0x00;
            out[1] = 0x01;
            out[2] = 0x00;
            out[3] = 0x01;
            out[4] = 0x01;
            n = dmrSms_utf8ToUtf16(text, out + 5, cap - 5, true, &cut);
            if (truncated != NULL)
                *truncated = cut;
            return (int)(5 + n);

        case DMR_SMS_APP_ANYTONE:
            if (cap < 28 + 6)
                return -1;
            body[0] = 0x00;
            body[1] = 0x0D;
            body[2] = 0x00;
            body[3] = 0x0A;
            n = dmrSms_utf8ToUtf16(text, body + 4, cap - 28 - 6, false, &cut);
            /* UTF-16 NUL, as AnyTone radios send */
            body[4 + n] = 0x00;
            body[5 + n] = 0x00;
            if (truncated != NULL)
                *truncated = cut;
            return (int)buildUdpIpv4(out, cap, src, dst, group, 5016, 5016, 1,
                                     body, n + 6);

        case DMR_SMS_APP_TMS: {
            /*
             * length | header | addr-len | addr | ext(seq low) |
             * ext(seq high, encoding) | CR LF in UCS-2 LE | text
             * The header is six octets and the length counts what follows it.
             * Type 0x1F with the peer's sequence number is the acknowledgement
             * of a received message: same record, no text.
             */
            uint8_t *rec = body;
            const size_t hdr = 6;
            size_t at = 0;
            uint8_t rh = 0x80; /* extensions follow */

            if (cap < 28 + hdr + 4)
                return -1;
            if (ackReq)
                rh |= 0x40; /* the sender wants an acknowledgement */
            if (tmsAck)
                rh |= 0x1F;

            rec[at++] = 0x00; /* length, filled at the end (BE) */
            rec[at++] = 0x00;
            rec[at++] = rh;
            rec[at++] = 0x00; /* address length 0 (radio to radio) */
            rec[at++] = (uint8_t)(0x80u | (tmsSeq & 0x1Fu)); /* seq low, ext */
            rec[at++] = (uint8_t)((((tmsSeq >> 5) & 0x03u) << 5) | 4u);

            if (!tmsAck) {
                /* CR LF in UCS-2 LE before the text */
                rec[at++] = 0x0D;
                rec[at++] = 0x00;
                rec[at++] = 0x0A;
                rec[at++] = 0x00;
                at += dmrSms_utf8ToUtf16(text, rec + at, cap - 28 - at, false,
                                         &cut);
                if (truncated != NULL)
                    *truncated = cut;
            }

            uint16_t len = (uint16_t)(at - 2);

            rec[0] = (uint8_t)(len >> 8);
            rec[1] = (uint8_t)(len & 0xFFu);
            return (int)buildUdpIpv4(out, cap, src, dst, group, 4007, 4007, 64,
                                     rec, at);
        }

        case DMR_SMS_APP_HYTERA:
            if (cap < 4)
                return -1;
            out[0] = 0x00; /* unknown 16-bit field, 0000 in the captures */
            out[1] = 0x00;
            n = dmrSms_utf8ToUtf16(text, out + 2, cap - 4, false, &cut);
            out[2 + n] = 0x00;
            out[3 + n] = 0x00; /* NUL terminator */
            if (truncated != NULL)
                *truncated = cut;
            return (int)(n + 4);

        case DMR_SMS_APP_RAW:
        default:
            return -1;
    }
}

/* ----------------------------------------------------------- decoding --- */

/** Compressed UDP/IPv4 header, TS 102 361-3 clause 7.2 (tables 7.14/7.17). */
static bool decodeCompressed(const uint8_t *d, size_t n,
                             struct dmrSmsMessage *m)
{
    uint8_t op1;
    uint8_t op2;
    int spidx;
    int dpidx;
    size_t ptr = 5;
    int sport = 0;
    int dport = 0;

    if (n < 5)
        return false;
    op1 = (uint8_t)((d[3] >> 7) & 1u);
    op2 = (uint8_t)((d[4] >> 7) & 1u);
    if (((op1 << 1) | op2) != 0)
        return false; /* only opcode 00 is defined */
    spidx = d[3] & 0x7F;
    dpidx = d[4] & 0x7F;

    /* Port index 0: the port follows in an extended header. */
    for (int k = 0; k < 2; k++) {
        int idx = (k == 0) ? spidx : dpidx;
        int *port = (k == 0) ? &sport : &dport;

        if (idx == 1) {
            *port = 5016; /* text */
        } else if (idx == 2) {
            *port = 5017; /* location */
        } else if ((idx == 0) && (ptr + 1 < n)) {
            *port = (d[ptr] << 8) | d[ptr + 1];
            ptr += 2;
        } else {
            return false;
        }
    }
    if (ptr > n)
        return false;
    m->udpSrc = sport;
    m->udpDst = dport;

    const uint8_t *p = d + ptr;
    size_t len = n - ptr;

    if (dport == 5016) {
        if ((len >= 4) && (p[0] == 0x00) && (p[1] == 0x0D) && (p[2] == 0x00)
            && (p[3] == 0x0A)) {
            m->app = DMR_SMS_APP_ANYTONE;
            m->textLen = (uint16_t)dmrSms_utf16ToUtf8(p + 4, len - 4, false,
                                                      m->text, DMR_SMS_TEXT_MAX,
                                                      &m->truncated);
            return true;
        }
        m->app = DMR_SMS_APP_IP;
        m->textLen = (uint16_t)dmrSms_utf16ToUtf8(p, len, true, m->text,
                                                  DMR_SMS_TEXT_MAX,
                                                  &m->truncated);
        return true;
    }
    m->app = DMR_SMS_APP_RAW;
    m->text[0] = '\0';
    m->textLen = 0;
    return false;
}

/** Motorola TMS record, UDP 4007. */
static bool decodeTms(const uint8_t *p, size_t n, struct dmrSmsMessage *m)
{
    int rlen;
    uint8_t rh;
    int type;
    size_t off = 3;
    int adl;
    int seq = -1;

    if (n < 4)
        return false;
    rlen = (p[0] << 8) | p[1];
    rh = p[2];
    type = rh & 0x1F;
    adl = p[off++];
    off += (size_t)adl;
    if ((rh & 0x80u) != 0) {
        if (off < n) {
            seq = p[off] & 0x1F;
            off++;
        }
        if (off < n) {
            int b = p[off++];

            seq = (seq & 0x1F) | ((b >> 5) << 5);
        }
    }
    m->app = DMR_SMS_APP_TMS;
    m->tmsSeq = seq;
    m->tmsAckReq = (rh & 0x40u) != 0;
    m->tmsAck = (type == 0x1F);

    if (type == 0x1F) {
        /* acknowledgement: no text, tmsSeq identifies what it answers */
        m->text[0] = '\0';
        m->textLen = 0;
        return true;
    }
    if (type != 0x00) {
        m->text[0] = '\0';
        m->textLen = 0;
        return false;
    }

    size_t avail = (n > off) ? n - off : 0;

    if ((rlen > 2) && ((size_t)(rlen - 2) < avail))
        avail = (size_t)(rlen - 2);
    avail &= ~(size_t)1;
    size_t len = dmrSms_utf16ToUtf8(p + off, avail, false, m->text,
                                    DMR_SMS_TEXT_MAX, &m->truncated);

    /* the record carries CR LF in front of the text */
    if ((len >= 2) && (m->text[0] == '\r') && (m->text[1] == '\n')) {
        memmove(m->text, m->text + 2, len - 1); /* includes the NUL */
        len -= 2;
    }
    m->textLen = (uint16_t)len;
    return true;
}

/** ETSI text over IP: plain IPv4/UDP or the compressed header. */
static bool decodeIpRecord(int sap, const uint8_t *d, size_t n,
                           struct dmrSmsMessage *m)
{
    if (sap == DMR_SMS_SAP_UDP_HC)
        return decodeCompressed(d, n, m);
    if ((n < 28) || ((d[0] >> 4) != 4))
        return false;

    int ihl = (d[0] & 0x0F) * 4;

    if ((ihl < 20) || ((size_t)ihl + 8 > n))
        return false;
    if (d[9] != 17) /* UDP */
        return false;

    const uint8_t *udp = d + ihl;
    uint16_t sport = get16(udp + 0);
    uint16_t dport = get16(udp + 2);
    uint16_t ulen = get16(udp + 4);

    if ((ulen < 8) || ((size_t)ihl + ulen > n))
        ulen = (uint16_t)(n - (size_t)ihl);
    const uint8_t *p = udp + 8;
    size_t len = (size_t)ulen - 8;

    m->udpSrc = sport;
    m->udpDst = dport;

    if ((dport == 4007) || (sport == 4007))
        return decodeTms(p, len, m);

    if ((dport == 5016) || (sport == 5016)) {
        if ((len >= 4) && (p[0] == 0x00) && (p[1] == 0x0D) && (p[2] == 0x00)
            && (p[3] == 0x0A)) {
            m->app = DMR_SMS_APP_ANYTONE;
            m->textLen = (uint16_t)dmrSms_utf16ToUtf8(p + 4, len - 4, false,
                                                      m->text, DMR_SMS_TEXT_MAX,
                                                      &m->truncated);
            return true;
        }
        m->app = DMR_SMS_APP_IP;
        m->textLen = (uint16_t)dmrSms_utf16ToUtf8(p, len, true, m->text,
                                                  DMR_SMS_TEXT_MAX,
                                                  &m->truncated);
        return true;
    }
    m->app = DMR_SMS_APP_RAW;
    m->text[0] = '\0';
    m->textLen = 0;
    return false;
}

bool dmrSms_parsePayload(int dpf, int sap, int dd, const uint8_t *p, size_t n,
                         struct dmrSmsMessage *m)
{
    m->truncated = false;
    m->text[0] = '\0';
    m->textLen = 0;

    if ((dpf == DMR_SMS_DPF_UNCONFIRMED) || (dpf == DMR_SMS_DPF_CONFIRMED)) {
        if ((sap == DMR_SMS_SAP_IP) || (sap == DMR_SMS_SAP_UDP_HC))
            return decodeIpRecord(sap, p, n, m);
        m->app = DMR_SMS_APP_RAW;
        return false;
    }

    if (dpf == DMR_SMS_DPF_SHORT_DEF) {
        /*
         * Hytera/AnyTone H-SMS: DD = BCD but the payload is two octets then
         * UTF-16LE, NUL terminated.
         */
        if ((dd == DMR_SMS_DD_BCD) && (n >= 6) && ((n % 2) == 0)) {
            size_t end = n;

            for (size_t i = 2; i + 1 < n; i += 2) {
                if ((p[i] == 0) && (p[i + 1] == 0)) {
                    end = i;
                    break;
                }
            }
            if (end > 2) {
                m->app = DMR_SMS_APP_HYTERA;
                m->textLen = (uint16_t)dmrSms_utf16ToUtf8(p + 2, end - 2, false,
                                                          m->text,
                                                          DMR_SMS_TEXT_MAX,
                                                          &m->truncated);
                return true;
            }
        }

        m->app = (dd == DMR_SMS_DD_UTF8) ? DMR_SMS_APP_DD_UTF8
                                         : DMR_SMS_APP_DD_UTF16BE;
        switch (dd) {
            case DMR_SMS_DD_UTF16BE:
                m->textLen = (uint16_t)dmrSms_utf16ToUtf8(p, n, true, m->text,
                                                          DMR_SMS_TEXT_MAX,
                                                          &m->truncated);
                return true;

            case DMR_SMS_DD_UTF16LE:
                m->textLen = (uint16_t)dmrSms_utf16ToUtf8(p, n, false, m->text,
                                                          DMR_SMS_TEXT_MAX,
                                                          &m->truncated);
                return true;

            case DMR_SMS_DD_UTF8:
                m->app = DMR_SMS_APP_DD_UTF8;
                for (size_t i = 0; (i < n) && (i < DMR_SMS_TEXT_MAX); i++) {
                    char c = (char)p[i];

                    if (c == '\0')
                        break;
                    m->text[m->textLen++] = c;
                }
                m->truncated = (n > DMR_SMS_TEXT_MAX);
                return true;

            case DMR_SMS_DD_8859_1:
            case DMR_SMS_DD_8859_15:
                m->textLen = (uint16_t)dmrSms_latin1ToUtf8(p, n, m->text,
                                                           DMR_SMS_TEXT_MAX,
                                                           &m->truncated);
                return true;

            case DMR_SMS_DD_7BIT:
                for (size_t i = 0; (i < n) && (i < DMR_SMS_TEXT_MAX); i++)
                    m->text[m->textLen++] = (char)(p[i] & 0x7Fu);
                m->truncated = (n > DMR_SMS_TEXT_MAX);
                return true;

            default:
                m->app = DMR_SMS_APP_RAW;
                return false;
        }
    }

    /* UDT (trunking short data) and everything else: no text handler */
    m->app = DMR_SMS_APP_RAW;
    return false;
}
