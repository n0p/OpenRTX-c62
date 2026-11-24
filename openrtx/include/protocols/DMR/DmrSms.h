/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_SMS_H
#define OPENRTX_DMR_SMS_H

/**
 * @file DmrSms.h
 * @brief Short messages over DMR: encoder, decoder and the delivery response.
 *
 * A DMR short message is not a signalling message: the text rides in a data
 * call, as user data of rate 1/2 blocks behind a data header (ETSI TS 102
 * 361-1 clause 8.2, TS 102 361-3 clauses 5.7 and 6.4).  Seven payload
 * flavours are in the air and all seven are here, both ways:
 *
 *  | app                | header                     | payload                    |
 *  |--------------------|----------------------------|----------------------------|
 *  | DMR_SMS_APP_DD_UTF16BE | DD_HEAD, DPF 0xD, SAP 0xA | UTF-16BE                |
 *  | DMR_SMS_APP_DD_UTF8    | DD_HEAD, DD 0x12          | UTF-8                      |
 *  | DMR_SMS_APP_IP         | U_HEAD, DPF 2, SAP 4      | IPv4+UDP 5016, UTF-16BE     |
 *  | DMR_SMS_APP_IP_HC      | U_HEAD, DPF 2, SAP 3      | compressed UDP/IPv4, 5016   |
 *  | DMR_SMS_APP_ANYTONE    | U_HEAD, DPF 2, SAP 4      | UDP 5016, 00 0d 00 0a + LE  |
 *  | DMR_SMS_APP_TMS        | U_HEAD, DPF 2, SAP 4      | UDP 4007, TMS record, UCS-2 |
 *  | DMR_SMS_APP_HYTERA     | DD_HEAD, DD BCD           | 2 octets + UTF-16LE + NUL   |
 *
 * The burst layer is not here: dmrSms_encode() stops at the 96-bit blocks of
 * the data header and of each rate 1/2 block, and OpMode_DMR hands them to
 * the transmit chain (DmrTx.h) slot by slot.  Nothing allocates: a packet is
 * a fixed struct, and the receiver keeps one struct per timeslot.
 *
 * Everything is bit-exact with the host prototype in work/dmr-sms (see
 * tests/unit/dmr_sms.cpp): the CRC-16 of the header with its 0xCCCC mask, the
 * packet CRC-32 with its second-octet-first words and the seven payload
 * layouts.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/datatypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ constants --- */

/** UTF-8 bytes kept per message, terminator excluded. */
#define DMR_SMS_TEXT_MAX 160

/** Rate 1/2 blocks per message: 32 x 12 = 384 payload octets, the largest
 *  text that fits with the longest header (IPv4/UDP plus 160 UTF-16 units). */
#define DMR_SMS_BLOCKS_MAX 32

/** Octets of user data a packet can carry. */
#define DMR_SMS_PAYLOAD_MAX (DMR_SMS_BLOCKS_MAX * 12)

/* Data Packet Format, TS 102 361-1 table 6.1 of -3. */
enum dmrSmsDpf {
    DMR_SMS_DPF_UDT = 0x0,         ///< Unified Data Transport (trunking)
    DMR_SMS_DPF_RESPONSE = 0x1,    ///< C_RHEAD / C_RDATA: delivery response
    DMR_SMS_DPF_UNCONFIRMED = 0x2, ///< U_HEAD
    DMR_SMS_DPF_CONFIRMED = 0x3,   ///< C_HEAD
    DMR_SMS_DPF_SHORT_DEF = 0xD,   ///< DD_HEAD: defined short data
    DMR_SMS_DPF_SHORT_RAW = 0xE,   ///< R_HEAD / SP_HEAD
    DMR_SMS_DPF_PROPRIETARY = 0xF,
};

/* Service access point, TS 102 361-1 table 9.31. */
enum dmrSmsSap {
    DMR_SMS_SAP_UDT = 0x0,
    DMR_SMS_SAP_TCP_HC = 0x2,
    DMR_SMS_SAP_UDP_HC = 0x3,
    DMR_SMS_SAP_IP = 0x4,
    DMR_SMS_SAP_ARP = 0x5,
    DMR_SMS_SAP_PROPRIETARY = 0x9,
    DMR_SMS_SAP_SHORT_DATA = 0xA,
};

/* Defined Data format, TS 102 361-1 table 9.50. */
enum dmrSmsDd {
    DMR_SMS_DD_BCD = 0x01,
    DMR_SMS_DD_7BIT = 0x02,
    DMR_SMS_DD_8859_1 = 0x03,
    DMR_SMS_DD_8859_15 = 0x11,
    DMR_SMS_DD_UTF8 = 0x12,
    DMR_SMS_DD_UTF16BE = 0x14,
    DMR_SMS_DD_UTF16LE = 0x15,
};

