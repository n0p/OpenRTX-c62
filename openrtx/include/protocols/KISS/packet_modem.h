/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The modems a packet TNC can be: the registry of modes, their framing and
 * their limits, and one engine that turns a frame into samples and samples
 * back into frames whatever the mode is.
 *
 * Four modes, three of which do something today:
 *
 *   AFSK1200  Bell 202 AFSK, 1200 bit/s, HDLC + NRZI, AX.25 frames. TX and RX.
 *             The modulator and demodulator are the APRS ones
 *             (protocols/APRS/afsk.c), already verified against direwolf.
 *
 *   AFSK9600  G3RUH baseband FSK, 9600 bit/s, HDLC + 17-bit scrambler, AX.25
 *             frames. TX and RX, protocols/KISS/afsk9600.c. 5 samples per bit
 *             at 48 kHz, so the sample rate must be 48 kHz (or 96 kHz).
 *
 *   NXDN      Receive only. The payload is the 9 octets of the layer 3
 *             message the SACCH of a superframe carries (nxdnStatusSacchSegment
 *             hands it over). There is no NXDN transmitter in this firmware and
 *             the short data call lives in FACCH1, which the receiver does not
 *             decode: see docs/RX_NXDN.md and docs/KISS.md.
 *
 *   DMRIP     Receive only. The payload is the IPv4/UDP datagram of a DMR
 *             packet data call, as the DMR package assembles it
 *             (protocols/DMR/dmr_data.c, dmr_packet_t). Transmitting a DMR
 *             data call would need the header and block builder, which the
 *             package does not have: see docs/KISS.md.
 *
 * The engine is freestanding: no allocation, no Zephyr, no file system. The
 * firmware and the host tests run the same code.
 */
#ifndef PACKET_MODEM_H
#define PACKET_MODEM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "protocols/APRS/afsk.h"
#include "protocols/APRS/hdlc.h"
#include "protocols/KISS/afsk9600.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    PKT_MODEM_AFSK1200 = 0,
    PKT_MODEM_AFSK9600,
    PKT_MODEM_NXDN,
    PKT_MODEM_DMRIP,
    PKT_MODEM_COUNT
} pkt_modem_t;

/** What a mode can do. */
typedef enum
{
    PKT_CAP_RX = 0x01,
    PKT_CAP_TX = 0x02
} pkt_cap_t;

typedef struct
{
    const char *name;       /**< "AFSK1200", for the log and the KISS docs  */
    const char *label;      /**< <= 5 chars, for the header of the screen   */
    uint16_t    baud;       /**< line rate, 0 if the mode is not a serial rate */
    uint8_t     caps;       /**< pkt_cap_t                                  */
    uint16_t    maxPayload; /**< longest KISS payload the mode can carry    */
    const char *framing;    /**< one line: what goes on the air             */
} pkt_modem_info_t;

/** Registry entry of a mode; never NULL for a valid enum value. */
const pkt_modem_info_t *pkt_modem_info(pkt_modem_t mode);

/** Name of a mode, "?" for an out-of-range value. */
const char *pkt_modem_name(pkt_modem_t mode);

/** Parse a name ("AFSK1200", "9600", "NXDN", "DMRIP"), -1 if unknown. */
int pkt_modem_parse(const char *name);

/** Samples per bit (1200: 40, 9600: 5 at 48 kHz), 0 for the payload modes. */
uint32_t pkt_modem_spb(pkt_modem_t mode, uint32_t fs);

/* --------------------------------------------------------------- engine --- */

/** Called for every frame received, with the payload and no FCS. */
typedef void (*pkt_frame_cb)(const uint8_t *payload, size_t len, void *user);

typedef struct
{
    pkt_modem_t mode;
    uint32_t    fs;

    /* transmit */
    bool             txActive;
    uint32_t         silenceLeft;   /**< TXDELAY, in samples                */
    uint32_t         tailLeft;      /**< TXtail, in samples                 */
    uint8_t          frame[HDLC_MAX_LEN];
    hdlc_framer_t    framer;
    afsk_mod_t       afsk;
    g3ruh_mod_t      g3ruh;
    uint32_t         txBits;

    /* receive */
    afsk_demod_t     afskDem;
    g3ruh_demod_t    g3ruhDem;
    hdlc_deframer_t  deframer;
    pkt_frame_cb     onFrame;
    void            *user;
    uint32_t         rxBits;

    /* counters */
    uint32_t txFrames;
    uint32_t rxFrames;
    uint32_t rxFcsErr;
} pkt_engine_t;

/**
 * Initialise the engine for a mode and a sample rate.
 *
 * @return false when the mode cannot run at that rate (9600 needs 48 kHz).
 */
bool pkt_engine_init(pkt_engine_t *e, pkt_modem_t mode, uint32_t fs);

/**
 * Start a transmission.
 *
 * @param payload: the frame as the host sent it (AX.25, no FCS); the FCS is
 *                 appended here.
 * @param txdelayMs: key-up delay before the first flag.
 * @param txtailMs: silence after the last flag.
 * @return false if the frame does not fit or the mode cannot transmit.
 */
bool pkt_tx_begin(pkt_engine_t *e, const uint8_t *payload, size_t len,
                  uint32_t txdelayMs, uint32_t txtailMs);

/**
 * Produce up to `max` samples of the transmission in progress.
 *
 * @return samples written; 0 means the transmission is complete.
 */
size_t pkt_tx_block(pkt_engine_t *e, int16_t *out, size_t max);

/** True while a frame is being transmitted (flags, tail included). */
bool pkt_tx_busy(const pkt_engine_t *e);

/** Abort a transmission in progress. */
void pkt_tx_abort(pkt_engine_t *e);

/** Start (or restart) the receiver. */
void pkt_rx_begin(pkt_engine_t *e, pkt_frame_cb cb, void *user);

/** Demodulate a block of discriminator audio, delivering any frame. */
void pkt_rx_block(pkt_engine_t *e, const int16_t *in, size_t n);

/**
 * Deliver a payload that did not come from the demodulator: the NXDN SACCH
 * layer 3 message, the UDP datagram of a DMR data call.
 */
void pkt_rx_payload(pkt_engine_t *e, const uint8_t *payload, size_t len);

/** True while the demodulator sees a carrier. */
bool pkt_rx_signal(const pkt_engine_t *e);

#ifdef __cplusplus
}
#endif

#endif /* PACKET_MODEM_H */
