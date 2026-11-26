/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The uplink listening session of the TETRA mode (TetraUplink.h).
 *
 * The radio has one receiver: it either receives a downlink and stays
 * synchronised to the cell, or it listens to the uplink frequency of that cell
 * and decodes the MS to BS direction (libtetra, TETRA_UPLINK_DETECT and
 * TETRA_UPLINK_DECODE).  This module owns the second position: the cell it
 * belongs to, the two frequencies, and when to give it up.  It is portable C
 * with no radio and no libtetra in it, so the quick menu, the TETRA mode and
 * the host tests all share the same rules.
 */

#include <string.h>

#include "protocols/TETRA/TetraUplink.h"
#include "rtx/rtx.h"

struct tetraUlSession
{
    bool active;    /* Following the uplink of the cell below */
    bool noFreq;    /* The operator asked for it, no uplink known */
    bool lost;      /* The mode says the cell went away: go back */
    uint16_t mcc;
    uint16_t mnc;
    uint8_t cc;
    uint32_t dl;    /* Where Off goes back to */
    uint32_t ul;
};

static struct tetraUlSession ul;

uint32_t tetraUl_hzFromDownlink(uint32_t dlHz, uint8_t band,
                                uint8_t duplexSpacing, bool reverse)
{
    /* Duplex spacing in kHz, TS 100 392-15 table 2; R is reserved. */
#define R 0xffffu
    static const uint16_t spacingKHz[8][16] = {
        { R, 1600, 10000, 10000, 10000, 10000, 10000, R, R, R, R, R, R, R, R,
          R },
        { R, 4500, R, 36000, 7000, R, R, R, 45000, 45000, R, R, R, R, R, R },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
        { R, R, R, 8000, 8000, R, R, R, 18000, 18000, R, R, R, R, R, R },
        { R, R, R, 18000, 5000, R, 30000, 30000, R, 39000, R, R, R, R, R, R },
        { R, R, R, R, 9500, R, R, R, R, R, R, R, R, R, R, R },
        { R, R, R, R, R, R, R, R, R, R, R, R, R, R, R, R },
        { R, R, R, R, R, R, R, R, R, R, R, R, R, R, R, R },
    };

    unsigned khz = spacingKHz[duplexSpacing & 7][band & 15];
    uint32_t delta;
#undef R

    if (khz == 0xffffu || dlHz == 0)
        return 0;

    delta = (uint32_t)khz * 1000u;
    if (reverse)
        return dlHz + delta;
    return (delta < dlHz) ? (dlHz - delta) : 0;
}

void tetraUplink_reset(void)
{
    memset(&ul, 0, sizeof(ul));
}

/**
 * Uplink frequency of the cell in the status: the one it published, or the
 * duplex conversion of its downlink.  Never invented: 0 when neither is known.
 */
static uint32_t uplinkOf(const struct tetraStatus *ts)
{
    if (ts == NULL || !ts->cell)
        return 0;
    if (ts->ulFreq != 0)
        return ts->ulFreq;
    if (ts->sysinfo && ts->dlFreq != 0)
        return tetraUl_hzFromDownlink(ts->dlFreq, ts->band, ts->duplexSpacing,
                                      ts->reverseOp);
    return 0;
}

int tetraUplink_start(const struct tetraStatus *ts, uint32_t fromHz)
{
    uint32_t target = uplinkOf(ts);

    /*
     * A cell whose uplink is not known, and the one whose uplink is the very
     * frequency being received (a zero duplex split): there is nowhere else to
     * tune to, so the row says "no uplink" instead of pretending to move.
     */
    if (ts == NULL || !ts->cell || target == 0 || target == fromHz) {
        /*
         * Remember that it was asked for, so the row says "no uplink" instead
         * of going back to Off under the operator's finger.  Nothing is tuned.
         */
        ul.active = false;
        ul.noFreq = true;
        ul.lost = false;
        ul.ul = 0;
        ul.dl = fromHz;
        return -1;
    }

    ul.active = true;
    ul.noFreq = false;
    ul.lost = false;
    ul.mcc = ts->mcc;
    ul.mnc = ts->mnc;
    ul.cc = ts->colourCode;
    ul.dl = fromHz;
    ul.ul = target;
    return 0;
}

bool tetraUplink_stop(void)
{
    bool was = ul.active;

    /* Keep `dl` and the cell: the caller asks backHz() right after this */
    ul.active = false;
    ul.noFreq = false;
    ul.lost = false;
    return was;
}

bool tetraUplink_active(void)
{
    return ul.active;
}

enum tetraUlDisplay tetraUplink_display(uint32_t nowHz)
{
    (void)nowHz;

    if (ul.active)
        return TETRA_UL_ON;
    if (ul.noFreq)
        return TETRA_UL_NO_FREQ;
    return TETRA_UL_OFF;
}

uint32_t tetraUplink_freq(void)
{
    return ul.active ? ul.ul : 0;
}

uint32_t tetraUplink_backHz(void)
{
    return ul.dl;
}

bool tetraUplink_cell(uint16_t *mcc, uint16_t *mnc, uint8_t *cc)
{
    if (!ul.active)
        return false;
    if (mcc != NULL)
        *mcc = ul.mcc;
    if (mnc != NULL)
        *mnc = ul.mnc;
    if (cc != NULL)
        *cc = ul.cc;
    return true;
}

void tetraUplink_cellLost(void)
{
    if (!ul.active)
        return;
    ul.active = false;
    ul.noFreq = false;
    ul.lost = true;
}

bool tetraUplink_tick(uint8_t mode, uint32_t nowHz, uint32_t *hz)
{
    /* A mode that is not TETRA has no uplink to listen to */
    if (mode != OPMODE_TETRA) {
        bool wasActive = ul.active;
        uint32_t back = ul.dl;

        tetraUplink_reset();
        if (wasActive && hz != NULL && back != 0) {
            *hz = back;
            return true;
        }
        return false;
    }

    /* The cell went away while the mode was on its uplink: put it back */
    if (ul.lost) {
        uint32_t back = ul.dl;

        tetraUplink_reset();
        if (back != 0 && hz != NULL) {
            *hz = back;
            return true;
        }
        return false;
    }

    /* The operator tuned somewhere else: the session ends where it is */
    if (ul.active && ul.ul != 0 && nowHz != ul.ul) {
        tetraUplink_reset();
        return false;
    }

    return false;
}