/* Layer 2 response, TS 102 361-1 table 9.13 and TS 102 361-3 table 6.6. */
enum dmrSmsRespClass {
    DMR_SMS_RESP_ACK_CLASS = 0x0,  ///< Class 00: ACK
    DMR_SMS_RESP_NACK_CLASS = 0x1, ///< Class 01: NACK
    DMR_SMS_RESP_SACK_CLASS = 0x2, ///< Class 10: selective retry
};

enum dmrSmsRespType {
    DMR_SMS_RESP_TYPE_ACK = 0x1,         ///< Type 001 with class 00
    DMR_SMS_RESP_TYPE_NACK_FORMAT = 0x0, ///< Illegal format
    DMR_SMS_RESP_TYPE_NACK_CRC = 0x1,    ///< Packet CRC failed
    DMR_SMS_RESP_TYPE_NACK_FULL = 0x2,   ///< Recipient memory full
    DMR_SMS_RESP_TYPE_NACK_UNDELIV = 0x4,
};

/** What a message turned out to carry. */
enum dmrSmsApp {
    DMR_SMS_APP_DD_UTF16BE = 0, ///< DD_HEAD with UTF-16BE text (ETSI)
    DMR_SMS_APP_DD_UTF8,        ///< DD_HEAD with UTF-8 text
    DMR_SMS_APP_IP,             ///< ETSI text over IP, UDP 5016, UTF-16BE
    DMR_SMS_APP_IP_HC,          ///< The same with the compressed header (SAP 3)
    DMR_SMS_APP_ANYTONE,        ///< UDP 5016, 00 0d 00 0a + UTF-16LE
    DMR_SMS_APP_TMS,            ///< Motorola TMS over UDP 4007
    DMR_SMS_APP_HYTERA,         ///< Hytera H-SMS: DD=BCD + UTF-16LE
    DMR_SMS_APP_RAW,            ///< Decoded data we have no text handler for
};

/* ------------------------------------------------------------- encoding --- */

/** What to send, and for whom. */
struct dmrSmsEncode {
    enum dmrSmsApp app; ///< Payload flavour to build.
    uint32_t src;       ///< Source radio ID, 24 bits.
    uint32_t dst;       ///< Destination unit or talkgroup, 24 bits.
    bool group;         ///< Destination is a talkgroup.
    bool confirmed;     ///< Ask for a layer 2 response (A = 1).
    uint8_t tmsSeq;     ///< TMS sequence number, 1..31 (TMS only).
    const char *text;   ///< UTF-8 text, NUL terminated. NULL for a TMS ACK.
};

/** A complete packet ready to be cut into bursts. */
struct dmrSmsPacket {
    enum dmrSmsApp app;
    uint32_t src;
    uint32_t dst;
    bool group;
    bool confirmed;    ///< The header asks for a response.
    bool truncated;    ///< The text did not fit and was cut.
    uint8_t header[12];                  ///< 96 header bits, packed MSB first
    uint8_t blocks[DMR_SMS_BLOCKS_MAX][12]; ///< 96 bits each, CRC-32 in the last
    uint8_t blockCount;                  ///< Data blocks, 1..DMR_SMS_BLOCKS_MAX
};

/**
 * Build the packet of a message: header, data blocks, CRC-32 of the packet.
 *
 * @param req: what to send.
 * @param out: destination packet.
 * @return 0 on success, -1 if the application is unknown or the text does not
 *         fit even after truncation (only an empty text can fail that way).
 */
int dmrSms_encode(const struct dmrSmsEncode *req, struct dmrSmsPacket *out);

/**
 * Build the TMS acknowledgement record of a message received from a peer:
 * the same record type 0x1F, addressed back, with the peer's sequence
 * number.  Nothing is transmitted by asking for it: the caller queues it.
 *
 * @param req: app must be DMR_SMS_APP_TMS, src/dst as on the wire, tmsSeq
 *             the sequence number of the message being acknowledged.
 * @param out: destination packet.
 * @return 0 on success, -1 on bad arguments.
 */
int dmrSms_encodeTmsAck(const struct dmrSmsEncode *req, struct dmrSmsPacket *out);

/**
 * Build the layer 2 response (C_RHEAD) of a received message.
 *
 * @param header: destination, the 96 bits of the response header.
 * @param dst: who receives the response (the original sender).
 * @param src: who sends it (us).
 * @param sap: SAP of the message being answered.
 * @param respClass: enum dmrSmsRespClass.
 * @param respType: enum dmrSmsRespType.
 * @param status: status field, 0 for a plain ACK.
 */
void dmrSms_buildResponse(uint8_t header[12], uint32_t dst, uint32_t src,
                          uint8_t sap, uint8_t respClass, uint8_t respType,
                          uint8_t status);

