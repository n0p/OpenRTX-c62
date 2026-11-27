/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <hwconfig.h>
#include <interfaces/radio.h>
#include "csk6_codec.h"

/*
 * Board layer: radio AF output and speaker GPIO, then the codec driver
 * connects the sources and sinks. This lock serializes route changes; the
 * codec worker never takes it, so route changes can wait for the worker.
 * audio_init() and audio_terminate() are called once each, so they don't
 * take it.
 *
 * Bounded, not K_FOREVER: a route change is called from the RTX thread (the
 * squelch of a mode, the mode change itself) and from the UI thread (a beep, a
 * voice prompt), and one of those callers holding this lock for ever is a dead
 * radio with a frozen screen. If the lock does not arrive in
 * BOARD_LOCK_TIMEOUT_MS the path is simply not opened: the mode asks again on
 * its next 30 ms update, and the caller that was going to wait for ever goes on
 * to report a missing path instead.
 */
#define BOARD_LOCK_TIMEOUT_MS 50

static K_MUTEX_DEFINE(boardMutex);
static bool initialized;
/* Active routes, by source and sink */
static bool paths[SOURCE_MCU + 1][SINK_MCU + 1];
/*
 * Analog source wired straight to the amplifier, outside the codec: the
 * BK1080 of the broadcast FM mode. It only needs the amplifier switch, but
 * the switch has to stay on while the source is selected even though no
 * codec route is open.
 */
static bool extSource;

/*
 * Both device tables use one driver: the config selects capture or playback,
 * and the instance is the codec channel, the same number as the endpoint.
 */
const struct audioDevice outputDevices[] = {
    { .driver = NULL, .config = NULL, .instance = 0, .endpoint = SINK_MCU },
    {
        .driver = &csk6_codec_audio_driver,
        .config = (const void *)CSK6_CODEC_MODE_OUTPUT,
        .instance = SINK_RTX,
        .endpoint = SINK_RTX,
    },
    {
        .driver = &csk6_codec_audio_driver,
        .config = (const void *)CSK6_CODEC_MODE_OUTPUT,
        .instance = SINK_SPK,
        .endpoint = SINK_SPK,
    },
};

const struct audioDevice inputDevices[] = {
    { .driver = NULL, .config = NULL, .instance = 0, .endpoint = SOURCE_MCU },
    {
        .driver = &csk6_codec_audio_driver,
        .config = (const void *)CSK6_CODEC_MODE_INPUT,
        .instance = SOURCE_RTX,
        .endpoint = SOURCE_RTX,
    },
    {
        .driver = &csk6_codec_audio_driver,
        .config = (const void *)CSK6_CODEC_MODE_INPUT,
        .instance = SOURCE_MIC,
        .endpoint = SOURCE_MIC,
    },
};

/**
 * Keep the speaker enabled while any active route plays to it.
 */
static void updateSpeaker(void)
{
    bool speaker = extSource;

    for (size_t i = 0; i < ARRAY_SIZE(paths); i++)
        speaker |= paths[i][SINK_SPK];
#ifndef C62_ENABLE_SWD
    gpio_pin_set_dt(&speaker_enable, speaker);
#endif
}

void audio_init(void)
{
#ifndef C62_ENABLE_SWD
    gpio_pin_configure_dt(&speaker_enable, GPIO_OUTPUT_INACTIVE);
#endif
    gpio_pin_configure_dt(&dtmf_enable, GPIO_OUTPUT_INACTIVE);

    const int err = csk6codec_init();
    initialized = (err == 0);

    /*
     * A codec that did not start leaves every audio_* call returning without a
     * word: no receiver audio, no transmit audio, and a UI, a PTT and a PA
     * that all work perfectly.  That is the failure nobody diagnoses, so it is
     * said here, once, with the code that explains it.
     */
    if (!initialized)
        printk("AUDIO: codec init failed: %d\n", err);
}

/**
 * Whether the audio path came up.
 *
 * @return true when the codec answered and the routes can be applied.
 */
bool audio_ready(void)
{
    return initialized;
}

void audio_terminate(void)
{
    if (!initialized)
        return;

#ifndef C62_ENABLE_SWD
    gpio_pin_set_dt(&speaker_enable, 0);
#endif
    extSource = false;
    radio_disableAfOutput();
    csk6codec_terminate();
    initialized = false;
}

void audio_setExtSource(const bool on)
{
    if (!initialized)
        return;

    if (k_mutex_lock(&boardMutex, K_MSEC(BOARD_LOCK_TIMEOUT_MS)) != 0)
    {
        printk("AUDIO: board lock busy, external source not set\r\n");
        return;
    }

    extSource = on;
    updateSpeaker();
    k_mutex_unlock(&boardMutex);
}

void audio_connect(const enum AudioSource source, const enum AudioSink sink)
{
    if (!initialized)
        return;

    if (k_mutex_lock(&boardMutex, K_MSEC(BOARD_LOCK_TIMEOUT_MS)) != 0)
    {
        printk("AUDIO: board lock busy, %d->%d not opened\r\n", (int)source,
               (int)sink);
        return;
    }

    if (!paths[source][sink]) {
        /*
         * The path is recorded as open only when the codec applied the route:
         * a worker that could not take the route in time leaves it closed, so
         * the next request (the mode asks again on every squelch edge) tries
         * instead of counting on audio that is not there.
         */
        if (csk6codec_setRoute(source, sink, true) == 0) {
            paths[source][sink] = true;
            if ((source == SOURCE_RTX) && (sink == SINK_SPK))
                radio_enableAfOutput();
        } else {
            printk("AUDIO: route %d->%d not applied by the codec\r\n",
                   (int)source, (int)sink);
        }
        updateSpeaker();
    }
    k_mutex_unlock(&boardMutex);
}

void audio_disconnect(const enum AudioSource source, const enum AudioSink sink)
{
    if (!initialized)
        return;

    if (k_mutex_lock(&boardMutex, K_MSEC(BOARD_LOCK_TIMEOUT_MS)) != 0)
    {
        printk("AUDIO: board lock busy, %d->%d not closed\r\n", (int)source,
               (int)sink);
        return;
    }

    if (paths[source][sink]) {
        paths[source][sink] = false;
        (void)csk6codec_setRoute(source, sink, false);
        updateSpeaker();
        if ((source == SOURCE_RTX) && (sink == SINK_SPK))
            radio_disableAfOutput();
    }
    k_mutex_unlock(&boardMutex);
}

bool audio_checkPathCompatibility(const enum AudioSource p1Source,
                                  const enum AudioSink p1Sink,
                                  const enum AudioSource p2Source,
                                  const enum AudioSink p2Sink)
{
    /*
     * Physical sources and sinks are exclusive. MCU is a memory endpoint,
     * so separate capture and playback streams can share it.
     *
     * A path into SINK_MCU is also compatible with another path from the same
     * source: the codec captures every physical source into the application
     * buffers and into the FM passthrough queues from the same frame
     * (captureFrame() in csk6_codec.c), so the discriminator can feed the
     * speaker and the APRS decoder at the same time without touching audio.
     * Only one capture per source is allowed, which the SOURCE_RTX/SINK_MCU
     * path is.
     */
    if ((p1Sink == SINK_MCU) && (p1Source == p2Source))
        return true;
    if ((p2Sink == SINK_MCU) && (p2Source == p1Source))
        return true;

    return ((p1Source != p2Source) || (p1Source == SOURCE_MCU))
        && ((p1Sink != p2Sink) || (p1Sink == SINK_MCU));
}
