/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DpmrPhy.c
 * @brief dPMR demodulator, see DpmrPhy.h.
 *
 * Chain, per ETSI TS 102 490 clause 12.2.2.2:
 *
 *   discriminator (48 kHz, +-1050 Hz = +-1 symbol)
 *     -> square root raised cosine, alpha = 0.2, T = 1/2400, span +-4 T
 *     -> decimate by 5: 4 samples per symbol at 9.6 kHz
 *     -> frame sync correlation over the four phases (FS2/FS3, and FS1 every
 *        fourth sample), fractional refinement with 4-point interpolation
 *     -> 192 symbol frame, sliced against levels estimated from the sync run
 *     -> CCH / HI / END FEC (DpmrDefs.c), AMBE+2 frames for the vocoder
 *
 * The standard's receiver also asks for the inverse sinc of the FM
 * differentiator (|D(f)| = pi f T / sin(pi f T)).  At 2400 symbols/s the
 * correction is +1.2 dB at 1.2 kHz and is not applied here: the slicer
 * estimates its own levels per frame, which is what makes the deviation of the
 * receiver irrelevant (the C62 delivers about 1.6 kHz where ETSI says 1.05
 * kHz).
 */

#include "protocols/DPMR/DpmrPhy.h"

#include <math.h>
#include <string.h>

/** Matched filter: RRC alpha = 0.2, 20 samples per symbol at 48 kHz, +-4 T. */
#define DPMR_MF_TAPS 161
/** Decimation from 48 kHz to 9.6 kHz. */
#define DPMR_DECIM 5
/** Ring of demodulated samples: 1024 / 4 = 256 symbols, one frame is 192. */
#define DPMR_RING 1024
/**
 * Sync thresholds, normalised to the ideal correlation.  The coarse scan runs
 * on the integer sample grid, where a symbol can be half a sample off the eye
 * centre, so it only has to be good enough to nominate a candidate; the
 * sub-sample refinement has to clear the higher bar before a frame is decoded.
 */
#define DPMR_SYNC_COARSE 0.78f
/** Largest level error a frame may show and still be an acquisition lock. */
#define DPMR_MAX_EVM_ACQ 0.26f
#define DPMR_SYNC_SCORE 0.80f
/** Fingerprint of a frame start: score of the sync correlation. */
#define DPMR_FRAME_SAMPLES (DPMR_FRAME_SYMBOLS * DPMR_PHY_SPS)
/** Consecutive frames that may fail before the grid is dropped. */
#define DPMR_MAX_MISSES 2
/** Samples without a decode before the squelch closes: 400 ms at 9.6 kHz. */
#define DPMR_SYNC_TIMEOUT 3840u


