/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STATE_H
#define STATE_H

#include "core/datatypes.h"
#include "core/settings.h"
#include <pthread.h>
#include <stdbool.h>
#include "core/cps.h"
#include "core/gps.h"
#include "core/wfm.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Part of this structure has been commented because the corresponding
 * functionality is not yet implemented.
 * Uncomment once the related feature is ready
 */
typedef struct {
    uint8_t devStatus;
    datetime_t time;
    uint16_t v_bat;
    uint8_t charge;
    rssi_t rssi;
    uint8_t volume;

    uint8_t ui_screen;
    uint8_t tuner_mode;

    uint16_t channel_index;
    channel_t channel;
    channel_t vfo_channel;
    bool bank_enabled;
    uint16_t bank;
    uint8_t rtxStatus;
    bool tone_enabled;

    bool emergency;
    settings_t settings;
    uint8_t settings_reset;   /**< enum settingsReset: why the defaults came back */
    gps_t gps_data;
    bool gps_set_time;
    bool gpsDetected;
    bool backup_eflash;
    bool restore_eflash;
    bool txDisable;

    /**
     * Disable transmitting without an operator (queued messages, APRS).
     *
     * Separate from txDisable, which is the PTT: the messaging and APRS
     * screens keep the PTT blocked while they are open -- a key pressed while
     * typing must not key the radio -- but the message or the beacon the
     * operator has just asked for still has to go out from there.
     */
    bool unattendedTxDisable;
    uint8_t step_index;

    /**
     * Broadcast FM receiver status (stereo, level, channel), valid while the
     * selected channel is OPMODE_WFM_BC.  Written by the operating mode at
     * the rate it polls the tuner, see core/wfm.h.
     */
    wfm_status_t wfm;
} state_t;

extern const uint32_t freq_steps[];
extern const size_t n_freq_steps;

enum TunerMode {
    VFO = 0,
    CH,
    SCAN,
    CHSCAN,
};

enum RtxStatus {
    RTX_OFF = 0,
    RTX_RX,
    RTX_TX,
};

enum DeviceStatus {
    PWROFF = 0,
    STARTUP,
    RUNNING,
    DATATRANSFER,
    SHUTDOWN,
};

extern state_t state;
extern pthread_mutex_t state_mutex;

/**
 * Initialise radio state mutex and radio state variable, reading the
 * informations from device drivers.
 */
void state_init();

/**
 * Terminate the radio state saving persistent settings to flash and destroy
 * the state mutex.
 */
void state_terminate();

/**
 * Update radio state fetching data from device drivers.
 */
void state_task();

/**
 * Reset the fields of radio state containing user settings and VFO channel.
 */
void state_resetSettingsAndVfo();

/**
 * Why the settings in RAM are not the ones from flash, as an enum
 * settingsReset value: SETTINGS_NO_RESET when they loaded, otherwise the
 * record was blank, written by another version of the firmware or damaged, and
 * the defaults are in use.  The operator interface shows this once and calls
 * state_settingsResetAck().
 */
uint8_t state_settingsResetCause(void);

/** Forget the "settings were reset" notice, after showing it once. */
void state_settingsResetAck(void);

/*
 * What the radio would transmit with, and where it would.
 *
 * Two questions, one answer each, and both are asked from more than one place:
 * the UI thread assembles the RTX configuration, the protocol reports the
 * status, the keying gate decides.  They have to agree, so the rule lives here
 * and nowhere else (docs/TX_SAFETY.md):
 *
 *  - The transmit frequency is the channel's, unless the radio is on a VFO
 *    whose record does not carry one: then it is the receive frequency, which
 *    is what a simplex VFO is.  It is never 0, because a driver that is given
 *    0 refuses to key in silence.
 *  - The transmit power is the channel's, unless that is 0 (a VFO, or a record
 *    written before the field existed): then it is settings_t.tx_power.  It
 *    can still be 0, and 0 means "do not key" -- state_keyingPowerOk() is what
 *    the gate asks.
 *
 * Both read one aligned word at a time and take no lock: they are called from
 * the protocol thread, which must never wait on a mutex the UI may hold.
 */
freq_t state_txFrequency(void);
uint32_t state_txPowerMw(void);

/** False when there is no power to transmit with: the keying gate refuses. */
static inline bool state_keyingPowerOk(void)
{
    return state_txPowerMw() != 0;
}

/**
 * Take state_mutex, but not for ever.
 *
 * A bounded acquisition for the callers that must keep answering while the
 * radio is busy: the PC protocol's transport thread is one frame away from a
 * client at any moment, and a plain lock parks it for as long as the UI or the
 * state task holds the mutex -- and for ever if that holder blocks
 * (docs/PC_PROTO.md).  Release with state_unlock().
 *
 * @return true when the lock was taken, false when timeoutMs went by.
 */
bool state_tryLock(uint32_t timeoutMs);
void state_unlock(void);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* STATE_H */
