/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AIRBAND_H
#define AIRBAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/datatypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file airband.h
 * @brief The aeronautical VHF band: 118.000-136.975 MHz, its 25 kHz and
 * 8.33 kHz channel rasters, and the channel designators used on the air.
 *
 * The band carries AM voice (DSB with carrier) and nothing this radio
 * transmits, so everything here is about receiving: which frequencies are in
 * the band, which channel a frequency belongs to, and what that channel is
 * called.
 *
 * Two rasters live in the band:
 *
 *  - 25 kHz: 118.000, 118.025, 118.050, ... 700 channels, the classic plan
 *    and still the one most charts print.  A channel is named by its
 *    frequency, "118.025".
 *  - 8.33 kHz: three channels per 25 kHz slot, at 118.000, 118.0083,
 *    118.0166, 118.025, ... 2278 channels, mandated in Europe above FL195
 *    since 2018 (EUROCONTROL, "8.33 kHz Voice Channel Spacing").  The spacing
 *    is exactly 25/3 kHz, which is why the frequencies cannot be written as
 *    decimal kHz: 118.0083 is 118.008333...  A channel is *named* with a
 *    designator whose last two digits are always 00, 05 or 10 in each 25 kHz
 *    slot: 118.005 is the channel whose carrier is 118.00833, and 118.010 the
 *    one at 118.01667.  So a designator is not a frequency, and a receiver
 *    that tunes to the designator is 3.3 or 6.7 kHz off the carrier.
 *
 * This file keeps the two apart: airband_channelFreq() returns the carrier of
 * an 8.33 kHz channel, airband_channelName() returns its designator, and
 * airband_nameToFreq() goes from a designator back to the carrier.
 *
 * @note The band plan, the designator rule and the 25/3 kHz spacing are from
 * ICAO Annex 10 Volume III and the EUROCONTROL 8.33 kHz documentation; the
 * frequencies are computed, never tabulated.
 */

/** Lowest frequency of the band, Hz (118.000 MHz). */
#define AIRBAND_FREQ_MIN 118000000U

/** Highest frequency of the band, Hz (136.975 MHz). */
#define AIRBAND_FREQ_MAX 136975000U

/** Spacing of the 25 kHz raster, Hz. */
#define AIRBAND_SPACING_25K 25000U

/** Number of channels of the 25 kHz raster, 118.000 to 136.975. */
#define AIRBAND_CHANNELS_25K 760U

/**
 * Number of channels of the 8.33 kHz raster, 118.000 to 136.975: the last one
 * is channel 2277, at 136.975 MHz exactly, because 2277 * 25/3 kHz is 18.975
 * MHz.
 */
#define AIRBAND_CHANNELS_8K33 2278U

/** Length of a channel designator including the terminator, "136.975". */
#define AIRBAND_NAME_LEN 8

/**
 * Check whether a frequency is in the aeronautical band.
 *
 * @param freq: frequency, in Hz.
 * @return true when it is inside 118.000-136.975 MHz.
 */
bool airband_inBand(freq_t freq);

/**
 * Carrier frequency of a channel of the 25 kHz raster.
 *
 * @param index: channel index, 0 for 118.000 MHz.
 * @return the carrier, in Hz.
 */
freq_t airband_freq25k(uint16_t index);

/**
 * Carrier frequency of a channel of the 8.33 kHz raster.
 *
 * The real spacing is 25/3 kHz, so the carrier is rounded to the nearest Hz
 * (8333 Hz instead of 8333.33).  The radio tunes in 10 Hz units, so it lands
 * within a third of a hertz of the ideal channel.
 *
 * @param channel: channel index, 0 for 118.000 MHz.
 * @return the carrier, in Hz.
 */
freq_t airband_channelFreq(uint16_t channel);

/**
 * Index of the 8.33 kHz channel a frequency is on, or nearest to.
 *
 * @param freq: frequency, in Hz.
 * @return the channel index, or -1 outside the band.
 */
int32_t airband_channelIndex(freq_t freq);

/**
 * Snap a frequency to the nearest 8.33 kHz carrier.
 *
 * The 8.33 kHz step of the tuning knob is a whole number of hertz, so a
 * frequency reached by stepping drifts off the raster by a fraction of a hertz
 * per step.  Tuning through this function puts the receiver back on the
 * channel; frequencies outside the band are returned unchanged.
 *
 * @param freq: frequency, in Hz.
 * @return the nearest channel carrier, or freq when it is out of the band.
 */
freq_t airband_snap(freq_t freq);

/**
 * Designator of the 8.33 kHz channel a frequency is on or nearest to, written
 * as "118.005".
 *
 * @param freq: frequency, in Hz.
 * @param buf: output buffer, at least AIRBAND_NAME_LEN bytes.
 * @param cap: size of the buffer.
 * @return buf, or an empty string when the band or the buffer does not fit.
 */
const char *airband_channelName(freq_t freq, char *buf, size_t cap);

/**
 * Carrier of the channel a designator names.
 *
 * @param name: designator, "118.005" or "118.005 MHz".
 * @return the carrier in Hz, -EINVAL when the text is not a designator and
 * -ERANGE when it names a channel outside the band.
 */
int32_t airband_nameToFreq(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* AIRBAND_H */
