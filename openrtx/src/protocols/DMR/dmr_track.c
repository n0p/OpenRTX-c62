/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Stations heard with an LRRP position (dmr_track.h), after gmtii's
 * c62/dmr (37893f91).
 */

#include <stdio.h>
#include <string.h>
#include "interfaces/delays.h"
#include "protocols/DMR/dmr_port.h"
#include "protocols/DMR/dmr_track.h"

DMR_LOCK_DECLARE(s_lock);

/* Under s_lock. The table is in PSRAM, not zeroed at boot: s_ready says it
 * was cleared. Kept newest first. */
static dmr_station_t s_st[DMR_TRACK_MAX] EXT_RAM_BSS_ATTR;
static int s_count;
static bool s_ready;
static volatile uint32_t s_serial;

static void clearLocked(void)
{
    memset(s_st, 0, sizeof(s_st));
    s_count = 0;
    s_ready = true;
}

void dmr_track_clear(void)
{
    DMR_LOCK(s_lock);
    clearLocked();
    s_serial++;
    DMR_UNLOCK(s_lock);
}

static int find(uint32_t src)
{
    for (int i = 0; i < s_count; i++) {
        if (s_st[i].src == src)
            return i;
    }
    return -1;
}

void dmr_track_add(uint32_t src, float lat, float lon, int16_t speed_kmh,
                   int16_t dir, const char *alias)
{
    DMR_LOCK(s_lock);
    if (!s_ready)
        clearLocked();

    /* The station's entry, else a new one (the oldest, the last, when the
     * table is full); then moved to the front */
    dmr_station_t s;
    int i = find(src);
    if (i >= 0) {
        s = s_st[i];
    } else {
        memset(&s, 0, sizeof(s));
        s.src = src;
        if (s_count < DMR_TRACK_MAX)
            s_count++;
        i = s_count - 1;
    }
    memmove(&s_st[1], &s_st[0], (size_t)i * sizeof(s_st[0]));

    s.lat = lat;
    s.lon = lon;
    s.speed_kmh = speed_kmh;
    s.dir = dir;
    s.ms = (uint32_t)getTick();
    if (s.reports < UINT16_MAX)
        s.reports++;
    if (alias != NULL && alias[0] != '\0')
        snprintf(s.alias, sizeof(s.alias), "%s", alias);
    s_st[0] = s;
    s_serial++;
    DMR_UNLOCK(s_lock);
}

void dmr_track_set_alias(uint32_t src, const char *alias)
{
    if (alias == NULL || alias[0] == '\0')
        return;

    DMR_LOCK(s_lock);
    const int i = s_ready ? find(src) : -1;
    if (i >= 0 && strcmp(s_st[i].alias, alias) != 0) {
        snprintf(s_st[i].alias, sizeof(s_st[i].alias), "%s", alias);
        s_serial++;
    }
    DMR_UNLOCK(s_lock);
}

int dmr_track_get(dmr_station_t *out, int max)
{
    DMR_LOCK(s_lock);
    int n = s_ready ? s_count : 0;
    if (n > max)
        n = max;
    if (n > 0)
        memcpy(out, s_st, (size_t)n * sizeof(*out));
    DMR_UNLOCK(s_lock);
    return n;
}

uint32_t dmr_track_serial(void)
{
    return s_serial;
}
