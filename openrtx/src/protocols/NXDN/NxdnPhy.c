/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <string.h>

#include "protocols/NXDN/NxdnPhy.h"

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

/** Matched filter: 81 taps, 10 per symbol at 24 kHz, 8 symbols of span. */
#define NXDN_MF_TAPS 81
#define NXDN_MF_CENTER 40

/** 48 kHz history: 128 doubles up so the filter taps are contiguous. */
#define NXDN_HIST_LEN 128

/**
 * Samples per symbol on the 24 kHz matched filter output stream: 24000 / 2400.
 * This is the one number that separates NXDN48 from NXDN96 (which runs the
 * same 192-symbol frame at twice the symbol rate, 4800 symbols/s).
 */
#define NXDN_SPS 10

/** Coarse sampling phase range, in 48 kHz samples: one symbol period. */
#define NXDN_PHASE_SPAN (2 * NXDN_SPS)

/** 24 kHz ring: 2048 samples = 204 symbols, 12 more than a frame needs. */
#define NXDN_RING_LEN 2048

/**
 * Samples of slack the guard keeps at the end of the ring: mf_at() needs two
 * samples of lookahead, and a frame is only decoded when all 192 of its
 * symbols are in the history, so the scan sits NXDN_FRAME_SYMBOLS behind the
 * newest sample and the ring (2048 samples) must still hold the whole frame.
 */
#define NXDN_RING_MARGIN 8

/**
 * Sync acceptance: bit errors out of the 20 bits of the 20-symbol sync field
 * (10 symbols, two bits each), and the distance of the window to the ideal
 * waveform.  A magnitude-blind sign correlation accepts any run of outer
 * symbols with the right signs -- 6.8 % of the positions of a real capture --
 * so the detector compares whole dibits, as the reference decoders do.
 */
#define NXDN_SYNC_MAX_ERRORS 1
#define NXDN_SYNC_MIN_SCORE 0.72f

/** Pre-gate: share of the sync window that must sit at the outer levels. */
#define NXDN_APERTURE_PREGATE 60
#define NXDN_APERTURE_LEVEL 0.55f

/** Quality gate of a frame. */
#define NXDN_MAX_EVM 0.42f

/** Fractional timing search: phases and span in 48 kHz samples. */
#define NXDN_TIMING_PHASES 16
#define NXDN_TIMING_SPAN 5.0f
#define NXDN_TIMING_LIMIT 10.0f

/** Symbols of the timing window: the sync and the head of the SACCH. */
#define NXDN_TIMING_WINDOW 64

/** Frame grid: positions searched around the prediction, misses to drop it. */
#define NXDN_GRID_TOLERANCE 3
#define NXDN_GRID_MISSES 8

/** A frame decoded less than this long ago keeps the mode "synced". */
#define NXDN_SYNC_TIMEOUT_MS 300

/*
 * Root-raised-cosine impulse response, alpha 0.2, 10 samples per symbol,
 * 8 symbols of span, normalised to unit energy -- the same table the DMR mode
 * uses (same symbol rate, same pulse shape).  Computed once from its closed
 * form; here it is a table so that the firmware needs no transcendental at
 * boot.
 */
static const float nxdn_rrc[NXDN_MF_TAPS] = {
    8.656555859e-03f,  6.031442977e-03f,  2.235020933e-03f,  -2.384808633e-03f,
    -7.330730141e-03f, -1.200305434e-02f, -1.576462128e-02f, -1.801719139e-02f,
    -1.828097858e-02f, -1.626786627e-02f, -1.193878807e-02f, -5.536830589e-03f,
    2.410222124e-03f,  1.111211611e-02f,  1.962566710e-02f,  2.685826042e-02f,
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
    3.177228436e-02f,  2.685826042e-02f,  1.962566710e-02f,  1.111211611e-02f,
    2.410222124e-03f,  -5.536830589e-03f, -1.193878807e-02f, -1.626786627e-02f,
    -1.828097858e-02f, -1.801719139e-02f, -1.576462128e-02f, -1.200305434e-02f,
    -7.330730141e-03f, -2.384808633e-03f, 2.235020933e-03f,  6.031442977e-03f,
    8.656555859e-03f,
};