static const float mfTaps[DPMR_MF_TAPS] =
{
    0.0061212f, 0.0053073f, 0.0042650f, 0.0030136f, 0.0015804f,
    -0.0000000f, -0.0016863f, -0.0034315f, -0.0051837f, -0.0068880f,
    -0.0084876f, -0.0099258f, -0.0111475f, -0.0121011f, -0.0127403f,
    -0.0130259f, -0.0129269f, -0.0124226f, -0.0115034f, -0.0101715f,
    -0.0084422f, -0.0063432f, -0.0039152f, -0.0012113f, 0.0017043f,
    0.0047571f, 0.0078640f, 0.0109355f, 0.0138777f, 0.0165949f,
    0.0189921f, 0.0209777f, 0.0224669f, 0.0233835f, 0.0236634f,
    0.0232568f, 0.0221300f, 0.0202682f, 0.0176758f, 0.0143786f,
    0.0104235f, 0.0058788f, 0.0008336f, -0.0046030f, -0.0103043f,
    -0.0161272f, -0.0219160f, -0.0275045f, -0.0327202f, -0.0373877f,
    -0.0413331f, -0.0443877f, -0.0463926f, -0.0472026f, -0.0466901f,
    -0.0447490f, -0.0412974f, -0.0362813f, -0.0296760f, -0.0214881f,
    -0.0117569f, -0.0005540f, 0.0120168f, 0.0258207f, 0.0406937f,
    0.0564454f, 0.0728619f, 0.0897100f, 0.1067413f, 0.1236969f,
    0.1403122f, 0.1563228f, 0.1714690f, 0.1855016f, 0.1981868f,
    0.2093109f, 0.2186852f, 0.2261492f, 0.2315746f, 0.2348679f,
    0.2359721f, 0.2348679f, 0.2315746f, 0.2261492f, 0.2186852f,
    0.2093109f, 0.1981868f, 0.1855016f, 0.1714690f, 0.1563228f,
    0.1403122f, 0.1236969f, 0.1067413f, 0.0897100f, 0.0728619f,
    0.0564454f, 0.0406937f, 0.0258207f, 0.0120168f, -0.0005540f,
    -0.0117569f, -0.0214881f, -0.0296760f, -0.0362813f, -0.0412974f,
    -0.0447490f, -0.0466901f, -0.0472026f, -0.0463926f, -0.0443877f,
    -0.0413331f, -0.0373877f, -0.0327202f, -0.0275045f, -0.0219160f,
    -0.0161272f, -0.0103043f, -0.0046030f, 0.0008336f, 0.0058788f,
    0.0104235f, 0.0143786f, 0.0176758f, 0.0202682f, 0.0221300f,
    0.0232568f, 0.0236634f, 0.0233835f, 0.0224669f, 0.0209777f,
    0.0189921f, 0.0165949f, 0.0138777f, 0.0109355f, 0.0078640f,
    0.0047571f, 0.0017043f, -0.0012113f, -0.0039152f, -0.0063432f,
    -0.0084422f, -0.0101715f, -0.0115034f, -0.0124226f, -0.0129269f,
    -0.0130259f, -0.0127403f, -0.0121011f, -0.0111475f, -0.0099258f,
    -0.0084876f, -0.0068880f, -0.0051837f, -0.0034315f, -0.0016863f,
    -0.0000000f, 0.0015804f, 0.0030136f, 0.0042650f, 0.0053073f,
    0.0061212f
};

/* Level patterns of the sync words, in units of 350 Hz */
static int8_t syncFs1[24];
static int8_t syncFs2[12];
static int8_t syncFs3[12];

static struct
{
    /* Matched filter history, newest at histIdx */
    float hist[DPMR_MF_TAPS];
    unsigned histIdx;

    /* Decimator phase */
    unsigned decim;

    /* Demodulated samples at 4 sps, absolute index of ring[ringHead] is
     * headSample - DPMR_RING */
    float ring[DPMR_RING];
    unsigned ringHead;
    uint32_t produced;          /* samples pushed at 4 sps */

    /* Framer */
    bool locked;
    float polarity;
    float fraction;             /* fractional sample offset of the symbol grid */
    uint32_t frameStart;        /* absolute sample of the current frame start */
    unsigned misses;
    unsigned sinceDecode;
    uint8_t expectFn;           /* frame number the next frame should carry */
    bool haveFn;

    /* Peak hold of the sync search: the strongest candidate of the last window */
    enum dpmrSync candSync;
    uint32_t candStart;
    float candScore;
    float candPolarity;
    unsigned candAge;
    bool firstFrame;            /* the grid was just acquired: sweep the start */
    enum dpmrSync expectedSync;
    bool synced;
    bool everLocked;            /* a grid has been acquired at least once */

    struct dpmrPhyStats stats;
    uint32_t evmCount;
    float evmSum;
} phy;

/* ------------------------------------------------------------ utilities --- */

static void processNewSample(dpmrFrameHandler handler, void *user);

static float absf(float v)
{
    return v < 0.0f ? -v : v;
}

/**
 * Sample the ring at a given age: 0 is the newest sample written, 1 the one
 * before it.  The ring is a circle, so the physical index is the write head
 * minus one minus the age.
 */
static float ringSample(int32_t age)
{
    if ((age < 0) || (age >= (int32_t)DPMR_RING))
        return 0.0f;

    int idx = (int)phy.ringHead - 1 - (int)age;

    while (idx < 0)
        idx += (int)DPMR_RING;

    return phy.ring[(unsigned)idx];
}

