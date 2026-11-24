/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/ui.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "core/event.h"
#include "core/state.h"
#include "core/battery.h"
#include "hwconfig.h"
#include "interfaces/platform.h"
#include "interfaces/nvmem.h"
#include "core/crc.h"
#include "core/channel_store.h"
#include "interfaces/delays.h"
#ifdef CONFIG_TETRA
#include "protocols/TETRA/TetraSds.h"
#endif

/*
 * Platforms without a graceful shutdown (the C62 cuts power with the volume
 * knob, so state_terminate() never runs) ask for the debounced write by
 * defining NVMEM_DEBOUNCED_WRITE in their hwconfig.h: settings and VFO are
 * then written to flash once they have been stable for the debounce window.
 */
#ifdef NVMEM_DEBOUNCED_WRITE

#ifndef NVM_WRITE_DEBOUNCE_MS
#define NVM_WRITE_DEBOUNCE_MS 3000
#endif

struct debouncer {
    long long lastChange;
    uint16_t crc;
    bool pending;
};

static struct debouncer settingsDebouncer;
static struct debouncer vfoDebouncer;

static bool hasChanged(struct debouncer *db, const void *data, const size_t len)
{
    uint16_t crc = crc_ccitt(data, len);
    if (crc != db->crc) {
        db->crc = crc;
        db->pending = true;
        db->lastChange = getTick();
    } else {
        long long elapsed = getTick() - db->lastChange;
        if (db->pending && (elapsed > NVM_WRITE_DEBOUNCE_MS)) {
            db->pending = false;
            return true;
        }
    }

    return false;
}

#endif /* NVMEM_DEBOUNCED_WRITE */

state_t state;
pthread_mutex_t state_mutex;
static long long int lastUpdate = 0;

// Commonly used frequency steps, expressed in Hz
const uint32_t freq_steps[] = { 1000,  5000,  6250,  10000, 12500,
                                15000, 20000, 25000, 50000, 100000 };
const size_t n_freq_steps = sizeof(freq_steps) / sizeof(freq_steps[0]);

void state_init()
{
    pthread_mutex_init(&state_mutex, NULL);

    /*
     * Try loading settings from nonvolatile memory and default to sane values
     * in case of failure.
     */
    if (nvm_readSettings(&state.settings) < 0) {
        state.settings = default_settings;
    }

    /*
     * Try loading VFO configuration from nonvolatile memory and default to sane
     * values in case of failure.
     */
    if (nvm_readVfoChannelData(&state.channel) < 0) {
        state.channel = cps_getDefaultChannel();
    }

    /*
     * Channels kept by the in-radio editor: come back on the channel that was
     * selected at power-off. With an empty bank the VFO above stays in use.
     */
    chstore_init();
    if (chstore_count() > 0) {
        channel_t ch;
        int idx = chstore_lastUsed();
        if (idx >= 0 && chstore_toChannel((uint16_t)idx, &ch) == 0) {
            state.channel = ch;
            state.channel_index = (uint16_t)idx;
        }
    }

/*
     * Initialise remaining fields
     */
#ifdef CONFIG_RTC
    state.time = platform_getCurrentTime();
#endif
    state.v_bat = platform_getVbat();
    state.charge = battery_getCharge(state.v_bat);
    state.rssi = -127.0f;
    state.volume = platform_getVolumeLevel();

    state.channel_index = 0; // Set default channel index (it is 0-based)
    state.bank_enabled = false;
    state.rtxStatus = RTX_OFF;
    state.emergency = false;
    state.txDisable = false;
    state.step_index = 4; // Default frequency step 12.5kHz

    // Force brightness field to be in range 0 - 100
    if (state.settings.brightness > 100) {
        state.settings.brightness = 100;
    }

#ifdef NVMEM_DEBOUNCED_WRITE
    settingsDebouncer.crc = crc_ccitt(&state.settings, sizeof(settings_t));
    vfoDebouncer.crc = crc_ccitt(&state.channel, sizeof(channel_t));
#endif

#ifdef CONFIG_TETRA
    /* Received SDS messages, kept across power cycles */
    tetraSds_load();
#endif
}

void state_terminate()
{
    // Never store a brightness of 0 to avoid booting with a black screen
    if (state.settings.brightness == 0) {
        state.settings.brightness = 5;
    }

    nvm_writeSettingsAndVfo(&state.settings, &state.channel);
    /* Flush a pending "last used channel" before the radio powers off */
    chstore_commit();
    pthread_mutex_destroy(&state_mutex);
}

void state_task()
{
#ifdef NVMEM_DEBOUNCED_WRITE
    settings_t settings;
    channel_t vfo;
#endif

    // Update radio state once every 100ms
    if ((getTick() - lastUpdate) < 100)
        return;

    lastUpdate = getTick();

    pthread_mutex_lock(&state_mutex);

    /*
     * Low-pass filtering with a time constant of 10s when updated at 1Hz
     * Original computation: state.v_bat = 0.02*vbat + 0.98*state.v_bat
     * Peak error is 18mV when input voltage is 49mV.
     *
     * NOTE: GD77 and DM-1801 already have an hardware low-pass filter on the
     * vbat pin. Adding also the digital one seems to cause more troubles than
     * benefits.
     */
    uint16_t vbat = platform_getVbat();
#if defined(PLATFORM_GD77) || defined(PLATFORM_DM1801)
    state.v_bat = vbat;
#else
    state.v_bat -= (state.v_bat * 2) / 100;
    state.v_bat += (vbat * 2) / 100;
#endif

    /*
     * Update volume level, as a 50% average between previous value and a new
     * read of the knob position. This gives a good reactivity while preventing
     * the volume level to jitter when the knob is not being moved.
     */
    uint16_t vol = platform_getVolumeLevel() + state.volume;
    state.volume = vol / 2;

    state.charge = battery_getCharge(state.v_bat);
    state.rssi = rtx_getRssi();

#ifdef CONFIG_RTC
    state.time = platform_getCurrentTime();
#endif

#ifdef NVMEM_DEBOUNCED_WRITE
    settings = state.settings;
    vfo = state.channel;
#endif
    pthread_mutex_unlock(&state_mutex);

#ifdef NVMEM_DEBOUNCED_WRITE
    /*
     * Persist here, and only here: this task runs every 100 ms and can afford
     * the tens of milliseconds of a 4 KiB sector erase, the audio path cannot.
     * hasChanged() restarts its timer on every change, so a knob being turned
     * postpones the write instead of triggering one per interval.
     */
    bool writeSettings = hasChanged(&settingsDebouncer, &settings,
                                    sizeof(settings_t));
    bool writeVfo = hasChanged(&vfoDebouncer, &vfo, sizeof(channel_t));
    if (writeSettings || writeVfo) {
        nvm_writeSettingsAndVfo(writeSettings ? &settings : NULL,
                                writeVfo ? &vfo : NULL);
    }
#endif

#ifdef CONFIG_TETRA
    /*
     * Persist SDS messages once the traffic has been quiet for a while: an
     * erase of the 4 KiB sector takes tens of milliseconds, which this task
     * (100 ms cadence, not the audio path) can afford, but the first slot
     * after a message must not wait for it.
     */
    tetraSds_task();
#endif

    /* Debounced write-back of the last used channel */
    chstore_task();

    ui_pushEvent(EVENT_STATUS, 0);
}

void state_resetSettingsAndVfo()
{
    state.settings = default_settings;
    state.channel = cps_getDefaultChannel();
}
