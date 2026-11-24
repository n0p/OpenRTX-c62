/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PROTOCOLS_DPMR_DPMRDEFS_H
#define PROTOCOLS_DPMR_DPMRDEFS_H

/**
 * @file DpmrDefs.h
 * @brief dPMR air interface, coding layer (ETSI TS 102 490 V1.9.1).
 *
 * Everything here is a pure function of the bits that come out of the
 * demodulator: no OS, no globals, no allocation, so the host unit tests and the
 * firmware run the same code.
 *
 * The layer structure of the standard, and where each piece lives:
 *
 *  - clause 12.2: 4FSK, 2400 symbols/s, h = 0.29 (+-1050 Hz outer, +-350 Hz
 *    inner), square root raised cosine with alpha = 0.2.  The receiver filter
 *    is the RRC plus the inverse sinc of the FM differentiator, clause
 *    12.2.2.2 ("Rx Baseband Filter").  DpmrPhy.c.
 *  - clause 4.2.2: an FDMA transmission is a Header frame (preamble, FS1, HI0,
 *    Channel Code, HI1) followed by Superframes (four 80 ms payload frames of
 *    384 bits) and closed by a shortened End frame (FS3 + 72 bits).
 *  - clause 5.1: payload frame = 24 bit FS2/CC + 72 bit CCH + 4 x 72 bit TCH.
 *  - clause 7: CRC-7 (CCH, END), CRC-8 (header information), shortened
 *    Hamming (12,8), X^9+X^5+1 scrambler, 12x6 / 12x10 interleavers.
 *  - clause 9.1: the TCH of a voice frame is four 72 bit AMBE+2 frames at
 *    3600 bit/s channel rate (AMBE+2 3600x2450), interleaved 4-way.
 *
 * There are NO TDMA timeslots in dPMR: the standard is FDMA, one carrier per
 * 6,25 kHz channel, and its frames are 80 ms of continuous transmission.  The
 * 40 ms two-slot structure belongs to DMR (TS 102 361) and NXDN, not here.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Symbols in one 80 ms payload frame: 384 bits at 2 bits/symbol. */
#define DPMR_FRAME_SYMBOLS 192
/** Bits in one 80 ms payload frame (clause 4.2.2). */
#define DPMR_FRAME_BITS 384
/** Bits of frame sync or channel code that open a payload frame. */
#define DPMR_SYNC_BITS 24
/** Symbols of frame sync or channel code that open a payload frame. */
#define DPMR_SYNC_SYMBOLS 12
/** Symbols of the control channel field of a payload frame (72 bits). */
#define DPMR_CCH_SYMBOLS 36
/** Symbols of the traffic channel field of a payload frame (288 bits). */
#define DPMR_TCH_SYMBOLS 144
/** Symbols of the header information fields HI0/HI1 (120 bits each). */
#define DPMR_HI_SYMBOLS 60
/** Symbols of an End frame: FS3 (12) + 72 bit END field. */
#define DPMR_END_SYMBOLS 48
/** Symbols of the END field itself: two scrambled copies of 36 bits. */
#define DPMR_END_FIELD_SYMBOLS 36
/** AMBE+2 frames in the TCH of one payload frame: 4 x 72 bits = 3600 bit/s. */
#define DPMR_AMBE_FRAMES 4
/** Bytes of one AMBE+2 3600x2450 frame, canonical A(24)|B(23)|C(25) order. */
#define DPMR_AMBE_BYTES 9
/** PCM samples one AMBE+2 frame synthesises (20 ms at 8 kHz). */
#define DPMR_PCM_SAMPLES 160

/** Frame sync sequences of clause 6.1, 48 or 24 bits, MSB first. */
#define DPMR_FS1_BITS 0x57FF5F75D577ULL
#define DPMR_FS2_BITS 0x5FF77DUL
#define DPMR_FS3_BITS 0x7DDFF5UL
#define DPMR_FS4_BITS 0xFD55F5DF7FDDULL
/** Preamble (clause 6.1.6): repeated 01011111 pattern, 8 bits each. */
#define DPMR_PREAMBLE_BYTE 0x5F

