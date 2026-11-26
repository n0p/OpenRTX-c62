/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PC_PROTO_H
#define PC_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file pc_proto.h
 * @brief Binary protocol between the radio and a computer, for a CPS.
 *
 * The radio's programming cable is the console UART, which the firmware also
 * uses for the log and, in capture builds, for the baseband dump.  This layer
 * puts a real protocol on top of that byte stream: framed, versioned and
 * extensible, so that a CPS on the PC can read and write the codeplug, follow
 * the radio live and receive events, without any of it being ad-hoc text.
 *
 * The whole of this file is portable C: framing, dispatch and the message
 * table live in openrtx/src/core/pc_proto.c and pc_proto_ops.c and are built
 * into the host test binaries as well as the firmware.  The transport (a
 * thread reading the UART, and the log arbitration) is the platform's job, see
 * platform/targets/c62/pc_proto_c62.c, docs/PC_PROTO.md.
 *
 * @section frame Frame format
 *
 * A frame is SLIP: 0xC0 END, 0xDB ESC, 0xDB 0xDC for an escaped END and
 * 0xDB 0xDD for an escaped ESC, sent as `END body END`.  Escaping makes the
 * delimiter unambiguous, so the receiver resynchronises on the first END that
 * follows garbage -- no magic to hunt for and no false lock on a byte pattern.
 * Inside the envelope the body carries its own length, so a frame cut short is
 * rejected before any field is parsed, and a CRC-16/CCITT-FALSE over the body
 * catches everything else (a corrupted length would otherwise re-frame the
 * rest of the stream).  See @ref pc_proto_encode.
 *
 * @section body Body layout, little endian
 *
 *   0  u8   envelope version (PC_PROTO_FRAME_VERSION)
 *   1  u8   type: 1 request, 2 response, 3 event
 *   2  u8   sequence: chosen by the client, echoed in the response
 *   3  u16  message identifier, see enum pcProtoMsg
 *   5  u8   flags: PC_PROTO_FLAG_MORE when a list has more entries
 *   6  u16  payload length n
 *   8  n    payload: a list of TLVs (tag u16, length u16, value)
 *   8+n u16 CRC-16/CCITT-FALSE over bytes 0 .. 8+n
 *
 * @section degrade Versioning
 *
 * A client opens the session with HELLO, carrying the protocol version it
 * speaks and the capabilities it implements.  The firmware answers with the
 * version it speaks, the oldest one it still serves, its capabilities and the
 * sizes of its stores.  A client newer than the firmware is served as long as
 * its version is not below PC_PROTO_VERSION_MIN; a message the firmware does
 * not know is answered with PC_ERR_UNKNOWN_MSG and one it knows but this build
 * cannot serve with PC_ERR_NOT_SUPPORTED, both carrying the identifier in
 * PC_TAG_MSG_ID, so a client can name what it lost instead of hanging.
 */

/** Protocol version this build speaks. */
#define PC_PROTO_VERSION 1

/** Oldest protocol version still served: clients below it get PC_ERR_VERSION. */
#define PC_PROTO_VERSION_MIN 1

/** Version of the frame envelope, byte 0 of every body. */
#define PC_PROTO_FRAME_VERSION 1

/** Largest body accepted or produced, escapes excluded. 400 bytes keeps the
 *  whole instance near 1,2 kB of RAM: the receive body, the transmit frame
 *  (worst case escapes included) and the response payload. */
#define PC_PROTO_MAX_BODY 400

/** Largest payload a body can carry (body minus header and CRC). */
#define PC_PROTO_MAX_PAYLOAD (PC_PROTO_MAX_BODY - 10)

/* Framing bytes, SLIP as in RFC 1055. */
#define PC_PROTO_END     0xC0
#define PC_PROTO_ESC     0xDB
#define PC_PROTO_ESC_END 0xDC
#define PC_PROTO_ESC_ESC 0xDD

/** Frame types, body byte 1. */
enum pcProtoType
{
    PC_TYPE_REQUEST  = 1,
    PC_TYPE_RESPONSE = 2,
    PC_TYPE_EVENT    = 3
};

/** Body flags, body byte 5. */
enum pcProtoFlags
{
    PC_PROTO_FLAG_MORE = 0x01 /**< The list in this payload continues     */
};

/**
 * Message identifiers, in ranges so that two unrelated features never pick the
 * same number:
 *
 *   0x0000-0x00FF  core: session, liveness, errors, events
 *   0x0100-0x01FF  CPS: channels, contacts, zones, settings, live status
 *   0x0200-0x02FF  messaging: inbox, outbox, compose
 *   0x0300-0x03FF  packet modem (reserved)
 *   0x0400-0x04FF  analyser and spectrum (reserved)
 *   0x0500-0xFEFF  free for the next feature, one block each
 *
 * Adding a message is three steps, see docs/PC_PROTO.md.
 */
enum pcProtoMsg
{
    /* Core */
    PC_MSG_HELLO           = 0x0001, /**< req: u16 version, u32 caps, str
                                      *   model; resp: identity and stores  */
    PC_MSG_PING            = 0x0002, /**< liveness, resp: uptime, counters  */
    PC_MSG_SESSION_CLOSE   = 0x0003, /**< release the link (log comes back) */
    PC_MSG_COUNTERS        = 0x0004, /**< resp: framing error counters      */

    /* Events (type 3, never answered) */
    PC_EVT_STATUS          = 0x0080, /**< live status, on change or on a
                                      *   subscription period             */
    PC_EVT_CHANNEL         = 0x0081, /**< selected channel changed          */
    PC_EVT_PTT             = 0x0082, /**< transmit state changed            */
    PC_EVT_MESSAGE         = 0x0083, /**< a message arrived                 */
    PC_EVT_SETTINGS        = 0x0084, /**< settings changed on the radio     */
    PC_EVT_LOG             = 0x0085, /**< one line of the firmware log      */
    PC_EVT_BENCH_EDGE      = 0x0086, /**< bench: TX/RX edge with its radio
                                      *   tick, see PC_MSG_BENCH_PTT       */

    /** Response used for every failed request; payload: PC_TAG_ERROR and
     *  PC_TAG_MSG_ID of the request that failed. */
    PC_MSG_ERROR           = 0x00FF,

    /* CPS, stores */
    PC_MSG_CHAN_COUNT      = 0x0101,
    PC_MSG_CHAN_GET        = 0x0102, /**< req: PC_TAG_INDEX; resp: record   */
    PC_MSG_CHAN_SET        = 0x0103, /**< req: index + record, verified     */
    PC_MSG_CHAN_ADD        = 0x0104, /**< req: record; resp: new index      */
    PC_MSG_CHAN_DELETE     = 0x0105, /**< req: index                        */
    PC_MSG_CHAN_LIST       = 0x0106, /**< req: first, count; resp: array    */
    PC_MSG_CHAN_WRITE_BLOCK= 0x0107, /**< req: first + array, each verified */
    PC_MSG_CHAN_LASTUSED   = 0x0108, /**< get (no index) or set (index)     */

