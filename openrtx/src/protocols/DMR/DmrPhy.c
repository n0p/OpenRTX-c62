/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrPhy.c
 * @brief DMR receive PHY, see DmrPhy.h.
 *
 * Streaming version of the chain validated by the host prototype in
 * work/dmr-rx (reports/dmr_rx_proto.md): the prototype filters and decodes a
 * whole file, this one runs burst after burst on the radio's audio stream.
 * The demodulation is the prototype's improved one:
 *
 *  - fractional symbol timing per burst: 16 phases of a four-point Lagrange
 *    interpolation around the carried offset, chosen by
 *    sync correlation minus 0.35 x EVM (EVM alone on a burst that has no sync
 *    field, i.e. an EMB burst of a voice superframe);
 *  - a cheap spectral-aperture pre-gate before correlating: the sync field is
 *    all outer levels, so a window whose magnitudes are spread over the four
 *    levels is not a sync.  Two passes over 24 samples reject noise, which is
 *    what replaces the payload-mix-dependent level test of the first port;
 *  - per-burst levels: DC from the median, then a 4-means (outer and inner =
 *    outer/3) with a linear drift term, plus the sync field as the cleanest
 *    estimate in the slot.  Unlike a mean over the slot, this does not depend
 *    on the payload mix;
 *  - polarity from the channel structure: the 20 bits flanking the sync are a
 *    Golay(20,8) word (slot type or EMB) and only one polarity makes them a
 *    valid codeword, so an inverted discriminator is detected instead of
 *    decoded as the other sync word;
 *  - quality gate on EVM, CACH corrections and aperture;
 *  - conservative gap filling: the five bursts of a voice superframe that carry
 *    embedded signalling instead of a sync word are coasted on the slot grid,
 *    and only where the Golay(20,8) field, the payload and the eye all agree
 *    that a burst is there: a slot the call does not use, or one that carries
 *    another channel, is left as a hole.
 *
 * Three things stay specific to the firmware, all of them because it sees the
 * audio 30 ms at a time:
 *  - the matched filter runs at 24 kHz (five samples per symbol) instead of
 *    48 kHz, which halves its cost; the Lagrange interpolation of the
 *    decimated stream provides the fractional timing;
 *  - the fractional offset is carried from burst to burst, so the symbol clock
 *    of the capture is followed instead of re-acquired (the prototype
 *    re-acquires it per burst around a fixed phase and drifts from 0.02 to
 *    0.39 EVM over the 34 s capture).  The pre-gate re-measures the instant on
 *    every candidate, so a burst whose timing has drifted is still acquired;
 *  - once a burst is decoded the next sync can only be a whole number of slots
 *    away, so the scan looks there first (20 times less CPU) and falls back to
 *    a free search after twelve empty slots.  That fallback is what lets the
 *    receiver pick up a second, unsynchronised transmitter: the sigidwiki
 *    capture carries a data channel 72 symbols off the voice slot grid.
 */

#include <math.h>
#include <string.h>

#include "protocols/DMR/DmrDefs.h"
#include "protocols/DMR/DmrFec.h"
#include "protocols/DMR/DmrPhy.h"

/** Matched filter: RRC alpha = 0.2, 8 symbols of span, unit energy. */
#define DMR_MF_TAPS 81
#define DMR_MF_CENTER 40
/**
 * Input history for the matched filter, at 48 kHz.  Power of two, and every
 * sample is written twice (at i and i + DMR_HIST_LEN) so that the 81 taps of
 * the filter are always contiguous in memory: the inner loop then has no ring
 * mask, which is what it costs to run the filter at 24 kHz on the M33.
 */
#define DMR_HIST_LEN 128
/** Matched filter output history, at 24 kHz: 205 symbols. Power of two. */
#define DMR_RING_LEN 1024
/** Symbols between the sync start and the end of the burst: 24 + 54. */
#define DMR_HEAD_SYMBOLS 78
/** Symbols before the sync that the burst needs: CACH 12 + payload 54. */
#define DMR_TAIL_SYMBOLS 66

/** Sync acceptance: sign errors and normalised correlation. */
#define DMR_SYNC_MAX_ERRORS 2
#define DMR_SYNC_MIN_SCORE 0.72f

/**
 * Aperture of the sync window: the fraction of the 24 symbols that sit at the
 * outer levels.  The sync field is all outer, so a real one gives ~1.0 while a
 * window of 4FSK payload gives ~0.5.  The gate is looser than it looks like it
 * should be because a weak burst (8 dB SNR, EVM 0.3) has three or four sync
 * symbols out of 24 that do not clear 0.55 of the mean deviation, and the
 * prototype's 0.85 acquisition gate sees them on a noisier window still.
 */
#define DMR_APERTURE_PREGATE 70
#define DMR_APERTURE_GATE 70
/** Level, relative to the mean deviation, above which a symbol is outer. */
#define DMR_APERTURE_LEVEL 0.55f

/**
 * A burst whose 4-level eye is closed past these values carries no usable bits
 * (the measured AMBE+2 Golay protection runs out around 5 % BER, EVM 0.4):
 * drop it, it is either noise or a spurious correlation of the sync.  A burst
 * found on the grid may be weaker than one the blind scan accepted (that is
 * why it was lost) and a coasted one has no sync to confirm it.
 */
#define DMR_MAX_EVM 0.42f
#define DMR_MAX_EVM_COASTED 0.50f
/** CACH corrections accepted: one bit, as in the prototype. */
#define DMR_MAX_CACH_FIXES 1

/**
 * Fractional timing search: 16 phases over half a symbol either side of the
 * carried offset.  Half a symbol is what makes the estimate unambiguous -- the
 * sync correlation peaks with a period of one symbol, so a range that wide
 * always contains the peak and never the one of the neighbouring symbol twice
 * -- and 16 phases are 1/16 of a symbol, the grid the prototype uses.
 */
#define DMR_TIMING_PHASES 16
#define DMR_TIMING_SPAN 5.0f
/**
 * The symbol timing drifts with the sample clock of the capture (5 ppm is
 * 0.24 samples of 48 kHz per second), so the fractional offset is carried from
 * burst to burst.  Past this many samples the coarse phase steps instead, to
 * keep the interpolation near the centre of a symbol where it is accurate.
 */
#define DMR_TIMING_LIMIT 4.0f

