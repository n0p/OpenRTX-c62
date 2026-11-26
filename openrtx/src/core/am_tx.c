/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/am_tx.h"

/*
 * Filter coefficients, Q14, computed for 48 kHz with the RBJ cookbook
 * formulas and the Butterworth quality factors. The response they give,
 * including the decimation boxcar, is measured in docs/TX_AIRBAND.md:
 *
 *   100 Hz -19.1 dB   300 Hz -3.0 dB   1000 Hz -0.3 dB   2000 Hz -1.1 dB
 *   2700 Hz -3.2 dB  3000 Hz -5.1 dB   4000 Hz -14.6 dB  6000 Hz -35.7 dB
 *
 * The transmit channel of the band is 300 to 2700 Hz, and everything that
 * could splatter into the 8.33 kHz neighbour is at least 14 dB down. Of the
 * 3.2 dB at 2.7 kHz, 1.6 dB are the boxcar that decimates to the duty rate;
 * it is also what puts a zero at 8 kHz, where the zero-order hold of the PWM
 * would otherwise fold back into the band.
 *
 * The feedback coefficients are stored already negated, so a section is
 * y = (b0 x + b1 x1 + b2 x2 + a1 y1 + a2 y2) >> 14.
 */
static const struct amTxBiquad hpf300 = {
    .b0 = 15935,
    .b1 = -31871,
    .b2 = 15935,
    .a1 = 31858,
    .a2 = -15499,
};

static const struct amTxBiquad lpf3000a = {
    .b0 = 461,
    .b1 = 921,
    .b2 = 461,
    .a1 = 22366,
    .a2 = -7825,
};

static const struct amTxBiquad lpf3000b = {
    .b0 = 544,
    .b1 = 1088,
    .b2 = 544,
    .a1 = 26407,
    .a2 = -12198,
};

/*
 * Level control and limiter. The detector runs at the duty rate, 8 kHz, so
 * its shifts look faster than the ones of core/agc.c even where the time
 * constant is the same.
 *
 * It looks at the *output* of the gain, as the receive AGC does, so that a
 * syllable the limiter is already holding down still lowers the gain. The
 * 16 ms decay is short enough to follow speech and long enough not to follow a
 * 300 Hz waveform cycle by cycle. The gain falls 10 dB in 9.6 ms and comes
 * back 10 dB in 640 ms: a microphone level that drops between phrases must not
 * bring the noise of the cabin up into the envelope.
 */
static const int32_t AGC_OPEN_LEVEL = 31129;  /* 0.95 of full scale */
static const int32_t AGC_CLOSE_LEVEL = 26214; /* 0.80, 1.5 dB of hysteresis */
static const uint8_t AGC_DECAY_SHIFT = 7;     /* 128 samples, 16 ms at 8 kHz */
static const uint8_t AGC_ATTACK_SHIFT = 6;    /* 10 dB in 9.6 ms */
static const uint8_t AGC_REL_SHIFT = 12;      /* 10 dB in 640 ms */
static const int32_t AGC_GAIN_MAX = 64 * 65536; /* +36 dB */
static const int32_t AGC_GAIN_MIN = 65536 / 4;  /* -12 dB */

/*
 * Soft limiter: linear below the knee, 4:1 above it, top at full scale. The
 * knee sits just under full scale because the limiter is what defines the
 * peaks, and the modulation depth the operator asked for is only reached if
 * the peaks do reach it: everything above the knee is a syllable that would
 * have overmodulated the carrier.
 */
static const int32_t LIMIT_KNEE = 32112; /* 0.98 of full scale */

/* Ramp of the modulation depth, percent * 256 per 8 kHz sample: 20 ms */
static const uint16_t DEPTH_RAMP_STEP = 1600;
/* Ramp of the carrier, 0 to 256: 16 ms */
static const uint16_t CARRIER_RAMP_STEP = 2;

/**
 * Magnitude of a sample, without the 16 bit wrap of -32768.
 *
 * @param s: sample.
 * @return the magnitude.
 */
static int32_t magnitude(int32_t s)
{
    return (s < 0) ? -s : s;
}

/**
 * Saturate to the Q15 range of the signal path.
 *
 * @param v: value.
 * @return v clamped to -32768..32767.
 */
static int32_t saturate(int32_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return v;
}

/**
 * One biquad section on one sample.
 *
 * The accumulator is 64-bit because five products of a Q14 coefficient and a
 * Q15 signal do not fit 32 bits with room for the transient of a section whose
 * poles sit at 300 Hz.
 *
 * @param s: section, its state updated in place.
 * @param x: input sample, Q15.
 * @return the output sample, Q15.
 */
static int32_t biquad(struct amTxBiquad *s, int32_t x)
{
    int64_t acc = (int64_t)s->b0 * x + (int64_t)s->b1 * s->x1
                + (int64_t)s->b2 * s->x2 + (int64_t)s->a1 * s->y1
                + (int64_t)s->a2 * s->y2;
    int32_t y = saturate((int32_t)(acc >> 14));

    s->x2 = s->x1;
    s->x1 = x;
    s->y2 = s->y1;
    s->y1 = y;

    return y;
}

