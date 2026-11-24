/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/radio.h"
#include "interfaces/platform.h"
#include "core/utils.h"
#include "hwconfig.h"
#include <stdio.h>
#include <string.h>
#include "rtx/rtx.h"
#include "rtx/OpMode_FM.hpp"
#include "rtx/OpMode_Airband.hpp"
#include "rtx/OpMode_M17.hpp"
#if defined(PLATFORM_C62)
#include "drivers/audio/csk6_codec.h"
#endif
#ifdef CONFIG_DMR
#include "rtx/OpMode_DMR.hpp"
#endif
#ifdef CONFIG_TETRA
#include "rtx/OpMode_TETRA.hpp"
#endif
<<<<<<< HEAD
#ifdef CONFIG_NXDN
#include "rtx/OpMode_NXDN.hpp"
=======
#ifdef CONFIG_P25
#include "rtx/OpMode_P25.hpp"
>>>>>>> c62/rx-p25
#endif

static pthread_mutex_t   *cfgMutex;     // Mutex for incoming config messages
static const rtxStatus_t *newCnf;       // Pointer for incoming config messages
static rtxStatus_t        rtxStatus;    // RTX driver status
static rssi_t             rssi;         // Current RSSI in dBm
static bool               reinitFilter; // Flag for RSSI filter re-initialisation

/*
 * Transmit supervision.
 *
 * The guard records the phases of a transmission and their deadlines; the
 * supervisor (rtx_supervise(), from the main thread) reads it and, when a
 * deadline is missed, unkeys and cancels the transmit streams from outside the
 * RTX thread. That thread may be blocked in a stream sync or in a DSP call --
 * which is exactly the failure this protects against -- so the unkey cannot be
 * its job. It then finds its streams cancelled, unwinds, and finishes the
 * transmission through the mode's normal exit path.
 *
 * txMutex protects the guard and the abort request: the RTX thread writes
 * phases, the supervisor reads them and sets the request. It is never held
 * across a call into audio or radio, so a blocked RTX thread never holds it.
 */
static pthread_mutex_t txMutex = PTHREAD_MUTEX_INITIALIZER;
static struct txGuard  txGuard;
static bool            txAbortPending;
static uint32_t        txAbortTime;

static OpMode  *currMode;               // Pointer to currently active opMode handler
static OpMode     noMode;               // Empty opMode handler for opmode::NONE
static OpMode_FM  fmMode;               // FM mode handler
static OpMode_Airband airbandMode;      // Airband AM mode handler
#ifdef CONFIG_M17
static OpMode_M17 m17Mode;              // M17 mode handler
#endif
#ifdef CONFIG_TETRA
static OpMode_TETRA tetraMode;          // TETRA mode handler
#endif
#ifdef CONFIG_DMR
static OpMode_DMR dmrMode;              // DMR mode handler
#endif
<<<<<<< HEAD
#ifdef CONFIG_NXDN
static OpMode_NXDN nxdnMode;            // NXDN mode handler
=======
#ifdef CONFIG_P25
static OpMode_P25 p25Mode;              // P25 Phase 1 mode handler
>>>>>>> c62/rx-p25
#endif


void rtx_init(pthread_mutex_t *m)
{
    // Initialise mutex for configuration access
    cfgMutex = m;
    newCnf   = NULL;

    /*
     * Default initialisation for rtx status
     */
    rtxStatus.opMode        = OPMODE_NONE;
    rtxStatus.bandwidth     = BW_25;
    rtxStatus.txDisable     = 0;
    rtxStatus.opStatus      = OFF;
    rtxStatus.rxFrequency   = 430000000;
    rtxStatus.txFrequency   = 430000000;
    rtxStatus.txPower       = 0.0f;
    rtxStatus.sqlLevel      = 1;
    rtxStatus.rxToneEn      = 0;
    rtxStatus.rxTone        = 0;
    rtxStatus.txToneEn      = 0;
    rtxStatus.txTone        = 0;
    rtxStatus.invertRxPhase = false;
    rtxStatus.lsfOk         = false;
    rtxStatus.M17_src[0]    = '\0';
    rtxStatus.M17_dst[0]    = '\0';
    rtxStatus.M17_link[0]   = '\0';
    rtxStatus.M17_refl[0]   = '\0';
    rtxStatus.M17_meta_text[0] = '\0';
    rtxStatus.dmrColourCode = 1;
    rtxStatus.dmrTimeslot   = 1;
    rtxStatus.dmrSourceId   = 0;   /* set from settings.dmr_id on the first sync */
    rtxStatus.dmrDestId     = 9;   /* matches default_settings */
    rtxStatus.dmrDestType   = DMR_DEST_GROUP;
    currMode = &noMode;

    pthread_mutex_lock(&txMutex);
    txg_init(&txGuard, (uint32_t)getTick());
    txAbortPending = false;
    pthread_mutex_unlock(&txMutex);

    /*
     * Initialise low-level platform-specific driver
     */
    radio_init(&rtxStatus);
    radio_updateConfiguration();

    /*
     * Initial value for RSSI filter
     */
    rssi         = radio_getRssi();
    reinitFilter = false;
}

