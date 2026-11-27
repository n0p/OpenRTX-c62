/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/ui.h"
#include <stdio.h>
#ifdef __ZEPHYR__
#include <zephyr/sys/printk.h>
#endif
#include <string.h>
#include <stdlib.h>
#include "core/event.h"
#include "core/state.h"
#ifdef CONFIG_C62_NOISE_SUPPRESS
#include "core/noise_suppress.h"
#endif
#include "core/battery.h"
#include "hwconfig.h"
#include "interfaces/platform.h"
#include "interfaces/nvmem.h"
#include "core/crc.h"
#include "core/channel_store.h"
#include "core/nvm_debounce.h"
#include "core/scan_service.h"
#ifdef CONFIG_VFO_SEARCH
#include "core/vfo_search_service.h"
#endif
#include "interfaces/delays.h"
#ifdef CONFIG_TETRA
#include "protocols/TETRA/TetraSds.h"
#endif
#ifdef CONFIG_MESSAGING
#include "core/messaging.h"
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

static struct nvmDebounce settingsDebouncer;
static struct nvmDebounce vfoDebouncer;

/*
 * A settings or VFO write that does not land used to be invisible: the change
 * stayed in RAM, the radio behaved as if it were saved and the operator found
 * out at the next power cycle.  One line per streak -- the retry may fail for
 * minutes -- puts it in the PC protocol's log ring and on the console.
 */
static void logNvmWriteFailure(int ret)
{
    char line[64];

    sniprintf(line, sizeof(line), "nvm: write failed (%d), retrying", ret);
#ifdef __ZEPHYR__
    printk("%s\n", line);
#else
    printf("%s\n", line);
#endif
}

#endif /* NVMEM_DEBOUNCED_WRITE */

state_t state;
pthread_mutex_t state_mutex;
static long long int lastUpdate = 0;

// Commonly used frequency steps, expressed in Hz. Only appended to: the index
// of the chosen step is stored in the settings.
const uint32_t freq_steps[] = { 1000,  5000,  6250,  10000, 12500,
                                15000, 20000, 25000, 50000, 100000,
                                8333 /* 8.33 kHz, the airband raster */ };
const size_t n_freq_steps = sizeof(freq_steps) / sizeof(freq_steps[0]);

void state_init()
{
    pthread_mutex_init(&state_mutex, NULL);

    /*
     * Try loading settings from nonvolatile memory and default to sane values
     * in case of failure.  A record written by another version of the firmware
     * is dropped as a whole (core/settings_record.h) and the reason is kept so
     * that the screen can say the settings were reset instead of leaving the
     * operator wondering why his callsign is gone.
     */
    int settingsRet = nvm_readSettings(&state.settings);

    if (settingsRet < 0) {
        int cause = -settingsRet;

        state.settings = default_settings;
        state.settings_reset = (cause >= SETTINGS_RESET_BLANK &&
                                cause <= SETTINGS_RESET_CORRUPT)
                             ? (uint8_t)cause : (uint8_t)SETTINGS_RESET_BLANK;
    } else {
        state.settings_reset = SETTINGS_NO_RESET;
    }

#ifdef CONFIG_C62_NOISE_SUPPRESS
    /*
     * Arm the noise suppressor from the stored setting. Off is the default and
     * costs nothing: the stage allocates and initialises only when enabled.
     */
    noiseSuppress_init();
    if (state.settings.noiseSuppression)
        state.settings.noiseSuppression = noiseSuppress_setEnabled(true);
#endif

    /*
     * Try loading VFO configuration from nonvolatile memory and default to sane
     * values in case of failure.
     */
    if (nvm_readVfoChannelData(&state.channel) < 0) {
        state.channel = cps_getDefaultChannel();
    }

    /*
     * Default channel index, and it comes before anything that can set it: a
     * VFO leaves it at 0, and the stored-channel block below overwrites it
     * with the index it restored.  Assigning 0 after that block -- which is
     * what this used to do, in the inherited "initialise remaining fields"
     * section -- threw the restored index away while keeping the channel: the
     * header drew "CH 1" next to another channel's name, the voice prompt
     * announced the wrong number and the first knob turn skipped a channel.
     */
    state.channel_index = 0;

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
            /*
             * And its DMR call destination, which channel_t cannot carry: the
             * radio comes back on the talkgroup of the channel it comes back
             * on, not on whatever the settings record held.
             */
            if (ch.mode == OPMODE_DMR)
                (void)chstore_applySettings((uint16_t)idx, &state.settings);
        }
    }

    /*
     * Seed the two debouncers with what came out of the flash, before the
     * repairs below: a field the firmware has to rewrite at load (a brightness
     * above 100, a power above the PA's ceiling, a VFO with no transmit
     * frequency) has to reach the record once, or every boot repairs RAM and
     * the flash keeps a value this firmware never accepts.  The debouncer sees
     * the difference on the first pass of state_task() and writes it after the
     * debounce window, like any other change.  Nothing is written when there
     * was nothing to repair, which is the common case.
     */
