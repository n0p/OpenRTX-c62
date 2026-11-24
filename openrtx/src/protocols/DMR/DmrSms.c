/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrSms.c
 * @brief DMR short messages: data headers, packet assembly and reassembly,
 * the delivery response.  See DmrSms.h.
 *
 * Bit numbering follows the wire: bit 0 is the first transmitted bit and is
 * the MSB of octet 0, exactly as the figures of ETSI TS 102 361-1 draw it.
 * Header CRC: CRC-CCITT over the 80 header bits, XORed with the data type
 * mask 0xCCCC (clause B.3.12), placed MSB first in bits 80..95.  Packet CRC:
 * CRC-32 of clause B.3.9, whose octet pairs are processed second octet first.
 *
 * Portable C11: no allocation, no floating point, no libc beyond memcpy and
 * memset.  The whole packet is a fixed struct of 12 + 32 x 12 octets.
 */

#include <string.h>

#include "protocols/DMR/DmrDefs.h"
#include "protocols/DMR/DmrFec.h"
#include "protocols/DMR/DmrSms.h"
#include "DmrSmsApp.h"

/* ------------------------------------------------------------ bit helpers --- */

static void putBits(uint8_t *b, int off, int n, uint32_t v)
{
    for (int i = 0; i < n; i++)
        b[off + i] = (uint8_t)((v >> (n - 1 - i)) & 1u);
}

static uint32_t getBits(const uint8_t *b, int off, int n)
{
    uint32_t v = 0;

    for (int i = 0; i < n; i++)
        v = (v << 1) | (uint32_t)(b[off + i] & 1u);
    return v;
}