/**
 * Reset a section's state.
 *
 * @param s: section.
 */
static void biquadReset(struct amTxBiquad *s)
{
    s->x1 = 0;
    s->x2 = 0;
    s->y1 = 0;
    s->y2 = 0;
}

/**
 * Soft limiter: linear up to the knee, 4:1 above it, and never past full
 * scale. Same shape as the one of core/agc.c, moved up because a transmitter
 * wants its peaks close to full scale: the depth of the modulation is what the
 * operator asked for only if the peaks reach it.
 *
 * @param v: sample, Q15.
 * @return the limited sample.
 */
static int32_t limit(int32_t v)
{
    int32_t a = magnitude(v);
    int32_t span = 32767 - LIMIT_KNEE;
    int32_t over = a - LIMIT_KNEE;

    if (over <= 0)
        return v;

    over = (over * span) / (4 * span + over);
    a = LIMIT_KNEE + over;
    if (a > 32767)
        a = 32767;

    return (v < 0) ? -a : a;
}

/**
 * Level control: move the envelope towards a magnitude and the gain towards
 * the level window, one duty-rate sample.
 *
 * @param t: instance.
 * @param m: magnitude of the current output, sample units.
 */
static void levelUpdate(struct amTx *t, int32_t m)
{
    int32_t diff = m - t->env;

    if (diff > 0)
        t->env = m; /* attack is the peak itself */
    else
        t->env += (diff + (1 << AGC_DECAY_SHIFT) - 1) >> AGC_DECAY_SHIFT;

    if (t->env > AGC_OPEN_LEVEL) {
        t->gain -= (t->gain + (1 << AGC_ATTACK_SHIFT) - 1) >> AGC_ATTACK_SHIFT;
        if (t->gain < AGC_GAIN_MIN)
            t->gain = AGC_GAIN_MIN;
    } else if (t->env < AGC_CLOSE_LEVEL) {
        t->gain += (t->gain + (1 << AGC_REL_SHIFT) - 1) >> AGC_REL_SHIFT;
        if (t->gain > AGC_GAIN_MAX)
            t->gain = AGC_GAIN_MAX;
    }
}

/**
 * One sample of the modulator, from the microphone to the relative envelope:
 * the three sections, the levelling and the limiter.
 *
 * @param t: instance.
 * @param sample: input sample.
 * @return the relative envelope, Q15, already limited to +-1.0.
 */
static int32_t envelopeSample(struct amTx *t, int32_t sample)
{
    int32_t v = biquad(&t->highPass, sample);
    v = biquad(&t->lowPassA, v);
    v = biquad(&t->lowPassB, v);
    v = limit((v * t->gain) >> 16);
    levelUpdate(t, magnitude(v));

    return v;
}

struct amTxConfig am_tx_defaultConfig(uint16_t dutyCarrier)
{
    struct amTxConfig cfg = {
        .dutyCarrier = dutyCarrier,
        .dutyMin = (uint16_t)(dutyCarrier / 10),
        .dutyMax = (uint16_t)((dutyCarrier * 19) / 10),
        .slope = dutyCarrier,
        .depth = AMTX_DEPTH_DEFAULT,
    };

    return cfg;
}

void am_tx_init(struct amTx *t, const struct amTxConfig *cfg)
{
    t->cfg = (cfg != NULL) ? *cfg : am_tx_defaultConfig(500);
    am_tx_setDepth(t, t->cfg.depth);
    am_tx_reset(t);
}

void am_tx_reset(struct amTx *t)
{
    t->highPass = hpf300;
    t->lowPassA = lpf3000a;
    t->lowPassB = lpf3000b;
    biquadReset(&t->highPass);
    biquadReset(&t->lowPassA);
    biquadReset(&t->lowPassB);

    t->env = AGC_OPEN_LEVEL;
    t->gain = 65536;
    t->decimSum = 0;
    t->decimCount = 0;
    t->depthRamp = 0;
    t->carrierRamp = 0;
    t->keyed = false;
    t->closing = false;
}

/**
 * Deepest modulation the calibrated window allows.
 *
 * The envelope swings from (1 - m) to (1 + m) times the carrier, so the duty
 * has to reach dutyCarrier * (1 - m) at the bottom and dutyCarrier * (1 + m) at
 * the top and stay inside [dutyMin, dutyMax]. Where the PA stops being linear
 * before the operator's depth is reached, the modulation is capped instead of
 * clipped: a depth that the radio cannot produce without flat-topping the
 * positive peaks would splatter into the neighbouring 8.33 kHz channel, which
 * is worse than a shallower modulation.
 *
 * @param cfg: calibration.
 * @return the depth the window allows, percent.
 */
static uint8_t windowDepth(const struct amTxConfig *cfg)
{
    if (cfg->dutyCarrier == 0)
        return 0;

    int32_t up = (100 * ((int32_t)cfg->dutyMax - (int32_t)cfg->dutyCarrier))
               / (int32_t)cfg->dutyCarrier;
    int32_t down = (100 * ((int32_t)cfg->dutyCarrier - (int32_t)cfg->dutyMin))
                 / (int32_t)cfg->dutyCarrier;
    int32_t m = (up < down) ? up : down;

    if (m < 0)
        m = 0;
    if (m > AMTX_DEPTH_MAX)
        m = AMTX_DEPTH_MAX;

    return (uint8_t)m;
}

