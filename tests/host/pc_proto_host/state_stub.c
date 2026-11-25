/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The bit of core/state.c the protocol needs, without the platform behind it.
 * The settings handlers read `state.settings` under `state_mutex` and nothing
 * else, so the host device gives them exactly that.
 */

#include <pthread.h>
#include <string.h>

#include "core/state.h"

state_t state;
pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

/* channel_store.c asks for a settle delay after an erase, and the time for its
 * "last used" debounce; the CTCSS table comes from cps.c, which carries the
 * whole codeplug reader. */
#include <time.h>

void delayMs(unsigned int mseconds)
{
    (void)mseconds;
}

long long getTick(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * What core/state.c answers with, without the platform behind it.
 *
 * The transport and the handlers are the same code as the firmware's, so they
 * ask the same questions: where the radio would transmit and with what, and
 * how to take the state lock without waiting for ever.  The answers here are
 * the firmware's rules, and the test drives `state` directly to check them.
 */
freq_t state_txFrequency(void)
{
    freq_t tx = state.channel.tx_frequency;
    return (tx != 0) ? tx : state.channel.rx_frequency;
}

uint32_t state_txPowerMw(void)
{
    if (state.channel.power != 0)
        return (uint32_t)state.channel.power;
    return (uint32_t)state.settings.tx_power;
}

bool state_tryLock(uint32_t timeoutMs)
{
    if (pthread_mutex_trylock(&state_mutex) == 0)
        return true;

    uint32_t waited = 0;
    while (waited < timeoutMs) {
        delayMs(2);
        waited += 2;
        if (pthread_mutex_trylock(&state_mutex) == 0)
            return true;
    }
    return false;
}

void state_unlock(void)
{
    pthread_mutex_unlock(&state_mutex);
}

/* The 50 CTCSS tones, as cps.c has them; only the lookup by index is used. */
const uint16_t ctcss_tone[] = {
    0, 670, 693, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000,
    1035, 1072, 1109, 1148, 1188, 1226, 1273, 1318, 1365, 1413, 1462, 1514,
    1567, 1598, 1622, 1655, 1713, 1773, 1835, 1899, 1966, 2035, 2107, 2181,
    2257, 2336, 2418, 2503
};
