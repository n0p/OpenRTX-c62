/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/hdlc.h"
#include <string.h>
#ifdef HDLC_DEBUG
#include <stdio.h>
#endif

void hdlc_deframer_init(hdlc_deframer_t *d)
{
    memset(d, 0, sizeof(*d));
    d->dcd = false;
}

/*
 * A frame is complete when the closing flag arrives: validate the FCS before
 * handing it out. The FCS covers every byte including the two FCS bytes and
 * the residue of a good frame is 0xF0B8.
 */
static void endFrame(hdlc_deframer_t *d, hdlc_frame_cb cb, void *user)
{
    if (!d->inFrame)
        return;

    d->inFrame = false;

    if (d->overflow || (d->frameLen < (7u * 2u + 2u + 2u)))
        return;

    if (!ax25_fcs_ok(d->frame, d->frameLen))
    {
        d->badFcs++;
        return;
    }

    d->frames++;
    if (cb != NULL)
        cb(d->frame, d->frameLen - 2u, user);
}

static void startFrame(hdlc_deframer_t *d)
{
    d->inFrame       = true;
    d->frameLen      = 0;
    d->bitBuf        = 0;
    d->bitCount      = 0;
    d->overflow      = false;
    d->bitsSinceFlag = 0;
    d->dcd           = true;
    d->flags++;
}

static void pushBit(hdlc_deframer_t *d, bool bit)
{
    if (!d->inFrame)
        return;

    d->bitBuf |= (uint8_t)(bit ? (1u << d->bitCount) : 0u);
    if (++d->bitCount == 8u)
    {
        d->bitCount = 0;
        if (d->frameLen < HDLC_MAX_LEN)
            d->frame[d->frameLen++] = d->bitBuf;
        else
            d->overflow = true;
        d->bitBuf = 0;
    }
}

void hdlc_rx_level(hdlc_deframer_t *d, bool level,
                   hdlc_frame_cb cb, void *user)
{
    bool bit;

    d->bits++;
    if (d->bitsSinceFlag < 0xFFFFu)
        d->bitsSinceFlag++;

    /*
     * NRZI: no transition means a one. The very first level carries no
     * information, because there is nothing to compare it with: priming the
     * decoder with a one would turn the leading zero of the opening flag into
     * a seventh one and abort the frame.
     */
    if (!d->levelValid)
    {
        d->levelValid = true;
        d->lastLevel  = level;
        return;
    }

    bit = (level == d->lastLevel);
    d->lastLevel = level;

    /*
     * Runs of ones are held back until the next zero tells what they were:
     * six ones and a zero are a flag, five ones and a zero are five data
     * bits followed by a stuffed zero, seven or more are an abort. Holding
     * the run costs one bit of latency and keeps the flag test exact.
     */
    if (bit)
    {
        if (d->ones < 0xFFu)
            d->ones++;
        return;
    }

    if (d->ones == 6u)
    {
#ifdef HDLC_DEBUG
        fprintf(stderr, "[flag at %u, len %zu]\n", d->bits, d->frameLen);
#endif
        /* Flag: close the frame in progress, open the next one */
        endFrame(d, cb, user);
        startFrame(d);
        d->ones = 0;
        return;
    }

    if (d->ones == 5u)
    {
        /* Five data ones then the stuffed zero */
        for (uint8_t i = 0; i < 5u; i++)
            pushBit(d, true);
        d->ones = 0;
        return;
    }

    if (d->ones > 6u)
    {
#ifdef HDLC_DEBUG
        fprintf(stderr, "[abort at %u, ones %u, len %zu, inFrame %d]\n",
                d->bits, d->ones, d->frameLen, d->inFrame);
#endif
        /* Seven or more ones: abort */
        d->aborts++;
        d->inFrame = false;
        d->ones    = 0;
        return;
    }

    while (d->ones > 0u)
    {
        pushBit(d, true);
        d->ones--;
    }
    pushBit(d, false);

    /* DCD drops when no flag has been seen for a whole maximum-size frame */
    if (d->bitsSinceFlag > (8u * (HDLC_MAX_LEN + 8u)))
        d->dcd = false;
}

void hdlc_tx_start(hdlc_framer_t *f, const uint8_t *frame, size_t len,
                   uint8_t preFlags, uint8_t postFlags)
{
    memset(f, 0, sizeof(*f));
    f->data      = frame;
    f->len       = len;
    f->preFlags  = (preFlags == 0) ? 1 : preFlags;
    f->postFlags = (postFlags == 0) ? 1 : postFlags;
    f->phase     = HDLC_TX_PRE;
}

static bool nextFlagBit(hdlc_framer_t *f)
{
    bool bit = ((HDLC_FLAG >> f->flagBits) & 1u) != 0;
    if (++f->flagBits == 8u)
        f->flagBits = 0;
    return bit;
}

bool hdlc_tx_bit(hdlc_framer_t *f)
{
    if (f->phase == HDLC_TX_PRE)
    {
        bool bit = nextFlagBit(f);
        if ((f->flagBits == 0u) && (--f->preFlags == 0u))
        {
            f->phase = HDLC_TX_DATA;
            f->ones  = 0;
        }
        return bit;
    }

    if (f->phase == HDLC_TX_DATA)
    {
        if (f->stuffPending)
        {
            f->stuffPending = false;
            f->ones         = 0;
            return false;
        }

        bool bit = ((f->data[f->byteIdx] >> f->bitIdx) & 1u) != 0;

        if (++f->bitIdx == 8u)
        {
            f->bitIdx = 0;
            if (++f->byteIdx == f->len)
            {
                f->phase    = HDLC_TX_POST;
                f->flagBits = 0;
                f->ones     = 0;
            }
        }

        if (bit)
        {
            if (++f->ones == 5u)
                f->stuffPending = true;
            return true;
        }

        f->ones = 0;
        return false;
    }

    if (f->phase == HDLC_TX_POST)
    {
        bool bit = nextFlagBit(f);
        if ((f->flagBits == 0u) && (--f->postFlags == 0u))
            f->phase = HDLC_TX_DONE;
        return bit;
    }

    return false;
}
