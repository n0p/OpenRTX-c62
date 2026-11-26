/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * HDLC framing for AX.25: flags, zero-bit insertion, NRZI, FCS check.
 *
 * AX.25 v2.2 §4.3: the frame is delimited by 0x7E flags, every run of five
 * contiguous ones is followed by an inserted zero, bits are transmitted least
 * significant bit first, and the line code is NRZI (a zero is a transition,
 * a one keeps the previous level).
 */
#ifndef HDLC_H
#define HDLC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ax25.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HDLC_FLAG        0x7Eu
#define HDLC_MAX_LEN     (7u * (2u + AX25_MAX_DIGIS) + 2u + AX25_MAX_INFO + 2u)

/** Called for every frame whose FCS is good. `frame` excludes the FCS. */
typedef void (*hdlc_frame_cb)(const uint8_t *frame, size_t len, void *user);

typedef struct
{
    /* NRZI decoder */
    bool     levelValid;
    bool     lastLevel;
    /* RX bit engine */
    bool     inFrame;       /**< inside a frame, i.e. after an opening flag */
    uint8_t  ones;          /**< contiguous ones seen in this frame      */
    uint8_t  bitBuf;        /**< byte being assembled (LSB first)        */
    uint8_t  bitCount;
    uint8_t  frame[HDLC_MAX_LEN];
    size_t   frameLen;
    bool     overflow;
    /* statistics */
    uint32_t flags;         /**< flags seen                              */
    uint32_t frames;        /**< frames delivered with good FCS          */
    uint32_t badFcs;        /**< complete frames rejected by FCS         */
    uint32_t aborts;        /**< aborts (seven or more ones)             */
    uint32_t stuffedErr;    /**< bad stuffing: zero after five ones      */
    uint32_t bits;          /**< raw bits processed, for BER measurement */
    /* carrier detect */
    uint16_t bitsSinceFlag; /**< bits since the last flag                */
    bool     dcd;
} hdlc_deframer_t;

void hdlc_deframer_init(hdlc_deframer_t *d);

/**
 * Feed one demodulated *level* (NRZI encoded bit) to the deframer.
 *
 * @param d: deframer state.
 * @param level: line level of this bit, true for the mark tone.
 * @param cb: called for each frame with a valid FCS, may be NULL.
 */
void hdlc_rx_level(hdlc_deframer_t *d, bool level,
                   hdlc_frame_cb cb, void *user);

/**
 * Feed one already-decoded data bit, bypassing the NRZI decoder: the path for
 * modems whose line code is a scrambler instead of NRZI (9600 G3RUH).
 */
void hdlc_rx_bit(hdlc_deframer_t *d, bool bit,
                 hdlc_frame_cb cb, void *user);

/** True when a frame has been received recently: the bit clock is locked. */
static inline bool hdlc_locked(const hdlc_deframer_t *d)
{
    return d->dcd;
}

/**
 * Transmitter phases. A named enum at file scope, not one declared inside the
 * struct: C++ scopes the enumerators of a nested enum to the class, and the
 * FM mode (C++) has to ask whether the frame is complete.
 */
typedef enum
{
    HDLC_TX_PRE = 0,
    HDLC_TX_DATA,
    HDLC_TX_POST,
    HDLC_TX_DONE
} hdlc_tx_phase_t;

typedef struct
{
    const uint8_t *data;    /**< frame bytes, FCS included                */
    size_t   len;
    size_t   byteIdx;
    uint8_t  bitIdx;
    uint8_t  ones;          /**< contiguous ones sent in the data phase   */
    uint8_t  preFlags;      /**< flags still to send before the frame     */
    uint8_t  postFlags;     /**< flags still to send after the frame      */
    uint8_t  flagBits;      /**< bits of the flag currently being sent    */
    bool     stuffPending;  /**< a stuffed zero is owed after five ones   */
    hdlc_tx_phase_t phase;
} hdlc_framer_t;

/**
 * Prepare a transmitter. `frame` must hold the AX.25 frame including the two
 * FCS bytes, as produced by ax25_build_ui().
 *
 * @param preFlags: opening flags, traditionally 1 to 4 (also serves as the
 *                  TXDELAY filler: each flag is 8.3 ms at 1200 baud).
 */
void hdlc_tx_start(hdlc_framer_t *f, const uint8_t *frame, size_t len,
                   uint8_t preFlags, uint8_t postFlags);

/** Next bit to modulate (0 or 1). Returns false once the frame is sent. */
bool hdlc_tx_bit(hdlc_framer_t *f);

/** True once the closing flags have been sent: nothing more to transmit. */
static inline bool hdlc_tx_finished(const hdlc_framer_t *f)
{
    return f->phase == HDLC_TX_DONE;
}

#ifdef __cplusplus
}
#endif

#endif /* HDLC_H */