/** Communication modes, clause 5.7 (field M of the CCH). */
enum dpmrMode
{
    DPMR_MODE_VOICE      = 0,   /**< Voice, no user data in SLD   */
    DPMR_MODE_VOICE_SLD  = 1,   /**< Voice + 2 bytes of user data per frame */
    DPMR_MODE_DATA_T1    = 2,   /**< Data type 1: user data, no FEC */
    DPMR_MODE_DATA_T2    = 3,   /**< Data type 2: user data with FEC */
    DPMR_MODE_DATA_T3    = 4,   /**< Data type 3: packet data, ARQ  */
    DPMR_MODE_VOICE_DATA = 5    /**< Voice then appended type 2 data */
};

/** Communication format, clause 5.8 (field F of the CCH). */
enum dpmrFormat
{
    DPMR_FMT_ALL_CALL   = 0,    /**< 0000: broadcast to every station */
    DPMR_FMT_PEER       = 1     /**< 0001: peer to peer             */
};

/** Header types, clause 5.11 (field HT of the header frame), table 5.11. */
enum dpmrHeaderType
{
    DPMR_HT_START         = 0x0,  /**< Communication start, superframe follows */
    DPMR_HT_CONNECT_REQ   = 0x1,  /**< Connection request, END follows         */
    DPMR_HT_DISCONNECT    = 0x2,  /**< Unconnect request, END follows          */
    DPMR_HT_ACK           = 0x3,  /**< ACK/NACK, single frame, CI says which   */
    DPMR_HT_SYSTEM_REQ    = 0x4,  /**< System request, END follows             */
    DPMR_HT_SYSTEM_ACK    = 0x5,  /**< Reply to a system request, SF follows   */
    DPMR_HT_SYSTEM_DATA   = 0x6,  /**< System delivery, superframe follows     */
    DPMR_HT_STATUS_RESP   = 0x7,  /**< Status response, END follows            */
    DPMR_HT_STATUS_REQ    = 0x8   /**< Status request                          */
};

/** Data formats carried in the SLD field, clause 5.9.2 (field Format). */
enum dpmrDataFormat
{
    DPMR_DFORM_STATUS   = 0x0,  /**< Status message                  */
    DPMR_DFORM_PRECODED = 0x1,  /**< Precoded message                */
    DPMR_DFORM_TEXT     = 0x2,  /**< Free text message               */
    DPMR_DFORM_FILE     = 0x3,  /**< Short file transfer             */
    DPMR_DFORM_USER1    = 0x4   /**< User defined data 1 to 4 (0x4-0x7) */
};

/**
 * Channel code group, clause 8.1.2 (Initial Services and Facilities, group A)
 * and clause 8.2.3 (Configured Services and Facilities, group B).
 */
enum dpmrChannelGroup
{
    DPMR_GROUP_A = 0,           /**< ISF radios                        */
    DPMR_GROUP_B                /**< CSF radios                        */
};

/** Frame sync of a payload frame, clause 6.1. */
enum dpmrSync
{
    DPMR_SYNC_NONE = 0,         /**< Not locked                       */
    DPMR_SYNC_FS1,              /**< Header frame (and ACK)           */
    DPMR_SYNC_FS2,              /**< Superframe payload frames 1 and 3 */
    DPMR_SYNC_FS3,              /**< End frame                        */
    DPMR_SYNC_FS4,              /**< Packet data header (type 3)      */
    DPMR_SYNC_CC                /**< Channel Code, payload frames 2 and 4 */
};

/** Control channel of a payload frame, clause 5.1 and 9.1. */
struct dpmrCch
{
    bool     crcOk;             /**< CRC-7 of the 41 information bits */
    uint8_t  fixes;             /**< Hamming blocks corrected (0..6)  */
    uint8_t  fn;                /**< Frame number 0..3 within the superframe */
    uint16_t id12;              /**< 12 bit address slice of this frame:
                                     called ID upper/lower (FN 0/1) or own ID
                                     upper/lower (FN 2/3)              */
    uint8_t  mode;              /**< enum dpmrMode                    */
    uint8_t  version;           /**< 0: TS 102 490, 3: manufacturer    */
    uint8_t  format;            /**< enum dpmrFormat                  */
    uint32_t sld;               /**< 18 bit slow data field, raw      */
};

