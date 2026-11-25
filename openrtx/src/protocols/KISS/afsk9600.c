/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/KISS/afsk9600.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * The receive filter is the one direwolf designs at init (dsp.c, gen_lowpass
 * with a cosine window): a sinc of `spb` taps, cut-off at the baud rate, i.e.
 * 0.2 of the sample rate at 48 kHz, windowed so that the truncation does not
 * ring into the next bit. Normalised to unity gain at DC, so the AGC sees the
 * same amplitude whatever the sample rate.
 */
static void designLowpass(g3ruh_demod_t *d)
{
    const float fc = (float)G3RUH_BAUD / (float)d->fs;
    const float center = 0.5f * (float)(d->spb - 1);
    float sum = 0.0f;

    for (uint32_t j = 0; j < d->spb; j++)
    {
        float x = (float)j - center;
        float sinc;

        if (x == 0.0f)
            sinc = 2.0f * fc;
        else
            sinc = sinf(2.0f * (float)M_PI * fc * x) / ((float)M_PI * x);

        float w = cosf((((float)j - center) / (float)d->spb) * (float)M_PI);
        d->fir[j] = sinc * w;
        sum += d->fir[j];
    }

    if (sum != 0.0f)
        for (uint32_t j = 0; j < d->spb; j++)
            d->fir[j] /= sum;
}

bool g3ruh_mod_init(g3ruh_mod_t *m, uint32_t fs)
{
    if ((fs % G3RUH_BAUD) != 0)
        return false;

    uint32_t spb = fs / G3RUH_BAUD;
    if ((spb < 4) || (spb > G3RUH_MAX_TAPS))
        return false;

    memset(m, 0, sizeof(*m));
    m->fs    = fs;
    m->spb   = spb;
    m->gain  = 0.7f;
    m->level = 1.0f;
    m->prev  = 1.0f;
    g3ruh_mod_reset(m);
    return true;
}

void g3ruh_mod_reset(g3ruh_mod_t *m)
{
    m->nrzi   = 1;
    m->lfsr   = 0;
    m->bitPos = 0;
    m->level  = 1.0f;
    m->prev   = 1.0f;
}

void g3ruh_mod_setGain(g3ruh_mod_t *m, float gain)
{
    if (gain < 0.0f)
        gain = 0.0f;
    if (gain > 1.0f)
        gain = 1.0f;
    m->gain = gain;
}

void g3ruh_mod_bit(g3ruh_mod_t *m, bool bit, int16_t *out)
{
    /*
     * Two line codes in series, as direwolf does it and as the G3RUH modems
     * of the nineties did: NRZI (a zero bit inverts the level, a one keeps it)
     * and then the 17-bit scrambler, which is what removes the DC component
     * and guarantees transitions. The scrambler alone would also work on the
     * air, but every 9600 TNC in the world expects the pair, and direwolf
     * cannot decode a stream that has only one of them.
     */
    if (!bit)
        m->nrzi ^= 1u;

    const bool   scrambled = g3ruh_scramble(&m->lfsr, m->nrzi != 0);
    const float  target = scrambled ? 1.0f : -1.0f;
    const float  from   = m->level;
    const float  amp    = m->gain * 32767.0f;

    /*
     * Raised cosine from the old level to the new one over exactly one bit:
     * the sample at the end of the bit is the new level, so consecutive bits
     * join without a step. Same choice as direwolf's transmitter, which
     * interpolates with a raised cosine rather than generating a square wave
     * and filtering it (less overshoot, no ringing added to the channel).
     */
    for (uint32_t k = 0; k < m->spb; k++)
    {
        float x = (float)(k + 1u) / (float)m->spb;
        float w = 0.5f * (1.0f - cosf((float)M_PI * x));
        out[k] = (int16_t)((from + (target - from) * w) * amp);
    }

    m->prev  = from;
    m->level = target;
}

size_t g3ruh_mod_silence(g3ruh_mod_t *m, int16_t *out, size_t max,
                         uint32_t ms)
{
    size_t want = ((size_t)m->fs * ms) / 1000u;
    if (want > max)
        want = max;
    memset(out, 0, want * sizeof(int16_t));
    return want;
}

/* ------------------------------------------------------------------ RX --- */

bool g3ruh_demod_init(g3ruh_demod_t *d, uint32_t fs)
{
    if ((fs % G3RUH_BAUD) != 0)
        return false;

    uint32_t spb = fs / G3RUH_BAUD;
    if ((spb < 4) || (spb > G3RUH_MAX_TAPS))
        return false;

    memset(d, 0, sizeof(*d));
    d->fs    = fs;
    d->spb   = spb;
    d->gain  = 1.0f;
    designLowpass(d);
    return true;
}

/*
 * Automatic gain control. The discriminator output can be anything from a
 * whisper to a saturated signal, and the slicer wants a fixed scale. The
 * envelope follows the peaks with a fast attack and a slow decay, and the
 * output is normalised to +-0.9. The DC blocker in front takes care of the
 * offset, so unlike direwolf's agc() this one does not have to track the
 * midpoint between the peaks, which is what makes it stable from the first
 * sample: the peak starts at zero and nothing else is remembered.
 */
static inline float agc(g3ruh_demod_t *d, float in)
{
    const float a = fabsf(in);

    if (a > d->peak)
        d->peak = 0.88f * d->peak + 0.12f * a;      /* attack, ~40 samples */
    else
        d->peak = 0.9999f * d->peak + 0.0001f * a;  /* decay, ~0.2 s       */

    d->valley = -d->peak;

    if (d->peak < 1e-3f)
        return 0.0f;

    return in * (0.9f / d->peak);
}

