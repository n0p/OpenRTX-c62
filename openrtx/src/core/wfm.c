/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/wfm.h"
#include <stdio.h>
#include <string.h>

/*
 * The band, its arithmetic and the last status the receiver published.  The
 * tuner itself lives in the platform layer: nothing here talks to hardware.
 */

static wfm_status_t lastStatus;

bool wfm_freqValid(freq_t freq)
{
    if (freq < WFM_LOW_HZ || freq > WFM_HIGH_HZ)
        return false;

    /*
     * Both rasters are accepted, and a frequency off both of them (a 10 kHz
     * keyboard entry, say) is snapped by the caller: what is refused here is
     * a frequency the tuner cannot put on a channel at all.
     */
    if ((freq % WFM_STEP_FINE_HZ) != 0)
        return false;

    return true;
}

freq_t wfm_snap(freq_t freq, uint32_t step)
{
    if (step == 0)
        step = WFM_STEP_HZ;

    uint32_t off = (freq > WFM_LOW_HZ) ? (freq - WFM_LOW_HZ) : 0;
    uint32_t snapped = ((off + (step / 2)) / step) * step;
    freq_t out = WFM_LOW_HZ + snapped;

    if (out < WFM_LOW_HZ)
        out = WFM_LOW_HZ;
    if (out > WFM_HIGH_HZ)
        out = WFM_HIGH_HZ;

    return out;
}

uint16_t wfm_chan(freq_t freq)
{
    if (freq <= WFM_LOW_HZ)
        return 0;

    return (uint16_t)((freq - WFM_LOW_HZ + (WFM_STEP_HZ / 2)) / WFM_STEP_HZ);
}

freq_t wfm_chanFreq(uint16_t chan)
{
    freq_t f = WFM_LOW_HZ + (freq_t)chan * WFM_STEP_HZ;

    return (f > WFM_HIGH_HZ) ? WFM_HIGH_HZ : f;
}

freq_t wfm_step(freq_t freq, int dir)
{
    uint32_t step = WFM_STEP_HZ;

    /* Off the 100 kHz raster: move to it first, then one channel */
    freq_t snapped = wfm_snap(freq, step);

    if (dir > 0) {
        if (snapped >= WFM_HIGH_HZ)
            return WFM_HIGH_HZ;
        return snapped + step;
    }

    if (snapped <= WFM_LOW_HZ)
        return WFM_LOW_HZ;
    return snapped - step;
}

const char *wfm_formatFreq(freq_t freq, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0)
        return "";

    unsigned mhz = (unsigned)(freq / 1000000U);
    unsigned frac = (unsigned)((freq % 1000000U) / 100000U);
    unsigned fine = (unsigned)((freq % 100000U) / 10000U);

    if (fine != 0)
        snprintf(buf, cap, "%u.%u%u", mhz, frac, fine);
    else
        snprintf(buf, cap, "%u.%u", mhz, frac);

    return buf;
}

rssi_t wfm_dbuVToDbm(uint8_t dbuV)
{
    /*
     * The tuner reports dBuV into 50 ohm, 0 to about 75.  P(dBm) = P(dBuV) -
     * 107 for 50 ohm, which puts a 60 dBuV station at -47 dBm: the same scale
     * the S-meter and the squelch use, roughly S9.
     */
    int dbm = (int)dbuV - 107;

    if (dbm < -127)
        dbm = -127;
    if (dbm > 0)
        dbm = 0;

    return (rssi_t)dbm;
}

wfm_status_t wfm_status(void)
{
    return lastStatus;
}

/*
 * The platform provides the tuner.  Only the C62 has one (the BK1080); on
 * every other target the mode can be selected in the code but nothing will
 * ever tune or report.
 */
#if defined(PLATFORM_C62)

extern bool c62_wfmPoll(wfm_status_t *st);

bool wfm_poll(wfm_status_t *st)
{
    return c62_wfmPoll(st);
}

#else

bool wfm_poll(wfm_status_t *st)
{
    (void)st;
    return false;
}

#endif

void wfm_publish(const wfm_status_t *st)
{
    if (st != NULL)
        lastStatus = *st;
}