#ifdef NVMEM_DEBOUNCED_WRITE
    nvmDebounce_seed(&settingsDebouncer, &state.settings, sizeof(settings_t));
    nvmDebounce_seed(&vfoDebouncer, &state.channel, sizeof(channel_t));
#endif

    /*
     * A VFO record written before settings_t.tx_power existed carries neither
     * a transmit frequency nor a power: the operator who boots on it gets a
     * radio that keys nothing and says nothing (docs/TX_SAFETY.md).  Fill the
     * gaps from what the radio does know, once, here, so every consumer sees
     * the same numbers -- and leave a real channel alone: a channel's power of
     * 0 is the operator's own "do not transmit" and it is not overwritten.
     */
    if (state.channel.tx_frequency == 0)
        state.channel.tx_frequency = state.channel.rx_frequency;

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

    state.bank_enabled = false;
    state.rtxStatus = RTX_OFF;
    state.emergency = false;
    state.txDisable = false;
    state.unattendedTxDisable = false;
    state.step_index = 4; // Default frequency step 12.5kHz

    // Force brightness field to be in range 0 - 100
    if (state.settings.brightness > 100) {
        state.settings.brightness = 100;
    }

    /* A power above the PA's ceiling is not transmitted: fall back, never clamp
     * to maximum, so a corrupt record does not become a 5 W setting. */
    if (!settings_txPowerValid(state.settings.tx_power))
        state.settings.tx_power = default_settings.tx_power;

    /*
     * The callsign is checked here too, not only on the keypad and on the PC
     * protocol: a record written by another tool or restored from a backup
     * never passed either check, and a callsign that fills the 10-byte field
     * has no room for its terminator -- which is the reason the field is nine
     * characters plus NUL in the first place.  N0CALL instead of a broken
     * identity: the operator can type his own again, and nothing reads past
     * the field meanwhile.
     */
    if (!settings_callsignFieldValid(state.settings.callsign))
        memcpy(state.settings.callsign, default_settings.callsign,
               SETTINGS_CALLSIGN_LEN);

#ifdef CONFIG_TETRA
    /* Received SDS messages, kept across power cycles */
    tetraSds_load();
#endif

#ifdef CONFIG_MESSAGING
    /* The message store: outbox and inbox, also kept across power cycles */
    messaging_init();
#endif

#ifdef CONFIG_SCAN
    /* Scan and vigilance: the machine is reset here, the bank and the settings
     * it will read are already loaded above */
    scan_service_init();
#endif

#ifdef CONFIG_VFO_SEARCH
    /* Carrier search: nothing to load, it only resets the machine */
    vfoSearchService_init();
#endif
}

void state_terminate()
{
#ifdef CONFIG_SCAN
    /*
     * A scan in progress must not be stored as if it were the operator's
     * channel: stop it first, which puts the receiver (and the DMV/VFO record)
     * back where the scan found it.
     */
    scan_service_stop();
#endif

    // Never store a brightness of 0 to avoid booting with a black screen
    if (state.settings.brightness == 0) {
        state.settings.brightness = 5;
    }

    nvm_writeSettingsAndVfo(&state.settings, &state.channel);
    /* Flush a pending "last used channel" before the radio powers off */
    chstore_commit();
    pthread_mutex_destroy(&state_mutex);
}

#ifdef NVMEM_DEBOUNCED_WRITE
/**
 * Is the receiver being moved by a service instead of by the operator?
 *
 * True while the channel scan, the bank scan or the carrier search is running:
 * those write state.channel themselves as they walk, so neither the VFO record
 * nor the settings may be taken as the operator's during that time.  Called
 * from state_task() (main thread), where both services are stepped, so what it
 * reads cannot change under it.
 */
static bool receiverBorrowed(void)
{
#ifdef CONFIG_SCAN
    struct scanStatus scan;

    if (scan_service_status(&scan) && scan.running)
        return true;
#endif
#ifdef CONFIG_VFO_SEARCH
    if (vfoSearchService_active())
        return true;
#endif
    return false;
}
#endif /* NVMEM_DEBOUNCED_WRITE */