/**
 * The frame sync word as dibits, one per symbol, sign bit first: 11 00 11 01
 * 11 11 01 01 10 01, i.e. hex CDF59.  Comparing these instead of the signs is
 * what makes the detector reject a run of outer symbols that merely has the
 * right signs.
 */
static const uint8_t nxdnFswDibit[NXDN_FSW_SYMBOLS] = {
    3, 0, 3, 1, 3, 3, 1, 1, 2, 1,
};

/** Four-level model of one window of symbols. */
struct nxdnLevels {
    float dc;    ///< Residual carrier offset
    float outer; ///< Outer deviation: the symbols sit at +-outer, +-outer/3
    float inner; ///< Inner deviation, always outer / 3
    float slope; ///< Residual carrier drift across the window, per symbol
};

/** A sync found in the stream, before its frame is decoded. */
struct nxdnCandidate {
    long symbol;   ///< Symbol index of the first sync symbol
    int errors;    ///< Sign errors of the match
    float score;   ///< Normalised correlation of the match
    float offset;  ///< Fractional timing the decode starts from
    bool inverted; ///< The inverted sync word matched instead
};

struct nxdnPhy {
    /* 48 kHz input history, doubled so the filter taps are contiguous */
    float hist[2 * NXDN_HIST_LEN];
    long inIndex;

    /* 24 kHz matched filter output */
    float ring[NXDN_RING_LEN];
    long ringIndex;

    int phase;       ///< Coarse sampling phase in 48 kHz samples, 0..8 even
    float timing;    ///< Fractional correction, in 48 kHz samples

    long scan;       ///< Next symbol position to test
    long grid;       ///< Last frame's sync position, -1 when free running
    long gridMisses;

    long lastFrame;  ///< Input index of the last decoded frame, -1 if none

    float win[NXDN_FRAME_SYMBOLS]; ///< The frame being decoded
};

static struct nxdnPhy phy;


/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

void nxdnPhy_init(void)
{
    nxdnPhy_reset();
}

void nxdnPhy_reset(void)
{
    memset(&phy, 0, sizeof(phy));
    phy.scan = 0;
    phy.grid = -1;
    phy.lastFrame = -1;
}

int nxdnPhy_phase(void)
{
    return phy.phase;
}

bool nxdnPhy_synced(void)
{
    if (phy.lastFrame < 0)
        return false;

    const long elapsed = phy.inIndex - phy.lastFrame;

    return elapsed < ((long)NXDN_SYNC_TIMEOUT_MS * 48);
}

/* ------------------------------------------------------------------ */
/* Sampling                                                           */
/* ------------------------------------------------------------------ */

/**
 * Matched filter output at a fractional 24 kHz index: the four-point Lagrange
 * interpolator through the samples around it.
 */
static float mf_at(float pos)
{
    long i = (long)pos;
    float f = pos - (float)i;
    long oldest = phy.ringIndex - NXDN_RING_LEN + 4;

    if ((i < oldest + 1) || (i > phy.ringIndex - 2))
        return 0.0f;

    float x0 = phy.ring[(unsigned)i & (NXDN_RING_LEN - 1)];
    if (f == 0.0f)
        return x0;

    float xm1 = phy.ring[(unsigned)(i - 1) & (NXDN_RING_LEN - 1)];
    float x1 = phy.ring[(unsigned)(i + 1) & (NXDN_RING_LEN - 1)];
    float x2 = phy.ring[(unsigned)(i + 2) & (NXDN_RING_LEN - 1)];

    /* Lagrange through f = -1, 0, 1, 2, in Horner form */
    float a = (-xm1 + 3.0f * x0 - 3.0f * x1 + x2) / 6.0f;
    float b = (xm1 - 2.0f * x0 + x1) / 2.0f;
    float c = (-2.0f * xm1 - 3.0f * x0 + 6.0f * x1 - x2) / 6.0f;

    return ((a * f + b) * f + c) * f + x0;
}

/**
 * Symbol k at the coarse phase, displaced by d 48 kHz samples: the ring holds
 * NXDN_SPS samples per symbol, so each 48 kHz sample is half a ring sample.
 */