void am_tx_setDepth(struct amTx *t, uint8_t percent)
{
    if (percent < AMTX_DEPTH_MIN)
        percent = AMTX_DEPTH_MIN;
    if (percent > AMTX_DEPTH_MAX)
        percent = AMTX_DEPTH_MAX;

    t->cfg.depth = percent;
    t->depthEff = windowDepth(&t->cfg);
    if (t->depthEff > percent)
        t->depthEff = percent;

    if ((t->keyed) && (!t->closing))
        t->depthRamp = (uint16_t)t->depthEff * 256; /* live, no ramp */
}

uint8_t am_tx_getDepth(const struct amTx *t)
{
    return t->depthEff;
}

void am_tx_setCarrier(struct amTx *t, uint16_t counts)
{
    t->cfg.dutyCarrier = counts;
    if (t->cfg.dutyMin > counts)
        t->cfg.dutyMin = counts;
    if (t->cfg.dutyMax < counts)
        t->cfg.dutyMax = counts;

    t->depthEff = windowDepth(&t->cfg);
    if (t->depthEff > t->cfg.depth)
        t->depthEff = t->cfg.depth;
}

void am_tx_keyDown(struct amTx *t)
{
    t->keyed = true;
    t->closing = false;
    t->carrierRamp = 0;
    t->depthRamp = 0;
}

void am_tx_keyUp(struct amTx *t)
{
    t->closing = true;
}

bool am_tx_busy(const struct amTx *t)
{
    return t->keyed && ((!t->closing) || (t->carrierRamp > 0));
}

size_t am_tx_process(struct amTx *t, const int16_t *in, size_t n,
                     uint16_t *duty)
{
    size_t out = 0;

    for (size_t i = 0; i < n; i++) {
        t->decimSum += envelopeSample(t, (int32_t)in[i]);
        t->decimCount++;

        if (t->decimCount < AMTX_DECIM)
            continue;

        /*
         * The boxcar is the decimation filter and the average of the
         * envelope over the period the duty value will be held for: what the
         * PWM produces is a step, and the mean of the six samples is the best
         * the modulator can do about the difference between a step and the
         * waveform it replaces.
         */
        int32_t x = t->decimSum / (int32_t)AMTX_DECIM;
        t->decimSum = 0;
        t->decimCount = 0;

        /* Depth and carrier ramps, both of them at the duty rate */
        if ((t->keyed) && (!t->closing)
            && (t->depthRamp < (uint16_t)t->depthEff * 256)) {
            uint16_t target = (uint16_t)t->depthEff * 256;
            t->depthRamp =
                (uint16_t)((t->depthRamp + DEPTH_RAMP_STEP > target) ?
                               target :
                               t->depthRamp + DEPTH_RAMP_STEP);
        }

        if (t->keyed) {
            if ((!t->closing) && (t->carrierRamp < 256))
                t->carrierRamp =
                    (uint16_t)((t->carrierRamp + CARRIER_RAMP_STEP > 256) ?
                                   256 :
                                   t->carrierRamp + CARRIER_RAMP_STEP);
            if ((t->closing) && (t->depthRamp > 0))
                t->depthRamp = (uint16_t)((t->depthRamp > DEPTH_RAMP_STEP) ?
                                              t->depthRamp - DEPTH_RAMP_STEP :
                                              0);
            if ((t->closing) && (t->depthRamp == 0) && (t->carrierRamp > 0))
                t->carrierRamp =
                    (uint16_t)((t->carrierRamp > CARRIER_RAMP_STEP) ?
                                   t->carrierRamp - CARRIER_RAMP_STEP :
                                   0);
            if ((t->closing) && (t->carrierRamp == 0))
                t->keyed = false;
        }

        if (!t->keyed) {
            duty[out++] = 0;
            continue;
        }

        /*
         * Envelope to duty: m * x counts of swing around the carrier, clamped
         * to the window where the PA is linear, and the whole thing scaled by
         * the carrier ramp so that the key-up starts from zero. Both products
         * fit 32 bits: the slope is a few hundred counts and the depth is at
         * most 90 % of 2^15.
         */
        int32_t mod = ((int32_t)t->cfg.slope * x) >> 15;
        mod = (mod * (int32_t)t->depthRamp) / 25600; /* percent * 256 */

        int32_t d = (int32_t)t->cfg.dutyCarrier + mod;

        if (d < (int32_t)t->cfg.dutyMin)
            d = t->cfg.dutyMin;
        if (d > (int32_t)t->cfg.dutyMax)
            d = t->cfg.dutyMax;

        d = (d * t->carrierRamp) / 256;
        duty[out++] = (uint16_t)d;
    }

    return out;
}

int32_t am_tx_envelope(struct amTx *t, int16_t sample)
{
    return envelopeSample(t, (int32_t)sample);
}
