/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "core/airband.h"

/** Thousands of hertz of a frequency: the unit the designators print in */
#define FREQ_TO_KHZ(f) ((f) / 1000U)

bool airband_inBand(freq_t freq)
{
    return (freq >= AIRBAND_FREQ_MIN) && (freq <= AIRBAND_FREQ_MAX);
}

freq_t airband_freq25k(uint16_t index)
{
    return AIRBAND_FREQ_MIN + (uint32_t)index * AIRBAND_SPACING_25K;
}

freq_t airband_channelFreq(uint16_t channel)
{
    /* channel * 25 kHz / 3, rounded down: the error is below one hertz */
    uint32_t offset = ((uint32_t)channel * AIRBAND_SPACING_25K + 1U) / 3U;

    return AIRBAND_FREQ_MIN + offset;
}

int32_t airband_channelIndex(freq_t freq)
{
    if (!airband_inBand(freq))
        return -1;

    uint32_t delta = freq - AIRBAND_FREQ_MIN;
    const uint32_t step = AIRBAND_SPACING_25K;

    /* Nearest channel: (delta * 3 / 25000), rounded half up */
    return (int32_t)((delta * 3U + step / 2U) / step);
}

freq_t airband_snap(freq_t freq)
{
    int32_t channel = airband_channelIndex(freq);

    if (channel < 0)
        return freq;

    return airband_channelFreq((uint16_t)channel);
}

/**
 * Thousands of hertz of a channel designator, given the channel index. In a
 * 25 kHz slot the three channels of the 8.33 kHz raster are named 0, 5 and 10
 * kHz above its base, which is the rule that makes every designator end in
 * 00, 05, 10, 25, 30, 35, 50, 55, 60, 75, 80 or 85.
 *
 * @param channel: channel index.
 * @return the designator, in thousands of hertz.
 */
static uint32_t channelKHz(uint16_t channel)
{
    uint32_t slot = (uint32_t)channel / 3U;
    uint32_t within = (uint32_t)channel % 3U;

    return FREQ_TO_KHZ(AIRBAND_FREQ_MIN) + slot * 25U + within * 5U;
}

const char *airband_channelName(freq_t freq, char *buf, size_t cap)
{
    if ((buf == NULL) || (cap < AIRBAND_NAME_LEN))
        return "";

    int32_t channel = airband_channelIndex(freq);

    if (channel < 0) {
        buf[0] = '\0';
        return buf;
    }

    uint32_t khz = channelKHz((uint16_t)channel);

    snprintf(buf, cap, "%u.%03u", (unsigned int)(khz / 1000U),
             (unsigned int)(khz % 1000U));

    return buf;
}

int32_t airband_nameToFreq(const char *name)
{
    if (name == NULL)
        return -EINVAL;

    while (isspace((unsigned char)*name))
        name++;

    /* Whole megahertz */
    uint32_t mhz = 0;
    size_t digits = 0;
    while (isdigit((unsigned char)*name) && (digits < 3)) {
        mhz = mhz * 10U + (uint32_t)(*name - '0');
        name++;
        digits++;
    }

    /* Three decimals of a megahertz, the designator's resolution */
    if ((digits == 0) || (*name != '.'))
        return -EINVAL;
    name++;

    uint32_t khz = 0;
    for (size_t i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*name))
            return -EINVAL;
        khz = khz * 10U + (uint32_t)(*name - '0');
        name++;
    }

    /* An optional unit, so that what the screen prints can be read back */
    while (isspace((unsigned char)*name))
        name++;
    if (strncmp(name, "MHz", 3) == 0)
        name += 3;
    while (isspace((unsigned char)*name))
        name++;
    if (*name != '\0')
        return -EINVAL;

    uint32_t designator = mhz * 1000U + khz;

    if ((designator < FREQ_TO_KHZ(AIRBAND_FREQ_MIN)) ||
        (designator > FREQ_TO_KHZ(AIRBAND_FREQ_MAX)))
        return -ERANGE;

    /*
     * Only 0, 5 and 10 kHz above the 25 kHz base are channels: 118.020 names
     * nothing.
     */
    uint32_t base = (designator / 25U) * 25U;
    uint32_t within = designator - base;

    if (((within % 5U) != 0) || (within > 10U))
        return -EINVAL;

    uint32_t slot = (base - FREQ_TO_KHZ(AIRBAND_FREQ_MIN)) / 25U;
    uint32_t channel = slot * 3U + within / 5U;

    if (channel >= AIRBAND_CHANNELS_8K33)
        return -ERANGE;

    return (int32_t)airband_channelFreq((uint16_t)channel);
}

/*
 * Confirmation state of the transmitter. A single variable, and deliberately
 * not part of the settings record: the operator confirms once per power cycle,
 * after reading the warning, and a radio handed to somebody else is locked
 * again the next time it is switched on.
 */
static enum airbandTxState txState = AIRBAND_TX_LOCKED;

enum airbandTxState airband_txState(void)
{
    return txState;
}

void airband_txSetState(enum airbandTxState state)
{
    txState = state;
}

bool airband_txAllowed(freq_t freq)
{
    return airband_inBand(freq) && (txState == AIRBAND_TX_ARMED);
}
