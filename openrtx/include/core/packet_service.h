/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Packet modem service: the radio as a KISS TNC.
 *
 * The service owns three things and nothing else owns them:
 *
 *  1. The KISS byte stream. Whatever transport is bound to it (the console
 *     UART in this build, the PC protocol layer when it lands) is only bytes
 *     in and bytes out: kiss_port_t below is the whole contract, so the
 *     service can be re-homed with one packet_service_bind() call and nothing
 *     else changes.
 *  2. The transmit queue and the channel access (PERSIST / SLOTTIME, or
 *     nothing at all in full duplex), which is what makes it a TNC and not a
 *     pipe.
 *  3. The receive tap: the discriminator of the FM mode goes into the modem of
 *     the selected mode and every frame comes back out as a KISS frame.
 *
 * It runs in its own thread, like the remote control and unlike APRS (which
 * is pumped from the FM mode's update loop): a host that sends a frame expects
 * it on the air even if the operator is looking at another screen, and the
 * service keys the radio itself, briefly, when it has something to send.
 *
 * The payload hooks at the bottom are how the modes that are not modems of
 * their own feed it: the NXDN receiver's SACCH layer 3 message and the DMR
 * package's data packet. Both are receive only, see docs/KISS.md.
 */
#ifndef PACKET_SERVICE_H
#define PACKET_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "protocols/KISS/packet_modem.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A byte transport for the KISS stream.
 *
 * read() returns the number of bytes it put in `buf` (0 when there is
 * nothing: the service polls). write() returns false only on a hard error; a
 * full transmit buffer is reported by dropping the frame, and counted.
 */
typedef struct
{
    size_t (*read)(uint8_t *buf, size_t cap, void *user);
    bool   (*write)(const uint8_t *buf, size_t len, void *user);
    void    *user;
} kiss_port_t;

/** Counters and state for the configuration screen. */
typedef struct
{
    bool     enabled;
    bool     txActive;          /**< a frame is on the air                  */
    bool     carrier;           /**< the demodulator sees a signal          */
    uint8_t  modem;             /**< pkt_modem_t                            */
    pkt_modem_t modemId;
    bool     portOpen;          /**< a transport is bound                   */
    uint32_t txFrames;          /**< frames sent                            */
    uint32_t rxFrames;          /**< frames received with a good FCS        */
    uint32_t rxFcs;             /**< frames received with a bad FCS         */
    uint32_t txDropped;         /**< frames the host gave that were refused */
    uint32_t txErrors;          /**< transmissions aborted                  */
    uint8_t  queued;            /**< frames waiting to go out               */
    /* KISS parameters in force (the stored ones or the host's) */
    uint8_t  txDelay10ms;
    uint8_t  persist;
    uint8_t  slotTime10ms;
    uint8_t  txTail10ms;
    bool     fullDuplex;
} packet_status_t;

/** Start the service: thread, transport and settings. Idempotent. */
void packet_service_init(void);

/**
 * Bind a transport. The service copies the structure, so `port` may live on
 * the caller's stack. Passing NULL unbinds (the service keeps running, it
 * just has nowhere to read from).
 */
void packet_service_bind(const kiss_port_t *port);

/** True when the operator has the packet modem enabled. */
bool packet_service_enabled(void);

/** Open or close the modem at run time (the "Enable" row of the page). */
void packet_service_setEnabled(bool on);

/** Select the modem. Returns false if the mode does not exist. */
bool packet_service_setModem(uint8_t modem);

/** Store one KISS parameter, from the page or from the host. */
void packet_service_setParam(uint8_t cmd, uint8_t value);

/** Read the counters and the parameters in force. */
void packet_service_status(packet_status_t *out);

/** Forget the counters (the page's "Clear" action). */
void packet_service_clearCounters(void);

/**
 * Ask the service to transmit a frame now, bypassing the queue: the page's
 * "Send test" action. The payload is an AX.25 frame without the FCS.
 */
bool packet_service_sendNow(const uint8_t *payload, size_t len);

/* ------------------------------------------------------- payload hooks --- */

/**
 * A layer 3 message from the NXDN receiver's SACCH (9 octets, NxdnFec.h).
 * Delivered to the host as one KISS frame when the modem is NXDN.
 */
void packet_service_nxdnPayload(const uint8_t *l3, size_t len);

/**
 * The user data of a DMR packet data call (dmr_packet_t: the UDP payload of
 * an IPv4/UDP datagram, or the raw user octets). Delivered when the modem is
 * DMRIP.
 */
void packet_service_dmrPayload(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PACKET_SERVICE_H */