/**
 * Once a burst is decoded the receiver knows the 30 ms slot grid: the next
 * sync can only be a multiple of 144 symbols away, give or take the sample
 * clock.  Searching only there costs 20 times less and, more important, keeps
 * a spurious correlation in the middle of a burst from shadowing the real sync
 * of the next slot.  Grid slots without a burst before falling back to a free
 * search.
 */
#define DMR_GRID_TOLERANCE 3
#define DMR_GRID_MISSES 12
/**
 * Empty slots of the grid kept as candidates for a coasted decode.  A DMR
 * voice superframe is six bursts and only the first one carries a sync word;
 * the other five carry embedded signalling in its place, so a call being
 * received normally shows holes on the slot grid that are real voice bursts.
 * Coasting them is what turns "one burst in six" into a continuous 60 ms
 * stream.  Four are kept because the receiver does not know which of them the
 * call actually uses -- it does not see the future -- and the check on the
 * payload of each coasted slot throws away the ones that carry nothing.
 */
#define DMR_FILL_RUN 4

/** Clean syncs needed to adopt the inverted polarity, and the margin. */
#define DMR_POLARITY_VOTES 3
#define DMR_POLARITY_MARGIN 2

/** A burst this recently decoded means the receiver is synchronised. */
#define DMR_SYNC_TIMEOUT_MS 300

/*
 * Root-raised-cosine impulse response, alpha 0.2, 10 samples per symbol,
 * 8 symbols of span, normalised to unit energy.  The host prototype computes
 * it at start-up from its closed form (work/dmr-rx/src/dsp.cpp, design_rrc);
 * here it is a table so that the firmware needs no transcendental at boot.
 * Regenerate with scripts/gen_dmr_rrc.py.
 */
static const float dmr_rrc[DMR_MF_TAPS] = {
    8.656555859e-03f,  6.031442977e-03f,  2.235020933e-03f,  -2.384808633e-03f,
    -7.330730141e-03f, -1.200305434e-02f, -1.576462128e-02f, -1.801719139e-02f,
    -1.828097858e-02f, -1.626786627e-02f, -1.193878807e-02f, -5.536830589e-03f,
    2.410222124e-03f,  1.112111611e-02f,  1.962566710e-02f,  2.685826042e-02f,
    3.177228436e-02f,  3.346444934e-02f,  3.129596224e-02f,  2.499686974e-02f,
    1.474070833e-02f,  1.178894526e-03f,  -1.457211399e-02f, -3.099328423e-02f,
    -4.627240185e-02f, -5.845259980e-02f, -6.560768581e-02f, -6.602845603e-02f,
    -5.840218720e-02f, -4.196726040e-02f, -1.662647831e-02f, 1.699400444e-02f,
    5.754840415e-02f,  1.030401309e-01f,  1.509519069e-01f,  1.984273216e-01f,
    2.424887347e-01f,  2.802725793e-01f,  3.092610513e-01f,  3.274891331e-01f,
    3.337079221e-01f,  3.274891331e-01f,  3.092610513e-01f,  2.802725793e-01f,
    2.424887347e-01f,  1.984273216e-01f,  1.509519069e-01f,  1.030401309e-01f,
    5.754840415e-02f,  1.699400444e-02f,  -1.662647831e-02f, -4.196726040e-02f,
    -5.840218720e-02f, -6.602845603e-02f, -6.560768581e-02f, -5.845259980e-02f,
    -4.627240185e-02f, -3.099328423e-02f, -1.457211399e-02f, 1.178894526e-03f,
    1.474070833e-02f,  2.499686974e-02f,  3.129596224e-02f,  3.346444934e-02f,
    3.177228436e-02f,  2.685826042e-02f,  1.962566710e-02f,  1.112111611e-02f,
    2.410222124e-03f,  -5.536830589e-03f, -1.193878807e-02f, -1.626786627e-02f,
    -1.828097858e-02f, -1.801719139e-02f, -1.576462128e-02f, -1.200305434e-02f,
    -7.330730141e-03f, -2.384808633e-03f, 2.235020933e-03f,  6.031442977e-03f,
    8.656555859e-03f,
};

/** Four-level model of one slot. */
struct dmrLevels {
    float dc;    ///< Residual carrier offset
    float outer; ///< Outer deviation: the symbols sit at +-outer, +-outer/3
    float inner; ///< Inner deviation, always outer / 3 in DMR
    float slope; ///< Residual carrier drift across the slot, per symbol
};

/** A sync found in the stream, before it is decoded. */
struct dmrCandidate {
    long symbol;       ///< Symbol index of the first sync symbol
    enum dmrSync sync; ///< Sync word that matched
    int errors;        ///< Sign errors of the match
    float score;       ///< Normalised correlation of the match
    float offset;      ///< Sampling offset the timing search starts from
    float span;        ///< Half span of that search, in 48 kHz samples
    bool relaxed;      ///< Predicted position of the slot grid
    bool coasted;      ///< Predicted position with no sync word at all
};

struct dmrPhy {
    float hist[2 * DMR_HIST_LEN]; ///< Input at 48 kHz, mirrored
    float ring[DMR_RING_LEN]; ///< Matched filter at 24 kHz, circular
    float slot[DMR_SLOT_SYMBOLS]; ///< One slot of symbols, for the level fit
    long inIndex;             ///< Absolute index of the newest 48 kHz sample
    long ringIndex;           ///< Absolute index of the newest 24 kHz sample
    long scan;                ///< Next symbol index to test for a sync
    long lastBurstSample;     ///< 48 kHz sample index of the last burst
    long grid;                ///< Symbol of the last burst, -1 without grid
    int phase;                ///< Coarse phase, in 48 kHz samples (even)
    float timing;             ///< Fractional offset, 48 kHz samples
    unsigned gridMisses;      ///< Consecutive grid slots without a burst
    enum dmrSync chainSync;   ///< Sync word of the decoded chain
    int polarity;             ///< 0 normal (or undecided), -1 inverted
    unsigned votesNormal;     ///< Clean bursts that read as normal polarity
    unsigned votesInverted;   ///< ... and as inverted
    struct dmrBurst held[DMR_FILL_RUN]; ///< Slots coasted on the grid, oldest
    unsigned heldCount;                 ///< first, held until the run closes
    long lastSymbol;     ///< Symbol of the last burst of the grid
    long lastTarget;     ///< ... and the grid slot it was found in
    long origin;         ///< First burst of the chain: a call repeats every
                         ///< 60 ms from it, not every 30 ms slot
};

static struct dmrPhy phy;

static const enum dmrSync syncCandidates[4] = {
    DMR_SYNC_BS_VOICE, DMR_SYNC_BS_DATA, DMR_SYNC_MS_VOICE, DMR_SYNC_MS_DATA
};

