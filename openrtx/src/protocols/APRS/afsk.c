/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/APRS/afsk.h"
#include <math.h>
#include <string.h>

/*
 * 512-entry Q15 sine table for the phase accumulators: one entry every
 * 2*pi/512, i.e. 0.7 degrees, which is 0.008 per cent of amplitude error on
 * the correlators. Cosine is a quarter-turn offset lookup.
 */
static const int16_t sinTable[512] = {
         0,    402,    804,   1206,   1608,   2009,   2410,   2811,
      3212,   3612,   4011,   4410,   4808,   5205,   5602,   5998,
      6393,   6786,   7179,   7571,   7962,   8351,   8739,   9126,
      9512,   9896,  10278,  10659,  11039,  11417,  11793,  12167,
     12539,  12910,  13279,  13645,  14010,  14372,  14732,  15090,
     15446,  15800,  16151,  16499,  16846,  17189,  17530,  17869,
     18204,  18537,  18868,  19195,  19519,  19841,  20159,  20475,
     20787,  21096,  21403,  21705,  22005,  22301,  22594,  22884,
     23170,  23452,  23731,  24007,  24279,  24547,  24811,  25072,
     25329,  25582,  25832,  26077,  26319,  26556,  26790,  27019,
     27245,  27466,  27683,  27896,  28105,  28310,  28510,  28706,
     28898,  29085,  29268,  29447,  29621,  29791,  29956,  30117,
     30273,  30424,  30571,  30714,  30852,  30985,  31113,  31237,
     31356,  31470,  31580,  31685,  31785,  31880,  31971,  32057,
     32137,  32213,  32285,  32351,  32412,  32469,  32521,  32567,
     32609,  32646,  32678,  32705,  32728,  32745,  32757,  32765,
     32767,  32765,  32757,  32745,  32728,  32705,  32678,  32646,
     32609,  32567,  32521,  32469,  32412,  32351,  32285,  32213,
     32137,  32057,  31971,  31880,  31785,  31685,  31580,  31470,
     31356,  31237,  31113,  30985,  30852,  30714,  30571,  30424,
     30273,  30117,  29956,  29791,  29621,  29447,  29268,  29085,
     28898,  28706,  28510,  28310,  28105,  27896,  27683,  27466,
     27245,  27019,  26790,  26556,  26319,  26077,  25832,  25582,
     25329,  25072,  24811,  24547,  24279,  24007,  23731,  23452,
     23170,  22884,  22594,  22301,  22005,  21705,  21403,  21096,
     20787,  20475,  20159,  19841,  19519,  19195,  18868,  18537,
     18204,  17869,  17530,  17189,  16846,  16499,  16151,  15800,
     15446,  15090,  14732,  14372,  14010,  13645,  13279,  12910,
     12539,  12167,  11793,  11417,  11039,  10659,  10278,   9896,
      9512,   9126,   8739,   8351,   7962,   7571,   7179,   6786,
      6393,   5998,   5602,   5205,   4808,   4410,   4011,   3612,
      3212,   2811,   2410,   2009,   1608,   1206,    804,    402,
         0,   -402,   -804,  -1206,  -1608,  -2009,  -2410,  -2811,
     -3212,  -3612,  -4011,  -4410,  -4808,  -5205,  -5602,  -5998,
     -6393,  -6786,  -7179,  -7571,  -7962,  -8351,  -8739,  -9126,
     -9512,  -9896, -10278, -10659, -11039, -11417, -11793, -12167,
    -12539, -12910, -13279, -13645, -14010, -14372, -14732, -15090,
    -15446, -15800, -16151, -16499, -16846, -17189, -17530, -17869,
    -18204, -18537, -18868, -19195, -19519, -19841, -20159, -20475,
    -20787, -21096, -21403, -21705, -22005, -22301, -22594, -22884,
    -23170, -23452, -23731, -24007, -24279, -24547, -24811, -25072,
    -25329, -25582, -25832, -26077, -26319, -26556, -26790, -27019,
    -27245, -27466, -27683, -27896, -28105, -28310, -28510, -28706,
    -28898, -29085, -29268, -29447, -29621, -29791, -29956, -30117,
    -30273, -30424, -30571, -30714, -30852, -30985, -31113, -31237,
    -31356, -31470, -31580, -31685, -31785, -31880, -31971, -32057,
    -32137, -32213, -32285, -32351, -32412, -32469, -32521, -32567,
    -32609, -32646, -32678, -32705, -32728, -32745, -32757, -32765,
    -32767, -32765, -32757, -32745, -32728, -32705, -32678, -32646,
    -32609, -32567, -32521, -32469, -32412, -32351, -32285, -32213,
    -32137, -32057, -31971, -31880, -31785, -31685, -31580, -31470,
    -31356, -31237, -31113, -30985, -30852, -30714, -30571, -30424,
    -30273, -30117, -29956, -29791, -29621, -29447, -29268, -29085,
    -28898, -28706, -28510, -28310, -28105, -27896, -27683, -27466,
    -27245, -27019, -26790, -26556, -26319, -26077, -25832, -25582,
    -25329, -25072, -24811, -24547, -24279, -24007, -23731, -23452,
    -23170, -22884, -22594, -22301, -22005, -21705, -21403, -21096,
    -20787, -20475, -20159, -19841, -19519, -19195, -18868, -18537,
    -18204, -17869, -17530, -17189, -16846, -16499, -16151, -15800,
    -15446, -15090, -14732, -14372, -14010, -13645, -13279, -12910,
    -12539, -12167, -11793, -11417, -11039, -10659, -10278,  -9896,
     -9512,  -9126,  -8739,  -8351,  -7962,  -7571,  -7179,  -6786,
     -6393,  -5998,  -5602,  -5205,  -4808,  -4410,  -4011,  -3612,
     -3212,  -2811,  -2410,  -2009,  -1608,  -1206,   -804,   -402,
};