void state_task()
{
#ifdef NVMEM_DEBOUNCED_WRITE
    settings_t settings;
    channel_t vfo;
#endif

#ifdef CONFIG_SCAN
    /*
     * The scan is stepped here, before the 100 ms gate below: its timers are in
     * milliseconds and a retune has to land between two audio blocks, so it
     * runs at the 5 ms cadence of this task.  It costs nothing while no scan is
     * running (one flag test).
     */
    scan_service_poll();
#endif

#ifdef CONFIG_VFO_SEARCH
    /*
     * The carrier search runs at the same cadence and for the same reason: its
     * dwell and its debounce are in milliseconds.  One flag test when it is
     * idle.
     */
    vfoSearchService_poll();
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
     * The debouncer restarts its window on every change, so a knob being
     * turned postpones the write instead of triggering one per interval.
     *
     * The write result is not optional: the change is only marked as stored
     * once the record is actually on flash.  A failed write leaves it pending
     * and is retried on a backoff (nvmDebounce_retryMs) instead of being lost
     * -- a power or a squelch level that stays in RAM while the radio behaves
     * as if it were saved is exactly the silent failure docs/TX_SAFETY.md is
     * about -- and the first failure of a streak says so in the log.
     */
    uint32_t now = (uint32_t)getTick();

    /*
     * The scanner and the carrier search move the receiver themselves: they
     * write state.channel on every hop and, on a DMR channel, the destination
     * in the settings.  A stop of more than the debounce window on a busy
     * channel would otherwise write that borrowed channel to flash as if the
     * operator had tuned it -- erasing and rewriting the VFO sector, and
     * leaving the radio booted on the channel the scan was sitting on with
     * that channel's talkgroup.  state_terminate() stops the scan before it
     * writes for the same reason, but on this radio power-off is the volume
     * knob cutting the supply and that path never runs.
     *
     * Skipping the comparison rather than the write is deliberate: the
     * debouncer's copy still describes what is on flash, so when the scan ends
     * and restores the channel it found, the two match again and nothing is
     * written at all.
     */
    if (receiverBorrowed())
        return;

    bool writeSettings = nvmDebounce_due(&settingsDebouncer, &settings,
                                         sizeof(settings_t), now,
                                         NVM_WRITE_DEBOUNCE_MS);
    bool writeVfo = nvmDebounce_due(&vfoDebouncer, &vfo, sizeof(channel_t), now,
                                    NVM_WRITE_DEBOUNCE_MS);

    if (writeSettings || writeVfo) {
        int ret = nvm_writeSettingsAndVfo(writeSettings ? &settings : NULL,
                                          writeVfo ? &vfo : NULL);

        if (ret == 0) {
            if (writeSettings)
                nvmDebounce_done(&settingsDebouncer);
            if (writeVfo)
                nvmDebounce_done(&vfoDebouncer);
        } else {
            bool first = (settingsDebouncer.failures == 0)
                         && (vfoDebouncer.failures == 0);

            if (writeSettings)
                nvmDebounce_failed(&settingsDebouncer, now);
            if (writeVfo)
                nvmDebounce_failed(&vfoDebouncer, now);

            if (first)
                logNvmWriteFailure(ret);
        }
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
#ifdef CONFIG_MESSAGING
    /* Encode what the messaging screen queued for TETRA (no transmit chain) */
    (void)tetraSds_prepareQueued();
#endif
#endif

#ifdef CONFIG_MESSAGING
    messaging_task((uint32_t)getTick());
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

uint8_t state_settingsResetCause(void)
{
    return state.settings_reset;
}

void state_settingsResetAck(void)
{
    state.settings_reset = SETTINGS_NO_RESET;
}

/*
 * ----------------------------------------------------------------------------
 * Transmit frequency, transmit power, and the bounded state lock
 * ----------------------------------------------------------------------------
 */

freq_t state_txFrequency(void)
{
    /* A VFO says nothing about transmit: simplex, i.e. the receive frequency.
     * A channel carries its own, offset included.  0 is never returned when
     * the receiver is on a real frequency, which is what keeps the driver's
     * range check from refusing the keying in silence. */
    freq_t tx = state.channel.tx_frequency;
    return (tx != 0) ? tx : state.channel.rx_frequency;
}

uint32_t state_txPowerMw(void)
{
    /*
     * The channel wins, the setting is what a VFO (or a channel record from
     * before the field existed, power 0) falls back to.  A setting of 0 means
     * the operator asked for no transmit at all: the gate refuses the keying
     * and the status says so (docs/TX_SAFETY.md).
     */
    if (state.channel.power != 0)
        return (uint32_t)state.channel.power;
    return (uint32_t)state.settings.tx_power;
}

/** Polling step of the bounded lock below, ms. */
#define STATE_LOCK_STEP_MS 2

bool state_tryLock(uint32_t timeoutMs)
{
    /*
     * A bounded acquisition, for the callers that must keep answering while the
     * radio is busy: the PC protocol's transport thread is one frame away from
     * a client at any moment, and a plain pthread_mutex_lock() parks it for as
     * long as the UI or the state task holds the lock -- across ui_updateFSM()
     * or across the battery read, and, if that holder ever blocks, for ever.
     * A radio that goes mute with no fault and no log line is exactly what that
     * costs (docs/PC_PROTO.md, "The transport never waits for ever").
     */
    if (pthread_mutex_trylock(&state_mutex) == 0)
        return true;

    uint32_t waited = 0;
    while (waited < timeoutMs) {
        delayMs(STATE_LOCK_STEP_MS);
        waited += STATE_LOCK_STEP_MS;
        if (pthread_mutex_trylock(&state_mutex) == 0)
            return true;
    }
    return false;
}

void state_unlock(void)
{
    pthread_mutex_unlock(&state_mutex);
}