static float symbol_at(long k, int phase, float d)
{
    return mf_at((float)NXDN_SPS * (float)k + 0.5f * (float)phase
                 + 0.5f * d);
}

/** Fill a window of `n` symbols from symbol k with fractional offset d. */
static void symbol_window(long k, float d, int n, float *out)
{
    for (int i = 0; i < n; i++)
        out[i] = symbol_at(k + i, phy.phase, d);
}

/**
 * Dibit of a symbol: +3 -> 3, +1 -> 2, -1 -> 0, -3 -> 1.  The sign bit is the
 * data bit of a LICH symbol and the high bit of a voice dibit, which is what
 * the scrambler and the AMBE schedule work on.
 */
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
 * buffer.
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
 * Four-level model of a window of symbols, as the DMR mode does it: a median
 * DC, a symmetric 4-means with a linear drift term, then the outer level from
 * every symbol the slicer puts at the outside.  The nonlinear fit is what
 * makes the estimate independent of the payload mix.
 */
static void estimate_levels(const float *y, int n, bool fit,
                            struct nxdnLevels *levels)
{
    const float centre = 0.5f * (float)n;
    float dc = sample_median(y, n);
    float acc = 0.0f;

    for (int i = 0; i < n; i++)
        acc += fabsf(y[i] - dc);

    /* uniform 4FSK (+-A, +-A/3): E|y-dc| = 2A/3, so A = 1.5 * mean|y-dc| */
    float outer = 1.5f * acc / (float)n;
    if (outer < 1e-6f)
        outer = 1e-6f;
    float inner = outer / 3.0f;
    float slope = 0.0f;

