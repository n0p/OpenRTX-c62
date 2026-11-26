/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * APRS on the analog FM path: messaging, beacons, and the received station
 * list.
 *
 * APRS is not an operating mode. It rides on the FM mode:
 *
 *   - the receiver taps the discriminator with a second audio path
 *     (SOURCE_RTX to SINK_MCU) while the normal one (SOURCE_RTX to SINK_SPK)
 *     keeps playing, so audio is never interrupted;
 *   - the tap only runs when settings.aprs_decode is set, so with decoding off
 *     nothing is opened and no sample is processed;
 *   - transmitting a packet is an explicit action (a queued message or the
 *     beacon timer), never the PTT. The PTT stays the voice PTT.
 */
#ifndef APRS_SERVICE_H
#define APRS_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "protocols/APRS/aprs.h"
#include "protocols/APRS/aprs_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Kinds of packet the service can ask the mode to transmit. */
typedef enum
{
    APRS_PKT_NONE = 0,
    APRS_PKT_BEACON,      /**< position or status, from the beacon timer  */
    APRS_PKT_MESSAGE,     /**< queued message, with retries               */
    APRS_PKT_ACK          /**< acknowledgement of a received message      */
} aprs_pkt_t;

/* ------------------------------------------------------------- RX side --- */

/** Initialise the service. Call once, before the RTX thread starts. */
void aprs_service_init(void);

/**
 * Open the receive tap. Called by OpMode_FM when the squelch is open and
 * settings.aprs_decode is set. Returns false if the path cannot be taken.
 */
bool aprs_rxBegin(void);

/** Close the receive tap and release the audio path. Safe to call twice. */
void aprs_rxEnd(void);

/**
 * Our station address: the plaintext callsign of the settings plus the APRS
 * SSID, in AX.25 form ("N0CALL-9", or "N0CALL" for SSID 0).
 *
 * It is what a received frame is compared against to drop our own packets and
 * what an incoming message must be addressed to, so anything that has to know
 * "who are we on the air" asks here instead of rebuilding the string.
 */
void aprs_ownCallsign(char *out, size_t cap);

#ifdef SIM_INJECT
/**
 * Feed one received AX.25 frame to the service, as the demodulator would.
 *
 * The simulator's control channel uses it to put a station on the list and a
 * message in the mailbox without a transmitter on the desk
 * (docs/SIMULATOR.md, `inject`). It goes through the same path as the air:
 * our own packets are dropped, the station list is updated, a message
 * addressed to aprs_ownCallsign() lands in the mailbox and is acknowledged.
 *
 * @param nowMs: uptime in milliseconds, as the RX poll would pass it.
 * @return false when the frame does not parse as an AX.25 UI frame.
 */
bool aprs_sim_rxFrame(const uint8_t *frame, size_t len, uint32_t nowMs);
#endif

/**
 * Wait for one block of discriminator audio and demodulate it. Blocking, and
 * it must only be called while the tap is open: OpMode_FM calls it from its
 * update loop, which is what paces the mode while decoding.
 */
void aprs_rxPoll(void);

/** True while the tap is open. */
bool aprs_rxActive(void);

/* ------------------------------------------------------------- TX side --- */

/**
 * Is there a packet waiting to be transmitted?
 *
 * @param nowMs: uptime in milliseconds.
 */
bool aprs_txPending(uint32_t nowMs);

/**
 * Build the information field of the next packet to send, honouring the
 * channel gap and the retry schedule.
 *
 * @param info: buffer for the information field, NUL terminated.
 * @param kind: what the packet is, for the caller's timing decisions.
 * @return false if there is nothing to send.
 */
bool aprs_txPrepare(char *info, size_t cap, aprs_pkt_t *kind, uint32_t nowMs);

/**
 * Report the outcome of a transmission, to update the retry state and the
 * beacon timer.
 */
void aprs_txDone(aprs_pkt_t kind, uint32_t nowMs);

/* ---------------------------------------------------------- UI actions --- */

/** Queue a beacon now (the "send beacon" menu entry). */
bool aprs_requestBeacon(uint32_t nowMs);

/** Queue a message for a peer. Returns false if it cannot be queued. */
bool aprs_sendMessage(const char *to, const char *text, uint32_t nowMs);

/**
 * Move the APRS messages of the shared message store into the mailbox.
 *
 * @param nowMs: uptime in milliseconds.
 */
void aprs_pumpMessaging(uint32_t nowMs);

/** Queue an acknowledgement for a message received from `peer`. */
void aprs_queueAck(const char *peer, const char *msgNo);

/* -------------------------------------------------------------- status --- */

/** Received stations, most recently heard last. */
const aprs_station_list_t *aprs_stations(void);

/** Mailbox: inbox, outbox and the unread counter. */
aprs_mbox_t *aprs_mailbox(void);

/** Forget every received station. */
void aprs_clearStations(void);

/** True when a beacon transmission is enabled in the settings. */
bool aprs_beaconEnabled(void);

/**
 * True when the station configuration is usable for transmission: callsign,
 * SSID, path, symbol table and position all inside what goes on the wire.
 * When it is false nothing is transmitted and aprs_configError() says why.
 */
bool aprs_configValid(void);

/**
 * The reason the configuration cannot be transmitted, in the operator's terms
 * ("path: WIDE1-1,WIDE2-1 or a callsign"), or "" when everything is fine.
 * The string is static and meant for the status line of the APRS screen.
 */
const char *aprs_configError(void);

/** Interval to the next beacon in seconds, 0 if beacons are off. */
uint32_t aprs_beaconIntervalS(void);

/** Number of packets received since the mode was enabled. */
uint16_t aprs_rxCount(void);

/** Uptime of the last received packet, 0 if none. */
uint32_t aprs_lastRxMs(void);

/** True while a message was received that has not been read yet. */
bool aprs_hasUnread(void);

#ifdef __cplusplus
}
#endif

#endif /* APRS_SERVICE_H */