#define RING_MASK (G3RUH_RING - 1u)

/** Filtered sample `age` samples in the past. */
static inline float ringAt(const g3ruh_demod_t *d, uint32_t age)
{
    return d->ring[(d->now - age) & RING_MASK];
}

/**
 * Decide one bit cell: the sign of the filtered signal integrated over the
 * middle of the cell, `age` samples in the past.
 */
static bool decideCell(const g3ruh_demod_t *d, float age)
{
    const float w = G3RUH_DEC_HALF_WIN * (float)d->spb;
    float sum = 0.0f;

    int lo = (int)ceilf(age - w);
    int hi = (int)floorf(age + w);

    if (lo < 0)
        lo = 0;
    if ((uint32_t)hi > (G3RUH_RING - 2u))
        hi = (int)(G3RUH_RING - 2u);

    for (int i = lo; i <= hi; i++)
        sum += ringAt(d, (uint32_t)i);

    return sum > 0.0f;
}

static size_t emitBit(g3ruh_demod_t *d, bool level,
                      void (*onBit)(bool bit, void *user), void *user)
{
    d->lastBit = level;
    d->bitsDecoded++;

    /*
     * Two line codes in reverse: descramble, then undo the NRZI (a level
     * equal to the previous one is a one bit, a change is a zero). The first
     * descrambled level carries no information, exactly like the first level
     * of the 1200 path.
     */
    bool desc = g3ruh_descramble(&d->lfsr, level);

    if (!d->nrziValid)
    {
        d->nrziValid   = true;
        d->prevDescram = desc;
        return 0;
    }

    bool dataBit = (desc == d->prevDescram);
    d->prevDescram = desc;

    if (onBit != NULL)
        onBit(dataBit, user);

    return 1;
}

size_t g3ruh_demod_sample(g3ruh_demod_t *d, int16_t sample,
                          void (*onBit)(bool bit, void *user), void *user)
{
    /* DC blocker: the discriminator has a DC term the AGC would chase */
    float x  = (float)sample;
    float dc = x - d->dcX1 + 0.995f * d->dcY1;
    d->dcX1  = x;
    d->dcY1  = dc;
    x = dc;

    /* One-bit low pass: sliding delay line, weighted sum */
    d->hist[d->firIdx] = x;
    d->firIdx = (uint8_t)((d->firIdx + 1u < d->spb) ? (d->firIdx + 1u) : 0u);

    float y = 0.0f;
    for (uint32_t j = 0; j < d->spb; j++)
    {
        uint32_t idx = (d->firIdx + j) % d->spb;
        y += d->fir[j] * d->hist[idx];
    }

    y = agc(d, y);

    d->energy = 0.98f * d->energy + 0.02f * fabsf(y);
    d->signal = d->energy > 0.12f;

    d->now++;
    d->ring[d->now & RING_MASK] = y;

    /* One more sample of the clock: the next cell centre comes closer */
    if (d->clockRunning)
        d->toNext -= 1.0f;

    const float spb = (float)d->spb;
    size_t emitted = 0;

    /*
     * A transition of the baseband can only be on a bit boundary: it is the
     * clock. The grid of cell centres free-runs at one bit, and every
     * transition pulls its phase towards the boundary that was observed
     * (interpolated to the sample). A transition is never a hard reset, so a
     * noisy or missed one costs a fraction of a bit instead of a frame.
     */
    if (d->primed && ((d->prevY < 0.0f) != (y < 0.0f)))
    {
        float denom = d->prevY - y;
        float alpha = (denom != 0.0f) ? (d->prevY / denom) : 0.5f;
        if ((alpha < 0.0f) || (alpha > 1.0f))
            alpha = 0.5f;

        float age = 1.0f - alpha;   /* the crossing is this old, in samples */

        if (!d->clockRunning)
        {
            /* First boundary: the next cell centre is half a bit after it */
            d->clockRunning = true;
            d->toNext = 0.5f * spb - age;
            if (d->toNext < 1.0f)
                d->toNext = 1.0f;
            d->transitions++;
        }
        else
        {
            /* The boundary that closes the current cell, predicted */
            float predicted = 0.5f * spb - d->toNext;
            float e = age - predicted;

            if (e > 0.5f * spb)
                e -= spb;
            if (e < -0.5f * spb)
                e += spb;

            d->lastError = e / spb;

            /* Trust it less when it is a long way from where it was expected:
             * that is a burst of noise, not a boundary. */
            float w = (fabsf(e) < 0.25f * spb) ? G3RUH_PLL_KP
                                               : (0.5f * G3RUH_PLL_KP);
            d->toNext -= w * e;
            d->transitions++;
        }
    }

    if (d->clockRunning)
    {
        /* Decide the cells whose centre is far enough in the past for the
         * integration window to be complete. */
        while (d->toNext <= -(G3RUH_DEC_MARGIN * spb))
        {
            float centre = -d->toNext;
            emitted += emitBit(d, decideCell(d, centre), onBit, user);
            d->toNext += spb;
        }
    }
    else
    {
        d->toNext -= 1.0f;
    }

    d->primed = true;
    d->prevY  = y;
    d->lastY  = y;

    return emitted;
}

size_t g3ruh_demod_block(g3ruh_demod_t *d, const int16_t *samples, size_t count,
                         void (*onBit)(bool bit, void *user), void *user)
{
    size_t n = 0;

    for (size_t i = 0; i < count; i++)
        n += g3ruh_demod_sample(d, samples[i], onBit, user);

    return n;
}
