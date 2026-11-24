/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/zones.h"
#include "core/airband.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * The plans of this build.  Frequencies are the channel the radio listens on
 * (the repeater output); the shift takes it to the repeater input.  See
 * docs/ZONES.md for the sources.
 */
static const zone_t zones[ZONE_COUNT] =
{
    {
        .id       = ZONE_VHF_REPEATERS,
        .prefix   = "R",
        .descr    = "2 m repeaters",
        .base     = 145600000U,   /* 145.600 MHz, R0 = RV48          */
        .step     =   25000,
        .shift    =  -600000,     /* input 600 kHz below the output  */
        .count    = 8,
        .first    = 0,
        .mode     = OPMODE_FM,
        .bandwidth= BW_12_5,
        .power    = 5000,         /* the PA's maximum, the operator can lower it */
        .flags    = ZONE_FLAG_REPEATER,
        .planEvery= 1,
    },
    {
        .id       = ZONE_UHF_REPEATERS,
        .prefix   = "U",
        .descr    = "70 cm repeaters",
        .base     = 438650000U,   /* 438.650 MHz, R70 = RU692        */
        .step     =   12500,
        .shift    = -7600000,     /* input 7.6 MHz below the output  */
        .count    = 101,          /* U0..U100                        */
        .first    = 0,
        .mode     = OPMODE_FM,
        .bandwidth= BW_12_5,
        .power    = 5000,
        .flags    = ZONE_FLAG_REPEATER,
        .planEvery= 2,            /* the 25 kHz raster is the even indices */
    },
    {
        .id       = ZONE_PMR446,
        .prefix   = "PMR",
        .descr    = "PMR446 500 mW",
        .base     = 446006250U,   /* 446.00625 MHz, PMR446 channel 1  */
        .step     =   12500,
        .shift    =        0,     /* simplex                          */
        .count    = 16,
        .first    = 1,            /* PMR1..PMR16, 1-based names       */
        .mode     = OPMODE_FM,
        .bandwidth= BW_12_5,
        .power    = 500,          /* 500 mW, fixed by the standard    */
        .flags    = ZONE_FLAG_PMR | ZONE_FLAG_FIXED_POWER | ZONE_FLAG_SIMPLEX,
        .planEvery= 1,
    },
    {
        .id       = ZONE_AIRBAND,
        .prefix   = "A",
        .descr    = "Airband AM 118-137",
        .base     = 118000000U,   /* 118.000 MHz, A0 = channel 118.000 */
        .step     =   25000,      /* the 25 kHz raster                */
        .shift    =        0,     /* receive only, no TX frequency    */
        .count    = AIRBAND_CHANNELS_25K,
        .first    = 0,
        .mode     = OPMODE_AIRBAND,
        .bandwidth= BW_12_5,      /* 8.33 kHz spacing: narrow IF      */
        .power    = 5000,         /* unused: the channel is RX only   */
        .flags    = ZONE_FLAG_AIRBAND | ZONE_FLAG_RX_ONLY | ZONE_FLAG_SIMPLEX,
        .planEvery= 1,
    },
};

static bool shiftInverted = false;

int zones_count(void)
{
    return ZONE_COUNT;
}

const zone_t *zones_get(uint8_t id)
{
    if (id >= ZONE_COUNT)
        return NULL;

    return &zones[id];
}

const zone_t *zones_at(int i)
{
    if (i < 0 || i >= ZONE_COUNT)
        return NULL;

    return &zones[i];
}

int zones_channels(const zone_t *z)
{
    return (z == NULL) ? 0 : (int)z->count;
}

static int32_t zoneShift(const zone_t *z)
{
    /* PMR is simplex: inverting it must not invent a shift */
    if ((z->shift == 0) || ((z->flags & ZONE_FLAG_SIMPLEX) != 0))
        return 0;

    return shiftInverted ? -z->shift : z->shift;
}

const char *zones_channelName(uint8_t id, uint16_t index, char *buf,
                              size_t cap)
{
    const zone_t *z = zones_get(id);

    if (buf == NULL || cap == 0)
        return "";

    if (z == NULL || index >= z->count) {
        buf[0] = '\0';
        return buf;
    }

    /* An airband channel is called by its frequency, "118.025" */
    if ((z->flags & ZONE_FLAG_AIRBAND) != 0) {
        freq_t freq = z->base + (freq_t)z->step * index;
        return airband_channelName(freq, buf, cap);
    }

    snprintf(buf, cap, "%s%u", z->prefix, (unsigned)(z->first + index));
    return buf;
}