void rtx_terminate()
{
    rtxStatus.opStatus = OFF;
    rtxStatus.opMode   = OPMODE_NONE;
    currMode->disable();
    radio_terminate();
}

void rtx_configure(const rtxStatus_t *cfg)
{
    /*
     * NOTE: an incoming configuration may overwrite a preceding one not yet
     * read by the radio task. This mechanism ensures that the radio driver
     * always gets the most recent configuration.
     */

    pthread_mutex_lock(cfgMutex);
    newCnf = cfg;
    pthread_mutex_unlock(cfgMutex);
}

rtxStatus_t rtx_getCurrentStatus()
{
    return rtxStatus;
}

struct txGuard *rtx_getTxGuard()
{
    return &txGuard;
}

/**
 * Force the transmitter off from outside the RTX thread.
 *
 * The PA goes first, so nothing is on the air while the streams are torn down,
 * and the streams go second: cancelling them is what wakes the RTX thread if it
 * is blocked waiting for one of their buffer boundaries.
 */
static void abortTransmission(uint32_t now, const char *why)
{
    radio_disableRtx();
    currMode->abortTx();

    #if defined(PLATFORM_C62)
    /*
     * Throttled: an abort is retried every 20 ms until the mode leaves
     * transmit, and a 115200 baud line cannot take that many lines without
     * becoming the reason the abort does not complete.
     */
    static uint32_t lastPrint = 0;
    if ((uint32_t)(now - lastPrint) < 500)
        return;
    lastPrint = now;

    char summary[96];
    char trace[96];
    txg_format(&txGuard, summary, sizeof(summary));
    txg_formatTrace(&txGuard, trace, sizeof(trace));
    printf("TX ABORT %s: %s | %s\r\n", why, trace, summary);
    #else
    (void)now;
    (void)why;
    #endif
}

/**
 * One line in the log per transmission, with the trace of what it did.
 *
 * Printed on the way out, never on the way in: a serial line at 115200 baud
 * takes about 5 ms for one of these lines, and the keying path has a budget of
 * its own. The live state of an ongoing transmission is on the remote control
 * instead ("tx" command), which is also where a transmission that never ends
 * can be seen.
 */
static void logTxExit(void)
{
    #if defined(PLATFORM_C62)
    /*
     * Keyed on the transmission counter, not on the phase: an over that starts
     * and fails inside a single rtx_task() never shows STARTING here, and the
     * one that matters most is exactly that one.
     */
    static uint32_t lastSeen = 0;
    uint32_t seen;

    pthread_mutex_lock(&txMutex);
    seen = txGuard.transmissions;
    pthread_mutex_unlock(&txMutex);

    if (seen == lastSeen)
        return;
    lastSeen = seen;

    char trace[110];
    char summary[130];
    txg_formatTrace(&txGuard, trace, sizeof(trace));
    txg_format(&txGuard, summary, sizeof(summary));
    printf("TX exit %s | %s | pk=%u\r\n", trace, summary,
           (unsigned int)csk6codec_getTxPeak());
    /* The next over starts its baseband peak from zero */
    csk6codec_clearTxPeak();
    #endif
}

void rtx_supervise()
{
    const uint32_t now = (uint32_t)getTick();
    const bool ptt = platform_getPttStatus() ? true : false;
    bool abort = false;

    pthread_mutex_lock(&txMutex);
    if (txg_tick(&txGuard, now, ptt) == TXG_ACTION_ABORT) {
        /*
         * Request the abort once, then repeat it every 20 ms until the mode
         * leaves transmit: the first attempt may have been refused by a radio
         * driver that was busy, and this is the last line before the watchdog.
         */
        if (!txAbortPending) {
            txAbortPending = true;
            txAbortTime = now;
            abort = true;
        } else if ((now - txAbortTime) >= 20) {
            txAbortTime = now;
            abort = true;
        }
    }
    pthread_mutex_unlock(&txMutex);

    if (abort)
        abortTransmission(now, txg_reasonName(txGuard.reason));
}

