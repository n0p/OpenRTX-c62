/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The board layer under the real audio path table, for the QEMU harness.
 *
 * On the radio this is platform/drivers/audio/audio_c62.c: audio_connect() and
 * audio_disconnect() take a k_mutex (boardMutex) and then let the CSK6 codec
 * apply the route (csk6codec_setRoute(), which takes audioMutex as well).  This
 * file keeps the same shape and the same rule -- a mutex, then the driver --
 * and replaces only the codec, which does not exist on the emulated board.
 *
 * That is the point of the harness: the interrupt takes the path table's
 * lock (audio_path.cpp) and, with it released, lands here.  Either one is a
 * mutex, and either one aborts the kernel when it is asked for in interrupt
 * context -- which is what the radio's console showed.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "core/audio_path.h"
#include "interfaces/delays.h"
#include "core/blackbox.h"

/* Same lock, same bound as audio_c62.c: BOARD_LOCK_TIMEOUT_MS is 50 there. */
static K_MUTEX_DEFINE(boardMutex);
#define BOARD_LOCK_TIMEOUT_MS 50

static bool paths[SOURCE_MCU + 1][SINK_MCU + 1];

void audio_init(void)
{
}

void audio_terminate(void)
{
}

bool audio_ready(void)
{
    return true;
}

void audio_setExtSource(const bool on)
{
    (void)on;
}

void audio_connect(const enum AudioSource source, const enum AudioSink sink)
{
    if (k_mutex_lock(&boardMutex, K_MSEC(BOARD_LOCK_TIMEOUT_MS)) != 0) {
        printk("AUDIO: board lock busy, %d->%d not opened\n", (int)source,
               (int)sink);
        return;
    }

    paths[source][sink] = true;

    k_mutex_unlock(&boardMutex);
}

void audio_disconnect(const enum AudioSource source, const enum AudioSink sink)
{
    if (k_mutex_lock(&boardMutex, K_MSEC(BOARD_LOCK_TIMEOUT_MS)) != 0)
        return;

    paths[source][sink] = false;

    k_mutex_unlock(&boardMutex);
}

bool audio_checkPathCompatibility(const enum AudioSource p1Source,
                                  const enum AudioSink p1Sink,
                                  const enum AudioSource p2Source,
                                  const enum AudioSink p2Sink)
{
    if ((p1Sink == SINK_MCU) && (p1Source == p2Source))
        return true;
    if ((p2Sink == SINK_MCU) && (p2Source == p1Source))
        return true;

    return ((p1Source != p2Source) || (p1Source == SOURCE_MCU))
        && ((p1Sink != p2Sink) || (p1Sink == SINK_MCU));
}

/* The black box is a single word write per breadcrumb on the radio; here it
 * only has to exist, so the path code under test is unchanged. */
void bb_mark(uint8_t mark)
{
    static volatile uint8_t last;
    last = mark;
}

void delayMs(unsigned int mseconds)
{
    k_sleep(K_MSEC(mseconds));
}

void sleepFor(unsigned int seconds, unsigned int mseconds)
{
    k_sleep(K_MSEC(seconds * 1000u + mseconds));
}