/**
 * The data type and 96 information bits of one burst of a packet.
 *
 * @param pkt: the packet.
 * @param index: 0 is the data header, 1..blockCount the data blocks.
 * @param dataType: destination of the burst data type (0x6 or 0x7).
 * @param info: destination, 96 bits, one per element.
 * @return false if the index is out of range.
 */
bool dmrSms_packetBurst(const struct dmrSmsPacket *pkt, unsigned index,
                        uint8_t *dataType, uint8_t info[96]);

/* ------------------------------------------------------------- decoding --- */

/** A decoded message. */
struct dmrSmsMessage {
    enum dmrSmsApp app;
    uint32_t src;
    uint32_t dst;
    bool group;
    bool confirmed; ///< The header asked for a response.
    bool crcOk;     ///< Packet CRC-32 (or, for a response, the header CRC).
    bool truncated; ///< The text did not fit in the buffer.
    int blocks;     ///< Data blocks the packet carried.
    int dpf;
    int sap;
    int dd;          ///< DD format of a short data header, -1 otherwise.
    int udpSrc;      ///< Ports, when the payload was IP.
    int udpDst;
    int tmsSeq;      ///< TMS sequence number, -1 when not TMS.
    bool tmsAck;     ///< The record is a TMS acknowledgement (type 0x1F).
    bool tmsAckReq;  ///< The sender asked for an acknowledgement.
    char text[DMR_SMS_TEXT_MAX + 1]; ///< UTF-8, NUL terminated.
    uint16_t textLen;
};

/** Outcome of feeding one block to the reassembler. */
struct dmrSmsRxResult {
    /** True when this block completed a message (see msg). */
    bool message;
    /** True when the block was a layer 2 response (C_RHEAD): class, type and
     *  status are valid, msg.dst is the original sender of the data. */
    bool response;
    /** True when this block was a data header that asks for a response. */
    bool headerWantsResponse;
    uint8_t respClass;
    uint8_t respType;
    uint8_t respStatus;
    struct dmrSmsMessage msg;
};

/** Reassembly state of one timeslot.  Two of these are needed: TS1 and TS2
 *  can carry different data calls. */
struct dmrSmsRx {
    bool haveHeader;
    uint8_t header[12]; ///< 96 bits of the data header, packed
    uint8_t blocks[DMR_SMS_BLOCKS_MAX][12];
    uint8_t blockCount;
    uint8_t want;     ///< Blocks the header announced
    uint8_t dpf;      ///< Data packet format of the header
    int8_t state;     ///< -1 idle, 0 collecting, 1 done
};

/** Drop the state of a reassembler (new frequency, mode change). */
void dmrSmsRx_reset(struct dmrSmsRx *rx);

/**
 * Feed one decoded data burst.
 *
 * @param rx: per timeslot state.
 * @param dataType: burst data type from the slot type (0x6 header, 0x7 rate
 *                  1/2 data, anything else is ignored).
 * @param block: 12 octets, the 96 information bits of the burst.
 * @param out: result; only valid when the return value is 1.
 * @return 1 when a message or a response was completed, 0 otherwise.
 */
int dmrSmsRx_feed(struct dmrSmsRx *rx, uint8_t dataType, const uint8_t block[12],
                  struct dmrSmsRxResult *out);

/**
 * Decode a whole packet at once, for tests and for stored payloads.
 *
 * @param header: 12 octets of header.
 * @param blocks: blockCount x 12 octets of data.
 * @param blockCount: number of data blocks.
 * @param out: decoded message.
 * @return 0 if the message decoded (crcOk tells whether it is intact), -1 if
 *         the header or the payload could not be parsed.
 */
int dmrSms_decodePacket(const uint8_t header[12],
                        const uint8_t blocks[][12], unsigned blockCount,
                        struct dmrSmsMessage *out);

/** @return a stable short name of the application, for logs and the UI. */
const char *dmrSms_appName(enum dmrSmsApp app);

/* -------------------------------------------------------------- settings --- */

/**
 * Payload format to build for a value of settings.sms_format (enum
 * dmrSmsFormat: 0 = auto).  Auto is the format this firmware has always sent,
 * DMR_SMS_APP_IP_HC, so that an upgrade changes nothing on the air until the
 * operator picks something else.
 *
 * @param format: enum dmrSmsFormat value from the settings.
 * @return the corresponding application, DMR_SMS_APP_IP_HC for AUTO and for an
 *         unknown value (a corrupt settings record must not stop the SMS).
 */
enum dmrSmsApp dmrSms_appForFormat(uint8_t format);

/**
 * True if a device that receives `app` can be expected to read our text: the
 * two defined data formats and the four IP flavours are text; the raw one is
 * not.
 */
bool dmrSms_appIsText(enum dmrSmsApp app);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_SMS_H */