static void bitsToBytes(const uint8_t *bits, int nbits, uint8_t *out)
{
    memset(out, 0, (size_t)((nbits + 7) / 8));
    for (int i = 0; i < nbits; i++)
        if ((bits[i] & 1u) != 0)
            out[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
}

static void bytesToBits(const uint8_t *bytes, int nbytes, uint8_t *bits)
{
    for (int i = 0; i < nbytes * 8; i++)
        bits[i] = (uint8_t)((bytes[i >> 3] >> (7 - (i & 7))) & 1u);
}

const char *dmrSms_appName(enum dmrSmsApp app)
{
    switch (app) {
        case DMR_SMS_APP_DD_UTF16BE: return "ETSI-DD-UTF16BE";
        case DMR_SMS_APP_DD_UTF8:    return "ETSI-DD-UTF8";
        case DMR_SMS_APP_IP:         return "ETSI-IP";
        case DMR_SMS_APP_IP_HC:      return "ETSI-IP-HC";
        case DMR_SMS_APP_ANYTONE:    return "AnyTone";
        case DMR_SMS_APP_TMS:        return "Motorola-TMS";
        case DMR_SMS_APP_HYTERA:     return "Hytera-HSMS";
        case DMR_SMS_APP_RAW:        return "Raw";
    }
    return "?";
}

/* ---------------------------------------------------------------- header --- */

/*
 * The CRC field is computed by running the CRC-16 over the whole 96-bit block
 * with the field zeroed (the polynomial division of B.3.8); the data type
 * mask (B.3.12) is XORed in before the BPTC encoder sees it.
 */
static uint16_t headerCrc(const uint8_t bits96[96])
{
    return (uint16_t)(dmr_fec_crc16(bits96, 96) ^ 0xCCCCu);
}

/* A received block still carries the mask: undo it, then the residue of the
 * 96 bits must be zero. */
static bool headerResidueOk(const uint8_t bits96[96])
{
    uint8_t b[96];

    memcpy(b, bits96, 96);
    for (int i = 0; i < 16; i++)
        b[80 + i] ^= (uint8_t)((0xCCCCu >> (15 - i)) & 1u);
    return dmr_fec_crc16(b, 96) == 0;
}

/** Fields common to every data header. */
struct headerFields {
    int dpf;
    int sap;
    uint32_t dst;
    uint32_t src;
    bool group;
    bool ack;
};

/** Build the 96 bits of a data header, with its CRC. */
static void buildHeader(uint8_t bits[96], const struct headerFields *f,
                        int blocks, int pocOrPadding, int dd)
{
    memset(bits, 0, 96);

    putBits(bits, 0, 1, f->group ? 1u : 0u);           /* G/I */
    putBits(bits, 1, 1, f->ack ? 1u : 0u);             /* A */
    putBits(bits, 4, 4, (uint32_t)f->dpf);             /* DPF */
    putBits(bits, 8, 4, (uint32_t)f->sap);             /* SAP */
    putBits(bits, 16, 24, f->dst & 0xFFFFFFu);
    putBits(bits, 40, 24, f->src & 0xFFFFFFu);

    switch (f->dpf) {
        case DMR_SMS_DPF_UNCONFIRMED:
        case DMR_SMS_DPF_CONFIRMED: {
            int poc = pocOrPadding & 0x1F;

            putBits(bits, 3, 1, (uint32_t)((poc >> 4) & 1)); /* POC MSB */
            putBits(bits, 12, 4, (uint32_t)(poc & 0xF));     /* POC LSBs */
            putBits(bits, 64, 1, 1);                        /* F: first try */
            putBits(bits, 65, 7, (uint32_t)(blocks & 0x7F)); /* BF */
            /* 72..79: reserved (0) and FSN 0 on a first transmission */
            break;
        }
        case DMR_SMS_DPF_SHORT_DEF:
        case DMR_SMS_DPF_SHORT_RAW:
            putBits(bits, 2, 2, (uint32_t)((blocks >> 4) & 0x3)); /* AB MSBs */
            putBits(bits, 12, 4, (uint32_t)(blocks & 0xF));      /* AB LSBs */
            if (f->dpf == DMR_SMS_DPF_SHORT_DEF)
                putBits(bits, 64, 6, (uint32_t)(dd & 0x3F)); /* DD format */
            else
                putBits(bits, 64, 6, 0); /* SP/DP */
            putBits(bits, 70, 1, 0);     /* SARQ */
            putBits(bits, 71, 1, 1);     /* F */
            putBits(bits, 72, 8, (uint32_t)(pocOrPadding & 0xFF)); /* bits */
            break;
        default:
            putBits(bits, 2, 2, 0);
            putBits(bits, 12, 4, 0);
            putBits(bits, 64, 8, (uint32_t)(blocks & 0xFF));
            break;
    }

    putBits(bits, 80, 16, headerCrc(bits));
}

void dmrSms_buildResponse(uint8_t header[12], uint32_t dst, uint32_t src,
                          uint8_t sap, uint8_t respClass, uint8_t respType,
                          uint8_t status)
{
    uint8_t bits[96];

    memset(bits, 0, sizeof(bits));

    /* Table 9.13: reserved(1) A=0(1) reserved(2) Format(4) SAP(4) ... */
    putBits(bits, 4, 4, (uint32_t)DMR_SMS_DPF_RESPONSE);
    putBits(bits, 8, 4, (uint32_t)sap);
    putBits(bits, 16, 24, dst & 0xFFFFFFu);
    putBits(bits, 40, 24, src & 0xFFFFFFu);
    putBits(bits, 64, 1, 0); /* FMF: not the last of a multi-packet message */
    putBits(bits, 65, 7, 0); /* BF: no data blocks follow an ACK */
    putBits(bits, 72, 2, (uint32_t)(respClass & 0x3));
    putBits(bits, 74, 3, (uint32_t)(respType & 0x7));
    putBits(bits, 77, 3, (uint32_t)(status & 0x7));
    putBits(bits, 80, 16, headerCrc(bits));

    bitsToBytes(bits, 96, header);
}

/** Parse the 96 bits of a data header into the fields of the message. */
static void parseHeader(const uint8_t bits[96], struct dmrSmsMessage *m,
                        int *blocks, int *padding)
{
    m->dpf = (int)getBits(bits, 4, 4);
    m->sap = (int)getBits(bits, 8, 4);
    m->group = getBits(bits, 0, 1) != 0;
    m->confirmed = getBits(bits, 1, 1) != 0;
    m->dst = getBits(bits, 16, 24);
    m->src = getBits(bits, 40, 24);
    m->dd = -1;
    *blocks = 0;
    *padding = 0;

    switch (m->dpf) {
        case DMR_SMS_DPF_UNCONFIRMED:
        case DMR_SMS_DPF_CONFIRMED:
            *padding = (int)((getBits(bits, 3, 1) << 4) | getBits(bits, 12, 4));
            *blocks = (int)getBits(bits, 65, 7);
            break;

        case DMR_SMS_DPF_SHORT_DEF:
        case DMR_SMS_DPF_SHORT_RAW:
            *blocks = (int)((getBits(bits, 2, 2) << 4) | getBits(bits, 12, 4));
            if (m->dpf == DMR_SMS_DPF_SHORT_DEF)
                m->dd = (int)getBits(bits, 64, 6);
            *padding = (int)getBits(bits, 72, 8);
            break;

        case DMR_SMS_DPF_RESPONSE:
            /* class, type and status live where the pad count would be */
            *padding = 0;
            break;

        default:
            break;
    }
}

/* --------------------------------------------------- CRC-32 of the packet --- */

/*
 * TS 102 361-1 B.3.9: bit serial, polynomial 0x04C11DB7, initial value 0, no
 * final xor.  Octet pairs are processed second octet first, because the
 * 16-bit word of the message travels with its least significant octet ahead.
 * An odd tail octet is the low octet of a word whose high octet is zero.
 */
static uint32_t crc32Dmr(const uint8_t *data, size_t n)
{
    uint32_t crc = 0;

    for (size_t i = 0; i < n; i += 2) {
        uint8_t second = ((i + 1) < n) ? data[i + 1] : 0x00;
        uint8_t first = data[i];

        for (int b = 7; b >= 0; b--) {
            uint32_t top = (crc >> 31) & 1u;

            crc <<= 1;
            if ((top ^ (uint32_t)((second >> b) & 1u)) != 0)
                crc ^= 0x04C11DB7u;
        }
        for (int b = 7; b >= 0; b--) {
            uint32_t top = (crc >> 31) & 1u;

            crc <<= 1;
            if ((top ^ (uint32_t)((first >> b) & 1u)) != 0)
                crc ^= 0x04C11DB7u;
        }
    }
    return crc;
}

/** Least significant octet first, per figure B.8C. */
static void crc32Put(uint8_t out[4], uint32_t crc)
{
    out[0] = (uint8_t)(crc & 0xFFu);
    out[1] = (uint8_t)((crc >> 8) & 0xFFu);
    out[2] = (uint8_t)((crc >> 16) & 0xFFu);
    out[3] = (uint8_t)((crc >> 24) & 0xFFu);
}

static uint32_t crc32Wire(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

/* -------------------------------------------------------------- encoding --- */

/** Header shape of each application. */
static bool headerShape(enum dmrSmsApp app, bool confirmed, int *dpf, int *sap,
                        int *dd)
{
    *dd = -1;
    switch (app) {
        case DMR_SMS_APP_DD_UTF16BE:
            *dpf = DMR_SMS_DPF_SHORT_DEF;
            *sap = DMR_SMS_SAP_SHORT_DATA;
            *dd = DMR_SMS_DD_UTF16BE;
            return true;

        case DMR_SMS_APP_DD_UTF8:
            *dpf = DMR_SMS_DPF_SHORT_DEF;
            *sap = DMR_SMS_SAP_SHORT_DATA;
            *dd = DMR_SMS_DD_UTF8;
            return true;

        case DMR_SMS_APP_HYTERA:
            *dpf = DMR_SMS_DPF_SHORT_DEF;
            *sap = DMR_SMS_SAP_SHORT_DATA;
            *dd = DMR_SMS_DD_BCD;
            return true;

        case DMR_SMS_APP_IP:
            *dpf = confirmed ? DMR_SMS_DPF_CONFIRMED
                             : DMR_SMS_DPF_UNCONFIRMED;
            *sap = DMR_SMS_SAP_IP;
            return true;

        case DMR_SMS_APP_IP_HC:
            *dpf = DMR_SMS_DPF_UNCONFIRMED;
            *sap = DMR_SMS_SAP_UDP_HC;
            return true;

        case DMR_SMS_APP_ANYTONE:
        case DMR_SMS_APP_TMS:
            /*
             * TMS and AnyTone acknowledge in the application layer (a record
             * of type 0x1F), never at layer 2: the header is unconfirmed and
             * its A bit stays clear whatever the sender asked for.  Asking for
             * a layer 2 response here would make a real radio answer a data
             * call that it never opened.
             */
            *dpf = DMR_SMS_DPF_UNCONFIRMED;
            *sap = DMR_SMS_SAP_IP;
            return true;

        default:
            return false;
    }
}

/** One packet: header plus blocks, with the payload CRC-32 in the last one. */
static int assemble(struct dmrSmsPacket *out, const struct headerFields *f,
                    int dd, const uint8_t *payload, size_t len, bool truncated)
{
    uint8_t data[DMR_SMS_PAYLOAD_MAX];
    int blocks;
    int pad;
    size_t room;

    if ((f == NULL) || (len + 4 > DMR_SMS_PAYLOAD_MAX))
        return -1;

    blocks = (int)((len + 4 + 11) / 12);
    if (blocks < 1)
        blocks = 1;
    if (blocks > DMR_SMS_BLOCKS_MAX)
        return -1;

    memcpy(data, payload, len);
    room = (size_t)blocks * 12;
    pad = (int)(room - len - 4);
    if (pad < 0)
        pad = 0;
    memset(data + len, 0x00, (size_t)pad);

    uint8_t crc[4];

    crc32Put(crc, crc32Dmr(data, len + (size_t)pad));
    memcpy(data + len + (size_t)pad, crc, 4);

    /*
     * The padding is declared the way each header family carries it: Pad
     * Octet Count for DPF 2/3, Bit Padding for the short data headers.
     * Without it the receiver cannot trim the last block and the packet CRC
     * would cover the wrong span.
     */
    int declared;

    if ((f->dpf == DMR_SMS_DPF_UNCONFIRMED) || (f->dpf == DMR_SMS_DPF_CONFIRMED))
        declared = pad;
    else
        declared = pad * 8;

    uint8_t bits[96];

    buildHeader(bits, f, blocks, declared, dd);
    bitsToBytes(bits, 96, out->header);

    memset(out->blocks, 0, sizeof(out->blocks));
    for (int i = 0; i < blocks; i++)
        memcpy(out->blocks[i], data + (size_t)i * 12, 12);

    out->blockCount = (uint8_t)blocks;
    out->src = f->src;
    out->dst = f->dst;
    out->group = f->group;
    out->confirmed = f->ack;
    out->truncated = truncated;
    return 0;
}

int dmrSms_encode(const struct dmrSmsEncode *req, struct dmrSmsPacket *out)
{
    uint8_t payload[DMR_SMS_PAYLOAD_MAX];
    struct headerFields f;
    bool truncated = false;
    int dpf;
    int sap;
    int dd;
    int len;

    if ((req == NULL) || (out == NULL) || (req->text == NULL))
        return -1;
    if (!headerShape(req->app, req->confirmed, &dpf, &sap, &dd))
        return -1;

    memset(&f, 0, sizeof(f));
    f.dpf = dpf;
    f.sap = sap;
    f.dst = req->dst;
    f.src = req->src;
    f.group = req->group;
    /* the A bit only exists in the headers that can be answered at layer 2 */
    f.ack = req->confirmed
            && ((dpf == DMR_SMS_DPF_CONFIRMED) || (dpf == DMR_SMS_DPF_SHORT_DEF));

    len = dmrSms_buildPayload(req->app, req->src, req->dst, req->group,
                              req->text, req->tmsSeq, req->confirmed, false,
                              payload, sizeof(payload), &truncated);
    if (len <= 0)
        return -1;

    memset(out, 0, sizeof(*out));
    out->app = req->app;
    return assemble(out, &f, dd, payload, (size_t)len, truncated);
}

int dmrSms_encodeTmsAck(const struct dmrSmsEncode *req, struct dmrSmsPacket *out)
{
    uint8_t payload[DMR_SMS_PAYLOAD_MAX];
    struct headerFields f;
    int len;

    if ((req == NULL) || (out == NULL) || (req->app != DMR_SMS_APP_TMS))
        return -1;

    memset(&f, 0, sizeof(f));
    f.dpf = DMR_SMS_DPF_UNCONFIRMED;
    f.sap = DMR_SMS_SAP_IP;
    f.dst = req->dst;
    f.src = req->src;
    f.group = req->group;
    f.ack = false;

    len = dmrSms_buildPayload(DMR_SMS_APP_TMS, req->src, req->dst, req->group,
                              NULL, req->tmsSeq, false, true, payload,
                              sizeof(payload), NULL);
    if (len <= 0)
        return -1;

    memset(out, 0, sizeof(*out));
    out->app = DMR_SMS_APP_TMS;
    return assemble(out, &f, -1, payload, (size_t)len, false);
}

bool dmrSms_packetBurst(const struct dmrSmsPacket *pkt, unsigned index,
                        uint8_t *dataType, uint8_t info[96])
{
    if (pkt == NULL)
        return false;

    if (index == 0) {
        if (dataType != NULL)
            *dataType = DMR_DT_DATA_HEADER;
        bytesToBits(pkt->header, 12, info);
        return true;
    }
    if ((index > pkt->blockCount) || (index > DMR_SMS_BLOCKS_MAX))
        return false;
    if (dataType != NULL)
        *dataType = DMR_DT_RATE_12;
    bytesToBits(pkt->blocks[index - 1], 12, info);
    return true;
}

/* -------------------------------------------------------------- decoding --- */

int dmrSms_decodePacket(const uint8_t header[12],
                        const uint8_t blocks[][12], unsigned blockCount,
                        struct dmrSmsMessage *out)
{
    uint8_t hbits[96];
    uint8_t data[DMR_SMS_PAYLOAD_MAX];
    struct dmrSmsMessage m;
    int want;
    int padding;

    if ((header == NULL) || (blocks == NULL) || (out == NULL))
        return -1;

    bytesToBits(header, 12, hbits);
    if (!headerResidueOk(hbits))
        return -1;

    memset(&m, 0, sizeof(m));
    parseHeader(hbits, &m, &want, &padding);
    if ((want <= 0) || (want > DMR_SMS_BLOCKS_MAX)
        || (blockCount < (unsigned)want))
        return -1;

    memset(data, 0, sizeof(data));
    for (unsigned i = 0; i < (unsigned)want; i++)
        memcpy(data + i * 12, blocks[i], 12);

    size_t total = (size_t)want * 12;
    size_t pad = ((m.dpf == DMR_SMS_DPF_UNCONFIRMED)
                  || (m.dpf == DMR_SMS_DPF_CONFIRMED))
                     ? (size_t)padding
                     : (size_t)((padding + 7) / 8);

    if ((total < 4) || (pad > total - 4))
        return -1;

    /*
     * The CRC covers everything before the four CRC octets, padding included:
     * strip the declared padding only after checking it.
     */
    size_t body = total - 4;

    m.crcOk = crc32Wire(data + body) == crc32Dmr(data, body);
    m.blocks = (int)want;
    m.tmsSeq = -1;
    m.udpSrc = 0;
    m.udpDst = 0;

    if (!dmrSms_parsePayload(m.dpf, m.sap, m.dd, data, body - pad, &m)) {
        m.app = DMR_SMS_APP_RAW;
        m.text[0] = '\0';
        m.textLen = 0;
    }

    *out = m;
    return 0;
}

void dmrSmsRx_reset(struct dmrSmsRx *rx)
{
    if (rx == NULL)
        return;
    rx->haveHeader = false;
    rx->blockCount = 0;
    rx->want = 0;
    rx->dpf = 0;
    rx->state = -1;
    memset(rx->header, 0, sizeof(rx->header));
    memset(rx->blocks, 0, sizeof(rx->blocks));
}

/** Decode a C_RHEAD: class, type and status of a delivery response. */
static bool parseResponse(const uint8_t block[12], struct dmrSmsRxResult *out)
{
    uint8_t bits[96];
    struct dmrSmsMessage m;
    int want;
    int padding;

    bytesToBits(block, 12, bits);
    if (!headerResidueOk(bits))
        return false;

    memset(&m, 0, sizeof(m));
    parseHeader(bits, &m, &want, &padding);
    (void)padding;
    if (m.dpf != DMR_SMS_DPF_RESPONSE)
        return false;

    out->response = true;
    out->respClass = (uint8_t)getBits(bits, 72, 2);
    out->respType = (uint8_t)getBits(bits, 74, 3);
    out->respStatus = (uint8_t)getBits(bits, 77, 3);
    memset(&out->msg, 0, sizeof(out->msg));
    out->msg.dpf = m.dpf;
    out->msg.sap = m.sap;
    out->msg.dd = -1;
    out->msg.src = m.src; /* the peer that answered */
    out->msg.dst = m.dst; /* us */
    out->msg.group = m.group;
    out->msg.crcOk = true;
    out->msg.tmsSeq = -1;
    out->msg.app = DMR_SMS_APP_RAW;
    return true;
}

int dmrSmsRx_feed(struct dmrSmsRx *rx, uint8_t dataType, const uint8_t block[12],
                  struct dmrSmsRxResult *out)
{
    if ((rx == NULL) || (block == NULL) || (out == NULL))
        return 0;

    memset(out, 0, sizeof(*out));
    out->msg.tmsSeq = -1;

    if (dataType == DMR_DT_DATA_HEADER) {
        uint8_t bits[96];
        struct dmrSmsMessage m;
        int want;
        int padding;

        bytesToBits(block, 12, bits);
        if (!headerResidueOk(bits)) {
            dmrSmsRx_reset(rx); /* bad header CRC: ignore it */
            return 0;
        }

        memset(&m, 0, sizeof(m));
        parseHeader(bits, &m, &want, &padding);
        (void)padding;

        if (m.dpf == DMR_SMS_DPF_RESPONSE) {
            dmrSmsRx_reset(rx);
            return parseResponse(block, out) ? 1 : 0;
        }

        if ((want <= 0) || (want > DMR_SMS_BLOCKS_MAX)) {
            dmrSmsRx_reset(rx);
            return 0;
        }

        memcpy(rx->header, block, 12);
        rx->haveHeader = true;
        rx->dpf = (uint8_t)m.dpf;
        rx->want = (uint8_t)want;
        rx->blockCount = 0;
        rx->state = 0;
        out->headerWantsResponse = m.confirmed;
        return 0;
    }

    if (dataType != DMR_DT_RATE_12)
        return 0; /* voice, CSBK, UDT and single blocks are not ours */
    if (!rx->haveHeader)
        return 0;

    memcpy(rx->blocks[rx->blockCount], block, 12);
    rx->blockCount++;
    if (rx->blockCount < rx->want)
        return 0;

    /* packet complete: assemble, check the CRC and decode the payload */
    uint8_t header[12];
    unsigned count = rx->blockCount;
    int rc;

    memcpy(header, rx->header, 12);
    rc = dmrSms_decodePacket(header, (const uint8_t (*)[12])rx->blocks, count,
                             &out->msg);
    dmrSmsRx_reset(rx);
    if (rc < 0)
        return 0;

    out->message = true;
    return 1;
}