#define TBL_MASK   511u
#define TBL_QUARTER 128u

/*
 * The table is Q15 in full-scale units: the modulator uses it directly (its
 * output is in sample units), the demodulator normalises it to +-1 so that the
 * correlator outputs are in the same units as the AGC target and the carrier
 * detect thresholds.
 */
#define TBL_SCALE (1.0f / 32768.0f)

static inline float sinRaw(uint32_t phase)
{
    return (float)sinTable[(phase >> 23) & TBL_MASK];
}

static inline float sinIdx(uint32_t phase)
{
    return sinRaw(phase) * TBL_SCALE;
}

static inline float cosIdx(uint32_t phase)
{
    /* Cosine is the sine table a quarter turn ahead; the index is masked
     * after the offset, not inside sinRaw(), which expects a phase */
    return (float)sinTable[(((phase >> 23) + TBL_QUARTER) & TBL_MASK)]
         * TBL_SCALE;
}

static uint32_t phaseInc(float hz, uint32_t fs)
{
    return (uint32_t)((hz * 4294967296.0f) / (float)fs);
}

/* ------------------------------------------------------------------ TX --- */

void afsk_mod_init(afsk_mod_t *m, uint32_t fs, float preemph)
{
    memset(m, 0, sizeof(*m));
    m->fs             = fs;
    m->incMark        = phaseInc(AFSK_MARK_HZ, fs);
    m->incSpace       = phaseInc(AFSK_SPACE_HZ, fs);
    m->samplesPerBit  = (uint32_t)((fs + 600u) / 1200u);
    m->preemph        = preemph;
    m->gain           = 0.8f;
    m->level          = true;
    m->levelValid     = false;
}

void afsk_mod_reset(afsk_mod_t *m)
{
    m->phase      = 0;
    m->bitPos     = 0;
    m->level      = true;
    m->levelValid = false;
    m->hpPrev     = 0.0f;
    m->hpState    = 0.0f;
}

