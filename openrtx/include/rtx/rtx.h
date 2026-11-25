/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RTX_H
#define RTX_H

#include "core/datatypes.h"
#include <stdint.h>
#include "core/cps.h"
#include "rtx/TxGuard.h"
#include "rtx/KeyGate.h"
#include "rtx/TxWatch.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint8_t opMode;         /**< Operating mode (FM, DMR, ...) */

    uint8_t bandwidth : 2,  /**< Channel bandwidth             */
            txDisable : 1,  /**< Disable TX operation          */
            scan      : 1,  /**< Scan enabled                  */
            opStatus  : 2,  /**< Operating status (OFF, ...)   */
            _padding  : 2;  /**< Padding to 8 bits             */

    freq_t rxFrequency;     /**< RX frequency, in Hz           */
    freq_t txFrequency;     /**< TX frequency, in Hz           */

    uint32_t txPower;       /**< TX power, in mW               */
    uint8_t  sqlLevel;      /**< Squelch opening level         */

    uint16_t rxToneEn : 1,  /**< RX CTC/DCS tone enable        */
             rxTone   : 15; /**< RX CTC/DCS tone               */

    uint16_t txToneEn : 1,  /**< TX CTC/DCS tone enable        */
             txTone   : 15; /**< TX CTC/DCS tone               */

    bool     toneEn;

    uint8_t  can      : 4,  /**< M17 Channel Access Number     */
             canRxEn  : 1,  /**< M17 Check CAN on RX           */
             _unused  : 3;

    char     source_address[10];       /**< M17 call source address    */
    char     destination_address[10];  /**< M17 call routing address   */
    bool     invertRxPhase;            /**< M17 RX phase inversion     */
    bool     lsfOk;                    /**  M17 LSF is valid           */
    char     M17_dst[10];              /**  M17 LSF destination        */
    char     M17_src[10];              /**  M17 LSF source             */
    char     M17_link[10];             /**  M17 LSF traffic originator */
    char     M17_refl[10];             /**  M17 LSF reflector module   */
    char     M17_meta_text[53];        /**< M17 Meta Text              */

    uint8_t  dmrColourCode;            /**< DMR colour code, 0..15     */
    uint8_t  dmrTimeslot;              /**< DMR timeslot, 1 or 2.  Comes from
                                        *   the settings (quick menu) and a
                                        *   stored channel can override it.  The
                                        *   transmitter emits the whole call --
                                        *   header, voice, terminator -- in this
                                        *   slot and leaves the other silent. */
    uint32_t dmrSourceId;              /**< DMR radio ID, 24 bit       */
    uint32_t dmrDestId;                /**< Call destination, 24 bit   */
    uint8_t  dmrDestType;              /**< enum dmrDestType: NONE, GROUP or USER */
}
rtxStatus_t;

/**
 * \enum bandwidth Enumeration type defining the current rtx bandwidth.
 *
 * The numeric values are stored in channels and in the settings journal: only
 * append new ones.
 */
enum bandwidth
{
    BW_12_5 = 0,    /**< 12.5 kHz bandwidth */
    BW_25   = 1,    /**< 25 kHz bandwidth   */
    BW_6_25 = 2     /**< 6.25 kHz bandwidth */
};

/**
 * \enum opmode Enumeration type defining the current rtx operating mode.
 *
 * The values are stored in channels and in the settings journal: only append
 * new ones. They are fixed explicitly rather than left to the compiler so that
 * a value never moves when a mode is inserted above it.
 */
enum opmode
{
    OPMODE_NONE = 0,        /**< No opMode selected */
    OPMODE_FM   = 1,        /**< Analog FM          */
    OPMODE_DMR  = 2,        /**< DMR                */
    OPMODE_M17  = 3,        /**< M17                */
    OPMODE_TETRA = 4,       /**< TETRA (receive)    */

    /*
     * Receive-only modes, numbered in the order they were added. Every new
     * mode takes the next free value, never a gap: two branches that both took
     * 5 is exactly what this table exists to prevent.
     */
    OPMODE_NXDN     = 5,    /**< NXDN (receive)                        */
    OPMODE_DPMR     = 6,    /**< dPMR (receive)                        */
    OPMODE_P25      = 7,    /**< P25 Phase 1 (receive)                 */
    OPMODE_YSF      = 8,    /**< Yaesu System Fusion (receive)         */
    OPMODE_DSTAR    = 9,    /**< D-STAR (receive)                      */
    OPMODE_AIRBAND  = 10,   /**< Airband AM, 118-137 MHz (receive)     */
    OPMODE_WFM_BC   = 11   /**< Broadcast FM, 85-108 MHz (receive)    */
};

/**
 * \enum opstatus Enumeration type defining the current rtx operating status.
 */
enum opstatus
{
    OFF = 0,        /**< OFF          */
    RX  = 1,        /**< Receiving    */
    TX  = 2         /**< Transmitting */
};


/**
 * Initialise rtx stage.
 * @param m: pointer to the mutex protecting the shared configuration data
 * structure.
 */
void rtx_init(pthread_mutex_t *m);

/**
 * Shut down rtx stage
 */
void rtx_terminate();