    PC_MSG_CONTACT_COUNT   = 0x0110,
    PC_MSG_CONTACT_GET     = 0x0111,
    PC_MSG_CONTACT_SET     = 0x0112,
    PC_MSG_CONTACT_ADD     = 0x0113,
    PC_MSG_CONTACT_DELETE  = 0x0114,
    PC_MSG_CONTACT_LIST    = 0x0115,

    PC_MSG_ZONE_COUNT      = 0x0120,
    PC_MSG_ZONE_GET        = 0x0121, /**< req: zone id; resp: name, kind    */
    PC_MSG_ZONE_CHANNEL    = 0x0122, /**< req: zone, index; resp: entry     */
    PC_MSG_ZONE_IMPORT     = 0x0123, /**< copy a zone channel into the bank */

    PC_MSG_SETTINGS_GET    = 0x0130, /**< req: optional field tags          */
    PC_MSG_SETTINGS_SET    = 0x0131, /**< req: field tags, verified in RAM  */
    PC_MSG_SETTINGS_INFO   = 0x0132, /**< version, size, CRC, reset cause   */

    PC_MSG_STATUS_GET      = 0x0140,
    PC_MSG_STATUS_SUBSCRIBE= 0x0141, /**< req: PC_TAG_PERIOD_MS, 0 = off    */

    PC_MSG_STATS_GET       = 0x0150, /**< per-mode counters, if compiled    */

    /* Messaging */
    PC_MSG_MSG_COUNT       = 0x0201, /**< resp: inbox, outbox, unread       */
    PC_MSG_MSG_INBOX       = 0x0202, /**< req: index; resp: one message     */
    PC_MSG_MSG_OUTBOX      = 0x0203,
    PC_MSG_MSG_SEND        = 0x0204, /**< req: transport, peer, text        */
    PC_MSG_MSG_DELETE      = 0x0205, /**< req: id: drop from a queue        */
    PC_MSG_MSG_MARK_READ   = 0x0206,

    /* Packet modem: reserved. Each handler reports NOT_SUPPORTED until the
     * feature is built in; the identifiers are assigned now so that its own
     * branch does not have to guess. */
    PC_MSG_PM_TX           = 0x0301,
    PC_MSG_PM_STATUS       = 0x0302,

    /* Analyser and spectrum: reserved, same rule. */
    PC_MSG_ANA_SPECTRUM    = 0x0401,
    PC_MSG_ANA_STATS       = 0x0402,

    /* Debug namespace.  Everything here is behind PC_CAP_DEBUG and, for the
     * writes, behind the session token: see docs/PC_PROTO.md.  With the
     * capability off every one of these answers PC_ERR_NOT_ALLOWED and no
     * radio data travels in the response. */
    PC_MSG_DBG_INFO        = 0x0501, /**< versions, reset cause, watchdog,
                                      *   heap, PSRAM, flash, stacks, stats */
    PC_MSG_DBG_LOG_SUB     = 0x0502, /**< subscribe: level, module, rate     */
    PC_MSG_DBG_LOG_DRAIN   = 0x0503, /**< take the buffered lines now        */
    PC_MSG_DBG_REG_READ    = 0x0510, /**< one BK4819 or BK1080 register      */
    PC_MSG_DBG_REG_WRITE   = 0x0511, /**< whitelisted write + token + confirm*/
    PC_MSG_DBG_AGC         = 0x0512, /**< RSSI and AGC state                 */
    PC_MSG_DBG_MEM_READ    = 0x0520, /**< bounded block of RAM               */
    PC_MSG_DBG_CAP_START   = 0x0530, /**< start an RX audio capture          */
    PC_MSG_DBG_CAP_STOP    = 0x0531, /**< stop it                            */
    PC_MSG_DBG_CAP_READ    = 0x0532, /**< read samples out of the capture    */
    PC_MSG_DBG_BB          = 0x0540, /**< baseband dump: state and trigger   */
    PC_MSG_DBG_TX_TRACE    = 0x0550, /**< live transmit cycle trace          */
    PC_MSG_DBG_RX_COUNTERS = 0x0551, /**< per-mode receive error counters    */
    PC_MSG_DBG_BLACKBOX    = 0x0552, /**< the post-mortem the last crash
                                      *   left: req PC_TAG_OFFSET, resp
                                      *   PC_TAG_BLACKBOX_LEN + that chunk
                                      *   in PC_TAG_BLACKBOX.  Read it in
                                      *   a loop from offset 0 until the
                                      *   length says it is complete.
                                      *   docs/BLACKBOX.md               */

    /* Bench namespace, 0x0600-0x060F: scripted measurements of the radio
     * against an external receiver (RTL-SDR, Anritsu...).  Every one of these
     * is behind PC_CAP_BENCH, which only a build with CONFIG_C62_BENCH_PROTO
     * advertises, and the ones that move the radio (TUNE, PTT, SWEEP) also
     * need the session token.  See docs/BENCH.md. */
    PC_MSG_BENCH_ARM       = 0x0601, /**< start a run: epoch + radio tick    */
    PC_MSG_BENCH_TUNE      = 0x0602, /**< req: freq, mode, bw; token         */
    PC_MSG_BENCH_PTT       = 0x0603, /**< req: tx on/off, tone, level; token */
    PC_MSG_BENCH_CAPTURE   = 0x0604, /**< arm a capture of N ms, then read it*/
    PC_MSG_BENCH_RSSI      = 0x0605, /**< n samples of (tick, RSSI)          */
    PC_MSG_BENCH_SWEEP     = 0x0606, /**< rx sweep over (f0, f1, step)       */
    PC_MSG_BENCH_REPORT    = 0x0607  /**< what the run collected, as text    */
};

/** Capability bits: what this build can serve, HELLO asks and answers. */
enum pcProtoCap
{
    PC_CAP_CHANNELS  = 0x00000001, /**< channel bank                     */
    PC_CAP_CONTACTS  = 0x00000002, /**< contact book                     */
    PC_CAP_MESSAGES  = 0x00000004, /**< inbox, outbox, compose           */
    PC_CAP_SETTINGS  = 0x00000008, /**< settings, whole record and fields*/
    PC_CAP_ZONES     = 0x00000010, /**< built-in channel plans           */
    PC_CAP_STATS     = 0x00000020, /**< per-mode receive statistics      */
    PC_CAP_PACKET_MODEM = 0x00000040,
    PC_CAP_ANALYZER  = 0x00000080,
    PC_CAP_EVENTS    = 0x00000100, /**< asynchronous events              */
    PC_CAP_CHAN_BLOCK= 0x00000200, /**< channel read/write in blocks     */
    PC_CAP_MSG_SEND  = 0x00000400, /**< queue a message from the PC      */
    PC_CAP_SETTINGS_FIELDS = 0x00000800, /**< settings as named fields   */
    PC_CAP_ZONE_IMPORT = 0x00001000, /**< copy a zone entry into the bank */
    /**
     * The debug namespace: log streaming, diagnostics, register and memory
     * access, capture, TX trace.  Compiled in only when the build option
     * C62_DEBUG_PROTO is on (CONFIG_C62_DEBUG_PROTO); a production build
     * does not advertise it and answers every one of those messages with
     * PC_ERR_NOT_ALLOWED.  The writes need the session token on top.
     */
    PC_CAP_DEBUG     = 0x00002000,