/** 4-point Lagrange interpolation of the ring at absolute position p. */
static float ringAt(float p)
{
    int i = (int)p;

    if (p < (float)i)
        i -= 1;                 /* p negative: the cast truncates towards 0 */

    float t = p - (float)i;
    int32_t newest = (int32_t)(phy.produced - 1u);
    int32_t age = newest - i;   /* age of the sample at p */

    if ((age + 1 >= (int32_t)DPMR_RING) || (age - 2 < 0))
        return 0.0f;            /* too new to interpolate, or before the ring */

    float p0 = ringSample(age + 1);
    float p1 = ringSample(age);
    float p2 = ringSample(age - 1);
    float p3 = ringSample(age - 2);

    return (-t * (t - 1.0f) * (t - 2.0f) / 6.0f * p0
            + (t + 1.0f) * (t - 1.0f) * (t - 2.0f) / 2.0f * p1
            - (t + 1.0f) * t * (t - 2.0f) / 2.0f * p2
            + (t + 1.0f) * t * (t - 1.0f) / 6.0f * p3);
}

/**
 * Read nsym symbols starting at absolute position start, with a fractional
 * offset.  Returns the mean absolute level of the first run symbols, which is
 * the +-1050 Hz reference the slicer uses.
 */
static float readSymbols(uint32_t start, unsigned nsym, float fraction,
                         float *out, unsigned runLen)
{
    float sum = 0.0f;

    for (unsigned k = 0; k < nsym; k++)
        out[k] = ringAt((float)start + (float)(k * DPMR_PHY_SPS) + fraction);

    for (unsigned k = 0; k < runLen; k++)
        sum += absf(out[k]);

    return runLen > 0 ? sum / (float)runLen : 0.0f;
}

/**
 * Normalised correlation of a level pattern with the samples, with the mean of
 * both removed: 1 for a perfect match, 0 for no correlation.
 *
 * The mean removal is what keeps a silent or DC-offset run from scoring: the
 * FS2 pattern has a non-zero mean, so a plain cosine similarity gives a
 * constant window 0,58, which is enough to hide the real sync behind it.
 */
static float scorePattern(const float *y, const int8_t *pattern, unsigned n,
                          float polarity)
{
    float my = 0.0f;
    float mp = 0.0f;

    for (unsigned k = 0; k < n; k++) {
        my += y[k];
        mp += (float)pattern[k] * polarity;
    }
    my /= (float)n;
    mp /= (float)n;

    float acc = 0.0f;
    float magY = 0.0f;
    float magP = 0.0f;

    for (unsigned k = 0; k < n; k++) {
        float a = y[k] - my;
        float b = (float)pattern[k] * polarity - mp;

        acc += a * b;
        magY += a * a;
        magP += b * b;
    }

    if ((magY <= 1.0f) || (magP <= 1e-6f))
        return 0.0f;

    return acc / sqrtf(magY * magP);
}

/**
 * Sync search at the sample that has just arrived: the last symbol of the
 * pattern is the newest sample, so the pattern covers
 * [now - 4*(n-1) - phase, now - phase].
 */