/** The four sync words as 24 symbol values, expanded once. */
static int8_t syncPattern[4][DMR_SYNC_SYMBOLS];

/** @return the symbol pattern of a sync word, or NULL if it is not a sync. */
static const int8_t *sync_pattern(enum dmrSync sync)
{
    for (size_t i = 0; i < 4; i++) {
        if (syncCandidates[i] == sync)
            return syncPattern[i];
    }

    return NULL;
}

void dmrPhy_init(void)
{
    memset(&phy, 0, sizeof(phy));
    for (size_t i = 0; i < 4; i++)
        dmr_sync_symbols(syncCandidates[i], syncPattern[i]);
    phy.inIndex = -1;
    phy.ringIndex = -1;
    phy.lastBurstSample = -1000000;
    phy.grid = -1;
    phy.chainSync = DMR_SYNC_BS_VOICE;
}

void dmrPhy_reset(void)
{
    memset(phy.hist, 0, sizeof(phy.hist));
    memset(phy.ring, 0, sizeof(phy.ring));
    phy.scan = 0;
    phy.phase = 0;
    phy.timing = 0.0f;
    phy.grid = -1;
    phy.gridMisses = 0;
    phy.chainSync = DMR_SYNC_BS_VOICE;
    phy.polarity = 0;
    phy.votesNormal = 0;
    phy.votesInverted = 0;
    phy.heldCount = 0;
    phy.lastSymbol = 0;
    phy.lastTarget = 0;
    phy.origin = 0;
}

int dmrPhy_phase(void)
{
    return phy.phase;
}

float dmrPhy_timing(void)
{
    return phy.timing;
}

bool dmrPhy_synced(void)
{
    long elapsed = phy.inIndex - phy.lastBurstSample;

    return elapsed < ((long)DMR_SYNC_TIMEOUT_MS * 48);
}

/**
 * Matched filter output at a fractional 24 kHz index: the four-point Lagrange
 * interpolator through the samples around it.  It has no ripple of its own
 * over the fractional range it is used on, which is what the 4FSK eye needs.
 */
static float mf_at(float pos)
{
    long i = (long)pos;
    float f = pos - (float)i;
    long oldest = phy.ringIndex - DMR_RING_LEN + 4;

    if ((i < oldest + 1) || (i > phy.ringIndex - 2))
        return 0.0f;

    float x0 = phy.ring[(unsigned)i & (DMR_RING_LEN - 1)];
    if (f == 0.0f)
        return x0;

    float xm1 = phy.ring[(unsigned)(i - 1) & (DMR_RING_LEN - 1)];
    float x1 = phy.ring[(unsigned)(i + 1) & (DMR_RING_LEN - 1)];
    float x2 = phy.ring[(unsigned)(i + 2) & (DMR_RING_LEN - 1)];

    /* Lagrange through f = -1, 0, 1, 2, in Horner form */
    float a = (-xm1 + 3.0f * x0 - 3.0f * x1 + x2) / 6.0f;
    float b = (xm1 - 2.0f * x0 + x1) / 2.0f;
    float c = (-2.0f * xm1 - 3.0f * x0 + 6.0f * x1 - x2) / 6.0f;

    return ((a * f + b) * f + c) * f + x0;
}

/**
 * Symbol k at a sampling phase, displaced by d 48 kHz samples: the ring holds
 * five samples per symbol, so each 48 kHz sample is half a ring sample.
 */
static float symbol_at(long k, int phase, float d)
{
    return mf_at(5.0f * (float)k + 0.5f * (float)phase + 0.5f * d);
}

/** The 144 symbols of the slot whose sync starts at symbol k. */
static void slot_window(long k, float d)
{
    for (int i = 0; i < DMR_SLOT_SYMBOLS; i++)
        phy.slot[i] = symbol_at(k - DMR_TAIL_SYMBOLS + i, phy.phase, d);
}

/** Dibit of a symbol: +3 -> 3, +1 -> 2, -1 -> 0, -3 -> 1 (ETSI mapping). */
static int level_to_dibit(float y, float thresholdMid, float thresholdOuter)
{
    if (y > thresholdOuter)
        return 3;
    if (y > thresholdMid)
        return 2;
    if (y > -thresholdOuter)
        return 0;
    return 1;
}

/** Nominal level of a dibit, for the EVM: 0 and 2 are the inner levels. */
static float nominal_level(int dibit, float inner, float outer)
{
    float magnitude = ((dibit == 0) || (dibit == 2)) ? inner : outer;

    return (dibit < 2) ? -magnitude : magnitude;
}

/**
 * Median of n samples through a 64-bin histogram: two passes and no sorting
 * buffer.  The four 4FSK levels are well separated, so this is exact to a
 * fraction of a bin and it is only the starting point of the 4-means below.
 */
static float sample_median(const float *y, int n)
{
    float lo = y[0];
    float hi = y[0];

    for (int i = 1; i < n; i++) {
        if (y[i] < lo)
            lo = y[i];
        if (y[i] > hi)
            hi = y[i];
    }

    if ((hi - lo) < 1e-6f)
        return 0.5f * (lo + hi);

    uint16_t hist[64];
    memset(hist, 0, sizeof(hist));

    const float scale = 64.0f / (hi - lo);
    for (int i = 0; i < n; i++) {
        int bin = (int)((y[i] - lo) * scale);
        if (bin < 0)
            bin = 0;
        if (bin > 63)
            bin = 63;
        hist[bin]++;
    }

    const int half = n / 2;
    int acc = 0;
    int bin = 0;
    while ((bin < 63) && ((acc + hist[bin]) <= half)) {
        acc += hist[bin];
        bin++;
    }

    float frac = 0.5f;
    if (hist[bin] > 1)
        frac = ((float)(half - acc) + 0.5f) / (float)hist[bin];

    return lo + ((float)bin + frac) / scale;
}

/**
 * Four-level model of a slot.
 *
 * The 144 symbols carry CACH + 132 burst symbols.  The levels are fitted with
 * a symmetric 4-means: every symbol is assigned to +outer / +inner / -inner /
 * -outer and each level is re-estimated from its own group, so the estimate
 * does not depend on the payload mix (a mean over the slot does) and a
 * residual carrier offset is removed together with the levels.  A linear term
 * absorbs the carrier drift over the 30 ms slot.
 *
 * @param fit: false for the cheap model used inside the timing search, true
 *             for the model the burst is decoded with.
 */