void rtx_task()
{
    const uint32_t now = (uint32_t)getTick();

    /*
     * A transmission the supervisor had to abort: it already unkeyed the PA and
     * cancelled the streams, so all that is left is to leave the mode's
     * transmit state -- the mode's own exit path releases the audio paths and
     * stops the encoder, from the thread that owns them.
     */
    bool aborted = false;
    pthread_mutex_lock(&txMutex);
    if (txAbortPending) {
        txAbortPending = false;
        aborted = true;
    }
    pthread_mutex_unlock(&txMutex);

    if (aborted)
        rtxStatus.opStatus = OFF;

    // Check if there is a pending new configuration and, in case, read it.
    bool reconfigure = false;
    if(pthread_mutex_trylock(cfgMutex) == 0)
    {
        if(newCnf != NULL)
        {
            // Copy new configuration and override opStatus flags
            uint8_t tmp = rtxStatus.opStatus;
            memcpy(&rtxStatus, newCnf, sizeof(rtxStatus_t));
            rtxStatus.opStatus = tmp;

            reconfigure = true;
            newCnf = NULL;
        }

        pthread_mutex_unlock(cfgMutex);
    }

    if(reconfigure)
    {
        // Force TX and RX tone squelch to off for OpModes different from FM.
        if(rtxStatus.opMode != OPMODE_FM)
        {
            rtxStatus.txToneEn = 0;
            rtxStatus.rxToneEn = 0;
        }

        /*
         * Handle change of opMode:
         * - deactivate current opMode and switch operating status to "OFF";
         * - update pointer to current mode handler to the OpMode object for the
         *   selected mode;
         * - enable the new mode handler
         */
        if(currMode->getID() != rtxStatus.opMode)
        {
            // Forward opMode change also to radio driver
            radio_setOpmode(static_cast< enum opmode >(rtxStatus.opMode));

            currMode->disable();
            rtxStatus.opStatus = OFF;

            switch(rtxStatus.opMode)
            {
                case OPMODE_NONE: currMode = &noMode;  break;
                case OPMODE_FM:   currMode = &fmMode;  break;
                case OPMODE_AIRBAND: currMode = &airbandMode; break;
                #ifdef CONFIG_M17
                case OPMODE_M17:  currMode = &m17Mode; break;
                #endif
                #ifdef CONFIG_DMR
                case OPMODE_DMR:   currMode = &dmrMode;   break;
                #endif
                #ifdef CONFIG_TETRA
                case OPMODE_TETRA: currMode = &tetraMode; break;
#ifdef CONFIG_NXDN
                case OPMODE_NXDN:  currMode = &nxdnMode;  break;
#endif
                #endif
                #ifdef CONFIG_P25
                case OPMODE_P25:  currMode = &p25Mode;  break;
                #endif
                default:   currMode = &noMode;
            }

            currMode->enable();
        }

        // Tell radio driver that there was a change in its configuration.
        radio_updateConfiguration();
    }

    /*
     * RSSI update block, run only when radio is in RX mode.
     *
     * RSSI value is passed through a filter with a time constant of 60ms
     * (cut-off frequency of 15Hz) at an update rate of 33.3Hz.
     *
     * The low pass filter skips an update step if a new configuration has
     * just been applied. This is a workaround for the AT1846S returning a
     * full-scale RSSI value immediately after one of its parameters changed,
     * thus causing the squelch to open briefly.
     *
     * Also, the RSSI filter is re-initialised every time radio stage is
     * switched back from TX/OFF to RX. This provides a workaround for some
     * radios reporting a full-scale RSSI value when transmitting.
     */
    if(rtxStatus.opStatus == RX)
    {

        if(!reconfigure)
        {
            if(!reinitFilter)
            {
                /*
                 * Filter RSSI value using 15.16 fixed point math. Equivalent
                 * floating point code is: rssi = 0.74*radio_getRssi() + 0.26*rssi
                 */
                int32_t filt_rssi = radio_getRssi() * 0xBD70    // 0.74 * radio_getRssi
                                  + rssi            * 0x428F;   // 0.26 * rssi
                rssi = (filt_rssi + 32768) >> 16;               // Round to nearest
            }
            else
            {
                rssi = radio_getRssi();
                reinitFilter = false;
            }
        }
    }
    else
    {
        // Reinit required if current operating status is TX or OFF
        reinitFilter = true;
    }

    /*
     * Forward the periodic update step to the currently active opMode handler.
     * Call is placed after RSSI update to allow handler's code have a fresh
     * version of the RSSI level.
     */
    currMode->update(&rtxStatus, reconfigure);

    logTxExit();
}

rssi_t rtx_getRssi()
{
    return rssi;
}

bool rtx_rxSquelchOpen()
{
    return currMode->rxSquelchOpen();
}

bool opmode_canTransmit(uint8_t mode)
{
    switch (mode) {
        case OPMODE_FM:
            return true;
        case OPMODE_M17:
            return true;
        case OPMODE_DMR:
#ifdef CONFIG_DMR_TX
            /* Transmit chain and AMBE+2 encoder compiled in: duplex mode */
            return true;
#else
            /* Receive only: the PTT branch is compiled out */
            return false;
#endif
        case OPMODE_TETRA:
            /* Receive only: OpMode_TETRA forces the status back to RX */
            return false;
<<<<<<< HEAD
        case OPMODE_NXDN:
        case OPMODE_DPMR:
        case OPMODE_P25:
        case OPMODE_YSF:
        case OPMODE_DSTAR:
            /* Receive only: there is no transmitter for these modes yet */
            /* fall through */
        case OPMODE_AIRBAND:
            /* Receive only: AM voice, and not a band this radio may key in */
=======
        case OPMODE_P25:
            /* Receive only: OpMode_P25 forces the status back to RX */
>>>>>>> c62/rx-p25
            return false;
        default:
            return false;
    }
}