    /**
     * The bench namespace: scripted measurements (tune, key, capture, sweep,
     * RSSI) with a radio tick on every response so the PC can line the radio
     * up against an external receiver.  Compiled in only with the build option
     * C62_BENCH_PROTO (CONFIG_C62_BENCH_PROTO); without it the messages answer
     * PC_ERR_NOT_ALLOWED and nothing is advertised.  TUNE, PTT and SWEEP also
     * need the session token: they move the radio and key the PA.
     */
    PC_CAP_BENCH     = 0x00004000
};

/** Error codes, PC_TAG_ERROR in a response. */
enum pcProtoError
{
    PC_OK                  = 0,
    PC_ERR_BAD_REQUEST     = 1,  /**< malformed frame or TLV             */
    PC_ERR_UNKNOWN_MSG     = 2,  /**< identifier not in the table        */
    PC_ERR_NOT_SUPPORTED   = 3,  /**< known, not in this build           */
    PC_ERR_BAD_INDEX       = 4,  /**< slot out of range                  */
    PC_ERR_INVALID_VALUE   = 5,  /**< the store refused it               */
    PC_ERR_IO              = 6,  /**< write failed or read-back mismatch */
    PC_ERR_VERSION         = 7,  /**< protocol version below the minimum */
    PC_ERR_BUSY            = 8,  /**< try again                          */
    PC_ERR_TOO_LARGE       = 9,  /**< payload does not fit the frame     */
    PC_ERR_PERMISSION      = 10, /**< disabled on the radio              */
    PC_ERR_NOT_ALLOWED     = 11  /**< debug capability off or bad token  */
};

/**
 * TLV tags.  A tag implies its type: the reader helpers check the width, so a
 * client that sends the wrong one gets PC_ERR_BAD_REQUEST instead of a
 * silently misread field.  Ranges: 0x0000-0x00FF the session and its scalars,
 * 0x0010-0x003F identifiers and counts, 0x0040-0x007F records and arrays,
 * 0x0080-0x00BF live status, 0x00C0-0x00FF settings fields.
 */
enum pcProtoTag
{
    PC_TAG_ERROR        = 0x0001, /**< u8,  enum pcProtoError              */
    PC_TAG_STATUS       = 0x0002, /**< u8,  PC_OK in a successful response */
    PC_TAG_VERSION      = 0x0003, /**< u16 protocol version                */
    PC_TAG_VERSION_MIN  = 0x0004, /**< u16 oldest version served           */
    PC_TAG_MSG_ID       = 0x0005, /**< u16 identifier being answered       */
    PC_TAG_MODEL        = 0x0006, /**< str model name                      */
    PC_TAG_FIRMWARE     = 0x0007, /**< str firmware version                */
    PC_TAG_HARDWARE     = 0x0008, /**< str hardware revision               */
    PC_TAG_CAPS         = 0x0009, /**< u32 capability bitmap               */
    PC_TAG_UPTIME_MS    = 0x000A, /**< u32                                 */
    PC_TAG_VERIFY       = 0x000B, /**< u8, 1 read back and equal           */

    PC_TAG_INDEX        = 0x0010, /**< u16 slot                            */
    PC_TAG_FIRST        = 0x0011, /**< u16 first slot of a block           */
    PC_TAG_COUNT        = 0x0012, /**< u16 entries in the payload          */
    PC_TAG_ZONE         = 0x0013, /**< u8  zone identifier                 */
    PC_TAG_ZONE_KIND    = 0x0014, /**< u8  enum zoneId                     */
    PC_TAG_ZONE_NAME    = 0x0015, /**< str zone description                */
    PC_TAG_ZONE_PREFIX  = 0x0016, /**< str channel name prefix             */
    PC_TAG_ZONE_COUNT   = 0x0017, /**< u16 channels in a zone              */
    PC_TAG_CHCOUNT      = 0x0018, /**< u16 channels in the bank            */
    PC_TAG_CTCOUNT      = 0x0019, /**< u16 contacts in the book            */
    PC_TAG_SETTINGS_VER = 0x001A, /**< u16 SETTINGS_VERSION                */
    PC_TAG_SETTINGS_CRC = 0x001B, /**< u16 CCITT of the RAM settings       */
    PC_TAG_SETTINGS_SIZE= 0x001C, /**< u16 sizeof(settings_t)              */
    PC_TAG_CHSTORE_VER  = 0x001D, /**< u16 CHSTORE_VERSION                 */
    PC_TAG_LASTUSED     = 0x001E, /**< u16 recalled channel, 0xFFFF none   */
    PC_TAG_MSG_STORE_ID = 0x001F, /**< u8  identifier inside the store     */

    PC_TAG_CHANNEL      = 0x0040, /**< bytes, one chstore_channel_t (37)   */
    PC_TAG_CHANNEL_LIST = 0x0041, /**< bytes, count * 37                   */
    PC_TAG_CONTACT      = 0x0042, /**< bytes, one cstore_contact_t (33)    */
    PC_TAG_CONTACT_LIST = 0x0043, /**< bytes, count * 33                   */
    PC_TAG_ZONE_CHANNEL = 0x0044, /**< bytes, one zone_channel_t (21)      */
    PC_TAG_SETTINGS     = 0x0045, /**< bytes, settings_t, packed           */
    PC_TAG_SETTINGS_REC = 0x0046, /**< bytes, a full settings record       */
    PC_TAG_STATS        = 0x0047, /**< bytes, pcProtoStats_t               */