/**
 * Post a new RTX configuration on the internal message queue. Data structure
 * \b must be protected by the same mutex whose pointer has been passed as a
 * parameter to rtx_init(). This driver only copies its content into the internal
 * data structure, eventual garbage collection has to be performed by caller.
 * @param cfg: pointer to a structure containing the new RTX configuration.
 */
void rtx_configure(const rtxStatus_t *cfg);

/**
 * Obtain a copy of the RTX driver's internal status data structure.
 * @return copy of the RTX driver's internal status data structure.
 */
rtxStatus_t rtx_getCurrentStatus();

/**
 * High-level code is in charge of calling this function periodically, since it
 * contains all the RTX management functionalities.
 */
void rtx_task();

/**
 * Supervise the transmit lifecycle. Must be called periodically (every few
 * milliseconds) by a thread that is *not* the RTX thread: its whole purpose is
 * to unkey a transmitter whose owning thread is blocked in a stream or in the
 * DSP, so it cannot live in that thread.
 *
 * It checks the phase deadlines of the transmission and, at the first overrun,
 * forces the PA off and cancels the transmit streams, which is also what
 * releases the RTX thread from the blocking call. The mode then unwinds through
 * its own exit path.
 */
void rtx_supervise();

/**
 * Get the transmit lifecycle guard, for the modes to report their phases and
 * for the diagnostics to print the trace of the last transmission.
 *
 * @return the guard, never NULL.
 */
struct txGuard *rtx_getTxGuard();

/**
 * May the PTT key the transmitter now?
 *
 * The one rule every mode asks before it starts a transmission: the radio is
 * not changing mode, the PTT has been released at least once since the last
 * change and the press has lasted long enough to be the operator's. It also
 * carries the per-configuration "no transmit" flag (menus, receive-only
 * channels, the spectrum analyser).
 *
 * @param status: the configuration the caller is acting on, or NULL to skip the
 *                txDisable check.
 * @return true only if keying is permitted.
 */
bool rtx_keyingAllowed(const rtxStatus_t *status);

/**
 * Why the last call to rtx_keyingAllowed() said no.
 *
 * "none" when the gate would let a press through, otherwise a short word the
 * status reports: "no tx power" (a channel or a VFO with 0 mW, which must
 * never key, docs/TX_SAFETY.md), "tx disabled" (menus, receive-only channel)
 * or whatever the gate itself is waiting for.  A PTT that does nothing has to
 * be explainable from the outside.
 */
const char *rtx_keyingVetoReason(void);

/**
 * May an unattended transmission start now? (a queued short message, the
 * packet modem: no operator and no PTT.)
 *
 * It waits for the mode change to end and settle, and never runs while the PTT
 * is down during it.
 *
 * @param status: the configuration the caller is acting on, or NULL.
 * @return true only if the transmitter may be keyed without the PTT.
 */
bool rtx_unattendedTxAllowed(const rtxStatus_t *status);

/**
 * Is the transmit path permitted at this instant?
 *
 * This is the choke point the platform's radio_enableTx() asks before it drives
 * the PA: the transmitter is keyed only while the RTX layer says a permitted
 * transmission is in progress. A path that reaches the driver without going
 * through the gate -- another thread, a mode that forgot, a call during a mode
 * change -- is refused instead of keying the radio.
 *
 * @return true if the PA may be keyed.
 */
bool rtx_txPermitted(void);

/**
 * True while a mode change is in progress.
 *
 * @return true during switchMode(), including the retune, the teardown and the
 *         construction of the new mode.
 */
bool rtx_isSwitching(void);

/**
 * Get the mode the RTX thread is running.
 *
 * @return OPMODE_* of the active mode, OPMODE_NONE while idle.
 */
uint8_t rtx_currentOpMode(void);

/**
 * Get the keying gate, for the diagnostics and the host tests.
 *
 * @return the gate, never NULL.
 */
struct keyGate *rtx_getKeyGate();

/**
 * Get the absolute transmit clock, for the diagnostics and the host tests.
 *
 * It counts the air time since the PA was last released and is what ends a
 * transmission at TXW_LIMIT_MS (docs/TX_SAFETY.md). The driver keeps a second,
 * independent one with the margin, reachable through radio_txWatch().
 *
 * @return the clock, never NULL.
 */
struct txWatch *rtx_getTxWatch();

/**
 * What the transmitter is doing, as one word, for the diagnostics.
 *
 * @return "idle" when off the air, "ptt", "unattended" or "mode" while keyed,
 *         "timeout" after the clock ended a transmission.
 */
const char *rtx_txWatchReason();

/**
 * Get current RSSI in dBm.
 * @return RSSI value in dBm.
 */
rssi_t rtx_getRssi();

/**
 * Get current status of the RX squelch. This function is thread-safe and can
 * be called also from threads other than the one running the RTX task.
 * @return true if RX squelch is open.
 */
bool rtx_rxSquelchOpen();

/**
 * Check whether an operating mode has a transmitter.
 *
 * TETRA and DMR are receive only on this radio: the UI must not ask for a TX
 * frequency, offer transmit power or show the TX side of anything in those
 * modes.
 *
 * @param mode: operating mode, from the opmode enum.
 * @return true if the mode can transmit.
 */
bool opmode_canTransmit(uint8_t mode);

#ifdef __cplusplus
}
#endif

#endif /* RTX_H */
