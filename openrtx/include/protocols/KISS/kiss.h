/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * KISS TNC2: the host-to-TNC protocol of Phil Karn, KA9Q ("KISS TNC: A simple
 * Host-to-TNC communications protocol", Chepponis & Karn, 1986), as a
 * freestanding codec: one byte at a time in, one message at a time out, no
 * allocation and no floating point.
 *
 * The link is a byte stream. A message is framed between two FEND (0xC0)
 * bytes; a FEND or an FESC (0xDB) inside the payload is sent as FESC followed
 * by TFEND (0xDC) or TFESC (0xDD). The first byte of a message is
 *
 *     bit 7..4  port (0 for the TNC's only port)
 *     bit 3..0  command
 *
 * Commands, from the specification and from what direwolf and the Linux
 * ax25 stack actually send (the three do not quite agree, see
 * docs/KISS.md):
 *
 *   0x00  data frame, AX.25 frame without FCS
 *   0x01  TXDELAY, 10 ms units
 *   0x02  PERSIST, 0..255
 *   0x03  SLOTTIME, 10 ms units
 *   0x04  TXTAIL, 10 ms units
 *   0x05  FULLDUPLEX, 0 or 1
 *   0x06  SET HARDWARE, TNC specific
 *   0x0F  end KISS (direwolf) / 0xFF return to normal TNC operation
 *
 * KISS simple ("cheese" without commands, as used by the simplest TNC
 * firmwares): data frames only, no command byte and no escaping, so a 0xC0 in
 * the payload cannot be represented; the caller picks it with `simple`.
 */
#ifndef KISS_H
#define KISS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KISS_FEND       0xC0u
#define KISS_FESC       0xDBu
#define KISS_TFEND      0xDCu
#define KISS_TFESC      0xDDu

/*
 * Longest payload carried in one message. An AX.25 frame is at most
 * 7 * (2 + 8) + 2 + 256 + 2 = 330 bytes with the FCS, and KISS carries it
 * without the FCS, so 328 is the real ceiling; the rest is slack for the
 * dashboard modes, whose payload is not an AX.25 frame.
 */
#define KISS_MAX_PAYLOAD 340u

/** Commands. Only the low nibble is used, the high one is the port. */
typedef enum
{
    KISS_CMD_DATA      = 0x00,
    KISS_CMD_TXDELAY   = 0x01,
    KISS_CMD_PERSIST   = 0x02,
    KISS_CMD_SLOTTIME  = 0x03,
    KISS_CMD_TXTAIL    = 0x04,
    KISS_CMD_FULLDUP   = 0x05,
    KISS_CMD_HARDWARE  = 0x06,
    KISS_CMD_END_KISS  = 0x0F,   /**< direwolf's end of KISS mode   */
    KISS_CMD_RETURN    = 0xFF    /**< classic return to terminal    */
} kiss_cmd_t;

/** Events produced by the decoder. */
typedef enum
{
    KISS_EV_NONE = 0,   /**< byte consumed, nothing complete yet        */
    KISS_EV_DATA,       /**< a data frame: msg.port and msg.data/len    */
    KISS_EV_PARAM,      /**< a parameter: msg.cmd and msg.data[0]       */
    KISS_EV_RETURN,     /**< 0xFF or 0x0F: leave KISS mode              */
    KISS_EV_UNKNOWN,    /**< a command this TNC does not implement      */
    KISS_EV_OVERFLOW    /**< the message did not fit: it was dropped    */
} kiss_ev_t;

/** One decoded message. `data` points into the decoder's own buffer. */
typedef struct
{
    uint8_t  port;      /**< 0..15                                      */
    uint8_t  cmd;       /**< low nibble of the first byte               */
    uint8_t  data[KISS_MAX_PAYLOAD];
    size_t   len;
} kiss_msg_t;

typedef struct
{
    bool      simple;   /**< KISS simple: no command byte, no escaping  */
    bool      inFrame;
    bool      escape;
    bool      first;    /**< the next unescaped byte is the command     */
    bool      overflow;
    size_t    len;
    kiss_msg_t msg;
    /* statistics */
    uint32_t  frames;   /**< messages delivered                         */
    uint32_t  bad;      /**< overflowed or truncated messages           */
    uint32_t  bytes;    /**< bytes consumed, for the link quality view  */
} kiss_dec_t;

/** Initialise a decoder. `simple` selects KISS simple framing. */
void kiss_dec_init(kiss_dec_t *d, bool simple);

/** Forget a partial message but keep the counters and the mode. */
void kiss_dec_reset(kiss_dec_t *d);

/**
 * Feed one byte.
 *
 * @return the event; when it is KISS_EV_DATA, KISS_EV_PARAM or
 *         KISS_EV_UNKNOWN, d->msg holds the message until the next byte.
 */
kiss_ev_t kiss_dec_byte(kiss_dec_t *d, uint8_t b);

/**
 * Human-readable name of a command, for the log and the docs table.
 * Returns "?" for the ones this TNC does not know.
 */
const char *kiss_cmd_name(uint8_t cmd);

/** True if the command sets a TNC parameter (and so takes one value byte). */
bool kiss_cmd_isParam(uint8_t cmd);

/* ------------------------------------------------------------- encoder --- */

/**
 * Encode a data frame.
 *
 * @param simple: KISS simple (no command byte, no escaping).
 * @return bytes written, 0 if it does not fit in `cap`.
 */
size_t kiss_enc_data(uint8_t *out, size_t cap, uint8_t port,
                     const uint8_t *payload, size_t len, bool simple);

/** Encode a parameter command with a one-byte value. */
size_t kiss_enc_param(uint8_t *out, size_t cap, uint8_t cmd, uint8_t value);

/** Encode a parameter command with a payload (SET HARDWARE responses). */
size_t kiss_enc_cmd(uint8_t *out, size_t cap, uint8_t port, uint8_t cmd,
                    const uint8_t *payload, size_t len);

/**
 * Encode `n` FENDs: the "cheese" a TNC sends when it enters KISS mode, so the
 * host sees a boundary before the first frame and can sync its parser.
 */
size_t kiss_enc_cheese(uint8_t *out, size_t cap, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* KISS_H */