void afsk_mod_setGain(afsk_mod_t *m, float gain)
{
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.0f) gain = 1.0f;
    m->gain = gain;
}

/* One-pole low pass at 800 Hz, used by the pre-emphasis shelf. */
#define PREEMPH_LP_HZ 800.0f

void afsk_mod_bit(afsk_mod_t *m, bool bit, int16_t *out)
{
    /*
     * NRZI: a zero data bit is a transition, a one keeps the level. Mark
     * (level true) is 1200 Hz, space is 2200 Hz.
     */
    if (!m->levelValid)
    {
        m->level      = true;
        m->levelValid = true;
    }
    else if (!bit)
    {
        m->level = !m->level;
    }

    afsk_mod_level(m, m->level, out);
}

void afsk_mod_level(afsk_mod_t *m, bool level, int16_t *out)
{
    m->level      = level;
    m->levelValid = true;

    const uint32_t inc = m->level ? m->incMark : m->incSpace;
    const float k = (float)(1.0 - exp(-2.0 * 3.14159265358979 * PREEMPH_LP_HZ
                                      / (double)m->fs));
    /*
     * The sine table is already in units of full scale, so the amplitude
     * scale is applied directly; the 1/(1 + preemph/2) factor leaves headroom
     * for the shelf boost, which would otherwise clip the DAC.
     */
    const float scale = m->gain / (1.0f + 0.5f * m->preemph);

    for (uint32_t i = 0; i < m->samplesPerBit; i++)
    {
        m->phase += inc;
        float y = sinRaw(m->phase);

        if (m->preemph > 0.0f)
        {
            m->hpState += k * (y - m->hpState);
            y += m->preemph * (y - m->hpState);
        }

        float v = y * scale;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        out[i] = (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
    }

    m->bitPos = 0;
}

size_t afsk_mod_silence(afsk_mod_t *m, int16_t *out, size_t max, uint32_t ms)
{
    size_t n = (size_t)((m->fs * ms) / 1000u);
    if (n > max)
        n = max;
    memset(out, 0, n * sizeof(int16_t));
    return n;
}

/* ------------------------------------------------------------------ RX --- */

bool afsk_demod_init(afsk_demod_t *d, uint32_t fs)
{
    memset(d, 0, sizeof(*d));

    if ((fs != 24000u) && (fs != 48000u))
        return false;

    d->fs       = fs;
    d->decim    = fs / AFSK_RATE;
    d->muInc    = 1.0f / (float)AFSK_BIT_SAMPLES;
    d->agcEnv   = 1.0f;
    d->agcGain  = 1.0f;
    d->phaseMark  = 0;
    d->phaseSpace = 0;
    d->incMark    = phaseInc(AFSK_MARK_HZ, AFSK_RATE);
    d->incSpace   = phaseInc(AFSK_SPACE_HZ, AFSK_RATE);
    d->level    = true;
    d->signal   = false;

    return true;
}

int afsk_level_percent(const int16_t *samples, size_t count)
{
    int32_t peak = 0;
    for (size_t i = 0; i < count; i++)
    {
        int32_t v = samples[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    return (int)((peak * 100) / 32768);
}

/*
 * Bit decision: the soft metric is integrated over the three thirds of the
 * bit, and the middle third weighs double. Ties fall back to the metric at
 * the decision instant, which is where the signal-to-noise is best.
 */
static bool decideBit(const afsk_demod_t *d)
{
    int score = 0;
    score += (d->subAcc[0] > 0.0f) ? 1 : ((d->subAcc[0] < 0.0f) ? -1 : 0);
    score += (d->subAcc[1] > 0.0f) ? 2 : ((d->subAcc[1] < 0.0f) ? -2 : 0);
    score += (d->subAcc[2] > 0.0f) ? 1 : ((d->subAcc[2] < 0.0f) ? -1 : 0);

    if (score == 0)
        return d->metric > 0.0f;

    return score > 0;
}

bool afsk_demod_sample(afsk_demod_t *d, int16_t sample, bool *bit)
{
    /* Decimate 48 kHz to 24 kHz: two-sample average, flat to 4 kHz */
    if (d->decim > 1)
    {
        d->decAcc += sample;
        if (++d->decCount < d->decim)
            return false;
        sample = (int16_t)(d->decAcc / (int32_t)d->decim);
        d->decAcc   = 0;
        d->decCount = 0;
    }

    /* DC block: removes the discriminator offset of the C62's FM detector */
    float x = (float)sample;
    float y = x - d->dcX1 + 0.998f * d->dcY1;
    d->dcX1 = x;
    d->dcY1 = y;

    /*
     * AGC on the mean magnitude over about two bits. A peak detector is
     * wrong here: the two tones of Bell 202 beat against each other during
     * every transition, so the envelope of an FSK signal swings by 2:1 and a
     * peak tracker would leave the gain 6 dB off for tens of milliseconds
     * (measured: it halved the correlator energy for ~100 bits and produced
     * the bulk of the bit errors).
     */
    float mag = fabsf(y);
    d->agcEnv += (1.0f / 40.0f) * (mag - d->agcEnv);
    if (d->agcEnv < 1.0f)
        d->agcEnv = 1.0f;

    /*
     * Target amplitude of the AGC'd signal, in sample units: about -12 dBFS,
     * which leaves room for the peaks and keeps the correlator outputs (and
     * therefore the carrier detect thresholds) in a fixed range. The 2/pi
     * factor converts the mean magnitude of a sine into its amplitude.
     */
    const float target = 5100.0f;   /* 0.637 * 8000 */
    d->agcGain = target / d->agcEnv;
    if (d->agcGain > 20.0f) d->agcGain = 20.0f;
    if (d->agcGain < 0.25f) d->agcGain = 0.25f;
    y *= d->agcGain;

    /* Quadrature products at both tones */
    d->phaseMark  += d->incMark;
    d->phaseSpace += d->incSpace;

    const float p0 = y * cosIdx(d->phaseMark);
    const float p1 = y * sinIdx(d->phaseMark);
    const float p2 = y * cosIdx(d->phaseSpace);
    const float p3 = y * sinIdx(d->phaseSpace);

    /* Sliding one-bit window: add the new product, drop the oldest */
    const uint8_t idx = d->histIdx;
    d->sum[0] += p0 - d->hist[0][idx];
    d->sum[1] += p1 - d->hist[1][idx];
    d->sum[2] += p2 - d->hist[2][idx];
    d->sum[3] += p3 - d->hist[3][idx];
    d->hist[0][idx] = p0;
    d->hist[1][idx] = p1;
    d->hist[2][idx] = p2;
    d->hist[3][idx] = p3;
    d->histIdx = (uint8_t)((idx + 1u) % AFSK_BIT_SAMPLES);

    const float eMark  = d->sum[0] * d->sum[0] + d->sum[1] * d->sum[1];
    const float eSpace = d->sum[2] * d->sum[2] + d->sum[3] * d->sum[3];
    const float metric = eMark - eSpace;
    d->metric = metric;

    /* Signal presence: correlator energy of a carrier at AGC level, the
     * threshold is ~10 dB below a full-deviation tone (measured) */
    /*
     * Signal presence: an FSK signal puts almost all of its correlator energy
     * on one of the two tones, white noise splits it. Both tests are needed:
     * the level one keeps the AGC from opening on a dead carrier, the
     * coherence one separates data from noise (see docs/APRS.md for the
     * measured numbers).
     */
    const float total = eMark + eSpace;
    d->energy += 0.002f * (total - d->energy);
    d->signal = (d->energy > AFSK_DCD_ENERGY)
             && (fabsf(metric) > AFSK_DCD_COHERENCE * total);

    /*
     * Bit clock. The correlator window is one bit long, so the metric crosses
     * zero about half a bit after a symbol boundary: the crossing is the
     * instant where the window straddles the transition evenly, and that is
     * where the decision should be taken (mu = 0.5).
     *
     * Only crossings that are preceded by a real excursion of the metric
     * count: near zero the metric dithers and would otherwise fire a
     * correction on every sample of the dither, which is what makes a naive
     * PLL walk away from the signal during long runs of one tone.
     */
    if (fabsf(metric) > 0.2f * total)
        d->crossArmed = true;

    if (d->crossArmed && ((metric > 0.0f) != (d->metricPrev > 0.0f)))
    {
        /*
         * mu is anchored to the symbol centre, where the crossing is: the
         * error is the distance from it, wrapped to half a bit either way.
         */
        float err = d->mu;
        if (err > 0.5f)  err -= 1.0f;
        if (err < -0.5f) err += 1.0f;

        if (!d->pllLocked)
        {
            /*
             * The first armed crossing anchors the bit clock. Without it the
             * loop has to walk in from wherever the demodulator started,
             * which takes dozens of transitions and costs the first frame.
             */
            d->mu        = 0.0f;
            d->pllLocked = true;
        }
        else
        {
            /*
             * Proportional plus integral. The integral term has to stay well
             * below one sample per bit: it corrects the clock offset between
             * the two stations (a few hundred ppm) and nothing more. Allowing
             * it to reach muInc stops the bit clock altogether, which is a
             * failure mode that costs an afternoon if it ever happens in the
             * field.
             */
            const float kp = AFSK_PLL_KP;
            const float ki = AFSK_PLL_KI;
            d->mu     -= kp * err;
            d->muTrim -= ki * err;

            if (d->muTrim >  AFSK_PLL_TRIM_MAX)
                d->muTrim =  AFSK_PLL_TRIM_MAX;
            if (d->muTrim < -AFSK_PLL_TRIM_MAX)
                d->muTrim = -AFSK_PLL_TRIM_MAX;

            d->lastError = err;
            d->timingCorrections++;
        }
        d->crossArmed = false;
    }
    d->metricPrev = metric;

    /*
     * Accumulate the metric over the middle half of the bit, in three slices:
     * the outer quarters are guard bands where a misalignment (or the beating
     * of the two tones during a transition) would poison the decision, and
     * the three slices give the majority vote something to vote on.
     */
    if ((d->mu >= AFSK_DEC_WIN_LO) && (d->mu < AFSK_DEC_WIN_HI))
    {
        float pos = (d->mu - AFSK_DEC_WIN_LO) * (3.0f / (AFSK_DEC_WIN_HI
                                                       - AFSK_DEC_WIN_LO));
        uint8_t slice = (uint8_t)pos;
        if (slice > 2u) slice = 2u;
        d->subAcc[slice] += metric;
    }

    /* A proportional correction can push the phase slightly out of [0,1) */
    if (d->mu < 0.0f)
        d->mu = 0.0f;

    d->mu += d->muInc + d->muTrim;

    if (d->mu < 1.0f)
        return false;

    /* End of bit: decide, reset the accumulators */
    d->mu -= 1.0f;
    bool decided = decideBit(d);
    d->lastAcc[0] = d->subAcc[0];
    d->lastAcc[1] = d->subAcc[1];
    d->lastAcc[2] = d->subAcc[2];
    d->subAcc[0] = d->subAcc[1] = d->subAcc[2] = 0.0f;
    d->bitsDecoded++;

    if (bit != NULL)
        *bit = decided;

    return (bit != NULL);
}

size_t afsk_demod_block(afsk_demod_t *d, const int16_t *samples, size_t count,
                        void (*onBit)(bool bit, void *user), void *user)
{
    size_t bits = 0;

    for (size_t i = 0; i < count; i++)
    {
        bool bit;
        if (afsk_demod_sample(d, samples[i], &bit))
        {
            bits++;
            if (onBit != NULL)
                onBit(bit, user);
        }
    }

    return bits;
}
