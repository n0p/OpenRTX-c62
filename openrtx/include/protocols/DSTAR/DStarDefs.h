/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DSTAR_DEFS_H
#define OPENRTX_DSTAR_DEFS_H

/**
 * @file DStarDefs.h
 * @brief D-STAR DV (JARL STD-7) constants: rates, sync words, frame geometry,
 * header layout, slow-data block types.
 *
 * Everything here is either quoted from JARL STD-7 ver 7.0 (2025) or derived
 * from it arithmetically; the citation for each constant is in the comment.
 * Sizes:
 *
 *   4800 bit/s, GMSK BT 0.5, h = 0.5, 6.25 kHz channel -> 96 bits per 20 ms
 *   frame = 72 voice bits (AMBE 3600x2400 channel frame) + 24 slow-data bits.
 *   A transmission is: 64-bit bit sync "1010..." + 15-bit frame sync + 660
 *   coded header bits + N x 96-bit frames + 48-bit end pattern.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Symbol rate, bit/s (STD-7 4.1.1). */
#define DSTAR_BAUD 4800

/** One DV frame: 96 bits, 20 ms (STD-7 4.1.2 b). */
#define DSTAR_FRAME_BITS 96
/** Voice bits per frame: AMBE-2020 channel frame (STD-7 4.1.2 b). */
#define DSTAR_VOICE_BITS 72
/** Slow data bits per frame (STD-7 4.1.2 b). */
#define DSTAR_SLOW_BITS 24
/** Bytes of one voice frame. */
#define DSTAR_VOICE_BYTES 9
/** Bytes of slow data in one frame. */
#define DSTAR_SLOW_BYTES 3
/** Frames between two resynchronisation words (STD-7 4.1.2 c). */
#define DSTAR_SUPERFRAME 21

/** Radio header, on air before coding: 3 flags + 4 calls + suffix + CRC. */
#define DSTAR_HEADER_BYTES 41
/** Information bits of the header, CRC included. */
#define DSTAR_HEADER_INFO_BITS 328
/** Coded header bits on air (rate 1/2, 328 + 2 flush bits). */
#define DSTAR_CODED_BITS 660
/** Bytes needed to hold the coded header (660 bits rounded up). */
#define DSTAR_CODED_BYTES 83

/** Bit sync: 64 bits of "1010..." (STD-7 4.1.1 a). */
#define DSTAR_BITSYNC_BITS 64
/** Bit sync, one byte (transmitted LSB first -> 0x0F would be all ones; the
 *  pattern is an alternating stream, so it is generated, not stored). */
#define DSTAR_BITSYNC_BYTE 0xAA

/** Frame sync, 15 bits "111011001010000" (STD-7 4.1.1 b). */
#define DSTAR_FRAME_SYNC 0x7650u
#define DSTAR_FRAME_SYNC_BITS 15

/** Resynchronisation word, 24 bits "101010101011010001101000"
 *  (STD-7 4.1.2 d): 10-bit bit sync + M-sequence 1101000 twice. */
#define DSTAR_DATA_SYNC 0xAAB468u
#define DSTAR_DATA_SYNC_BITS 24

/** End pattern, 48 bits: 32 bits of bit sync + "000100110101111" + 0
 *  (STD-7 4.1.2 h).  Stored as the 8 on-air bytes, LSB first per byte. */
#define DSTAR_END_BYTES 6
extern const uint8_t dstarEndPattern[DSTAR_END_BYTES];

/** Slow-data scrambler: first three bytes of the PN sequence (STD-7 Ap1.1,
 *  S(x) = x^7 + x^4 + 1, all-ones seed), applied to every 24-bit field. */
extern const uint8_t dstarSlowScrambler[3];

/** Null AMBE frame a D-STAR radio sends when it has no voice (the "silence"
 *  channel frame used by Icom radios and by MMDVMHost, air order). */
extern const uint8_t dstarNullVoice[DSTAR_VOICE_BYTES];

/* --------------------------------------------------------------- header --- */

/** Header field offsets (STD-7 4.1.1 c..k). */
#define DSTAR_HDR_FLAGS 0    ///< flags 1..3
#define DSTAR_HDR_RPT2 3     ///< destination repeater, 8 chars
#define DSTAR_HDR_RPT1 11    ///< source repeater, 8 chars
#define DSTAR_HDR_UR 19      ///< "other station" (CQCQCQ for a general call)
#define DSTAR_HDR_MY 27      ///< own callsign, 8 chars
#define DSTAR_HDR_SFX 35     ///< own callsign suffix, 4 chars
#define DSTAR_HDR_CRC 39     ///< P_FCS, CRC-CCITT, low byte first

/** Flag 1 bits (STD-7 4.1.1 c). */
#define DSTAR_FLAG1_DATA 0x80       ///< 1: data packet, 0: voice
#define DSTAR_FLAG1_REPEATER 0x40   ///< 1: via a repeater, 0: direct
#define DSTAR_FLAG1_INTERRUPT 0x20  ///< break-in
#define DSTAR_FLAG1_CONTROL 0x10    ///< control signal
#define DSTAR_FLAG1_EMERGENCY 0x08  ///< emergency
#define DSTAR_FLAG1_SUB_MASK 0x07   ///< 111 repeater control ... 000 NULL

/* ------------------------------------------------------------ slow data --- */

/** Slow-data block: 6 bytes spanning two frames (G4KLX, "The Format of
 *  D-STAR Slow Data").  Byte 0 is the type in the top nibble and the payload
 *  length in the bottom nibble. */
#define DSTAR_SLOW_BLOCK 6

#define DSTAR_SLOW_TYPE_MASK 0xF0
#define DSTAR_SLOW_LEN_MASK 0x0F

#define DSTAR_SLOW_GPS 0x30     ///< NMEA / D-PRS text fragments
#define DSTAR_SLOW_TEXT 0x40    ///< 20-character TX message, 4 fragments
#define DSTAR_SLOW_HEADER 0x50  ///< copy of the radio header, 5 bytes each
#define DSTAR_SLOW_FILLER 0x60  ///< 0x66 filler
#define DSTAR_SLOW_FAST 0x80    ///< fast data (not supported)
#define DSTAR_SLOW_SQUELCH 0xC0 ///< code squelch

/** Filler byte of the slow-data channel. */
#define DSTAR_SLOW_FILL 0x66
/** Length of the free text message, characters. */
#define DSTAR_TEXT_LEN 20
/** Room for one reassembled NMEA / D-PRS sentence. */
#define DSTAR_GPS_LEN 90

/** Own callsign suffix: 4 characters as on air ("    " when unused). */
#define DSTAR_SUFFIX_LEN 4
/** Repeater and station callsign: 8 characters as on air. */
#define DSTAR_CALL_LEN 8

/** General call destination. */
#define DSTAR_UR_GENERAL "CQCQCQ  "
/** Destination/source repeater of a simplex (direct) call. */
#define DSTAR_RPT_DIRECT "DIRECT  "

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DSTAR_DEFS_H */