    PC_TAG_RX_FREQ      = 0x0080, /**< u32 Hz                              */
    PC_TAG_TX_FREQ      = 0x0081, /**< u32 Hz                              */
    PC_TAG_MODE         = 0x0082, /**< u8  enum opmode                     */
    PC_TAG_RSSI         = 0x0083, /**< i16 dBm                             */
    PC_TAG_TX_STATE     = 0x0084, /**< u8  0 off, 1 receiving, 2 tx        */
    PC_TAG_CHANNEL_IDX  = 0x0085, /**< u16 selected channel, 0xFFFF VFO    */
    PC_TAG_BATTERY_MV   = 0x0086, /**< u16                                 */
    PC_TAG_SQUELCH      = 0x0087, /**< u8  1 open                          */
    PC_TAG_PERIOD_MS    = 0x0088, /**< u16 subscription period, 0 off      */
    PC_TAG_CHANNEL_NAME = 0x0089, /**< str name of the selected channel    */
    PC_TAG_TX_POWER     = 0x008A, /**< u16 mW the radio would key with     */
    /*
     * The accessory port (docs/ACCESSORY.md): which mode owns the pad at the
     * jack, whether its PTT switch is down right now, and how much of the
     * serial stream that press has cost.  A client that sees portLost grow
     * knows its own frames were the ones dropped, instead of blaming the cable.
     */
    PC_TAG_PORT_MODE    = 0x008B, /**< u8 enum portMode: 0 auto, 1 always,
                                   *   2 never                             */
    PC_TAG_PORT_PTT     = 0x008C, /**< u8 1 the accessory PTT is down      */
    PC_TAG_PORT_LOST    = 0x008D, /**< u32 bytes/errors discarded while it
                                   *   was down                           */
    /*
     * TETRA uplink (MS to BS): what the "Cell uplink" row of the TETRA quick
     * menu hears. Both travel with every status, zero outside TETRA, so a
     * client can watch the uplink without a TETRA-specific subscription.
     */
    PC_TAG_TETRA_UL     = 0x008E, /**< u8 1 an MS is heard on the uplink   */
    PC_TAG_TETRA_UL_SSI = 0x008F, /**< u32 sending MS, 0 if unknown        */

    PC_TAG_MSG_TRANSPORT= 0x00A0, /**< u8  enum msgTransport               */
    PC_TAG_MSG_PEER     = 0x00A1, /**< str peer address                    */
    PC_TAG_MSG_PEER_ID  = 0x00A2, /**< u32 numeric peer                    */
    PC_TAG_MSG_TEXT     = 0x00A3, /**< str UTF-8 text                      */
    PC_TAG_MSG_STATE    = 0x00A4, /**< u8  enum msgState                   */
    PC_TAG_MSG_FLAGS    = 0x00A5, /**< u8  enum msgFlags                   */
    PC_TAG_MSG_UNREAD   = 0x00A6, /**< u8  unread messages                 */
    PC_TAG_MSG_INBOX    = 0x00A7, /**< u8  entries in the inbox            */
    PC_TAG_MSG_OUTBOX   = 0x00A8, /**< u8  entries in the outbox           */

    /* Settings as named fields: the tag is the field, PC_MSG_SETTINGS_SET
     * writes it under the state mutex and reads it back. */
    PC_TAG_SET_CALLSIGN   = 0x00C0, /**< str, 10 bytes incl. NUL           */
    PC_TAG_SET_BRIGHTNESS = 0x00C1, /**< u8                                */
    PC_TAG_SET_CONTRAST   = 0x00C2, /**< u8                                */
    PC_TAG_SET_SQLLEVEL   = 0x00C3, /**< u8                                */
    PC_TAG_SET_VOXLEVEL   = 0x00C4, /**< u8                                */
    PC_TAG_SET_TZ         = 0x00C5, /**< i8, half hours                    */
    PC_TAG_SET_DMR_ID     = 0x00C6, /**< u32                               */
    PC_TAG_SET_DMR_DEST   = 0x00C7, /**< u32 talkgroup or subscriber       */
    PC_TAG_SET_DMR_TYPE   = 0x00C8, /**< u8  enum dmrDestType              */
    PC_TAG_SET_DMR_TS     = 0x00C9, /**< u8  1 or 2                        */
    PC_TAG_SET_M17_CAN    = 0x00CA, /**< u8                                */
    PC_TAG_SET_M17_DEST   = 0x00CB, /**< str                               */
    PC_TAG_SET_PC_PORT    = 0x00CC, /**< u8 enum portMode (docs/ACCESSORY.md) */

    /* Debug namespace */
    PC_TAG_TOKEN       = 0x00D0, /**< u32 session token for the writes    */
    PC_TAG_CONFIRM     = 0x00D1, /**< u8, 1: the client means it          */
    PC_TAG_LEVEL       = 0x00D2, /**< u8 minimum log level                */
    PC_TAG_MODULE      = 0x00D3, /**< str module tag filter, "" = all     */
    PC_TAG_LOG_SEQ     = 0x00D4, /**< u32 line counter, gaps are losses   */
    PC_TAG_DROPPED     = 0x00D5, /**< u32 lines or frames dropped         */
    PC_TAG_CHIP        = 0x00D6, /**< u8 0 BK4819, 1 BK1080               */
    PC_TAG_REG         = 0x00D7, /**< u8 register address                 */
    PC_TAG_VALUE       = 0x00D8, /**< u16 register value                  */
    PC_TAG_ADDR        = 0x00D9, /**< u32 address                         */
    PC_TAG_LENGTH      = 0x00DA, /**< u16 bytes or samples                */
    PC_TAG_DATA        = 0x00DB, /**< bytes: memory or samples            */
    PC_TAG_TEXT        = 0x00DC, /**< str free text                       */
    PC_TAG_RESET_CAUSE = 0x00DD, /**< u32 raw reset cause register        */
    PC_TAG_RESET_NAME  = 0x00DE, /**< str why the radio last restarted    */
    PC_TAG_WDT_RESETS  = 0x00DF, /**< u32 watchdog resets                 */
    PC_TAG_HEAP_FREE   = 0x00E0, /**< u32 bytes                           */
    PC_TAG_HEAP_TOTAL  = 0x00E1, /**< u32 bytes                           */
    PC_TAG_PSRAM_FREE  = 0x00E2, /**< u32 bytes                           */
    PC_TAG_PSRAM_TOTAL = 0x00E3, /**< u32 bytes                           */
    PC_TAG_FLASH_USED  = 0x00E4, /**< u32 bytes                           */
    PC_TAG_FLASH_TOTAL = 0x00E5, /**< u32 bytes                           */
    PC_TAG_RAM_USED    = 0x00E6, /**< u32 bytes                           */
    PC_TAG_RAM_TOTAL   = 0x00E7, /**< u32 bytes                           */
    PC_TAG_TASK_STACK  = 0x00E8, /**< bytes, pcProtoTaskStack_t array     */
    PC_TAG_AGC         = 0x00E9, /**< u8 AGC state, 0xff unknown          */
    PC_TAG_RATE        = 0x00EA, /**< u16 lines per second allowed        */
    PC_TAG_OFFSET      = 0x00EB, /**< u32 offset in samples or bytes      */
    PC_TAG_RUNNING     = 0x00EC, /**< u8 1 capture or dump in progress    */
    PC_TAG_BB_STATE    = 0x00ED, /**< u8 baseband dump state              */
    PC_TAG_TX_SECONDS  = 0x00EE, /**< u16 air time since the last release */
    PC_TAG_TX_REASON   = 0x00EF, /**< str what the transmitter is doing   */