static enum dpmrSync searchSync(float *window, uint32_t *start,
                                float *fraction, float *polarity,
                                float *outScore)
{
    enum dpmrSync best = DPMR_SYNC_NONE;
    float bestScore = DPMR_SYNC_COARSE;
    const int8_t *patterns[3] = { syncFs2, syncFs3, syncFs1 };
    const enum dpmrSync kinds[3] = { DPMR_SYNC_FS2, DPMR_SYNC_FS3, DPMR_SYNC_FS1 };
    const unsigned lengths[3] = { 12, 12, 24 };

    /* FS1 is 24 symbols and far rarer by chance: only every fourth sample */
    unsigned npatterns = ((phy.produced & 3u) == 0) ? 3 : 2;

    for (unsigned phase = 0; phase < DPMR_PHY_SPS; phase++) {
        for (unsigned pi = 0; pi < npatterns; pi++) {
            unsigned nsym = lengths[pi];
            uint32_t s = phy.produced - 3u - phase
                         - (uint32_t)((nsym - 1) * DPMR_PHY_SPS);

            float amp = readSymbols(s, nsym, 0.0f, window, nsym);

            if (amp <= 1e-6f)
                continue;

            for (int pol = 1; pol >= -1; pol -= 2) {
                float sc = scorePattern(window, patterns[pi], nsym, (float)pol);
#ifdef DPMR_PHY_DEBUG
                if ((getenv("DPMR_DEBUG_SCORE") != NULL) && (phy.produced >= 138820)
                    && (phy.produced <= 138870) && (pol == 1)) {
                    printf("SCORE t=%u ph=%u pat=%u s=%.3f y0=%.0f y1=%.0f\n",
                           phy.produced, phase, pi, sc, window[0], window[1]);
                }
#endif

                if (sc > bestScore) {
                    bestScore = sc;
                    best = kinds[pi];
                    *start = s;
                    *fraction = 0.0f;
                    *polarity = (float)pol;
                }
            }
        }
    }

    if (outScore != NULL)
        *outScore = (best == DPMR_SYNC_NONE) ? 0.0f : bestScore;

    return best;
}

/* --------------------------------------------------------------- slicing --- */

/** Slice n read symbols into dibits; returns the normalised level error. */
static float slice(const float *y, unsigned n, float amplitude, float polarity,
                   uint8_t *dibits)
{
    float error = 0.0f;
    float outer = 2.0f * amplitude / 3.0f;

    for (unsigned k = 0; k < n; k++) {
        float v = y[k] * polarity;
        float ideal;

        if (v > outer) {
            dibits[k] = 1;      /* 01: +3, +1050 Hz */
            ideal = amplitude;
        } else if (v > 0.0f) {
            dibits[k] = 0;      /* 00: +1, +350 Hz  */
            ideal = amplitude / 3.0f;
        } else if (v > -outer) {
            dibits[k] = 2;      /* 10: -1, -350 Hz  */
            ideal = -amplitude / 3.0f;
        } else {
            dibits[k] = 3;      /* 11: -3, -1050 Hz */
            ideal = -amplitude;
        }
        error += absf(v - ideal);
    }

    return (n > 0 && amplitude > 0.0f) ? error / ((float)n * amplitude) : 1.0f;
}

/* ---------------------------------------------------------------- frames --- */

/**
 * Slice and decode one 192 symbol payload frame.  Pure: no state is touched
 * except the statistics, so the caller can retry at another timing offset.
 */
static bool decodePayloadAt(uint32_t start, float fraction, float polarity,
                            struct dpmrFrame *frame)
{
    float y[DPMR_FRAME_SYMBOLS];
    uint8_t dibits[DPMR_FRAME_SYMBOLS];

    memset(frame, 0, sizeof(*frame));

    /*
     * The level reference comes from the run that opens the frame: FS2 and FS3
     * are all outer symbols, and so is every channel code (clause 7.1 encodes
     * each code bit as 01 or 11).
     */
    float amp = readSymbols(start, DPMR_FRAME_SYMBOLS, fraction, y,
                            DPMR_SYNC_SYMBOLS);
    frame->evm = slice(y, DPMR_FRAME_SYMBOLS, amp, polarity, dibits);

    frame->sync = DPMR_SYNC_CC;
    if (scorePattern(y, syncFs2, 12, polarity) > DPMR_SYNC_SCORE)
        frame->sync = DPMR_SYNC_FS2;
    else if (scorePattern(y, syncFs3, 12, polarity) > DPMR_SYNC_SCORE)
        frame->sync = DPMR_SYNC_FS3;

    frame->channelCode = (uint8_t)dpmrDefs_channelCode(dibits);

    frame->cchOk = dpmrFec_cchDecode(dibits + DPMR_SYNC_SYMBOLS, &frame->cch);
    frame->fixes = frame->cch.fixes;

    /*
     * A CRC-7 that closes on a mistimed frame is not rare (one in 128 per
     * try), so the fields have to make sense as well: a defined communication
     * mode and a version of 00 or 11, with the reserved bits clear.
     */
    if ((frame->cchOk)
        && ((frame->cch.mode > DPMR_MODE_VOICE_DATA)
            || ((frame->cch.version != 0) && (frame->cch.version != 3)))) {
        frame->cchOk = false;
    }