/** Header information HI0/HI1 of a header frame, clauses 5.2 and 7.7. */
struct dpmrHeader
{
    bool     crcOk;             /**< CRC-8 of the 72 information bits */
    uint8_t  fixes;             /**< Hamming blocks corrected (0..10) */
    uint8_t  ht;                /**< enum dpmrHeaderType              */
    uint32_t calledId;          /**< 24 bit called station address    */
    uint32_t ownId;             /**< 24 bit own address               */
    uint8_t  mode;              /**< enum dpmrMode                    */
    uint8_t  version;           /**< 0 standard, 3 manufacturer       */
    uint8_t  format;            /**< enum dpmrFormat                  */
    uint16_t ci;                /**< 11 bit call information          */
    bool     fromHi1;           /**< decoded from HI1 (HI0 failed)    */
};

/** End frame, clause 5.3 and 7.8. Each half carries its own CRC-7. */
struct dpmrEnd
{
    bool    crcOk[2];           /**< CRC-7 of END0 and END1           */
    bool    same;               /**< both halves decoded and agree    */
    uint8_t fixes[2];           /**< Hamming blocks corrected per half */
    uint8_t et;                 /**< End type (0 normal, 1 with status) */
    uint8_t arq;                /**< Ack request                      */
    uint8_t wait;               /**< Tx wait, clause 5.14             */
    uint8_t status;             /**< 5 bit status message, clause 5.15 */
    uint8_t reserved;           /**< 4 reserved bits, must be 0       */
};

/** One decoded frame handed to the mode. */
struct dpmrFrame
{
    enum dpmrSync sync;         /**< What opened it                   */
    bool     cchOk;             /**< CCH present and CRC valid        */
    struct dpmrCch cch;         /**< Control channel of a payload frame */
    bool     headerOk;          /**< HI0 present and CRC valid        */
    struct dpmrHeader header;   /**< Header frame information         */
    bool     endOk;             /**< End frame present and CRC valid  */
    struct dpmrEnd end;         /**< End frame field                  */
    bool     voice;             /**< TCH decoded as 4 AMBE+2 frames   */
    uint8_t  ambe[DPMR_AMBE_FRAMES][DPMR_AMBE_BYTES];
    uint8_t  channelCode;       /**< 12 bit channel code (frames 2 and 4) */
    float    evm;               /**< Normalised level error, 0 is ideal */
    uint8_t  fixes;             /**< Total corrected blocks in the frame */
};

/**
 * Level pattern of a frame sync, in units of 350 Hz (+3 +1 -1 -3).
 *
 * Every symbol of FS1 to FS4 is an outer symbol, so the pattern doubles as a
 * level reference: the demodulator takes the amplitude of the sync run as the
 * +-1050 Hz full scale.
 *
 * @param sync frame sync
 * @param out at least 24 entries
 * @return number of symbols written (0 for DPMR_SYNC_NONE)
 */
size_t dpmrDefs_syncLevels(enum dpmrSync sync, int8_t *out);

/** Name of a frame sync, for the diagnostics. */
const char *dpmrDefs_syncName(enum dpmrSync sync);

/** Name of a communication mode, for the diagnostics. */
const char *dpmrDefs_modeName(uint8_t mode);

/** Name of a header type, clause 5.11 table 5.11, for the diagnostics. */
const char *dpmrDefs_headerTypeName(uint8_t ht);