    /*
     * The post-mortem record (docs/BLACKBOX.md): the bytes are the record as
     * it sits on flash, little endian, 516 of them, and the length is what the
     * client compares its offset against to stop asking.
     */
    PC_TAG_BLACKBOX     = 0x00F0, /**< bytes: one chunk of the record     */
    PC_TAG_BLACKBOX_LEN = 0x00F1, /**< u16 total length of the record     */
    /*
     * Memory per region (docs/PC_PROTO.md): one pcProtoRegion_t per tag, as
     * many as the platform can measure.  A region that is not measurable is not
     * sent at all, and so are HEAP_FREE/HEAP_TOTAL and PSRAM_FREE/PSRAM_TOTAL
     * when the platform cannot ask its allocator: the client shows what the
     * radio measured, never a zero standing for "unknown".
     */
    PC_TAG_MEM_REGION   = 0x00F2, /**< bytes: pcProtoRegion_t             */

    /*
     * Bench namespace, 0x0100-0x011F.  A separate block because these are not
     * debug telemetry but measurement scaffolding: every one of them is
     * stamped with a radio tick (milliseconds since boot, the same base as
     * PC_TAG_UPTIME_MS) so the PC can put its own clock and the radio's on one
     * line.  docs/BENCH.md has the precision that can be expected from it.
     */
    PC_TAG_BENCH_EPOCH   = 0x0100, /**< u32 run counter, +1 on every ARM     */
    PC_TAG_BENCH_TICK    = 0x0101, /**< u32 radio ms when the action ran     */
    PC_TAG_BENCH_T0      = 0x0102, /**< u32 radio ms of sample/point zero    */
    PC_TAG_BENCH_WRITTEN = 0x0103, /**< u32 samples or points collected      */
    PC_TAG_BENCH_TONE    = 0x0104, /**< u16 Hz of the tone actually on air   */
    PC_TAG_BENCH_LEVEL   = 0x0105, /**< u8  AF level actually applied        */
    PC_TAG_BENCH_BW      = 0x0106, /**< u8  bandwidth index programmed      */
    PC_TAG_BENCH_BLOCK   = 0x0107, /**< u16 samples per audio push, 0 varies */
    PC_TAG_BENCH_BLOCKS  = 0x0108, /**< u32 audio pushes in the capture      */
    PC_TAG_BENCH_STEP    = 0x0109, /**< u32 sweep step, Hz                   */
    PC_TAG_BENCH_F0      = 0x010A, /**< u32 sweep start, Hz                  */
    PC_TAG_BENCH_F1      = 0x010B, /**< u32 sweep end, Hz (exclusive)        */
    PC_TAG_BENCH_DWELL   = 0x010C, /**< u16 ms the sweep waits per point     */
    PC_TAG_BENCH_POINTS  = 0x010D, /**< u16 points the sweep will visit      */
    PC_TAG_BENCH_DELTA   = 0x010E, /**< i32 Hz, what the radio measures
                                    *   against the programmed carrier (0
                                    *   when it cannot: a radio without a
                                    *   reference cannot measure its own
                                    *   carrier, the SDR does)              */
    PC_TAG_BENCH_WINDOW  = 0x010F, /**< u16 ms of capture asked for          */
    PC_TAG_BENCH_RSSI_TS = 0x0110, /**< bytes, n * (u32 tick, i16 dBm)       */
    PC_TAG_BENCH_POINTS_TS = 0x0111, /**< bytes, n * (u32 Hz, i16 dBm)       */
    PC_TAG_BENCH_OVERFLOW = 0x0112, /**< u8, 1 the capture filled up early   */
    PC_TAG_BENCH_POWER    = 0x0113  /**< u16 mW: TUNE in, TUNE/PTT out        */
};

/** Counters, PC_TAG_STATS; all are cumulative since boot. */
typedef struct
{
    uint32_t frames;      /**< frames that passed the CRC               */
    uint32_t requests;    /**< requests dispatched                      */
    uint32_t crcErrors;   /**< frames dropped on a bad CRC              */
    uint32_t formErrors;  /**< frames dropped on length or version      */
    uint32_t overflows;   /**< frames longer than PC_PROTO_MAX_BODY      */
    uint32_t unknown;     /**< requests with an identifier not served   */
    uint32_t events;      /**< events emitted                           */
    uint32_t txFrames;    /**< frames written to the port               */
    uint32_t txDropped;   /**< frames the port could not take           */
    uint16_t lastError;   /**< last error code sent                     */
    uint16_t lastErrorMsg;/**< identifier of the request that failed    */
}
__attribute__((packed)) pcProtoStats_t;

/**
 * Debug: one task's stack.  `unused` is the high-water mark (the fewest free
 * bytes ever seen), which is what tells whether a task is close to dying.
 */
typedef struct
{
    char     name[12]; /**< task name, NUL terminated                         */
    uint16_t unused;   /**< bytes never used, high-water mark                 */
    uint16_t size;     /**< stack size                                        */
}
__attribute__((packed)) pcProtoTaskStack_t;

/**
 * Debug: static usage of one memory region, PC_TAG_MEM_REGION.
 *
 * The regions of the CSK6011B are separate MEMORY areas of the linker script
 * (RAM, ITCM, DTCM, PSRAMAP, FLASH) and a firmware can be far from full in one
 * of them while another is the one that hurts. `used` is what the linker
 * placed, measured from its own symbols; `known` says whether this platform can
 * measure it at all, because a region whose usage is not measurable is left out
 * of the answer instead of being reported as zero.
 */
typedef struct
{
    char     name[8];    /**< "SRAM", "ITCM", "DTCM", "PSRAM", "FLASH"      */
    uint32_t used;       /**< bytes placed there, 0 when not measurable     */
    uint32_t total;      /**< bytes the region has                          */
    uint8_t  known;      /**< 1 when `used` was measured                    */
    uint8_t  reserved[3];
}
__attribute__((packed)) pcProtoRegion_t;

/** Regions a diagnostic answer can carry. */
#define PC_DIAG_REGIONS 5