    if ((frame->cchOk) && ((frame->cch.mode == DPMR_MODE_VOICE)
                           || (frame->cch.mode == DPMR_MODE_VOICE_SLD)
                           || (frame->cch.mode == DPMR_MODE_VOICE_DATA))) {
        frame->voice = true;
        for (unsigned f = 0; f < DPMR_AMBE_FRAMES; f++) {
            const uint8_t *tch = dibits + DPMR_SYNC_SYMBOLS
                                 + DPMR_CCH_SYMBOLS + 36 * f;
            dpmrFec_voiceFrame(tch, frame->ambe[f]);
        }
    }

    return frame->cchOk;
}

/** Slice and decode a header frame: FS1, then HI0 (or HI1 if HI0 fails). */
static bool decodeHeaderAt(uint32_t start, float fraction, float polarity,
                           struct dpmrFrame *frame)
{
    float sync[DPMR_SYNC_SYMBOLS * 2];
    float y[DPMR_HI_SYMBOLS];
    uint8_t dibits[DPMR_HI_SYMBOLS];

    memset(frame, 0, sizeof(*frame));
    frame->sync = DPMR_SYNC_FS1;

    /*
     * The level reference is FS1, which is all outer symbols: the header
     * information itself mixes outer and inner levels, so using it as its own
     * reference would read low and smear the slice.
     */
    float amp = readSymbols(start, DPMR_SYNC_SYMBOLS * 2, fraction, sync, 24);

    float amp2 = readSymbols(start + 24 * DPMR_PHY_SPS, DPMR_HI_SYMBOLS, fraction,
                             y, 0);
    frame->evm = slice(y, DPMR_HI_SYMBOLS, amp, polarity, dibits);
    (void)amp2;

    frame->headerOk = dpmrFec_hiDecode(dibits, &frame->header);
    frame->fixes = frame->header.fixes;

    if (!frame->headerOk) {
        /* HI1 repeats the fields 72 symbols after HI0: a second chance */
        readSymbols(start + (24 + DPMR_HI_SYMBOLS + 12) * DPMR_PHY_SPS,
                    DPMR_HI_SYMBOLS, fraction, y, 0);
        frame->evm = slice(y, DPMR_HI_SYMBOLS, amp, polarity, dibits);
        frame->headerOk = dpmrFec_hiDecode(dibits, &frame->header);
        frame->fixes = frame->header.fixes;
        frame->header.fromHi1 = frame->headerOk;
    }

    /*
     * A CRC-8 closes one time in 256 on noise, and the sweep tries ~150
     * alignments: the header type and the mode have to be defined values too.
     * Table 5.11 defines header types 0000 to 1000; everything above is
     * reserved, and so is every communication mode above 101.
     */
    if ((frame->headerOk)
        && ((frame->header.mode > DPMR_MODE_VOICE_DATA)
            || (frame->header.ht > DPMR_HT_STATUS_REQ)
            || ((frame->header.version != 0) && (frame->header.version != 3)))) {
        frame->headerOk = false;
    }

    return frame->headerOk;
}

/** Slice and decode an end frame: FS3 plus the 72 bit END field. */
static bool decodeEndAt(uint32_t start, float fraction, float polarity,
                        struct dpmrFrame *frame)
{
    float y[36];
    uint8_t dibits[36];

    memset(frame, 0, sizeof(*frame));
    frame->sync = DPMR_SYNC_FS3;

    float amp = readSymbols(start + DPMR_SYNC_SYMBOLS * DPMR_PHY_SPS, 36,
                            fraction, y, 36);
    frame->evm = slice(y, 36, amp, polarity, dibits);

    bool h0 = dpmrFec_endDecode(dibits, &frame->end);

    (void)h0;
    frame->endOk = frame->end.crcOk[0] || frame->end.crcOk[1];
    frame->fixes = frame->end.fixes[0] + frame->end.fixes[1];

    return frame->endOk;
}

