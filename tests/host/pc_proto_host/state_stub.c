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

/* The 50 CTCSS tones, as cps.c has them; only the lookup by index is used. */
const uint16_t ctcss_tone[] = {
    0, 670, 693, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000,
    1035, 1072, 1109, 1148, 1188, 1226, 1273, 1318, 1365, 1413, 1462, 1514,
    1567, 1598, 1622, 1655, 1713, 1773, 1835, 1899, 1966, 2035, 2107, 2181,
    2257, 2336, 2418, 2503
};