/** Debug: the diagnostic state, PC_MSG_DBG_INFO. */
typedef struct
{
    uint32_t resetCause;   /**< raw reset cause register                     */
    uint32_t wdtResets;    /**< watchdog resets since the counters started   */
    uint32_t heapFree;     /**< only meaningful when heapKnown is 1          */
    uint32_t heapTotal;
    uint32_t psramFree;    /**< only meaningful when psramKnown is 1         */
    uint32_t psramTotal;
    uint32_t flashUsed;    /**< .text + .rodata + .data, if the linker knows */
    uint32_t flashTotal;
    uint32_t ramUsed;
    uint32_t ramTotal;
    uint8_t  taskCount;    /**< entries used in tasks[]                      */
    uint8_t  agc;          /**< AGC state, 0xFF unknown                      */
    pcProtoTaskStack_t tasks[8];
    /**
     * Power the radio would transmit with, mW, and why it would not ("none"
     * when it would).  Appended after the block the first version defined, so
     * a client that knows only the older fields still reads them where they
     * were (docs/TX_SAFETY.md).
     */
    uint16_t txPower;
    char     txReason[24];
    /**
     * Accessory port (docs/ACCESSORY.md): the mode that owns the pad, whether
     * its PTT switch is down at this instant and how many bytes the transport
     * has discarded while it was.  Appended after the transmit block for the
     * same reason: what came before keeps its offset.
     */
    uint8_t  portMode;   /**< enum portMode                                   */
    uint8_t  portPtt;    /**< 1 the accessory PTT is down right now          */
    uint32_t portLost;   /**< bytes and framing errors discarded             */
    /**
     * Memory, measured or absent (docs/PC_PROTO.md, "Las cifras de memoria").
     *
     * `heapKnown` is the whole point: a platform that cannot ask its own
     * allocator how much it has free reports 0 here and the answer carries no
     * heap figure at all, instead of a zero that a client would print as "0 of
     * 40960 bytes free".  `psramKnown` is the same for the free PSRAM, which no
     * backend of this tree can measure.  `regions[]` carries the static usage
     * of each memory region, which a linker can always tell.
     */
    uint8_t  heapKnown;
    uint8_t  psramKnown;
    uint8_t  regionCount;
    pcProtoRegion_t regions[PC_DIAG_REGIONS];
}
__attribute__((packed)) pcProtoDiag_t;

/** Debug: state of an RX audio capture. */
typedef struct
{
    uint32_t capacity;  /**< samples the buffer holds                      */
    uint32_t written;   /**< samples recorded                              */
    uint32_t read;      /**< samples already handed to the client          */
    uint16_t rate;      /**< sample rate, Hz                               */
    uint8_t  running;   /**< 1 while recording                             */
    uint8_t  overflow;  /**< 1 when the buffer filled up before the stop   */
    /* Bench additions: what a capture needs to be put on a time line. */
    uint32_t t0Ms;      /**< radio tick of sample zero, 0 when unknown     */
    uint32_t blocks;    /**< audio pushes recorded                         */
    uint16_t blockLen;  /**< samples per push, 0 when it varies            */
}
__attribute__((packed)) pcProtoCapture_t;

/** Debug: log line levels, lowest first. */
enum pcProtoLogLevel
{
    PC_LOG_ERROR = 0,
    PC_LOG_WARN  = 1,
    PC_LOG_INFO  = 2,
    PC_LOG_DEBUG = 3
};

/** Lines the log ring holds, and the longest line it keeps. */
#define PC_PROTO_LOG_LINES 4
#define PC_PROTO_LOG_LEN   64

/** One buffered log line. */
typedef struct
{
    uint32_t seq;                  /**< counter, gaps mean losses          */
    uint32_t timeMs;               /**< uptime when the line was produced  */
    char     module[8];            /**< tag, NUL terminated                */
    char     text[PC_PROTO_LOG_LEN];
    uint8_t  level;
}
__attribute__((packed)) pcProtoLogLine_t;

/**
 * Write side of the transport: the platform hands the bytes to the UART, a
 * host test to a buffer.  Return the number of bytes taken, a negative value
 * when nothing could be sent (the frame is then dropped and counted).
 */
struct pcProtoPort
{
    int (*write)(void *ctx, const uint8_t *buf, size_t len);
    void *ctx;
};

/** Live status as the platform sees it. */
typedef struct
{
    uint32_t rxFreq;      /**< Hz                                       */
    uint32_t txFreq;      /**< Hz                                       */
    int16_t  rssi;        /**< dBm                                      */
    uint16_t channelIdx;  /**< selected channel, 0xFFFF for VFO         */
    uint16_t batteryMv;   /**< millivolts                               */
    uint8_t  mode;        /**< enum opmode                              */
    uint8_t  txState;     /**< 0 off, 1 receiving, 2 transmitting        */
    uint8_t  squelchOpen; /**< 1 when the squelch is open               */
    char     channelName[16]; /**< empty in VFO                         */
    /**
     * Power the radio would transmit with, mW: the channel's, or
     * settings_t.tx_power when the channel carries none (a VFO).  Zero means
     * it will not key at all, and txReason says why -- the one thing that must
     * not be a mystery when the PTT does nothing (docs/TX_SAFETY.md).
     */
    uint16_t txPower;
    char     txReason[24];    /**< "none" or why the keying is refused   */
    /**
     * Accessory port (docs/ACCESSORY.md), appended after the transmit block so
     * a client that predates it still reads everything above where it was:
     * the mode that owns the pad at the jack, and how many bytes and framing
     * errors the transport has thrown away because the speaker-mic PTT held
     * the line low.  A press costs the client frames; this is where it sees
     * how many, instead of wondering why its requests went unanswered.
     */
    uint8_t  portMode;        /**< enum portMode: 0 auto, 1 always, 2 never */
    uint32_t portLost;        /**< bytes/errors discarded, cumulative     */
    /**
     * TETRA uplink (MS to BS), from protocols/TETRA/TetraStatus.h: 1 while an
     * uplink call, SDS or slot arrived in the last seconds, and the SSI of the
     * MS that sent the last uplink PDU. Zero in every other mode and in a
     * build without TETRA, so a client that shows them has nothing to hide.
     */
    uint8_t  tetraUplink;     /**< 1 while an MS is heard on the uplink  */
    uint32_t tetraUlSsi;      /**< Sending MS, 0 if unknown              */
}
pcProtoStatus_t;

/** Protocol instance.  One per port; all the memory it needs is here. */
typedef struct pcProto
{
    const struct pcProtoPort *port;

    uint8_t  rxBody[PC_PROTO_MAX_BODY];
    uint16_t rxLen;
    bool     rxEsc;        /**< previous byte was ESC                    */
    bool     rxInFrame;    /**< between an END and the next one          */
    bool     rxOverflow;   /**< drop until the next END                  */

    uint8_t  txBody[PC_PROTO_MAX_BODY + PC_PROTO_MAX_BODY / 8 + 2];
    uint8_t  resp[PC_PROTO_MAX_PAYLOAD]; /**< payload being built        */

    bool     linked;       /**< HELLO accepted, the link owns the UART   */
    bool     events;       /**< the client implements what we emit       */
    uint16_t clientVersion;
    uint32_t clientCaps;

    uint16_t subPeriodMs;
    uint32_t subNextMs;
    uint32_t nowMs;

    /* Debug: session token, and the log ring the transport fills. */
    uint32_t dbgToken;     /**< handed out in HELLO, needed for the writes  */
    uint32_t dbgNonce;     /**< source of the next token                    */
    pcProtoLogLine_t logRing[PC_PROTO_LOG_LINES];
    uint32_t logSeq;       /**< next line number                            */
    uint32_t logDropped;   /**< lines lost to a full ring or the rate limit */
    uint32_t logTokens;    /**< rate limiter, milli-lines                   */
    uint32_t logLastMs;
    uint16_t logRate;      /**< lines per second allowed, 0 not subscribed  */
    uint8_t  logLevel;     /**< minimum level kept                          */
    uint8_t  logHead;      /**< next slot to write                          */
    uint8_t  logCount;     /**< lines in the ring                           */
    char     logModule[8]; /**< tag filter, empty: every module             */

    pcProtoStats_t stats;
}
pcProto_t;