static void estimate_levels(int n, int syncOffset, bool fit,
                            struct dmrLevels *levels)
{
    const float centre = 0.5f * (float)n;
    float dc = sample_median(phy.slot, n);
    float acc = 0.0f;

    for (int i = 0; i < n; i++)
        acc += fabsf(phy.slot[i] - dc);

    /* uniform 4FSK (+-A, +-A/3): E|y-dc| = 2A/3, so A = 1.5 * mean|y-dc| */
    float outer = 1.5f * acc / (float)n;
    if (outer < 1e-6f)
        outer = 1e-6f;
    float inner = outer / 3.0f;
    float slope = 0.0f;

    /*
     * A DMR sync word is all outer levels and sign balanced, so the 24 symbols
     * at the sync position give an outer level and a DC that do not depend on
     * the payload mix at all -- the cleanest estimate in the slot.  Bursts B..F
     * of a voice superframe carry embedded signalling there instead; that is
     * detected (the field is not all-outer) and the whole-slot estimate kept.
     */
    if ((syncOffset >= 0) && ((syncOffset + DMR_SYNC_SYMBOLS) <= n)) {
        float c = 0.0f;
        float a = 0.0f;

        for (int i = 0; i < DMR_SYNC_SYMBOLS; i++)
            c += phy.slot[syncOffset + i];
        c /= (float)DMR_SYNC_SYMBOLS;

        for (int i = 0; i < DMR_SYNC_SYMBOLS; i++)
            a += fabsf(phy.slot[syncOffset + i] - c);
        a /= (float)DMR_SYNC_SYMBOLS;

        if ((a > 0.8f * outer) && (a < 1.25f * outer)) {
            dc = c;
            outer = a;
            inner = a / 3.0f;
        }
    }

    for (int it = 0; fit && (it < 3); it++) {
        const float mid = 0.5f * (outer + inner);
        float residual = 0.0f;

        for (int i = 0; i < n; i++) {
            float v = phy.slot[i] - (dc + slope * ((float)i - centre));
            float av = fabsf(v);
            float nom = (av > mid) ? outer : inner;

            residual += v - ((v > 0.0f) ? nom : -nom);
        }
        /* with the sync fixing the levels only a slow DC drift is left */
        dc += 0.5f * residual / (float)n;

        /* least-squares drift of the residuals, clamped to a fraction of a
         * level over the slot */
        float num = 0.0f;
        float den = 0.0f;

        for (int i = 0; i < n; i++) {
            float t = (float)i - centre;
            float v = phy.slot[i] - dc;
            float av = fabsf(v);
            float nom = (av > mid) ? outer : inner;

            num += t * (v - ((v > 0.0f) ? nom : -nom));
            den += t * t;
        }

        float lim = 0.15f * outer / (float)n;
        slope = (den > 1e-6f) ? (num / den) : 0.0f;
        if (slope > lim)
            slope = lim;
        if (slope < -lim)
            slope = -lim;
    }

    if (fit) {
        /* outer level from every symbol the slicer puts at the outside, which
         * keeps it unbiased and cuts the noise of the 24-symbol estimate */
        const float mid = 0.5f * (outer + inner);
        float sum = 0.0f;
        int count = 0;

        for (int i = 0; i < n; i++) {
            float v = phy.slot[i] - (dc + slope * ((float)i - centre));
            float av = fabsf(v);

            if (av > mid) {
                sum += av;
                count++;
            }
        }
        if (count >= 24)
            outer = sum / (float)count;
    }

    levels->dc = dc;
    levels->outer = outer;
    levels->inner = outer / 3.0f;
    levels->slope = slope;
}

/**
 * RMS distance of the 132 burst symbols to the four levels, relative to the
 * outer level: the eye opening of the burst.
 */
static float payload_evm(const struct dmrLevels *levels)
{
    const float thresholdMid = 0.5f * levels->inner;
    const float thresholdOuter = 0.5f * (levels->inner + levels->outer);
    float acc = 0.0f;

    for (int i = DMR_CACH_SYMBOLS; i < DMR_SLOT_SYMBOLS; i++) {
        float v = phy.slot[i]
                - (levels->dc + levels->slope * ((float)i - 72.0f));
        int dibit = level_to_dibit(v, thresholdMid, thresholdOuter);
        float error = v - nominal_level(dibit, levels->inner, levels->outer);

        acc += error * error;
    }

    return sqrtf(acc / (float)DMR_BURST_SYMBOLS) / levels->outer;
}

/**
 * Normalised correlation of a 24-symbol window with a sync pattern, with the
 * sign errors and the aperture (the fraction of the window that sits at the
 * outer levels) of that window.
 */
static float match_window(const float *v, const int8_t *pattern, int *errors,
                          float *aperture)
{
    float sum = 0.0f;

    for (int i = 0; i < DMR_SYNC_SYMBOLS; i++)
        sum += v[i];
    const float centre = sum / (float)DMR_SYNC_SYMBOLS;

    float numerator = 0.0f;
    float denominator = 0.0f;
    float deviation = 0.0f;

    for (int i = 0; i < DMR_SYNC_SYMBOLS; i++) {
        float y = v[i] - centre;
        float a = fabsf(y);

        numerator += y * (float)pattern[i];
        denominator += 3.0f * a; /* |pattern[i]| is 3 */
        deviation += a;
    }
    deviation /= (float)DMR_SYNC_SYMBOLS;

    int outer = 0;
    int signErrors = 0;

    for (int i = 0; i < DMR_SYNC_SYMBOLS; i++) {
        if (fabsf(v[i] - centre) > (DMR_APERTURE_LEVEL * deviation))
            outer++;
        if ((v[i] - centre > 0.0f) != (pattern[i] > 0))
            signErrors++;
    }

    *errors = signErrors;
    *aperture = (deviation > 1e-9f)
              ? (float)outer / (float)DMR_SYNC_SYMBOLS : 0.0f;

    return (denominator > 1e-12f) ? (numerator / denominator) : -1.0f;
}

/**
 * Cheap pre-gate before correlating, and the coarse sampling instant it picks.
 *
 * The sync field is all outer levels, so a window whose magnitudes are spread
 * over the four levels cannot be one.  Two passes over 24 samples, without
 * interpolation, reject noise before the 24 Lagrange evaluations and the four
 * correlations are paid for -- that is what replaces the payload-mix-dependent
 * level test of the first port.
 *
 * The same window is measured at five sampling instants a fifth of a symbol
 * apart, which is one full symbol period on the decimated stream.  The one
 * whose magnitudes are clearest is the coarse timing: it does not depend on the
 * fractional offset carried from the last burst, so a burst whose timing has
 * drifted is still found instead of being lost with the whole search window.
 *
 * @param offset: coarse sampling offset, in 48 kHz samples.
 * @return false if no instant looks like a sync field.
 */
