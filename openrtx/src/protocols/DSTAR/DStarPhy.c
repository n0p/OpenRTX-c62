/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DStarPhy.c
 * @brief D-STAR DV receive PHY, see DStarPhy.h.
 *
 * JARL STD-7 fixes the waveform (GMSK, BT 0.5, h 0.5, 4800 bit/s, "1" is a
 * positive frequency deviation) but says nothing about how to receive it.
 * This demodulator is the one the host prototype validated
 * (tests/host/dstar, docs/RX_DSTAR.md):
 *
 *  - a Gaussian matched filter, BT 0.5, on the discriminator audio, decimated
 *    2:1 to 24 kHz, i.e. 5 samples per symbol.  The transmit pulse is a
 *    Gaussian and the modulation is MSK, so the same Gaussian is the matched
 *    filter; its group delay is removed by the symmetric tap layout;
 *  - symbol timing in blocks of 64 symbols: 3 integer phases x 3 quarter
 *    sample offsets around the previous choice, the widest eye wins.  At 5
 *    samples per symbol the GMSK eye is narrow enough that a fixed phase loses
 *    the frame sync after a few seconds of clock drift, and re-measuring every
 *    13 ms costs ~9 interpolations per symbol;
 *  - hard bits for the sync hunt, soft values (normalised to +-96 by the
 *    block's mean |sample|) for the header Viterbi, which is where the ~2 dB of
 *    coding gain lives;
 *  - a frame state machine that counts 96 bits per frame, keeps the 24 slow
 *    bits of each frame, recognises the resync word in the slow field of every
 *    21st frame and the 16-bit tail 0x135E of the end pattern.
 *
 * Polarity: a discriminator can be inverted (DMR sees both on the bench), so
 * the demodulator tries the other polarity once when the header CRC fails and
 * latches whichever decodes.
 */

#include "protocols/DSTAR/DStarPhy.h"
#include "core/psram_state.h"

#include <string.h>

/* Gaussian pulse, BT 0.5, 5 samples/symbol, +-2 symbols, Q15, sum 32768.
 * scripts/gen_dstar_tables.py regenerates it; the transmit pulse in DStarTx.c
 * is the same function at 10 samples/symbol. */
static const int16_t gauss5[21] = {
    0, 0, 0, 0, 0, 8, 104, 760, 3158, 7421, 9866,
    7421, 3158, 760, 104, 8, 0, 0, 0, 0, 0,
};

#define SPS 5                  /* samples per symbol at 24 kHz */
#define BLOCK_SYMBOLS 64       /* symbols per timing block */
#define FILT_TAPS 21           /* gauss5 length */

enum rxState {
    RX_SEARCH = 0,  /* hunting the frame sync */
    RX_HEADER,      /* collecting the 660 coded header bits */
    RX_DATA,        /* 96-bit frames */
};

/* --- decimating matched filter ------------------------------------------- */
static int16_t inRing[64];
static unsigned inPos;
static unsigned inCount;
static unsigned decPhase;

/* --- 24 kHz stream ------------------------------------------------------- */
#define YRING 512u
/*
 * The two largest buffers of the D-STAR front end go to PSRAM: the 24 kHz
 * matched-filter ring (written per sample block, read per symbol) and the 660
 * soft bits of the header (written once per header, read once).  The 5 760 B
 * audioBuffer of the mode stays in SRAM: it is the one handed to
 * audioStream_start(), so it is DMA.  dstarPhy_reset() memsets both of these
 * when the mode opens; the section is NOLOAD (core/psram_state.h).
 */
static int16_t yRing[YRING] PSRAM_STATE;
static uint32_t yTotal;      /* filtered samples produced so far */
static int32_t symPosQ8;     /* absolute Q8 position of the next symbol */

/* --- bit and frame state ------------------------------------------------- */
static enum rxState state;
/*
 * The end pattern is 48 bits long, so the history has to hold at least that
 * many: with a 32-bit accumulator the comparison in RX_DATA asked for 32 bits
 * that were not there, was never true, and every D-STAR transmission ended by
 * timeout -- with four seconds of all-zero voice frames going to the speaker --
 * instead of by its own end pattern.
 */
static uint64_t bitHist;     /* last bits, newest in bit 0 */
static uint16_t headerBit;
static int8_t headerSoft[DSTAR_CODED_BITS] PSRAM_STATE;
static uint16_t frameBit;
static uint8_t frameVoice[DSTAR_VOICE_BYTES];
static uint8_t frameSlow[DSTAR_SLOW_BYTES];
static uint32_t frameIndex;
static unsigned framesSinceSync;
static uint8_t polarity;     /* 0: positive deviation is a 1 */
static int lastSyncErrors;
static uint32_t totalBits;

static dstarHeaderHandler hdrCb;
static dstarFrameHandler frmCb;
static dstarEotHandler eotCb;
static void *cbUser;

/* ------------------------------------------------------------------ FIR --- */

/**
 * @brief Decimate 2:1 through the Gaussian matched filter.
 *
 * @return true and the filtered sample when a new 24 kHz sample is available.
 */
static bool decimate(int16_t sample, int16_t *out)
{
    inRing[inPos] = sample;
    inPos = (inPos + 1) & 63u;
    if (inCount < 64u)
        inCount++;
    decPhase ^= 1u;
    if (decPhase != 0u || inCount < (unsigned)(FILT_TAPS * 2))
        return false;

    int32_t acc = 0;
    unsigned idx = inPos;
    for (int k = 0; k < FILT_TAPS; k++) {
        idx -= 2u;
        acc += (int32_t)gauss5[k] * inRing[idx & 63u];
    }
    *out = (int16_t)(acc >> 15);
    return true;
}

/* --------------------------------------------------------- interpolator --- */

/* Cubic Lagrange interpolation of y at sample (i + mu/256). */
static inline int32_t interpAt(uint32_t i, int32_t mu)
{
    int32_t y0 = yRing[(i - 1u) & (YRING - 1u)];
    int32_t y1 = yRing[i & (YRING - 1u)];
    int32_t y2 = yRing[(i + 1u) & (YRING - 1u)];
    int32_t y3 = yRing[(i + 2u) & (YRING - 1u)];

    int32_t c0 = y1;
    int32_t c1 = (y2 - y0) / 2;
    int32_t c2 = y0 - (5 * y1) / 2 + 2 * y2 - y3 / 2;
    int32_t c3 = (3 * (y1 - y2)) / 2 + (y3 - y0) / 2;

    int32_t r = c0;
    r += (c1 * mu) >> 8;
    r += (c2 * mu * mu) >> 16;
    r += (c3 * mu * mu * mu) >> 24;
    return r;
}

/* ------------------------------------------------------------ services --- */

static inline int popcount32(uint32_t v)
{
    int n = 0;
    while (v) {
        v &= v - 1;
        n++;
    }
    return n;
}

static void resetFrame(void)
{
    frameBit = 0;
    memset(frameVoice, 0, sizeof(frameVoice));
    memset(frameSlow, 0, sizeof(frameSlow));
}

/* Feed one bit (hard decision + soft value, already polarity corrected). */
static void bitIn(uint8_t bit, int8_t soft)
{
    bitHist = ((bitHist << 1) | (uint64_t)bit) & 0xFFFFFFFFFFFFull;
    totalBits++;

    switch (state) {
    case RX_SEARCH: {
        /* 15-bit frame sync 111011001010000 (STD-7 4.1.1 b), MSB first.  Both
         * polarities are accepted: a discriminator can be inverted, and a false
         * lock on the wrong one is thrown away by the header CRC. */
        uint32_t want = (uint32_t)DSTAR_FRAME_SYNC & 0x7FFFu;
        int errsN = popcount32((uint32_t)(bitHist ^ want) & 0x7FFFu);
        int errsI = popcount32((uint32_t)(bitHist ^ (want ^ 0x7FFFu)) & 0x7FFFu);
        if (errsN <= 2 || errsI <= 2) {
            state = RX_HEADER;
            headerBit = 0;
            lastSyncErrors = (errsN < errsI) ? errsN : errsI;
        }
        break;
    }

    case RX_HEADER:
        headerSoft[headerBit++] = soft;
        if (headerBit >= DSTAR_CODED_BITS) {
            struct dstarHeader hdr;
            bool ok = dstarHeaderDecode(headerSoft, &hdr);
            if (!ok) {
                /* One polarity retry: if the discriminator is inverted the
                 * whole header is complemented. */
                for (int i = 0; i < DSTAR_CODED_BITS; i++)
                    headerSoft[i] = (int8_t)(-headerSoft[i]);
                ok = dstarHeaderDecode(headerSoft, &hdr);
                if (ok) {
                    polarity ^= 1u;
                    /* The stored history is in the old polarity; restart the
                     * hunt with the new one. */
                    state = RX_DATA;
                    frameIndex = 0;
                    framesSinceSync = 0;
                    resetFrame();
                    if (hdrCb != NULL)
                        hdrCb(&hdr, lastSyncErrors, cbUser);
                } else {
                    state = RX_SEARCH;
                }
            } else {
                state = RX_DATA;
                frameIndex = 0;
                framesSinceSync = 0;
                resetFrame();
                if (hdrCb != NULL)
                    hdrCb(&hdr, lastSyncErrors, cbUser);
            }
        }
        break;

    case RX_DATA: {
        /* The end pattern is 32 alternating bits then 0001001101011110
         * (STD-7 4.1.2 h).  All 48 are required: 16 bits alone would fire on
         * noise a few times a second and cut the call short.  It ends the
         * transmission wherever it lands, not on a frame boundary. */
        if ((bitHist & 0xFFFFu) == 0x135Eu &&
            ((bitHist >> 16) & 0xFFFFFFFFu) == 0xAAAAAAAAu) {
            state = RX_SEARCH;
            resetFrame();
            if (eotCb != NULL)
                eotCb(cbUser);
            break;
        }

        if (frameBit < DSTAR_VOICE_BITS) {
            frameVoice[frameBit >> 3] =
                (uint8_t)(frameVoice[frameBit >> 3] | (bit << (frameBit & 7)));
        } else {
            uint16_t k = (uint16_t)(frameBit - DSTAR_VOICE_BITS);
            frameSlow[k >> 3] = (uint8_t)(frameSlow[k >> 3] | (bit << (k & 7)));
        }

        if (++frameBit >= DSTAR_FRAME_BITS) {
            struct dstarPhyFrame f;
            memset(&f, 0, sizeof(f));
            f.index = frameIndex;

            bool resync = false;
            if ((frameIndex % DSTAR_SUPERFRAME) == 0u) {
                uint32_t got = ((uint32_t)frameSlow[0]) |
                               ((uint32_t)frameSlow[1] << 8) |
                               ((uint32_t)frameSlow[2] << 16);
                int errs = popcount32((got ^ (uint32_t)DSTAR_DATA_SYNC) & 0xFFFFFFu);
                resync = (errs <= 2);
                framesSinceSync = resync ? 0u : framesSinceSync + 1u;
            }

            if (!resync) {
                for (int i = 0; i < DSTAR_SLOW_BYTES; i++)
                    f.slow[i] = (uint8_t)(frameSlow[i] ^ dstarSlowScrambler[i]);
            }
            memcpy(f.voice, frameVoice, DSTAR_VOICE_BYTES);
            f.sync = resync;

            if (frmCb != NULL)
                frmCb(&f, cbUser);

            frameIndex++;
            resetFrame();

            if (framesSinceSync > 2u * DSTAR_SUPERFRAME) {
                state = RX_SEARCH;
                if (eotCb != NULL)
                    eotCb(cbUser);
            }
        }
        break;
    }

    default:
        state = RX_SEARCH;
        break;
    }
}

/* -------------------------------------------------------------- timing --- */

/**
 * @brief Extract one block of BLOCK_SYMBOLS symbols at the best timing.
 */
static void runBlock(void)
{
    int32_t bestMetric = -1;
    int32_t bestPos = symPosQ8;
    uint32_t bestI = 0;
    int32_t bestMu = 0;

    for (int dp = -1; dp <= 1; dp++) {
        for (int df = -1; df <= 1; df++) {
            int32_t p0 = symPosQ8 + (int32_t)dp * 256 + (int32_t)df * 64;
            if (p0 < 256)
                continue;
            uint32_t i0 = (uint32_t)(p0 >> 8);
            int32_t mu = p0 & 255;
            if (i0 < 2u || (i0 + BLOCK_SYMBOLS * SPS + 2u) > yTotal)
                continue;

            int32_t acc = 0;
            for (int s = 0; s < BLOCK_SYMBOLS; s++) {
                int32_t y = interpAt(i0 + (uint32_t)(s * SPS), mu);
                acc += (y < 0) ? -y : y;
            }
            if (acc > bestMetric) {
                bestMetric = acc;
                bestPos = p0;
                bestI = i0;
                bestMu = mu;
            }
        }
    }

    if (bestMetric < 0)
        return;

    int32_t level = bestMetric / BLOCK_SYMBOLS;
    if (level < 1)
        level = 1;

    for (int s = 0; s < BLOCK_SYMBOLS; s++) {
        int32_t y = interpAt(bestI + (uint32_t)(s * SPS), bestMu);
        int32_t sc = (y * 96) / level;
        if (sc > 127)
            sc = 127;
        if (sc < -127)
            sc = -127;

        /* JARL STD-7 Ap1.5: a positive deviation is a 1.  The discriminator's
         * sign then depends on the receiver, hence the polarity flag. */
        uint8_t bit = (uint8_t)((y > 0) ? 1u : 0u);
        int8_t soft = (int8_t)sc;
        if (polarity) {
            bit ^= 1u;
            soft = (int8_t)(-soft);
        }
        bitIn(bit, soft);
    }

    symPosQ8 = bestPos + (int32_t)(BLOCK_SYMBOLS * SPS) * 256;
}

/* -------------------------------------------------------------- public --- */

void dstarPhy_init(void)
{
    dstarPhy_reset();
}

void dstarPhy_reset(void)
{
    memset(inRing, 0, sizeof(inRing));
    memset(yRing, 0, sizeof(yRing));
    inPos = 0;
    inCount = 0;
    decPhase = 0;
    yTotal = 0;
    symPosQ8 = 3 * 256;
    state = RX_SEARCH;
    bitHist = 0;
    headerBit = 0;
    frameIndex = 0;
    framesSinceSync = 0;
    polarity = 0;
    lastSyncErrors = 0;
    totalBits = 0;
    resetFrame();
}

void dstarPhy_push(const int16_t *samples, size_t count, dstarHeaderHandler hh,
                   dstarFrameHandler fh, dstarEotHandler eh, void *user)
{
    hdrCb = hh;
    frmCb = fh;
    eotCb = eh;
    cbUser = user;

    for (size_t n = 0; n < count; n++) {
        int16_t y;
        if (!decimate(samples[n], &y))
            continue;

        yRing[yTotal & (YRING - 1u)] = y;
        yTotal++;

        /* Enough for a block (plus the interpolator margin) and the ring has
         * not wrapped past the symbol we are about to read? */
        if (((uint32_t)((symPosQ8 >> 8) + BLOCK_SYMBOLS * SPS + 3) <= yTotal) &&
            ((yTotal - (uint32_t)(symPosQ8 >> 8)) < YRING - 16u)) {
            runBlock();

            /*
             * Rebase: symPosQ8 is the Q8 position of the next symbol, so an
             * int32 overflows at 2^23 samples -- 349 s of D-STAR at 24 kHz --
             * and the cast to unsigned in the test above then never agrees
             * again: the demodulator stops receiving for good (a mode change
             * was the only way back).  A whole number of rings changes no ring
             * slot, and both counters are shifted by the same amount.
             */
            if ((symPosQ8 >> 8) >= (int32_t)(1u << 20)) {
                yTotal -= 1u << 20;
                symPosQ8 -= 1 << 28; /* (1 << 20) * 256 */
            }
        }
    }
}

bool dstarPhy_synced(void)
{
    return state == RX_DATA;
}

int dstarPhy_state(void)
{
    return (int)state;
}