/**
 * Is a decoded END frame believable?
 *
 * One half of an END field is 36 bits with a CRC-7 and a 4 bit reserved field,
 * so a random alignment closes its CRC one time in 128 and sets the reserved
 * bits to zero one time in 16.  The sweep tries 51 alignments per frame, which
 * makes a bare CRC test a coin toss on noise, so an END frame is only accepted
 * when both copies closed, they agree, the reserved bits are zero and the end
 * type is one of the two defined values (table 5.12).
 */
static bool endAccept(const struct dpmrFrame *frame)
{
    return frame->end.same
           && (frame->end.reserved == 0)
           && (frame->end.et <= 1);
}

/* --------------------------------------------------------------- framer --- */

/**
 * Decode a payload frame with timing retries.
 *
 * The acquisition only knows the frame start to within a sample or two, and a
 * symbol is four samples wide, so on acquisition the start is swept over two
 * whole symbols; once the grid is locked only the fractional offset is
 * re-checked, which follows the transmitter's clock over a long transmission.
 *
 * @return true when the frame closed and passed the field sanity checks
 */
static bool payloadWithRetries(uint32_t start, float fraction, float polarity,
                               bool wide, struct dpmrFrame *out,
                               uint32_t *winStart)
{
    static const float fracs[3] = { 0.0f, 0.5f, -0.5f };
    struct dpmrFrame frame;

    /*
     * Acquisition sweeps two whole symbols: the sync correlation can peak up
     * to half a symbol off the eye, and only the CRC of the frame says which
     * alignment is the right one.  Tracking only re-checks the fraction.
     */
    int lo = wide ? -16 : -2;
    int hi = wide ? 16 : 2;

    for (int ds = lo; ds <= hi; ds++) {
        for (unsigned fi = 0; fi < 3; fi++) {
            if (!decodePayloadAt((uint32_t)((int32_t)start + ds),
                                 fraction + fracs[fi], polarity, &frame))
                continue;

            if (winStart != NULL)
                *winStart = (uint32_t)((int32_t)start + ds);
            if (out != NULL)
                *out = frame;
            return true;
        }
    }

    return false;
}

/** Header frame with the same start sweep as a payload frame. */
static bool headerWithRetries(uint32_t start, float fraction, float polarity,
                              dpmrFrameHandler handler, void *user,
                              uint32_t *winStart)
{
    static const float fracs[3] = { 0.0f, 0.5f, -0.5f };
    struct dpmrFrame frame;

    for (int ds = -8; ds <= 8; ds++) {
        for (unsigned fi = 0; fi < 3; fi++) {
            if (!decodeHeaderAt((uint32_t)((int32_t)start + ds),
                                fraction + fracs[fi], polarity, &frame))
                continue;
            if (frame.evm > DPMR_MAX_EVM_ACQ)
                continue;

            if (winStart != NULL)
                *winStart = (uint32_t)((int32_t)start + ds);
            phy.stats.frames++;
            phy.stats.headers++;
            phy.evmSum += frame.evm;
            phy.evmCount++;
            if (handler != NULL)
                handler(&frame, user);
            return true;
        }
    }

    return false;
}

/** End frame with the same start sweep. */
static bool endWithRetries(uint32_t start, float fraction, float polarity,
                           dpmrFrameHandler handler, void *user)
{
    static const float fracs[3] = { 0.0f, 0.5f, -0.5f };
    struct dpmrFrame frame;

    for (int ds = -8; ds <= 8; ds++) {
        for (unsigned fi = 0; fi < 3; fi++) {
            if (!decodeEndAt((uint32_t)((int32_t)start + ds),
                             fraction + fracs[fi], polarity, &frame))
                continue;
            if (!endAccept(&frame))
                continue;

            phy.stats.frames++;
            phy.stats.ends++;
            phy.evmSum += frame.evm;
            phy.evmCount++;
            if (handler != NULL)
                handler(&frame, user);
            return true;
        }
    }

    return false;
}

static void noteDecode(bool ok)
{
    if (ok) {
        phy.misses = 0;
        phy.sinceDecode = 0;
        phy.synced = true;
    } else {
        if (phy.misses < 0xFF)
            phy.misses++;
        if (phy.sinceDecode < 0xFFFFu)
            phy.sinceDecode++;
        if (phy.sinceDecode > DPMR_SYNC_TIMEOUT)
            phy.synced = false;
    }
}