/**
 * Initialise an instance.  The port must stay valid for the life of the
 * instance; passing NULL gives an instance that parses but never transmits.
 */
void pc_proto_init(pcProto_t *p, const struct pcProtoPort *port);

/**
 * Forget the session: HELLO required again, subscriptions off, partial frame
 * dropped.  The counters survive, they are diagnostics.
 */
void pc_proto_reset(pcProto_t *p);

/**
 * Feed received bytes.  May be called with any chunking: the parser keeps its
 * state between calls, so one byte at a time and a whole buffer behave the
 * same.  Frames that fail the CRC, the length check or the version are
 * counted and dropped without a reply; a request is answered through the
 * port.
 *
 * @return the number of frames that passed the checks.
 */
uint32_t pc_proto_rx(pcProto_t *p, const uint8_t *data, size_t len);

/**
 * Drop whatever has been received of a frame that has not ended yet.
 *
 * For a transport whose receive line was borrowed by something else -- on the
 * C62 the accessory PTT grounds the same pad the UART reads, docs/ACCESSORY.md
 * -- the bytes that arrive while that happens are neither a frame nor a
 * preamble: the half frame the parser was building has to go, and the parser
 * has to wait for the next end delimiter before it believes anything again.
 * The session is untouched: a client that keeps talking is still linked.
 */
void pc_proto_rxResync(pcProto_t *p);

/**
 * Time base and housekeeping: stamps the uptime in replies and emits the
 * subscribed status event when its period expires.  Call from the transport
 * thread, at least every 100 ms.
 */
void pc_proto_tick(pcProto_t *p, uint32_t nowMs);

/** True when a client completed HELLO: the transport owns the UART. */
bool pc_proto_linked(const pcProto_t *p);

/** Capabilities this build serves (bitmap of enum pcProtoCap). */
uint32_t pc_proto_caps(void);

/**
 * Encode a frame into out.  Exposed for the tests and for anything that needs
 * to build a frame outside the dispatcher.
 *
 * @return the number of bytes written, 0 if it does not fit.
 */
size_t pc_proto_encode(uint8_t type, uint8_t seq, uint16_t id, uint8_t flags,
                       const uint8_t *payload, size_t len,
                       uint8_t *out, size_t cap);

/* Events.  Each is dropped when the link is not established, when the client
 * did not announce PC_CAP_EVENTS or when the port cannot take the frame. */

/** The selected channel changed: index, name, and the status that follows. */
void pc_proto_eventChannel(pcProto_t *p, uint16_t index, const char *name);

/** The transmit state changed (0 off, 1 receiving, 2 transmitting). */
void pc_proto_eventPtt(pcProto_t *p, uint8_t txState);

/** A message arrived: transport, peer, text, unread count. */
void pc_proto_eventMessage(pcProto_t *p, uint8_t transport, uint32_t peer,
                           const char *text, uint8_t unread);

/** The settings changed on the radio: version and CRC of the new record. */
void pc_proto_eventSettings(pcProto_t *p, uint16_t settingsCrc);

/**
 * Platform hooks with a weak default, so a host test links without a radio:
 * the live status and the identity strings.
 *
 * @return 0 on success, a negative value when there is nothing to report.
 */
int pc_proto_platformStatus(pcProtoStatus_t *out);

/**
 * Platform work deferred until after the answer: the bench PTT press is
 * applied here, from the transport loop, so that keying the radio can never
 * eat the reply to the frame that asked for it.
 */
void pc_proto_platformTick(uint32_t nowMs);

/** Model name, firmware version and hardware revision, or NULL. */
const char *pc_proto_platformModel(void);
const char *pc_proto_platformFirmware(void);
const char *pc_proto_platformHardware(void);

/**
 * Feed one line of the firmware log into the ring.  The transport calls this
 * from its printk hook instead of writing the line to the UART: while the
 * protocol owns the cable, log text never goes out as raw bytes, it can only
 * leave inside a PC_EVT_LOG frame.  That is what keeps a log line from
 * corrupting a frame (the corruption is impossible, not unlikely).
 *
 * @param level:  enum pcProtoLogLevel
 * @param module: short tag, at most 7 characters, may be NULL
 * @param text:   the line, without the newline; longer lines are cut
 */
void pc_proto_logPush(pcProto_t *p, uint8_t level, const char *module,
                      const char *text);

/** Lines dropped by the ring or the rate limit since boot. */
uint32_t pc_proto_logDropped(const pcProto_t *p);

/* Platform hooks of the debug namespace, all with a weak default that reports
 * "not implemented" so a host test and any other target link unchanged. */

/** Fill the diagnostic state.  Fields a platform cannot measure stay 0. */
int pc_proto_platformDiag(pcProtoDiag_t *out);

/** Read one register.  chip: 0 BK4819, 1 BK1080. @return 0 on success. */
int pc_proto_platformReg(int chip, uint8_t reg, uint16_t *value);

/** Write one register, already checked against the whitelist. */
int pc_proto_platformRegWrite(int chip, uint8_t reg, uint16_t value);

/**
 * Copy len bytes of radio memory at addr.  The platform decides which ranges
 * it will hand over; returning a negative value refuses the read.
 */
int pc_proto_platformMem(uint32_t addr, uint8_t *buf, uint16_t len);

/** Operations of pc_proto_platformCapture(). */
enum pcProtoCaptureOp
{
    PC_CAP_OP_START = 0, /**< st->capacity samples asked for               */
    PC_CAP_OP_STOP  = 1,
    PC_CAP_OP_STATUS= 2,
    PC_CAP_OP_READ  = 3, /**< buf, off and count valid                     */
    /**
     * Start a capture the audio path will fill, empty.  On a radio it is the
     * same as PC_CAP_OP_START (the codec worker is what fills it either way);
     * it exists so a simulator can tell "record what arrives" from "hand me a
     * full buffer at once", which is what the bench needs to watch a capture
     * fill over time and what the debug capture does not care about.
     */
    PC_CAP_OP_START_LIVE = 4
};

/** RX audio capture, implemented by the platform when it has an audio path. */
int pc_proto_platformCapture(int op, pcProtoCapture_t *st, uint8_t *buf,
                             uint32_t off, uint32_t count);