    for (int it = 0; fit && (it < 3); it++) {
        const float mid = 0.5f * (outer + inner);
        float residual = 0.0f;

        for (int i = 0; i < n; i++) {
            float v = y[i] - (dc + slope * ((float)i - centre));
            float av = fabsf(v);
            float nom = (av > mid) ? outer : inner;

            residual += v - ((v > 0.0f) ? nom : -nom);
        }
        dc += 0.5f * residual / (float)n;

        float num = 0.0f;
        float den = 0.0f;

        for (int i = 0; i < n; i++) {
            float t = (float)i - centre;
            float v = y[i] - dc;
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
        const float mid = 0.5f * (outer + inner);
        float sum = 0.0f;
        int count = 0;

        for (int i = 0; i < n; i++) {
            float v = y[i] - (dc + slope * ((float)i - centre));
            float av = fabsf(v);

            if (av > mid) {
                sum += av;
                count++;
            }
        }
        if (count >= 16)
            outer = sum / (float)count;
    }

    levels->dc = dc;
    levels->outer = outer;
    levels->inner = outer / 3.0f;
    levels->slope = slope;
}

/**
 * RMS distance of the payload symbols to the four levels, relative to the
 * outer level: the eye opening of the frame.  The sync symbols are left out
 * because they are all outer and known.
 */
static float frame_evm(const struct nxdnLevels *levels)
{
    const float thresholdMid = 0.5f * levels->inner;
    const float thresholdOuter = 0.5f * (levels->inner + levels->outer);
    float acc = 0.0f;
    int n = 0;

    for (int i = NXDN_FSW_SYMBOLS; i < NXDN_FRAME_SYMBOLS; i++) {
        float v = phy.win[i]
                - (levels->dc + levels->slope * ((float)i - 96.0f));
        int dibit = level_to_dibit(v, thresholdMid, thresholdOuter);
        float error = v - nominal_level(dibit, levels->inner, levels->outer);

        acc += error * error;
        n++;
    }

    return sqrtf(acc / (float)n) / levels->outer;
}

/**
 * Sign errors and eye aperture of a 10-symbol window against the sync word.
 *
 * Only the signs are matched, not the four levels, and that is deliberate.
 * The transmit pulse of NXDN is a square-root raised cosine times the sinc of
 * a symbol held for its period (TS 1-A 3.4/3.5), so a receiver that is not
 * the exact inverse of it sees the inner symbols pulled towards the outer
 * ones: on real captures the +-1 symbols of the sync field do not land on a
 * clean inner level, while their signs are unambiguous.  The reference
 * decoder matches this field on signs too, through five accepted readings of
 * each polarity.  The levels are recovered from the payload, where the
 * four-level fit has 192 symbols to work with.
 */
static float match_window(const float *v, int8_t polarity, int *errors,
                          float *aperture)
{
    float sum = 0.0f;

    for (int i = 0; i < NXDN_FSW_SYMBOLS; i++)
        sum += v[i];
    const float centre = sum / (float)NXDN_FSW_SYMBOLS;

    float deviation = 0.0f;
    float numerator = 0.0f;
    float denominator = 0.0f;

    for (int i = 0; i < NXDN_FSW_SYMBOLS; i++) {
        float y = v[i] - centre;

        deviation += fabsf(y);
        numerator += y * (float)(polarity * nxdnFsw[i]);
        denominator += fabsf(y) * (float)((nxdnFsw[i] == 3 || nxdnFsw[i] == -3)
                                         ? 3 : 1);
    }
    deviation /= (float)NXDN_FSW_SYMBOLS;

    int signErrors = 0;
    for (int i = 0; i < NXDN_FSW_SYMBOLS; i++) {
        if (((v[i] - centre) > 0.0f) != ((polarity * nxdnFsw[i]) > 0))
            signErrors++;
    }

    *errors = signErrors;
    *aperture = (deviation > 1e-9f) ? 1.0f : 0.0f;

    /* +1 for a perfect field, -1 for a field with every sign wrong */
    return (denominator > 1e-12f) ? (numerator / denominator) : -1.0f;
}

/**
 * Cheap pre-gate before correlating, and the coarse sampling instant it picks:
 * the same trick as the DMR mode.  The sync field is two-level, so a window
 * whose magnitudes are spread over four levels cannot be one; measuring the
 * window at five instants a fifth of a symbol apart also gives an acquisition
 * that does not depend on the fractional offset carried from the last frame.
 */
static bool aperture_pregate(long k, float *offset)
{
    const long base = (long)lroundf((float)NXDN_SPS * (float)k
                                    + 0.5f * ((float)phy.phase + phy.timing));
    float best = -1.0f;
    long bestSkew = 0;

    /* A full symbol period of instants: with NXDN_SPS samples per symbol the
     * scan visits every symbol position once, so the window has to be tried
     * over the whole eye and not just around the carried offset. */
    for (int skew = -(NXDN_SPS / 2); skew <= (NXDN_SPS / 2); skew++) {
        float v[NXDN_FSW_SYMBOLS];
        float sum = 0.0f;

        for (int i = 0; i < NXDN_FSW_SYMBOLS; i++) {
            v[i] = phy.ring[(unsigned)(base + skew + NXDN_SPS * i)
                            & (NXDN_RING_LEN - 1)];
            sum += v[i];
        }

        const float centre = sum / (float)NXDN_FSW_SYMBOLS;
        float deviation = 0.0f;

        for (int i = 0; i < NXDN_FSW_SYMBOLS; i++)
            deviation += fabsf(v[i] - centre);
        deviation /= (float)NXDN_FSW_SYMBOLS;

        if (deviation < 1e-6f)
            continue;

        int outer = 0;
        for (int i = 0; i < NXDN_FSW_SYMBOLS; i++) {
            /* every sync symbol is at least the inner level */
            if (fabsf(v[i] - centre) > (NXDN_APERTURE_LEVEL * deviation * 0.5f))
                outer++;
        }

        if (((outer * 100) >= (NXDN_APERTURE_PREGATE * NXDN_FSW_SYMBOLS))
            && (deviation > best)) {
            best = deviation;
            bestSkew = skew;
        }
    }

    if (best < 0.0f)
        return false;

    *offset = phy.timing + 2.0f * (float)bestSkew;

    return true;
}

/**
 * Test a sync at symbol k.  NXDN has a single frame sync word, so the only
 * ambiguity is the sign of the discriminator: an inverted one matches the
 * negated pattern.
 */
static bool find_sync(long k, float offset, struct nxdnCandidate *c)
{
    float v[NXDN_FSW_SYMBOLS];

    symbol_window(k, offset, NXDN_FSW_SYMBOLS, v);


    int bestErrors = 99;
    float bestScore = -2.0f;
    int8_t bestPolarity = 1;

    for (int p = 0; p < 2; p++) {
        int8_t polarity = (p == 0) ? 1 : -1;
        int errors;
        float aperture;
        float score = match_window(v, polarity, &errors, &aperture);

        if ((errors < bestErrors)
            || ((errors == bestErrors) && (score > bestScore))) {
            bestErrors = errors;
            bestScore = score;
            bestPolarity = polarity;
        }
    }

    if ((bestErrors > NXDN_SYNC_MAX_ERRORS)
        || (bestScore < NXDN_SYNC_MIN_SCORE))
        return false;

    c->symbol = k;
    c->errors = bestErrors;
    c->score = bestScore;
    c->offset = offset;
    c->inverted = (bestPolarity < 0);

    return true;
}

/**
 * Fractional symbol timing of one frame, in 48 kHz samples: the 16 phases of
 * the four-point interpolator over +-2.5 samples, scored by the distance of
 * the sync field to its ideal waveform with the payload eye as a tie-break.
 */
static float refine_timing(long k, int8_t polarity, float centre, float *evmOut,
                           float *scoreOut)
{
    const float step = 2.0f * NXDN_TIMING_SPAN / (float)NXDN_TIMING_PHASES;
    float bestOffset = centre;
    float bestMetric = -1e9f;
    float bestEvm = 9.0f;
    float bestScore = 0.0f;
    struct nxdnLevels levels;

    for (int i = 0; i <= NXDN_TIMING_PHASES; i++) {
        float d = centre - NXDN_TIMING_SPAN + step * (float)i;
        float v[NXDN_TIMING_WINDOW];
        int errors;
        float aperture;

        symbol_window(k, d, NXDN_TIMING_WINDOW, v);

        float distance = match_window(v, polarity, &errors, &aperture);

        estimate_levels(v, NXDN_TIMING_WINDOW, false, &levels);

        const float thresholdMid = 0.5f * levels.inner;
        const float thresholdOuter = 0.5f * (levels.inner + levels.outer);
        float acc = 0.0f;

        for (int s = NXDN_FSW_SYMBOLS; s < NXDN_TIMING_WINDOW; s++) {
            float y = polarity * v[s] - levels.dc;
            int dibit = level_to_dibit(y, thresholdMid, thresholdOuter);
            float error = y - nominal_level(dibit, levels.inner, levels.outer);

            acc += error * error;
        }
        float evm = sqrtf(acc / (float)(NXDN_TIMING_WINDOW - NXDN_FSW_SYMBOLS))
                  / levels.outer;

        float metric = (1.0f - distance) + 0.35f * evm;

        if (metric > bestMetric) {
            bestMetric = metric;
            bestOffset = d;
            bestEvm = evm;
            bestScore = distance;
        }
    }

    *evmOut = bestEvm;
    *scoreOut = bestScore;

    return bestOffset;
}

/** Follow the symbol clock of the capture, and the coarse phase with it. */
static void track_timing(float offset)
{
    phy.timing = offset;

    while (phy.timing > NXDN_TIMING_LIMIT) {
        phy.timing -= 2.0f;
        phy.phase = (phy.phase + 2) % NXDN_PHASE_SPAN;
    }
    while (phy.timing < -NXDN_TIMING_LIMIT) {
        phy.timing += 2.0f;
        phy.phase = (phy.phase - 2 + NXDN_PHASE_SPAN) % NXDN_PHASE_SPAN;
    }
}

/* ------------------------------------------------------------------ */
/* Frame decode                                                       */
/* ------------------------------------------------------------------ */

/**
 * Decode the frame whose sync is at symbol k with fractional offset d.
 *
 * @return false if the frame does not pass the gates.
 */
static bool decode_frame(struct nxdnCandidate *c, struct nxdnFrame *out,
                         float *offsetOut)
{
    struct nxdnLevels levels;
    float evm, score;
    float d = refine_timing(c->symbol, c->inverted ? -1 : 1, c->offset, &evm,
                            &score);

    if ((evm > NXDN_MAX_EVM) || (score < NXDN_SYNC_MIN_SCORE))
        return false;

    symbol_window(c->symbol, d, NXDN_FRAME_SYMBOLS, phy.win);
    estimate_levels(phy.win, NXDN_FRAME_SYMBOLS, true, &levels);
    evm = frame_evm(&levels);

    if (evm > NXDN_MAX_EVM)
        return false;

    memset(out, 0, sizeof(*out));
    out->symbol = c->symbol;
    out->syncErrors = c->errors;
    out->score = score;
    out->evm = evm;

    const float thresholdMid = 0.5f * levels.inner;
    const float thresholdOuter = 0.5f * (levels.inner + levels.outer);
    const float dc = levels.dc;
    const float slope = levels.slope;

    /*
     * The sign of the discriminator is not settled by the sync word alone:
     * the two inner symbols of the field are the ones a real channel distorts,
     * so the negated word can match as well as the true one.  The LICH decides
     * it -- its parity is a 1-bit check on the very first symbols of the
     * frame, and the polarity that makes it pass is the right one.
     */
    for (int attempt = 0; attempt < 2; attempt++) {
        const int8_t pol = ((attempt == 0) == (!c->inverted)) ? 1 : -1;

        for (int i = NXDN_FSW_SYMBOLS; i < NXDN_FRAME_SYMBOLS; i++) {
            float v = pol * phy.win[i] - (dc + slope * ((float)i - 96.0f));

            out->dibits[i - NXDN_FSW_SYMBOLS] =
                (uint8_t)level_to_dibit(v, thresholdMid, thresholdOuter);
        }

        nxdnFecDescramble(out->dibits, pol < 0);

        out->inverted = (pol < 0);
        out->lichOk = nxdnFecLich(out->dibits + NXDN_OFF_LICH, &out->lich);

        if (out->lichOk)
            break;
    }

    if (!out->lichOk)
        return true;    /* a frame worth reporting, but with nothing in it */

    /* SACCH: 30 dibits = 60 bits, one punctured convolutional block */
    {
        uint8_t bits[NXDN_SACCH_BITS];
        uint8_t info[26];

        for (int i = 0; i < NXDN_SACCH_BITS; i++) {
            int dibit = out->dibits[NXDN_OFF_SACCH + (i >> 1)];

            bits[i] = (uint8_t)((i & 1) ? (dibit & 1u)
                                        : ((dibit >> 1) & 1u));
        }

        out->sacchOk = nxdnFecSacch(bits, info);
        out->sacchSr = (uint8_t)((info[0] << 7) | (info[1] << 6)
                                 | (info[2] << 5) | (info[3] << 4)
                                 | (info[4] << 3) | (info[5] << 2)
                                 | (info[6] << 1) | info[7]);
        for (int i = 0; i < 18; i++)
            out->sacchBits[i] = info[8 + i];
    }

    nxdnLichVoice(&out->lich, &out->voiceFirst, &out->voiceCount);

    for (int v = 0; v < out->voiceCount; v++) {
        const uint8_t *dib = out->dibits + NXDN_OFF_VCH1
                           + (out->voiceFirst + v) * NXDN_VCH_DIBITS;

        nxdnFecVchToAmbe(dib, out->frame[out->frameCount]);
        out->frameCount++;
    }

    *offsetOut = d;

    return true;
}

/* ------------------------------------------------------------------ */
/* Scan                                                               */
/* ------------------------------------------------------------------ */

/** First symbol position of the next frame after the last one decoded. */
static long grid_target(long after)
{
    long next = phy.grid + NXDN_FRAME_SYMBOLS;

    while (next <= after)
        next += NXDN_FRAME_SYMBOLS;

    return next;
}

static void emit(const struct nxdnFrame *frame, nxdnFrameHandler handler,
                 void *user)
{
    phy.lastFrame = phy.inIndex;

    if (handler != NULL)
        handler(frame, user);
}

/** Search [from, to] for the best sync inside a predicted frame position. */
static bool search_window(long from, long to, struct nxdnCandidate *best)
{
    bool found = false;
    float bestScore = -2.0f;

    for (long k = from; k <= to; k++) {
        struct nxdnCandidate c;
        float offset;

        if (!aperture_pregate(k, &offset))
            continue;

        if (!find_sync(k, offset, &c))
            continue;

        if ((!found) || (c.errors < best->errors)
            || ((c.errors == best->errors) && (c.score > bestScore))) {
            *best = c;
            bestScore = c.score;
            found = true;
        }
    }

    return found;
}

static void scan(nxdnFrameHandler handler, void *user)
{
    const long newest = phy.ringIndex;

    for (;;) {
        struct nxdnCandidate c;
        long target = -1;

        if (phy.grid >= 0) {
            target = grid_target(phy.scan);
            if (phy.scan < (target - NXDN_GRID_TOLERANCE))
                phy.scan = target - NXDN_GRID_TOLERANCE;

            /* The whole frame has to be in the ring before it can be
             * decoded.  ringIndex counts 24 kHz samples, scan counts
             * symbols, so the guard is in symbols: mf_at() past the newest
             * sample returns zero and the search would be judging silence. */
            if (((target + NXDN_GRID_TOLERANCE + NXDN_FRAME_SYMBOLS)
                 * NXDN_SPS + NXDN_RING_MARGIN) > newest)
                return;

            if (!search_window(phy.scan, target + NXDN_GRID_TOLERANCE, &c)) {
                if (++phy.gridMisses >= NXDN_GRID_MISSES)
                    phy.grid = -1;

                phy.scan = target + NXDN_GRID_TOLERANCE + 1;

                if (phy.grid < 0)
                    phy.scan = (target > phy.scan) ? target : phy.scan;

                continue;
            }
        } else {
            if (((phy.scan + NXDN_FRAME_SYMBOLS) * NXDN_SPS
                 + NXDN_RING_MARGIN) > newest)
                return;

            float offset;

            if (!aperture_pregate(phy.scan, &offset)) {
                phy.scan++;
                continue;
            }
            if (!find_sync(phy.scan, offset, &c)) {
                phy.scan++;
                continue;
            }
        }

        struct nxdnFrame frame;
        float offset = c.offset;

        if (!decode_frame(&c, &frame, &offset)) {
            phy.scan = c.symbol + 1;
            continue;
        }

        /* Stay on the predicted position, not on where the correlation
         * peaked: snapping to the prediction keeps the grid from creeping. */
        phy.grid = (target >= 0) ? target : c.symbol;
        phy.gridMisses = 0;
        phy.scan = c.symbol + NXDN_FRAME_SYMBOLS - 4;

        track_timing(offset);
        emit(&frame, handler, user);
    }
}

/** Matched filter at 24 kHz and symbol scan for one new input sample. */
static void push_sample(float sample, nxdnFrameHandler handler, void *user)
{
    phy.inIndex++;
    const unsigned slot = (unsigned)phy.inIndex & (NXDN_HIST_LEN - 1);
    phy.hist[slot] = sample;
    phy.hist[slot + NXDN_HIST_LEN] = sample;

    if (phy.inIndex < NXDN_MF_CENTER)
        return;

    long mfIndex = phy.inIndex - NXDN_MF_CENTER;
    if ((mfIndex & 1) != 0)
        return;

    const float *x = &phy.hist[(unsigned)(phy.inIndex - NXDN_MF_TAPS + 1)
                               & (NXDN_HIST_LEN - 1)];
    float acc0 = 0.0f;
    float acc1 = 0.0f;

    for (int t = 0; t < (NXDN_MF_TAPS - 1); t += 4) {
        acc0 += nxdn_rrc[t] * x[t] + nxdn_rrc[t + 2] * x[t + 2];
        acc1 += nxdn_rrc[t + 1] * x[t + 1] + nxdn_rrc[t + 3] * x[t + 3];
    }
    acc0 += nxdn_rrc[NXDN_MF_TAPS - 1] * x[NXDN_MF_TAPS - 1];

    phy.ringIndex++;
    phy.ring[(unsigned)phy.ringIndex & (NXDN_RING_LEN - 1)] = acc0 + acc1;

    scan(handler, user);
}

void nxdnPhy_push(const int16_t *samples, size_t count, nxdnFrameHandler handler,
                  void *user)
{
    for (size_t i = 0; i < count; i++)
        push_sample((float)samples[i], handler, user);
}
