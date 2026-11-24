/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * AX.25 v2.2 UI frames and HDLC framing, as needed by APRS.
 *
 * References:
 *   - AX.25 Link Access Protocol for Amateur Packet Radio, v2.2 (1998),
 *     sections 2.2 (frame format), 2.3 (address field), 3.5 (FCS),
 *     4.3.4 (bit stuffing), 6.1.2 (UI frames).
 *   - APRS Protocol Reference 1.0.1 (APRS101.PDF, 2000), chapter 3.
 *
 * Everything here is allocation free and static-size: the same translation
 * unit is used by the firmware and by the host test tools.
 */
#ifndef AX25_H
#define AX25_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AX25_CALL_LEN      6u   /* characters, space padded      */
#define AX25_MAX_DIGIS     8u   /* AX.25 v2.2 address field limit */
#define AX25_MIN_FRAME     15u  /* dst+src+control+pid (UI)       */
#define AX25_MAX_INFO      256u
#define AX25_ADDR_SIZE     7u

/** One AX.25 address: 6 shifted characters plus the SSID byte. */
typedef struct
{
    char    call[AX25_CALL_LEN];  /**< upper case, space padded */
    uint8_t ssid;                 /**< 0..15                    */
    bool    repeated;             /**< H bit: address already used */
} ax25_addr_t;

/** A decoded UI frame. `info` points into the caller's buffer. */
typedef struct
{
    ax25_addr_t dst;
    ax25_addr_t src;
    uint8_t     nDigis;
    ax25_addr_t digi[AX25_MAX_DIGIS];
    uint8_t     control;
    uint8_t     pid;
    const uint8_t *info;
    size_t      infoLen;
} ax25_frame_t;

/**
 * CRC-16/X.25 (reflected polynomial 0x8408, init 0xFFFF, final XOR 0xFFFF).
 * This is the AX.25 FCS, transmitted low byte first.
 */
uint16_t ax25_fcs(const uint8_t *data, size_t len);

/** CRC-16/X.25 without the final XOR, for the receiver-side residue test. */
uint16_t ax25_fcs_raw(const uint8_t *data, size_t len);

/** True if the frame (FCS bytes included) has the AX.25 residue 0xF0B8. */
bool ax25_fcs_ok(const uint8_t *frame, size_t len);

/** Fill an address from a plain callsign ("N0CALL-9" or "N0CALL", case free). */
void ax25_addr_set(ax25_addr_t *a, const char *call);

/** Plain text form of an address, "CALL-SSID" or "CALL" for SSID 0. */
size_t ax25_addr_format(char *out, size_t cap, const ax25_addr_t *a);

/**
 * Serialise a UI frame: destination, source, digipeaters, control 0x03,
 * PID 0xF0, info, FCS. Returns the frame length or 0 if it does not fit.
 *
 * @param out: output buffer, at least 7*(2+digis)+2+infoLen+2 bytes.
 * @param cap: capacity of `out`.
 */
size_t ax25_build_ui(uint8_t *out, size_t cap, const ax25_frame_t *f);

/**
 * Parse a frame (without flags and without FCS, i.e. as delivered by the
 * HDLC deframer). Returns false if the address field or the control field is
 * not a UI frame.
 */
bool ax25_parse(uint8_t *frame, size_t len, ax25_frame_t *out);

/**
 * Split a comma separated path ("WIDE1-1,WIDE2-1") into digipeater
 * addresses. Returns the number of addresses stored, capped at max.
 *
 * This does not validate: it copies whatever it finds, upper-cased and
 * truncated to six characters.  Use ax25_pathValid() first, an invalid path
 * that reaches the air is a frame no digipeater will repeat.
 */
uint8_t ax25_parse_path(const char *path, ax25_addr_t *digis, uint8_t max);

/* ------------------------------------------------------------ validation --- */

/**
 * True if `call` is an address an AX.25 frame can carry: one to six
 * characters, letters and digits, at least one letter and at least one digit.
 * An SSID is not part of it ("N0CALL-9" is not a valid call here, the SSID is
 * a separate field: see ax25_ssidValid()).  Lower case is accepted, the
 * address is upper-cased on the wire.
 */
bool ax25_addrValid(const char *call);

/** True if ssid is 0..15 (0 means "no SSID"). */
bool ax25_ssidValid(int ssid);

/**
 * True if `alias` is a valid APRS digipeater alias with a used field:
 * WIDE, WIDE1..WIDE7 followed by -n with n in 1..7, or a plain callsign (see
 * ax25_addrValid) with an optional -SSID.  The old TRACE, RELAY and GATE
 * aliases are refused: APRS 1.0.1 replaced them with WIDE, and a frame that
 * carries them is dropped by most digipeaters today.
 */
bool ax25_digiValid(const char *alias);

/**
 * True if `path` is a path a digipeater network can use: zero to eight
 * comma-separated aliases, each one accepted by ax25_digiValid(), no spaces,
 * no empty items.  An empty string or NULL means "no
 * digipeaters" and is valid (a direct frame).
 *
 * @param nDigis: optional, receives the number of digipeaters in the path.
 */
bool ax25_pathValid(const char *path, uint8_t *nDigis);

#ifdef __cplusplus
}
#endif

#endif /* AX25_H */