static bool aperture_pregate(long k, float *offset)
{
    const long base = (long)lroundf(5.0f * (float)k
                                    + 0.5f * ((float)phy.phase + phy.timing));
    float best = -1.0f;
    long bestSkew = 0;

    for (int skew = -2; skew <= 2; skew++) {
        float v[DMR_SYNC_SYMBOLS];
        float sum = 0.0f;

        for (int i = 0; i < DMR_SYNC_SYMBOLS; i++) {
            v[i] = phy.ring[(unsigned)(base + skew + 5 * i)
                            & (DMR_RING_LEN - 1)];
            sum += v[i];
        }

        const float centre = sum / (float)DMR_SYNC_SYMBOLS;
        float deviation = 0.0f;

        for (int i = 0; i < DMR_SYNC_SYMBOLS; i++)
            deviation += fabsf(v[i] - centre);
        deviation /= (float)DMR_SYNC_SYMBOLS;

        if (deviation < 1e-6f)
            continue;

        int outer = 0;
        for (int i = 0; i < DMR_SYNC_SYMBOLS; i++) {
            if (fabsf(v[i] - centre) > (DMR_APERTURE_LEVEL * deviation))
                outer++;
        }

        if (((outer * 100) >= (DMR_APERTURE_PREGATE * DMR_SYNC_SYMBOLS))
            && (deviation > best)) {
            best = deviation;
            bestSkew = skew;
        }
    }

    if (best < 0.0f)
        return false;

    /* one sample of the 24 kHz stream is two of the 48 kHz one */
    *offset = phy.timing + 2.0f * (float)bestSkew;

    return true;
}

/**
 * Test a sync at symbol k.
 *
 * @return the best candidate, or DMR_SYNC_NONE.
 */
static enum dmrSync find_sync(long k, float offset, int *signErrors,
                              float *score)
{
    float v[DMR_SYNC_SYMBOLS];
    enum dmrSync best = DMR_SYNC_NONE;
    int bestErrors = DMR_SYNC_MAX_ERRORS + 1;
    float bestScore = -2.0f;

    for (int i = 0; i < DMR_SYNC_SYMBOLS; i++)
        v[i] = symbol_at(k + i, phy.phase, offset);

    for (size_t c = 0; c < 4; c++) {
        int errors;
        float aperture;

        float s = match_window(v, syncPattern[c], &errors, &aperture);

        if ((errors < bestErrors)
            || ((errors == bestErrors) && (s > bestScore))) {
            bestErrors = errors;
            bestScore = s;
            best = syncCandidates[c];
        }
    }

    *signErrors = bestErrors;
    *score = bestScore;

    if ((bestErrors > DMR_SYNC_MAX_ERRORS) || (bestScore < DMR_SYNC_MIN_SCORE))
        return DMR_SYNC_NONE;

    return best;
}

/** Normalised correlation of the sync field of the slot in phy.slot. */
static float sync_correlation(const struct dmrLevels *levels,
                              const int8_t *pattern)
{
    float numerator = 0.0f;
    float denominator = 0.0f;

    for (int s = 0; s < DMR_SYNC_SYMBOLS; s++) {
        float v = phy.slot[DMR_TAIL_SYMBOLS + s] - levels->dc;

        numerator += v * (float)pattern[s];
        denominator += 3.0f * fabsf(v);
    }

    return (denominator > 1e-12f) ? (numerator / denominator) : -1.0f;
}

/**
 * Fractional symbol timing of one burst, in 48 kHz samples.
 *
 * The 2-level sync correlates well even half a symbol off, so it cannot place
 * the sampling instant on its own; the 4-level payload can but its eye is
 * noisy when it is closed.  The prototype's metric is used: the sync
 * correlation with the payload eye as a tie-break, and the eye alone on a
 * burst that has no sync field (the EMB of a voice superframe).
 */
static float refine_timing(long k, enum dmrSync sync, bool coasted, float centre,
                           float span, float *evmOut, float *scoreOut)
{
    const int8_t *pattern = sync_pattern(sync);
    struct dmrLevels levels;
    const float step = 2.0f * span / (float)DMR_TIMING_PHASES;
    float bestOffset = centre;
    float bestMetric = -1e9f;
    float bestEvm = 9.0f;
    float bestScore = -2.0f;

    for (int i = 0; i <= DMR_TIMING_PHASES; i++) {
        float d = centre - span + step * (float)i;

        slot_window(k, d);
        estimate_levels(DMR_SLOT_SYMBOLS, DMR_TAIL_SYMBOLS, false, &levels);

        float score = sync_correlation(&levels, pattern);
        float evm = payload_evm(&levels);
        float metric = coasted ? (-evm) : (score - 0.35f * evm);

        if (metric > bestMetric) {
            bestMetric = metric;
            bestOffset = d;
            bestEvm = evm;
            bestScore = score;
        }
    }

    *evmOut = bestEvm;
    *scoreOut = bestScore;

    return bestOffset;
}

/** The complementary sync word: an inverted discriminator flips every dibit. */
static enum dmrSync swap_voice_data(enum dmrSync sync)
{
    switch (sync) {
        case DMR_SYNC_BS_VOICE:
            return DMR_SYNC_BS_DATA;
        case DMR_SYNC_BS_DATA:
            return DMR_SYNC_BS_VOICE;
        case DMR_SYNC_MS_VOICE:
            return DMR_SYNC_MS_DATA;
        case DMR_SYNC_MS_DATA:
            return DMR_SYNC_MS_VOICE;
        default:
            return DMR_SYNC_NONE;
    }
}

/** The 20 bits flanking the sync (slot type, or EMB on a voice burst). */
static uint32_t slot_codeword(const uint8_t bits[DMR_BURST_BITS])
{
    uint32_t codeword = 0;

    for (int i = 0; i < 10; i++)
        codeword = (codeword << 1) | (uint32_t)(bits[98 + i] & 1u);
    for (int i = 0; i < 10; i++)
        codeword = (codeword << 1) | (uint32_t)(bits[156 + i] & 1u);

    return codeword;
}