/**
 * Ready-made feed for the capture: the platform's audio worker calls this for
 * every block it delivers, and it does nothing unless DBG_CAP_START asked for
 * one.  A platform without a capture leaves pc_proto_platformCapture() weak
 * and this symbol undefined, which is fine: nothing calls it.
 */
void pc_proto_capturePush(const int16_t *samples, size_t stride,
                          size_t count);

/** Operations of pc_proto_platformBbDump(). */
enum pcProtoBbOp
{
    PC_BB_OP_STATUS = 0, /**< fills *state                                */
    PC_BB_OP_ARM    = 1, /**< start recording and sending                 */
    PC_BB_OP_DISARM = 2,
    PC_BB_OP_PAUSE  = 3,
    PC_BB_OP_RESUME = 4
};

/**
 * Baseband dump control, so it can be triggered from the PC instead of by
 * holding a key at power on.
 *
 * @param state: 0 off, 1 armed and recording, 2 paused; bit 7 set when the
 *               build has no dump at all.
 */
int pc_proto_platformBbDump(int op, uint8_t *state);

/** Format the live transmit trace into buf (two short lines at most). */
int pc_proto_platformTxTrace(char *buf, size_t cap);

/**
 * The absolute transmit clock, as the diagnostics report it
 * (docs/TX_SAFETY.md).
 *
 * @param seconds: filled with the air time since the PA was last released,
 *                 whole seconds.
 * @param reason: filled with the pointer to what the transmitter is doing:
 *                "idle", "ptt", "unattended", "mode" or "timeout".
 * @return 0 when the platform has a clock to report, negative to leave both
 *         fields out of the answer.
 */
int pc_proto_platformTxWatch(uint16_t *seconds, const char **reason);

/*
 * Bench namespace: one hook with an operation code and a structure in and out,
 * so a platform (and a host simulator) implements one function rather than
 * eight.  Weak default: every operation reports "not implemented" and the
 * messages answer PC_ERR_NOT_SUPPORTED.  The core module owns the sequencing,
 * the timestamps and the report; the platform owns the radio.
 */

/** Operations of pc_proto_platformBench(). */
enum pcProtoBenchOp
{
    /** Fill *a with the radio's own view: nowMs, txState, rssi, tuned state,
     *  plus whatever the platform applied since the last poll (tone, level).
     *  Called once per tick by the core, so this is where a platform lands
     *  anything that had to wait for the transmitter to be up. */
    PC_BENCH_OP_POLL   = 0,
    /** Program the receiver/transmitter: rxHz, txHz, mode, bw in; what was
     *  applied back.  @return 0 on success, negative when the frequency or the
     *  mode is out of reach. */
    PC_BENCH_OP_TUNE   = 1,
    /** Key or unkey: ptt in, toneHz (0 for none) and level in; the values
     *  really programmed come back in toneHz/level.  @return 0 on success. */
    PC_BENCH_OP_PTT    = 2,
    /** One RSSI reading right now, in rssi (dBm) and rssiRaw (register). */
    PC_BENCH_OP_RSSI   = 3,
    /** Take down anything a run left behind: unkey, restore the tone and the
     *  microphone gain, release a capture.  Called on ARM and on reset. */
    PC_BENCH_OP_RELEASE= 4
};

/** In and out of pc_proto_platformBench(). */
typedef struct
{
    uint32_t nowMs;    /**< out: radio tick, milliseconds since boot       */
    uint32_t rxHz;     /**< in/out: receive frequency                      */
    uint32_t txHz;     /**< in/out: transmit frequency                     */
    int32_t  deltaHz;  /**< out: measured carrier error, 0 when unknown    */
    uint16_t toneHz;   /**< in/out: tone asked for / tone programmed       */
    int16_t  rssi;     /**< out: filtered RSSI, dBm                        */
    int16_t  rssiRaw;  /**< out: raw RSSI as the transceiver reports it    */
    uint8_t  mode;     /**< in/out: enum opmode                            */
    uint8_t  bw;       /**< in/out: bandwidth index, 0 keeps the mode's    */
    uint8_t  ptt;      /**< in/out: 1 keyed, 0 not                          */
    uint8_t  level;    /**< in/out: AF level asked for / applied           */
    uint8_t  txState;  /**< out: 0 off, 1 receiving, 2 transmitting        */
    /**
     * in: transmit power asked for with TUNE, mW; 0 keeps the one in force.
     * out: the power the radio will key with.  A keying at 0 mW is refused:
     * PC_BENCH_OP_PTT answers PC_ERR_INVALID_VALUE instead of putting the PA
     * up with nothing behind it (docs/BENCH.md).
     */
    uint16_t power;
}
pcProtoBenchArgs_t;

int pc_proto_platformBench(uint8_t op, pcProtoBenchArgs_t *a);

/** Reset the bench state: new epoch on the next ARM, nothing left keyed. */
void pc_proto_benchReset(void);

/** Advance the bench state machine: captures, the RSSI collector and the
 *  sweep.  Called from pc_proto_tick(), i.e. from the transport thread. */
void pc_proto_benchTick(uint32_t nowMs);

/* TLV writing: each returns the new offset, or 0 when it does not fit. */

size_t pc_tlv_u8(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                 uint8_t value);
size_t pc_tlv_u16(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                  uint16_t value);
size_t pc_tlv_u32(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                  uint32_t value);
size_t pc_tlv_i8(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                 int8_t value);
size_t pc_tlv_i16(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                  int16_t value);
size_t pc_tlv_str(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                  const char *str);
size_t pc_tlv_bytes(uint8_t *buf, size_t cap, size_t off, uint16_t tag,
                    const void *data, size_t len);

/**
 * Open a container tag whose length is not known yet, for a list of records
 * written one at a time: returns the offset of the value (the slot the caller
 * keeps appending to), 0 when it does not fit.  Close it with pc_tlv_close().
 */
size_t pc_tlv_open(uint8_t *buf, size_t cap, size_t off, uint16_t tag);

/** Patch the length of a container opened at `start`; returns the new end. */
size_t pc_tlv_close(uint8_t *buf, size_t off, size_t start);

/* TLV reading: false when the tag is absent or its width does not match. */

bool pc_tlv_find(const uint8_t *tlv, size_t len, uint16_t tag,
                 const uint8_t **value, uint16_t *valueLen);
bool pc_tlv_get_u8(const uint8_t *tlv, size_t len, uint16_t tag,
                   uint8_t *out);
bool pc_tlv_get_u16(const uint8_t *tlv, size_t len, uint16_t tag,
                    uint16_t *out);
bool pc_tlv_get_u32(const uint8_t *tlv, size_t len, uint16_t tag,
                    uint32_t *out);
bool pc_tlv_get_i8(const uint8_t *tlv, size_t len, uint16_t tag,
                   int8_t *out);
bool pc_tlv_get_i16(const uint8_t *tlv, size_t len, uint16_t tag,
                    int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PC_PROTO_H */