/**
 * Channel code of a 6,25 kHz dPMR channel, clause 6.1.5 and tables 6.1/6.2.
 *
 * The code is not a property of the RF channel alone: group A (Initial
 * Services and Facilities radios) and group B (Configured Services and
 * Facilities) use different codes on the same frequency. On 446,003125 MHz
 * group A sends 0x145 and group B 0xD11. Group A covers 446,003125 to
 * 446,196875 MHz in 6,25 kHz steps; group B only 446,003125 to 446,096875.
 *
 * @param freqHz channel centre frequency
 * @param group  channel code group, clause 8.1.2 and 8.2.3
 * @param out 12 bit code, most significant bit first, one bit per entry
 * @return true if the frequency is one of the documented channels of the group
 */
bool dpmrDefs_channelCodeBits(uint32_t freqHz, enum dpmrChannelGroup group,
                              uint8_t out[12]);

/**
 * Decode the 36 dibits of a CCH field (clause 7.6).
 * @param dibits 36 two-bit symbols, most significant bit first
 * @param out decoded control channel, out->crcOk set on a valid CRC-7
 * @return true if the CRC-7 closed
 */
bool dpmrFec_cchDecode(const uint8_t dibits[DPMR_CCH_SYMBOLS],
                       struct dpmrCch *out);

/**
 * Decode the 60 dibits of an HI0 or HI1 field (clause 7.7).
 * @return true if the CRC-8 closed
 */
bool dpmrFec_hiDecode(const uint8_t dibits[DPMR_HI_SYMBOLS],
                      struct dpmrHeader *out);

/**
 * Decode the 36 dibits (72 bits) of an END field (clause 7.8 and 9.6).
 *
 * The field is the same 36 bit block sent twice, and clause 9.6 says that each
 * 36 bit block is scrambled, so both copies start from the reset state and
 * carry their own CRC-7. out->same says whether both copies decoded and agree.
 *
 * @param dibits 36 two-bit symbols, most significant bit first
 * @param out decoded end field
 * @return true if at least one half closed its CRC-7
 */
bool dpmrFec_endDecode(const uint8_t dibits[DPMR_END_FIELD_SYMBOLS],
                       struct dpmrEnd *out);

/**
 * De-interleave the 72 TCH bits of one 20 ms group into the canonical AMBE+2
 * frame the vocoder takes (see lib/dmr_vocoder/dmr_vocoder.h).
 *
 * dPMR carries the same AMBE+2 3600x2450 frame as DMR, with its own bit order:
 * the 72 bits of the frame are transmitted as four interleaved streams of 18
 * bits, canonical bit (4*k + j) going out as transmitted bit (18*j + k).  The
 * result is bit-identical to the four-block voice interleave of TS 102 361-1
 * (the one dmr_enc.c writes and dmr_voice.c undoes) on the same payload,
 * which the unit test checks, so the DMR vocoder is reused unchanged.
 */
void dpmrFec_voiceFrame(const uint8_t dibits[36], uint8_t frame[DPMR_AMBE_BYTES]);

/** 12 bit value of the channel code dibits of a payload frame (clause 6.1.5). */
uint16_t dpmrDefs_channelCode(const uint8_t dibits[DPMR_SYNC_SYMBOLS]);

/** Scrambler state of clause 7.4, X^9+X^5+1 preset to all ones. */
uint16_t dpmrFec_scramblerReset(void);
/** Next scrambling bit. */
uint8_t dpmrFec_scramblerNext(uint16_t *state);

/** CRC-7 of clause 7.2 (X^7+X^3+1) over nbits, MSB first. */
uint8_t dpmrFec_crc7(const uint8_t *bits, size_t nbits);
/** CRC-8 of clause 7.2 (X^8+X^2+X+1) over nbits, MSB first. */
uint8_t dpmrFec_crc8(const uint8_t *bits, size_t nbits);
/** Encode one byte with the shortened Hamming (12,8) of clause 7.3. */
void dpmrFec_hammingEncode(uint8_t byte, uint8_t out[12]);
/** Decode one (12,8) codeword, correcting one bit; returns corrected count or -1. */
int dpmrFec_hammingDecode(const uint8_t in[12], uint8_t out[8]);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOLS_DPMR_DPMRDEFS_H */