/** Deliver a payload frame and its counters. */
static void emitPayload(const struct dpmrFrame *frame, dpmrFrameHandler handler,
                        void *user)
{
    phy.stats.frames++;
    phy.stats.cchOk++;
    phy.stats.voice += frame->voice ? 1u : 0u;
    phy.evmSum += frame->evm;
    phy.evmCount++;
    phy.expectFn = (uint8_t)((frame->cch.fn + 1u) & 3u);
    phy.haveFn = true;

    if (handler != NULL)
        handler(frame, user);
}

/**
 * Look for a transmission.  Runs once per demodulated sample while there is no
 * lock, which is why the sweep of the timing is only paid here.
 */
/**
 * Sync peak hold.
 *
 * The correlation of a 12 symbol sync is a peak about four samples wide, and
 * around a peak several shifted windows score almost as well, so taking the
 * first window above a threshold can report a frame start a whole symbol or
 * more off.  Instead the strongest candidate of the last DPMR_CAND_WINDOW
 * samples is kept, and only when it has not been beaten for that long is it
 * worth a frame decode.
 */
#define DPMR_CAND_WINDOW 64

static void acquire(dpmrFrameHandler handler, void *user)
{
    (void)handler;
    (void)user;

    float window[24];
    uint32_t start = 0;
    float fraction = 0.0f;
    float polarity = 1.0f;
    float score = 0.0f;

    enum dpmrSync sync = searchSync(window, &start, &fraction, &polarity,
                                    &score);

    if ((sync != DPMR_SYNC_NONE) && (score > phy.candScore)) {
        phy.candSync = sync;
        phy.candStart = start;
        phy.candScore = score;
        phy.candPolarity = polarity;
        phy.candAge = 0;
    } else if (phy.candSync != DPMR_SYNC_NONE) {
        phy.candAge++;
    }

    if ((phy.candSync == DPMR_SYNC_NONE) || (phy.candAge < DPMR_CAND_WINDOW)) {
        noteDecode(false);
        return;
    }

    /* Take the peak and clear it: the next acquisition starts afresh */
    sync = phy.candSync;
    start = phy.candStart;
    polarity = phy.candPolarity;
    phy.candSync = DPMR_SYNC_NONE;
    phy.candScore = 0.0f;


    /*
     * Nothing is decoded here: a frame is 768 samples long and the ring only
     * holds what has already arrived.  The candidate becomes the grid and the
     * frame is decoded when its last symbol is in.
     */
    if (phy.everLocked)
        phy.stats.resyncs++;
    phy.everLocked = true;
    phy.locked = true;
    phy.polarity = polarity;
    phy.fraction = 0.0f;
    phy.frameStart = start;
    phy.haveFn = false;
    phy.firstFrame = true;
    phy.expectedSync = sync;
}

void dpmrPhy_init(void)
{
    dpmrDefs_syncLevels(DPMR_SYNC_FS1, syncFs1);
    dpmrDefs_syncLevels(DPMR_SYNC_FS2, syncFs2);
    dpmrDefs_syncLevels(DPMR_SYNC_FS3, syncFs3);
    dpmrPhy_reset();
}

void dpmrPhy_reset(void)
{
    memset(phy.hist, 0, sizeof(phy.hist));
    memset(phy.ring, 0, sizeof(phy.ring));
    phy.histIdx = 0;
    phy.decim = 0;
    phy.ringHead = 0;
    phy.produced = 0;
    phy.locked = false;
    phy.polarity = 1.0f;
    phy.fraction = 0.0f;
    phy.frameStart = 0;
    phy.misses = 0;
    phy.sinceDecode = 0;
    phy.expectFn = 0;
    phy.haveFn = false;
    phy.candSync = DPMR_SYNC_NONE;
    phy.candStart = 0;
    phy.candScore = 0.0f;
    phy.candPolarity = 1.0f;
    phy.candAge = 0;
    phy.firstFrame = false;
    phy.expectedSync = DPMR_SYNC_NONE;
    phy.synced = false;
    phy.everLocked = false;
    memset(&phy.stats, 0, sizeof(phy.stats));
    phy.evmCount = 0;
    phy.evmSum = 0.0f;
}

