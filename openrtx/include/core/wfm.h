/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WFM_H
#define WFM_H

/**
 * @file wfm.h
 * @brief Commercial FM broadcast reception: the band, its channels and what
 * the receiver reports about them.
 *
 * The channel plan is data, as in core/zones.h: the band the operator asked
 * for is 85.0-110.0 MHz, and the receiver on this radio (a Beken BK1080, see
 * docs/RX_WFM.md) covers 76.0-108.0 MHz with BAND=01 and 87.5-108.0 MHz with
 * BAND=00.  Two of the three edges of that request therefore cannot be met by
 * the hardware:
 *
 *  - 85.0 to 87.5 MHz is inside the 76-108 MHz band of the chip, so it is
 *    receivable; it is offered because the operator asked for it and because
 *    stations do exist below 87.5 in some countries (Japan, the former Soviet
 *    bloc, and the 87.5-88.0 sub-band used in several European countries).
 *    With BAND=01 the whole plan uses one band register value, which is also
 *    what the stock firmware writes.
 *  - 108.0 to 110.0 MHz is above the top of the chip's VCO range.  Nothing is
 *    hidden: wfm_freqValid() is false there and the mode refuses to tune, so
 *    the band object simply ends at 108.0 MHz.
 *
 * Channels are on the 100 kHz raster of the CCIR plan; wfm_snap() and the
 * receiver driver also accept a 50 kHz raster (Italy and a few others offset
 * some stations by 50 kHz) as long as the frequency is a multiple of the step.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "core/datatypes.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bottom of the band this build offers, Hz. */
#define WFM_LOW_HZ      85000000U

/**
 * Top of the band, Hz.  Not 110.0: that is where the BK1080 stops, and a
 * frequency the hardware cannot reach is refused rather than silently clamped.
 */
#define WFM_HIGH_HZ    108000000U

/** Channel raster, Hz (CCIR 100 kHz). */
#define WFM_STEP_HZ       100000U

/** Offset raster used by the countries that put stations 50 kHz off, Hz. */
#define WFM_STEP_FINE_HZ   50000U

/** Number of 100 kHz channels between WFM_LOW_HZ and WFM_HIGH_HZ, inclusive. */
#define WFM_CHANNELS (((WFM_HIGH_HZ - WFM_LOW_HZ) / WFM_STEP_HZ) + 1U)

/**
 * What the receiver reports about the channel it is on.  All of it comes from
 * the tuner chip: this radio has no software demodulator for broadcast FM.
 */
typedef struct
{
    uint32_t freq;      /**< Channel the tuner is on, Hz                    */
    uint16_t chan;      /**< Channel index the tuner reports (READCHAN)     */
    uint8_t  rssi;      /**< Level, dBuV (1 dB steps, 0..75)                */
    uint8_t  snr;       /**< SNR, 0..15 (the chip's own SNR estimate)       */
    int16_t  freqd;     /**< Frequency deviation, units of 148 Hz           */
    bool     tuned;     /**< A tune has completed (STC) since the last one  */
    bool     afcrl;     /**< AFC railed: no valid channel, audio is muted   */
    bool     stereo;    /**< Stereo pilot present (ST)                      */
    bool     stereoSync;/**< Stereo decoder locked (STEN)                   */
    bool     rds;       /**< Always false: this chip has no RDS decoder     */
} wfm_status_t;

/** True if the frequency is inside the band and on the 100 or 50 kHz raster. */
bool wfm_freqValid(freq_t freq);

/**
 * @return freq rounded to the nearest multiple of step, clamped to the
 * band edges.
 */
freq_t wfm_snap(freq_t freq, uint32_t step);

/** @return the 100 kHz channel index of a frequency inside the band. */
uint16_t wfm_chan(freq_t freq);

/** @return the frequency of a 100 kHz channel index. */
freq_t wfm_chanFreq(uint16_t chan);

/** @return the next channel up (dir > 0) or down (dir < 0), clamped. */
freq_t wfm_step(freq_t freq, int dir);

/**
 * Format a frequency the way a broadcast receiver shows it: "101.1", or
 * "101.15" when it is on the 50 kHz raster.  Always one digit after the point
 * for multiples of 100 kHz.
 */
const char *wfm_formatFreq(freq_t freq, char *buf, size_t cap);

/** Level in dBuV (50 ohm) to the dBm scale the RSSI filter and squelch use. */
rssi_t wfm_dbuVToDbm(uint8_t dbuV);

/** Latest receiver status, as published by the operating mode. */
wfm_status_t wfm_status(void);

/**
 * Poll the receiver chip for its status.  Implemented by the platform layer,
 * because only it knows whether there is a broadcast tuner and how to reach
 * it; returns false where there is none.
 */
bool wfm_poll(wfm_status_t *st);

/**
 * Publish a receiver status.  Called by the mode that polls the tuner; a copy
 * also lands in state.wfm so that the screen code does not need to know about
 * the tuner.
 */
void wfm_publish(const wfm_status_t *st);

#ifdef __cplusplus
}
#endif

#endif /* WFM_H */