int zones_channel(uint8_t id, uint16_t index, zone_channel_t *out)
{
    const zone_t *z = zones_get(id);
    zone_channel_t zc;

    if (z == NULL || out == NULL || index >= z->count)
        return -EINVAL;

    memset(&zc, 0, sizeof(zc));
    zc.zone = z->id;
    zc.index = index;
    zones_channelName(id, index, zc.name, sizeof(zc.name));

    zc.rx = (uint32_t)((int64_t)z->base + (int64_t)z->step * (int64_t)index);
    zc.shift = zoneShift(z);
    zc.tx = (uint32_t)((int64_t)zc.rx + (int64_t)zc.shift);

    zc.mode = z->mode;
    zc.bandwidth = z->bandwidth;
    zc.power = z->power;
    zc.flags = z->flags;

    if (zc.shift == 0)
        zc.flags |= ZONE_FLAG_SIMPLEX;

    /*
     * Half channels: the Spanish UHF plan is a 25 kHz raster (32 channels,
     * BOE 137/2006 §5.2).  The odd indices of the U zone are the 12.5 kHz
     * offsets between them, kept because the operator numbering goes to U100
     * and because half channels are in use on VHF; they are flagged so the
     * screen can say "off plan" instead of pretending they are equivalent.
     */
    if (z->planEvery > 1 && (index % z->planEvery) != 0)
        zc.flags |= ZONE_FLAG_INTERLEAVED;

    *out = zc;
    return 0;
}

bool zones_channelValid(const zone_channel_t *zc)
{
    if (zc == NULL)
        return false;

    if (!chstore_freqValid(zc->rx) || !chstore_freqValid(zc->tx))
        return false;

    /*
     * PMR446 is 500 mW ERP and nothing else: a channel saved with 5 W would
     * be an illegal transmission, so the plan's power is the only accepted
     * one.  Everything else can be lowered by the operator.
     */
    if ((zc->flags & ZONE_FLAG_FIXED_POWER) != 0 && zc->power != 500)
        return false;

    if (zc->power == 0 || zc->power > 5000)
        return false;

    if (zc->mode != OPMODE_FM && zc->mode != OPMODE_M17 &&
        zc->mode != OPMODE_DMR && zc->mode != OPMODE_TETRA &&
<<<<<<< HEAD
        zc->mode != OPMODE_AIRBAND)
=======
        zc->mode != OPMODE_YSF)
>>>>>>> c62/rx-fusion
        return false;

    return true;
}

int zones_toChannel(uint8_t id, uint16_t index, channel_t *out)
{
    zone_channel_t zc;

    if (out == NULL || zones_channel(id, index, &zc) != 0)
        return -EINVAL;

    memset(out, 0, sizeof(channel_t));
    out->mode = zc.mode;
    out->bandwidth = zc.bandwidth;
    out->power = zc.power;
    out->rx_frequency = zc.rx;
    out->tx_frequency = zc.tx;
    out->rx_only = 0;
    strncpy(out->name, zc.name, CPS_STR_SIZE - 1);

    return 0;
}

int zones_toStore(uint8_t id, uint16_t index, chstore_channel_t *out)
{
    zone_channel_t zc;

    if (out == NULL || zones_channel(id, index, &zc) != 0)
        return -EINVAL;

    memset(out, 0, sizeof(*out));
    strncpy(out->name, zc.name, CHSTORE_NAME_LEN - 1);
    out->rx_freq = zc.rx;
    out->tx_freq = zc.tx;
    out->power = zc.power;
    out->mode = zc.mode;
    out->bandwidth = zc.bandwidth;
    out->flags = 0;

    if (zc.tx == zc.rx)
        out->flags |= CHSTORE_FLAG_SIMPLEX;

    if ((zc.flags & ZONE_FLAG_RX_ONLY) != 0)
        out->flags |= CHSTORE_FLAG_RX_ONLY;

    /* FM repeater channels: no tone known from the plan, colour code unused */
    out->rx_tone_en = 0;
    out->tx_tone_en = 0;
    out->dmr_cc = 1;
    out->dmr_ts = 0;
    out->dmr_dest_type = DMR_DEST_NONE;
    out->dmr_dest_id = 0;

    return 0;
}

void zones_setShiftInverted(bool inverted)
{
    shiftInverted = inverted;
}

bool zones_shiftInverted(void)
{
    return shiftInverted;
}