/**
 * Polarity of the channel.
 *
 * Voice and data syncs are dibit complements of each other, so "which sync
 * matched" alone cannot tell a voice burst from a data burst read upside down.
 * The 20 bits flanking the sync are a Golay(20,8) word, and only one polarity
 * makes them a valid codeword.  The prototype votes over the whole file; here
 * the votes accumulate and either polarity is latched with a clear majority,
 * which is as sticky as it is on the air: the discriminator of a radio does
 * not change sign in the middle of a transmission.  Latching after a handful
 * of bursts also keeps the search from running twice per burst forever.
 *
 * @param normal: distance of the word as received, already decoded.
 */
static void vote_polarity(uint32_t codeword, int normal)
{
    uint8_t field;
    int inverted = dmr_fec_golay_decode(~codeword & 0xFFFFFu, &field);

    if ((normal + 2) < inverted)
        phy.votesNormal++;
    else if ((inverted + 2) < normal)
        phy.votesInverted++;

    if ((phy.votesInverted >= DMR_POLARITY_VOTES)
        && (phy.votesInverted > (DMR_POLARITY_MARGIN * phy.votesNormal)))
        phy.polarity = -1;
    else if ((phy.votesNormal >= DMR_POLARITY_VOTES)
             && (phy.votesNormal > (DMR_POLARITY_MARGIN * phy.votesInverted)))
        phy.polarity = 1;
}

/**
 * Decode the burst whose sync starts at c->symbol: fractional timing, levels,
 * polarity, bits, FEC and the quality gate.
 *
 * @return true if the burst passed the gate and out is filled.
 */