void dpmrPhy_push(const int16_t *samples, size_t count,
                  dpmrFrameHandler handler, void *user)
{
    for (size_t n = 0; n < count; n++) {
        phy.hist[phy.histIdx] = (float)samples[n];
        if (++phy.histIdx == DPMR_MF_TAPS)
            phy.histIdx = 0;

        if (++phy.decim < DPMR_DECIM)
            continue;

        phy.decim = 0;

        /* Convolution, newest sample first: hist[histIdx - 1 - j] */
        float acc = 0.0f;
        unsigned idx = phy.histIdx;

        for (unsigned j = 0; j < DPMR_MF_TAPS; j++) {
            idx = (idx == 0) ? DPMR_MF_TAPS - 1 : idx - 1;
            acc += mfTaps[j] * phy.hist[idx];
        }

        phy.ring[phy.ringHead] = acc;
        if (++phy.ringHead == DPMR_RING)
            phy.ringHead = 0;
        phy.produced++;

        if (phy.produced < DPMR_RING)
            continue;

        processNewSample(handler, user);
    }
}

bool dpmrPhy_synced(void)
{
    return phy.synced;
}

void dpmrPhy_stats(struct dpmrPhyStats *out)
{
    if (out == NULL)
        return;

    *out = phy.stats;
    out->evm = phy.evmCount > 0 ? phy.evmSum / (float)phy.evmCount : 0.0f;
}


static void processNewSample(dpmrFrameHandler handler, void *user)
{
    if (!phy.locked) {
        acquire(handler, user);
        return;
    }

    if ((int32_t)(phy.produced - (phy.frameStart + DPMR_FRAME_SAMPLES)) < 0)
        return;

    struct dpmrFrame frame;
    uint32_t winStart = phy.frameStart;
    bool ok = false;

    if (phy.expectedSync == DPMR_SYNC_FS1) {
        ok = headerWithRetries(phy.frameStart, phy.fraction, phy.polarity,
                               handler, user, &winStart);
        if (ok) {
            phy.haveFn = false;
            phy.expectedSync = DPMR_SYNC_FS2;
            phy.frameStart = winStart + DPMR_FRAME_SAMPLES;
        }
    } else if (phy.expectedSync == DPMR_SYNC_FS3) {
        ok = endWithRetries(phy.frameStart, phy.fraction, phy.polarity,
                            handler, user);
        phy.locked = false;
    } else {
        ok = payloadWithRetries(phy.frameStart, phy.fraction, phy.polarity,
                                phy.firstFrame, &frame, &winStart);

        if (ok) {
            /*
             * The frame number is the grid's own check: inside a superframe it
             * runs 0,1,2,3 and a mistimed or noise frame almost never carries
             * the one expected next.
             */
            if ((phy.haveFn) && (frame.cch.fn != phy.expectFn))
                ok = false;
        }

        if (ok) {
            phy.firstFrame = false;
            phy.haveFn = true;
            phy.expectFn = (uint8_t)((frame.cch.fn + 1u) & 3u);
            phy.frameStart = winStart + DPMR_FRAME_SAMPLES;
            emitPayload(&frame, handler, user);
        } else {
            /*
             * A transmission ends with an END frame where the next payload
             * frame would be, so the same grid position gets one try at it.
             * Without this the END frame is only ever seen if the acquisition
             * happens to lock on its FS3 first, which on a real capture it
             * almost never does: the grid is still expecting payload frames.
             */
            if (endWithRetries(phy.frameStart, phy.fraction, phy.polarity,
                               handler, user)) {
                ok = true;
                phy.locked = false;
                phy.haveFn = false;
            }
        }
    }

    if (!ok)
        phy.stats.cchFail++;


    noteDecode(ok);

    if (phy.misses >= DPMR_MAX_MISSES) {
        phy.locked = false;
        phy.stats.syncLost++;
    }
}