static bool decode_burst(const struct dmrCandidate *c, struct dmrBurst *out,
                         float *offsetOut)
{
    struct dmrLevels levels;
    float evm;
    float score;

    const float d = refine_timing(c->symbol, c->sync, c->coasted, c->offset,
                                  c->span, &evm, &score);

    /* sample the slot at the chosen instant and refit the levels */
    slot_window(c->symbol, d);
    estimate_levels(DMR_SLOT_SYMBOLS, DMR_TAIL_SYMBOLS, true, &levels);
    if (levels.outer < 1e-6f)
        return false;
    evm = payload_evm(&levels);
    *offsetOut = d;

    memset(out, 0, sizeof(*out));
    out->symbol = c->symbol;
    out->score = c->score;
    out->evm = evm;
    out->relaxed = c->relaxed;
    out->coasted = c->coasted;

    const float thresholdMid = 0.5f * levels.inner;
    const float thresholdOuter = 0.5f * (levels.inner + levels.outer);
    uint8_t bits[DMR_BURST_BITS];

    for (int i = 0; i < DMR_BURST_SYMBOLS; i++) {
        int index = DMR_CACH_SYMBOLS + i;
        float v = phy.slot[index]
                - (levels.dc + levels.slope * ((float)index - 72.0f));
        int dibit = level_to_dibit(v, thresholdMid, thresholdOuter);

        bits[2 * i] = (uint8_t)((dibit >> 1) & 1);
        bits[2 * i + 1] = (uint8_t)(dibit & 1);
    }

    /* CACH, 12 symbols before the payload */
    uint8_t cach[DMR_CACH_BITS];

    for (int i = 0; i < DMR_CACH_SYMBOLS; i++) {
        float v = phy.slot[i] - (levels.dc + levels.slope * ((float)i - 72.0f));
        int dibit = level_to_dibit(v, thresholdMid, thresholdOuter);

        cach[2 * i] = (uint8_t)((dibit >> 1) & 1);
        cach[2 * i + 1] = (uint8_t)(dibit & 1);
    }

    /* sync errors and aperture at the refined instant */
    int signErrors;
    float aperture;

    (void)match_window(&phy.slot[DMR_TAIL_SYMBOLS], sync_pattern(c->sync),
                       &signErrors, &aperture);
    out->syncErrors = signErrors;
    out->aperture = aperture;

    /* ---- polarity ---------------------------------------------------- */
    if ((!c->coasted) && (phy.polarity == 0)
        && (signErrors <= DMR_SYNC_MAX_ERRORS) && (c->score >= 0.85f)) {
        const uint32_t raw = slot_codeword(bits);

        vote_polarity(raw, dmr_fec_golay_decode(raw, NULL));
    }

    const bool inverted = (phy.polarity < 0);
    out->sync = inverted ? swap_voice_data(c->sync) : c->sync;
    out->voice = dmr_sync_is_voice(out->sync);

    if (inverted) {
        for (int i = 0; i < DMR_BURST_BITS; i++)
            bits[i] ^= 1u;
        for (int i = 0; i < DMR_CACH_BITS; i++)
            cach[i] ^= 1u;
    }

    /* ---- CACH -------------------------------------------------------- */
    int fixes = dmr_fec_cach_decode(cach);
    out->cachValid = fixes >= 0;
    out->accessType = cach[dmr_cach_tact_pos[0]];
    out->timeslot2 = cach[dmr_cach_tact_pos[1]] != 0;
    out->lateEntry = cach[dmr_cach_tact_pos[2]] != 0;

    /* ---- slot type (data) or EMB (voice) ----------------------------- */
    /*
     * The 20 bits flanking the sync are a Golay(20,8) codeword on every burst,
     * data or voice, so a distance of two or less is a strong statement that
     * what was sampled is a burst and not noise: random bits get there with
     * probability 4e-4.  That is what lets a coasted burst -- one decoded on
     * the grid without a sync word to confirm it -- be trusted.
     */
    uint8_t field = 0;
    int distance = dmr_fec_golay_decode(slot_codeword(bits), &field);
    out->slotTypeValid = distance <= 2;
    out->colourCode = (uint8_t)((field >> 4) & 0x0F);
    out->dataType = (uint8_t)(field & 0x0F);

    /* ---- quality gate ------------------------------------------------ */
    /*
     * A burst found at a position predicted by the slot grid may be weaker
     * than one the blind scan accepted (that is why it was lost), so its
     * limits are relaxed: the grid already proves a burst belongs there.  A
     * coasted burst has no sync word at all (it is the EMB of a voice
     * superframe), so the eye is the only structure left to check.
     */
    if (c->coasted) {
        /* No sync word to confirm the slot, so the eye, the Golay codeword and
         * the payload itself have to: a slot with no signal decodes to a
         * constant, its all-zero codeword passes the Golay check and its
         * all-zero frames would drag the vocoder's prediction to silence. */
        int ones = 0;

        for (int i = 0; i < DMR_HALF_BITS; i++) {
            ones += bits[i];
            ones += bits[2 * (54 + DMR_SYNC_SYMBOLS) + i];
        }

        if ((evm > DMR_MAX_EVM_COASTED) || (!out->voice)
            || (!out->slotTypeValid) || (ones < 24) || (ones > 192))
            return false;
    } else {
        if ((signErrors > DMR_SYNC_MAX_ERRORS) || (c->score < DMR_SYNC_MIN_SCORE)
            || (evm > DMR_MAX_EVM) || (fixes < 0)
            || (fixes > DMR_MAX_CACH_FIXES)
            || ((int)(aperture * 100.0f) < DMR_APERTURE_GATE))
            return false;
    }

    /* ---- payload ------------------------------------------------------ */
    if (out->voice) {
        /* 216 payload bits: 3 AMBE+2 frames, no further FEC outside them. */
        uint8_t payload[DMR_PAYLOAD_BITS];

        memcpy(payload, bits, DMR_HALF_BITS);
        memcpy(payload + DMR_HALF_BITS, bits + 2 * (54 + DMR_SYNC_SYMBOLS),
               DMR_HALF_BITS);

        out->frameCount = DMR_FRAMES_PER_BURST;
        for (int f = 0; f < DMR_FRAMES_PER_BURST; f++)
            dmr_fec_voice_deinterleave(payload + f * DMR_AMBE_BITS,
                                       out->frame[f]);
        return true;
    }

    /* Data burst: BPTC(196,96) over the payload, slot type next to the sync */
    uint8_t info[DMR_INFO_BITS];

    memcpy(info, bits, 98);
    memcpy(info + 98, bits + 166, 98);

    uint8_t data[96];
    (void)dmr_fec_bptc_decode(info, data);

    for (int i = 0; i < 96; i++) {
        if (data[i] != 0)
            out->lc[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
    }

    /*
     * CRC-16 over the 96-bit block, masked per data type (TS 102 361-1
     * annex B): a valid block leaves a zero residue.  This is the link
     * quality measurement of the receiver.
     */
    uint16_t mask = 0;

    switch (out->dataType) {
        case DMR_DT_CSBK:
            mask = 0xA5A5;
            break;
        case DMR_DT_DATA_HEADER:
            mask = 0xCCCC;
            break;
        case DMR_DT_PI_HEADER:
            mask = 0x6969;
            break;
        case DMR_DT_MBC_HEADER:
            mask = 0xAAAA;
            break;
        default:
            break;
    }

    if ((mask != 0) && out->slotTypeValid) {
        uint8_t block[96];

        memcpy(block, data, sizeof(block));
        for (int i = 0; i < 16; i++)
            block[80 + i] ^= (uint8_t)((mask >> (15 - i)) & 1u);

        out->crcChecked = true;
        out->crcOk = dmr_fec_crc16(block, 96) == 0;
    }

    return true;
}

/**
 * Carry the fractional timing to the next burst and step the coarse phase
 * when it has drifted out of the range the interpolation is accurate in.
 */
static void track_timing(float offset)
{
    phy.timing = offset;

    while (phy.timing > DMR_TIMING_LIMIT) {
        phy.phase = (phy.phase + 2) % 10;
        phy.timing -= 2.0f;
    }
    while (phy.timing < -DMR_TIMING_LIMIT) {
        phy.phase = (phy.phase + 8) % 10;
        phy.timing += 2.0f;
    }
}

/** Next slot boundary of the grid at or after the scan pointer. */
static long grid_target(long scan)
{
    long relative = scan - phy.grid;
    long slot = (relative + DMR_SLOT_SYMBOLS - 1 - DMR_GRID_TOLERANCE)
              / DMR_SLOT_SYMBOLS;

    if (slot < 1)
        slot = 1;
    return phy.grid + slot * DMR_SLOT_SYMBOLS;
}

/** Hand a burst to the mode and mark the receiver as synchronised. */
static void emit(const struct dmrBurst *burst, dmrBurstHandler handler,
                 void *user)
{
    if (handler != NULL) {
        handler(burst, user);
        phy.lastBurstSample = phy.inIndex;
    }
}

/**
 * Look for a sync word in [from, to] with the full acquisition path.
 *
 * @return true if a candidate was found, in best.
 */
static bool search_window(long from, long to, struct dmrCandidate *best)
{
    bool found = false;
    int bestErrors = DMR_SYNC_MAX_ERRORS + 1;
    float bestScore = -2.0f;

    for (long k = from; k <= to; k++) {
        float offset;

        if (!aperture_pregate(k, &offset))
            continue;

        int errors;
        float score;
        enum dmrSync sync = find_sync(k, offset, &errors, &score);

        if (sync == DMR_SYNC_NONE)
            continue;
        if ((errors > bestErrors)
            || ((errors == bestErrors) && (score <= bestScore)))
            continue;

        bestErrors = errors;
        bestScore = score;
        best->symbol = k;
        best->sync = sync;
        best->errors = errors;
        best->score = score;
        best->offset = offset;
        found = true;
    }

    if (found) {
        best->span = DMR_TIMING_SPAN;
        best->relaxed = false;
        best->coasted = false;
    }

    return found;
}

/**
 * @return true if the slot at @p target is one where the call can be: the
 * bursts of one call repeat every 60 ms (288 symbols) from the first burst of
 * the chain, so every other slot of the 30 ms grid is the other timeslot of
 * the TDMA frame and carries either nothing or another call.
 */
static bool is_chain_slot(long target)
{
    const long relative = target - phy.origin;

    return (relative >= 0) && ((relative % DMR_FRAME_SYMBOLS) == 0);
}

/**
 * Coast the slot of the grid at @p target: decode it without a sync word.
 *
 * A DMR voice superframe is six bursts and only the first one carries a sync
 * word: the other five carry the embedded signalling in that field.  They are
 * still voice, they are still on the slot grid, and the grid says where their
 * symbols are, so the level fit and the four-level slicer work on them with
 * the eye as the only criterion -- there is no correlation to align on.
 *
 * The burst is held, not emitted: whether a hole may be closed depends on the
 * length of the run of empty slots, and that is only known when the run ends.
 */
static void fill_slot(long target)
{
    if (phy.heldCount >= DMR_FILL_RUN)
        return;
    if (!dmr_sync_is_voice(phy.chainSync))
        return;

    struct dmrCandidate c;
    struct dmrBurst burst;
    float refined;

    /* the grid slot plus the local correction of the last burst found: over
     * one or two slots the symbol clock of the capture has not moved */
    c.symbol = target + (phy.lastSymbol - phy.lastTarget);
    c.sync = phy.chainSync;
    c.errors = 0;
    c.score = 0.0f;
    c.relaxed = true;
    c.coasted = true;
    c.offset = phy.timing;
    c.span = DMR_TIMING_SPAN;

    if (!decode_burst(&c, &burst, &refined))
        return;
    if (!burst.voice)
        return;

    phy.held[phy.heldCount++] = burst;
}

/** Emit the slots coasted on the grid, oldest first. */
static void emit_held(dmrBurstHandler handler, void *user)
{
    for (unsigned i = 0; i < phy.heldCount; i++)
        emit(&phy.held[i], handler, user);

    phy.heldCount = 0;
}

/**
 * Run the sync search over the symbols that are fully in the matched filter
 * history, and decode what it finds.
 */
static void scan(dmrBurstHandler handler, void *user)
{
    /*
     * Newest complete symbol: 5k + phase/2 <= ringIndex, and two samples of
     * the interpolation kernel must exist past it.
     */
    long newest = (phy.ringIndex - phy.phase / 2) / 5;
    long oldest = newest - (DMR_RING_LEN / 5) + 8;

    if (newest < DMR_HEAD_SYMBOLS)
        return;

    if (phy.scan < DMR_TAIL_SYMBOLS)
        phy.scan = DMR_TAIL_SYMBOLS;
    if (phy.scan < oldest + DMR_TAIL_SYMBOLS)
        phy.scan = oldest + DMR_TAIL_SYMBOLS;

    while (1) {
        struct dmrCandidate c;
        long target = -1;

        if (phy.grid >= 0) {
            target = grid_target(phy.scan);
            if (phy.scan < (target - DMR_GRID_TOLERANCE))
                phy.scan = target - DMR_GRID_TOLERANCE;

            /* The whole window has to be in the history before it is judged:
             * searching past the newest symbol would read zeros, find nothing
             * and count a slot the receiver never saw as empty. */
            if ((target + DMR_GRID_TOLERANCE + DMR_HEAD_SYMBOLS) > newest)
                return;

            if (!search_window(phy.scan, target + DMR_GRID_TOLERANCE, &c)) {
                /* Empty slot: a voice superframe has five of these for every
                 * sync word, so they are what the grid fill is for.  The grid
                 * is given up after a long run of them (a call that ended, or
                 * another channel on this slot). */
                if (is_chain_slot(target))
                    fill_slot(target);

                if (++phy.gridMisses >= DMR_GRID_MISSES) {
                    phy.grid = -1;
                    phy.heldCount = 0;
                }
                phy.scan = target + DMR_GRID_TOLERANCE + 1;
                continue;
            }
        } else {
            if ((phy.scan + DMR_HEAD_SYMBOLS) > newest)
                return;

            float offset;

            if (!aperture_pregate(phy.scan, &offset)) {
                phy.scan++;
                continue;
            }

            int errors;
            float score;
            enum dmrSync sync = find_sync(phy.scan, offset, &errors, &score);

            if (sync == DMR_SYNC_NONE) {
                phy.scan++;
                continue;
            }

            c.symbol = phy.scan;
            c.sync = sync;
            c.errors = errors;
            c.score = score;
            c.offset = offset;
            c.span = DMR_TIMING_SPAN;
            c.relaxed = false;
            c.coasted = false;
        }

        struct dmrBurst burst;
        float offset;
        if (!decode_burst(&c, &burst, &offset)) {
            phy.scan = c.symbol + 1;
            continue;
        }

        /* A burst was decoded: the slots of the grid that came up empty
         * before it are the EMB bursts of the same superframe, and are part of
         * the call.  A data burst means the run was not this voice call. */
        if (burst.voice)
            emit_held(handler, user);
        else
            phy.heldCount = 0;

        phy.lastSymbol = c.symbol;
        phy.lastTarget = (target >= 0) ? target : c.symbol;

        /*
         * Stay on the predicted slot, not on where the correlation peaked:
         * snapping to the prediction keeps the grid from creeping by the
         * couple of symbols a spurious peak can be off by.
         */
        if (phy.grid < 0)
            phy.origin = (target >= 0) ? target : c.symbol;

        phy.grid = (target >= 0) ? target : c.symbol;
        phy.gridMisses = 0;
        if (burst.voice)
            phy.chainSync = c.sync;
        phy.scan = c.symbol + DMR_BURST_SYMBOLS - 4;

        /* Follow the symbol clock of the capture */
        track_timing(offset);

        emit(&burst, handler, user);
    }
}

/** Matched filter at 24 kHz and symbol scan for one new input sample. */
static void push_sample(float sample, dmrBurstHandler handler, void *user)
{
    phy.inIndex++;
    const unsigned slot = (unsigned)phy.inIndex & (DMR_HIST_LEN - 1);
    phy.hist[slot] = sample;
    phy.hist[slot + DMR_HIST_LEN] = sample;

    if (phy.inIndex < DMR_MF_CENTER)
        return;

    long mfIndex = phy.inIndex - DMR_MF_CENTER;
    if ((mfIndex & 1) != 0)
        return;

    /*
     * The taps are contiguous from the oldest sample of the window; the
     * impulse response is symmetric, so the order they are read in does not
     * matter.  Two accumulators and four taps per iteration keep the FPU fed:
     * this loop is two thirds of the cost of the whole demodulator.
     */
    const float *x = &phy.hist[(unsigned)(phy.inIndex - DMR_MF_TAPS + 1)
                               & (DMR_HIST_LEN - 1)];
    float acc0 = 0.0f;
    float acc1 = 0.0f;

    for (int t = 0; t < (DMR_MF_TAPS - 1); t += 4) {
        acc0 += dmr_rrc[t] * x[t] + dmr_rrc[t + 2] * x[t + 2];
        acc1 += dmr_rrc[t + 1] * x[t + 1] + dmr_rrc[t + 3] * x[t + 3];
    }
    acc0 += dmr_rrc[DMR_MF_TAPS - 1] * x[DMR_MF_TAPS - 1];

    phy.ringIndex++;
    phy.ring[(unsigned)phy.ringIndex & (DMR_RING_LEN - 1)] = acc0 + acc1;

    scan(handler, user);
}

void dmrPhy_push(const int16_t *samples, size_t count, dmrBurstHandler handler,
                 void *user)
{
    for (size_t i = 0; i < count; i++)
        push_sample((float)samples[i], handler, user);
}
