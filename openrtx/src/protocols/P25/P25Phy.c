/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file P25Phy.c
 * @brief P25 Phase 1 receive PHY, see P25Phy.h.
 *
 * The matched filter runs at the 48 kHz of the discriminator stream and its
 * output is decimated to 24 kHz (five samples per symbol) into a ring that holds
 * a little more than one frame.  Once the ring holds a whole frame beyond the
 * last one decoded, the sync is searched for and the frame is sliced out of the
 * ring: the sync fixes the symbol phase of that frame and the four-level eye is
 * measured on that frame.  This is the algorithm the host prototype in
 * work/p25-rx validates against dsd-neo and op25 on real captures, with the
 * samples arriving in blocks instead of sitting in a vector.
 *
 * The frame decode (NID, link control, TSBK) is the same code, ported from the
 * prototype; that is why the bit-position tables and the Golay/Hamming/RS
 * decoders live in this directory instead of being rewritten here.
 */

#include "protocols/P25/P25Phy.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "protocols/P25/P25Fec.h"

/** Matched filter: RRC, alpha 0.2, 8 symbols either side, 10 samples/symbol. */
#define P25_MF_TAPS 81
#define P25_MF_CENTER 40

/** The radio hands over 48 kHz; the symbol clock runs at 5 samples/symbol. */
#define P25_DECIM 2
#define P25_SPS 5.0f
#define P25_DECIM_RATE 24000

/** Sync gate: the 24 symbol field is all outer levels. */
#define P25_APERTURE_GATE 0.75f
#define P25_SYNC_MIN_SCORE 0.55f
#define P25_SYNC_MAX_ERRORS 3

/** Grid search around the predicted frame start, in samples. */
#define P25_GRID_SAMPLES 8

/**
 * Sub-phases tried per candidate start.
 *
 * The ring is written at 24 kHz and the symbol clock is 5 samples per symbol,
 * so a start taken from the ring grid leaves the sampling instant anywhere
 * inside a symbol.  Only the sample instants that happen to fall on the eye
 * pass the aperture gate, which is one in five: the other four are evaluated
 * here at the interpolated phases between them.  This is the same fractional
 * search the host prototype does over the whole capture, and skipping it is
 * what kept the firmware from decoding a frame its own slicer could slice
 * perfectly (see docs/RX_P25.md).
 */
#define P25_SYNC_PHASES 5

/**
 * Fine timing: +-0.5 symbol around the best sub-phase, 0.05 symbol steps.
 *
 * The span has to cover the coarse grid, one sample or 0.2 symbol at 24 kHz:
 * the score is flat to within its own rounding over that distance, so which
 * sub-phase wins the coarse pass is arbitrary among the good ones, and a narrow
 * refinement would clamp against its own edge instead of finding the eye.
 */
#define P25_FINE_SPAN 1.0f
#define P25_FINE_STEPS 21

/** A frame this recent keeps the synchronisation indicator on. */
#define P25_SYNC_HOLD_MS 300

/** The RRC impulse response, unit energy, 81 taps at 10 samples/symbol. */
static const float p25_rrc[P25_MF_TAPS] =
{
    0.008657f, 0.006031f, 0.002235f, -0.002385f, -0.007331f,
    -0.012003f, -0.015765f, -0.018017f, -0.018281f, -0.016268f,
    -0.011939f, -0.005537f, 0.002410f, 0.011121f, 0.019626f,
    0.026858f, 0.031772f, 0.033464f, 0.031296f, 0.024997f,
    0.014741f, 0.001179f, -0.014572f, -0.030993f, -0.046272f,
    -0.058453f, -0.065608f, -0.066028f, -0.058402f, -0.041967f,
    -0.016626f, 0.016994f, 0.057548f, 0.103040f, 0.150952f,
    0.198427f, 0.242489f, 0.280273f, 0.309261f, 0.327489f,
    0.333708f, 0.327489f, 0.309261f, 0.280273f, 0.242489f,
    0.198427f, 0.150952f, 0.103040f, 0.057548f, 0.016994f,
    -0.016626f, -0.041967f, -0.058402f, -0.066028f, -0.065608f,
    -0.058453f, -0.046272f, -0.030993f, -0.014572f, 0.001179f,
    0.014741f, 0.024997f, 0.031296f, 0.033464f, 0.031772f,
    0.026858f, 0.019626f, 0.011121f, 0.002410f, -0.005537f,
    -0.011939f, -0.016268f, -0.018281f, -0.018017f, -0.015765f,
    -0.012003f, -0.007331f, -0.002385f, 0.002235f, 0.006031f,
    0.008657f,
};


/**
 * Ring of matched-filter output at 24 kHz (5 samples per symbol).  It holds two
 * frames, so a frame that does not start where the grid predicted it is still
 * in the ring when the search comes back for it (a call that follows a header,
 * a lost frame, a late entry).  The ring is written once per decimated sample
 * and read by interpolation, so it is not on the per-sample path: it lives in
 * PSRAM (the section is NOLOAD, so p25Phy_reset() zeroes it).
 */
#define P25_RING_SYMBOLS (2 * P25_FRAME_SYMBOLS + 40)
/*
 * The ring length is a power of two on purpose: the index wraps with a mask
 * instead of a 64-bit modulo, which on the M33 is a libgcc division the
 * interpolator paid four times per symbol (it was 155 instructions per
 * interpolation and 36 % of the demodulator's cost in QEMU).
 */
#define P25_RING_LEN 16384u
#define P25_RING_MASK (P25_RING_LEN - 1u)
/*
 * The matched filter of a full-scale signal peaks around 1.6 times the sample
 * scale, so the ring keeps half of it in an int16 without clipping: half the
 * size for the same window, and the quantisation step is 1/16000 of a level,
 * far below the eye.
 */
#define P25_RING_SCALE 0.5f

/**
 * Zeroed by p25Phy_reset(), because .psram_section is NOLOAD.  The 256 KiB of
 * SRAM cannot hold every mode's demodulator (DMR RX + APRS + NXDN already sit at
 * 80 % of it), so the ring, the sliced bits and the symbol buffer - none of them
 * touched per input sample - go to the 4 MiB of PSRAM; only the matched filter
 * history, which is touched once per sample, stays in SRAM.
 */
#ifdef __ZEPHYR__
#define P25_PSRAM __attribute__((section(".psram_section")))
#else
#define P25_PSRAM
#endif

/** DC and outer level of one frame. */
struct p25Levels
{
    float dc;
    float outer;
};

struct p25PhyState
{
    /* Matched filter at 48 kHz: touched once per input sample, stays in SRAM */
    float inHist[2 * P25_MF_TAPS]; ///< Input samples, written twice: no wrap
    unsigned inPos;               ///< Write position in inHist
    int decim;                    ///< Decimation phase

    /* Results */
    long ringCount;               ///< Samples written since the reset
    long search;                  ///< Ring index to try next
    long rescan;                  ///< Where to restart when the grid fails
    float lastFine;               ///< Phase of the sync of the last frame
    long lastFrameAt;             ///< Sample count of the last frame
    uint32_t frames;              ///< Frames decoded since the reset
    uint32_t syncTries;           ///< Candidate positions examined
    uint32_t gridMisses;          ///< Grid predictions that found nothing
    bool haveLevels;              ///< lastLevels is worth using
    struct p25Levels lastLevels;  ///< Levels the previous frame settled on
    bool haveGrid;                ///< A frame decoded, the next sync is known
    int8_t syncPat[P25_SYNC_SYMBOLS]; ///< The sync as +-1

    p25FrameHandler handler;      ///< Callback of the current push
    void *user;                   ///< Opaque pointer for the callback
};

/** Matched-filter output at 24 kHz, five samples per symbol. */
static int16_t ring[P25_RING_LEN] P25_PSRAM;
/** The frame being decoded, one bit per byte. */
static uint8_t bits[P25_FRAME_BITS] P25_PSRAM;
/** Symbols of one sliced frame, kept out of the stack. */
static float symBuf[P25_FRAME_SYMBOLS] P25_PSRAM;

static struct p25PhyState phy;

/** Matched-filter sample at an absolute ring index. */
static float ringAt(long index)
{
    if (index < 0)
        index = 0;
    /* Scale up with a multiply: dividing by 0.5 costs an FP division per tap */
    return (float)ring[(unsigned)index & P25_RING_MASK] * (1.0f / P25_RING_SCALE);
}

/**
 * Four-point Lagrange interpolation of the ring at a fractional index.
 *
 * Single precision throughout: the M33 has no double-precision FPU, so a
 * double here becomes libgcc calls on the per-symbol path, and the coefficients
 * do not need more than a float to the 1e-7 the eye cares about.
 */
static float ringInterp(float pos)
{
    float f = (pos >= 0.0f) ? pos : 0.0f;
    long i = (long)f;
    if ((float)i > f)
        i--;
    float t = f - (float)i;
    float y0 = ringAt(i - 1), y1 = ringAt(i), y2 = ringAt(i + 1),
          y3 = ringAt(i + 2);
    float c0 = -t * (t - 1.0f) * (t - 2.0f) * (1.0f / 6.0f);
    float c1 = (t + 1.0f) * (t - 1.0f) * (t - 2.0f) * 0.5f;
    float c2 = -(t + 1.0f) * t * (t - 2.0f) * 0.5f;
    float c3 = (t + 1.0f) * t * (t - 1.0f) * (1.0f / 6.0f);
    return c0 * y0 + c1 * y1 + c2 * y2 + c3 * y3;
}

/**
 * Correlation of a 24-symbol window with the sync pattern, plus the aperture.
 *
 * The sync field is all outer levels, so the normalised correlation is close to
 * 1 for a real sync and to 0 for payload, and the aperture (fraction of the
 * window at the outer level) rejects a lone noise spike.
 *
 * The polarity is returned instead of assumed: a discriminator with the
 * opposite slope flips every level, and the prototype decodes those captures
 * too, so the sign of the correlation is the polarity of the frame.
 */
static float syncScore(const float *w, float *aperture, int *errors, int *pol)
{
    float sum = 0.0f;
    for (int i = 0; i < P25_SYNC_SYMBOLS; i++)
        sum += w[i];
    float c = sum / P25_SYNC_SYMBOLS;

    float num = 0.0f, den = 0.0f, dev = 0.0f;
    for (int i = 0; i < P25_SYNC_SYMBOLS; i++)
    {
        float y = w[i] - c;
        num += y * (float)phy.syncPat[i];
        den += fabsf(y);
        dev += fabsf(y);
    }
    dev /= P25_SYNC_SYMBOLS;

    int outerCount = 0;
    for (int i = 0; i < P25_SYNC_SYMBOLS; i++)
        if (fabsf(w[i] - c) > 0.55f * dev)
            outerCount++;
    *aperture = (dev > 1e-9f) ? (float)outerCount / P25_SYNC_SYMBOLS : 0.0f;

    int p = (num >= 0.0f) ? 1 : -1;
    int errs = 0;
    for (int i = 0; i < P25_SYNC_SYMBOLS; i++)
        if (((w[i] - c) > 0.0f) != (phy.syncPat[i] * p > 0))
            errs++;
    *errors = errs;
    *pol = p;

    if (den < 1e-9f)
        return -1.0f;
    return fabsf(num) / den;
}

/* ------------------------------------------------------------ frame decode */

static void decodeLdu(const uint8_t *bits, struct p25Frame *f);
static void decodeTsbk(const uint8_t *bits, struct p25Frame *f, int nSym);

/**
 * IMBE header decode: four Golay(23,12) words and three Hamming(15,11) words
 * descrambled by the PN sequence the first word seeds, then the seven raw
 * bits (TIA-102.BABA-A, as in op25's imbe_header_decode).
 */
static bool decodeImbe(const uint8_t *cw, uint8_t out[P25_IMBE_BYTES],
                       int *errors)
{
    uint32_t u[8];
    uint32_t v = 0;
    for (int i = 0; i < 23; i++)
        v = (v << 1) | (cw[i] & 1);

    int ec = p25Fec_golayDecode(&v);
    if (ec < 0)
        return false;
    u[0] = v >> 11;
    *errors = ec;

    uint32_t pn = u[0] << 4;    /* 16-bit LFSR, advanced per word */
    for (int i = 1; i <= 3; i++)
    {
        uint32_t m = 0;
        for (int b = 22; b >= 0; b--)
        {
            pn = (173u * pn + 13849u) & 0xFFFFu;
            if (pn & 0x8000u)
                m |= 1u << b;
        }
        uint32_t w = 0;
        for (int b = 0; b < 23; b++)
            w = (w << 1) | (cw[23 * i + b] & 1);
        w ^= m;
        int e2 = p25Fec_golayDecode(&w);
        if (e2 < 0)
            return false;
        u[i] = w >> 11;
        *errors += e2;
    }

    for (int i = 4; i <= 6; i++)
    {
        uint32_t m = 0;
        for (int b = 14; b >= 0; b--)
        {
            pn = (173u * pn + 13849u) & 0xFFFFu;
            if (pn & 0x8000u)
                m |= 1u << b;
        }
        uint16_t w = 0;
        for (int b = 0; b < 15; b++)
            w = (uint16_t)((w << 1) | (cw[92 + 15 * (i - 4) + b] & 1));
        w ^= (uint16_t)m;
        int e2 = p25Fec_hamDecode(&w);
        if (e2 < 0)
            return false;
        u[i] = w >> 4;
        *errors += e2;
    }

    uint32_t u7 = 0;
    for (int b = 0; b < 7; b++)
        u7 = (u7 << 1) | (cw[137 + b] & 1);

    /* Pack the 88 parameter bits, MSB first: 4 x 12, 3 x 11, 7. */
    memset(out, 0, P25_IMBE_BYTES);
    int p = 0;
    for (int i = 0; i < 4; i++)
        for (int b = 11; b >= 0; b--)
        {
            if ((u[i] >> b) & 1)
                out[p >> 3] |= (uint8_t)(0x80 >> (p & 7));
            p++;
        }
    for (int i = 4; i <= 6; i++)
        for (int b = 10; b >= 0; b--)
        {
            if ((u[i] >> b) & 1)
                out[p >> 3] |= (uint8_t)(0x80 >> (p & 7));
            p++;
        }
    for (int b = 6; b >= 0; b--)
    {
        if ((u7 >> b) & 1)
            out[p >> 3] |= (uint8_t)(0x80 >> (p & 7));
        p++;
    }
    return true;
}

static void decodeLdu(const uint8_t *bits, struct p25Frame *f)
{
    f->voiceCount = 0;
    f->voiceBad = 0;
    f->imbeErrors = 0;

    for (int n = 0; n < P25_IMBE_PER_LDU; n++)
    {
        uint8_t cw[P25_IMBE_BITS];
        for (int i = 0; i < P25_IMBE_BITS; i++)
            cw[i] = bits[p25_voice_bits[n][i]] & 1;

        int errs = 0;
        if (decodeImbe(cw, f->voice[n], &errs))
        {
            f->voiceValid[n] = true;
            f->voiceCount++;
            f->imbeErrors += errs;
        }
        else
        {
            f->voiceValid[n] = false;
            f->voiceBad++;
        }
    }

    /* Low-speed data: 24 Hamming(10,6) words; in LDU1 they carry the LCW. */
    uint8_t hb[P25_LC_HEXBITS];
    for (int i = 0; i < P25_LC_HEXBITS; i++)
    {
        uint32_t w = p25_gather(bits, &p25_ldu_ls_bits[10 * i], 10);
        int v = p25Fec_ham10Decode(w >> 4, w & 0xF);
        hb[i] = (uint8_t)((v < 0) ? (int)(w >> 4) : v);
    }

    if (f->duid != P25_DUID_LDU1)
        return;

    int rs = p25Fec_rsDecode(hb, 24, 12);
    f->lcRsErrors = rs;
    if (rs < 0)
        return;

    /* Twelve six-bit symbols -> nine bytes: LCF, MFID, service options,
     * reserved, group (16), source (24).  TIA-102.AABF-D 7.3.1. */
    for (int k = 0; k < 3; k++)
    {
        const uint8_t *h = &hb[4 * k];
        uint8_t *b = &f->lc[3 * k];
        b[0] = (uint8_t)((h[0] << 2) | (h[1] >> 4));
        b[1] = (uint8_t)(((h[1] & 0xF) << 4) | (h[2] >> 2));
        b[2] = (uint8_t)(((h[2] & 0x3) << 6) | h[3]);
    }
    f->lcValid = true;
    f->lcFormat = (uint8_t)(f->lc[0] & 0x3F);
    f->lcService = f->lc[2];
    f->talkgroup = (uint16_t)((f->lc[4] << 8) | f->lc[5]);
    f->source = ((uint32_t)f->lc[6] << 16) | ((uint32_t)f->lc[7] << 8) |
                f->lc[8];
}

static void decodeTsbk(const uint8_t *bits, struct p25Frame *f, int nSym)
{
    /* Three 196-bit blocks: 98 dibits plus three status symbols each. */
    for (int b = 0; b < 3; b++)
    {
        uint8_t blk[P25_TSBK_BITS];
        int n = 0;
        int start = 57 + 101 * b;
        for (int s = 0; s < 101 && n < P25_TSBK_BITS; s++)
        {
            int sym = start + s;
            if (sym >= nSym)
                break;
            if ((sym % 36) == 35)
                continue;   /* status symbol, not payload */
            blk[n++] = bits[2 * sym];
            if (n < P25_TSBK_BITS)
                blk[n++] = bits[2 * sym + 1];
        }
        if (n < P25_TSBK_BITS)
            continue;

        uint8_t bytes[P25_TSBK_BYTES];
        if (p25Fec_trellisDecode(blk, bytes) < 0)
            continue;
        if (p25Fec_crc16(bytes, P25_TSBK_BYTES) != 0)
            continue;

        memcpy(f->tsbk, bytes, P25_TSBK_BYTES);
        f->tsbkValid = true;
        break;   /* the mode shows one block per frame */
    }
}

/**
 * Decode the NID of the frame in @p bits: BCH(63,16), the DUID parity bit and
 * the parity that the DUID itself announces.
 *
 * @param bits: the frame, one byte per bit, sync first.
 * @param nac: receives the network access code.
 * @param duid: receives the data unit ID.
 * @param ec: receives the bits corrected by the BCH decoder, -1 if it failed.
 * @return true when the NID decoded and its DUID parity checks out.
 *
 * The sync and the NID are the only part of a frame whose length does not
 * depend on what the frame turns out to be, so this runs on the first 56
 * symbols of the cut, before the rest of the frame is sliced: the DUID it
 * returns is what says how long the frame is.
 */
static bool decodeNid(const uint8_t *bits, uint16_t *nac, uint8_t *duid,
                      int *ec)
{
    uint32_t w = p25_gather(bits, p25_imbe_nid_bits, 32);
    uint32_t w2 = p25_gather(bits, &p25_imbe_nid_bits[32], 32);
    uint64_t nid = ((uint64_t)w << 32) | w2;

    int parity = (int)(nid & 1);
    uint64_t cw = nid >> 1;         /* 63 BCH bits, bit i = x^i */
    int e = p25Fec_bchDecode(&cw);
#ifdef P25_PHY_TRACE
    fprintf(stderr, "[nid] %016llx ec=%d\n", (unsigned long long)nid, e);
#endif
    *ec = e;
    if (e < 0 || e > 4)
        return false;

    uint64_t fixed = (cw << 1) | (uint64_t)parity;
    *nac = (uint16_t)((fixed >> 52) & 0xFFF);
    *duid = (uint8_t)((fixed >> 48) & 0xF);

    /* The last bit of the NID is the parity of the DUID (TIA-102.BAAC). */
    bool parityOk;
    switch (*duid)
    {
        case P25_DUID_HDU:
        case P25_DUID_TDU:
        case P25_DUID_TSBK:
        case P25_DUID_PDU:
        case P25_DUID_TDU15:
            parityOk = (parity == 0);
            break;
        case P25_DUID_LDU1:
        case P25_DUID_LDU2:
            parityOk = (parity == 1);
            break;
        default:
            parityOk = false;
            break;
    }
    return parityOk;
}

/**
 * Decode a complete frame body: NID first (it carries the NAC and the DUID),
 * then whatever that DUID announces.
 *
 * @param nSym: symbols the frame occupies on the air, sync included, as
 *              announced by its DUID (p25_frame_symbols()).
 */
static bool decodeFrame(struct p25Frame *f, p25FrameHandler handler, void *user,
                        int nSym)
{
    uint16_t nac = 0;
    uint8_t duid = 0;
    int ec = -1;

    if (!decodeNid(bits, &nac, &duid, &ec))
        return false;

    f->duid = (enum p25Duid)duid;
    f->nac = nac;
    f->bchErrors = ec;

    switch (f->duid)
    {
        case P25_DUID_LDU1:
        case P25_DUID_LDU2:
            decodeLdu(bits, f);
            break;

        case P25_DUID_TSBK:
            decodeTsbk(bits, f, nSym);
            break;

        case P25_DUID_HDU:
        {
            /* 36 words of the (18,6) Golay code carry the RS(36,20) symbols
             * of the header: MI, algorithm, key and talkgroup. */
            uint8_t hb[36];
            int bad = 0;
            for (int i = 0; i < 36; i++)
            {
                uint8_t air[18];
                uint32_t hex = 0;

                for (int b = 0; b < 18; b++)
                    air[b] = bits[p25_hdu_bits[18 * i + b]] & 1u;

                if (p25Fec_golay18Decode(air, &hex) < 0)
                    bad++;
                hb[i] = (uint8_t)(hex & 0x3Fu);
            }
            f->hdu = true;
#ifdef P25_PHY_TRACE
            fprintf(stderr, "[hdu] words");
            for (int i = 0; i < 36; i++)
                fprintf(stderr, " %02X", hb[i]);
            fprintf(stderr, "\n");
#endif
            if (bad == 0 && p25Fec_rsDecode(hb, 36, 20) >= 0)
            {
                int j = 0;
                for (int i = 0; i < 9;)
                {
                    i += 3;
                    j += 4;
                }
                f->hduAlgid = (uint8_t)((hb[j + 1] & 0xF) << 4 |
                                        (hb[j + 2] >> 2));
                f->hduKeyid = (uint16_t)(((hb[j + 2] & 0x3) << 14) |
                                         (hb[j + 3] << 8) | (hb[j + 4] << 2) |
                                         (hb[j + 5] >> 4));
                f->hduGroup = (uint16_t)(((hb[j + 5] & 0xF) << 12) |
                                         (hb[j + 6] << 6) | hb[j + 7]);
            }
            break;
        }

        default:
            break;
    }

    phy.frames++;
    if (handler)
        handler(f, user);
    return true;
}

/* ------------------------------------------------------------ frame slice */

/**
 * Four-level model of one frame.  The sync field is all outer levels and sign
 * balanced, so it gives the DC and the outer level directly; the refinement
 * passes re-fit the outer level from the symbols the slicer puts at the outer
 * level, which keeps a compressed eye from pushing inner symbols across the
 * threshold.
 */
static struct p25Levels frameLevels(const float *sym, int n)
{
    struct p25Levels L;
    float sum = 0.0f, acc = 0.0f;

    for (int i = 0; i < P25_SYNC_SYMBOLS; i++)
        sum += sym[i];
    L.dc = sum / P25_SYNC_SYMBOLS;
    for (int i = 0; i < P25_SYNC_SYMBOLS; i++)
        acc += fabsf(sym[i] - L.dc);
    L.outer = acc / P25_SYNC_SYMBOLS;
    if (!(L.outer > 1e-6f))
        L.outer = 1.0f;

    for (int it = 0; it < 3; it++)
    {
        float so = 0.0f;
        int no = 0;
        const float mid = 0.666f * L.outer;   /* two of three units */
        for (int i = 0; i < n; i++)
        {
            float v = fabsf(sym[i] - L.dc);
            if (v > mid)
            {
                so += v;
                no++;
            }
        }
        if (no >= 16)
            L.outer = so / (float)no;
    }
    if (L.outer < 1e-6f)
        L.outer = 1.0f;
    return L;
}

/** Slice one symbol against the frame levels; returns the C4FM dibit. */
static int sliceSymbol(float y, struct p25Levels L, int pol)
{
    float v = 3.0f * (float)pol * (y - L.dc) / L.outer;
    if (v > 2.0f)
        return 1;   /* +1800 Hz, dibit 01 */
    if (v > 0.0f)
        return 0;   /* +600 Hz, dibit 00 */
    if (v > -2.0f)
        return 2;   /* -600 Hz, dibit 10 */
    return 3;       /* -1800 Hz, dibit 11 */
}

/**
 * What trying a candidate frame start produced.
 */
enum tryResult
{
    TRY_NONE = 0,   ///< Not a frame: the sync or the NID did not hold up
    TRY_DONE,       ///< A frame was decoded
    TRY_WAIT,       ///< The frame is longer than what the ring holds so far
};

/**
 * Try to decode the frame whose sync starts at ring sample @p start.
 *
 * The frame start comes from the ring grid, which steps in whole symbols, so
 * the sampling instant is anywhere inside one symbol period.  The sync is
 * therefore evaluated at the five interpolated sub-phases that cover that
 * period, then refined over +-0.25 symbol by maximising the DC-free
 * correlation with the pattern, exactly like the prototype's acquire() and
 * decodeFrame().
 *
 * Evaluating only the sample instants (the previous version) lost four frames
 * in five: at a sampling instant 0.4-0.45 symbol off the eye, the matched
 * filter attenuates the outer levels enough for only 16 of the 24 sync symbols
 * to clear the aperture threshold, so the true sync was thrown away by the
 * gate before the refinement could run.
 */
static enum tryResult tryFrame(long start, struct p25Frame *f)
{
    float bestScore = -2.0f, bestAp = 0.0f;
    int bestErrs = P25_SYNC_MAX_ERRORS + 1, bestPol = 1;
    float bestPh = (float)start;
    bool found = false;

    phy.syncTries++;

    /* Coarse: every sub-phase in one symbol period. */
    for (int p = 0; p < P25_SYNC_PHASES; p++)
    {
        float ph = (float)start + (float)p * (P25_SPS / P25_SYNC_PHASES);
        float w[P25_SYNC_SYMBOLS];
        for (int i = 0; i < P25_SYNC_SYMBOLS; i++)
            w[i] = ringInterp(ph + i * P25_SPS);

        float ap;
        int er, pol;
        float sc = syncScore(w, &ap, &er, &pol);
        if (sc < P25_SYNC_MIN_SCORE || ap < P25_APERTURE_GATE ||
            er > P25_SYNC_MAX_ERRORS)
            continue;

        if (!found || er < bestErrs || (er == bestErrs && sc > bestScore))
        {
            found = true;
            bestScore = sc;
            bestAp = ap;
            bestErrs = er;
            bestPol = pol;
            bestPh = ph;
        }
    }
    if (!found)
        return TRY_NONE;

    /* Fine: the correlation does not distinguish +-0.1 symbol, where the eye
     * is attenuated but every sign is still right. */
    float fine = bestPh;
    float bestCorr = -1.0f;
    for (int s = 0; s < P25_FINE_STEPS; s++)
    {
        float ph = bestPh + (((float)s / (P25_FINE_STEPS - 1)) - 0.5f) *
                            P25_FINE_SPAN * P25_SPS;
        float w[P25_SYNC_SYMBOLS];
        float sum = 0.0f;
        for (int i = 0; i < P25_SYNC_SYMBOLS; i++)
        {
            w[i] = ringInterp(ph + i * P25_SPS);
            sum += w[i];
        }
        float c = sum / P25_SYNC_SYMBOLS;
        float corr = 0.0f;
        for (int i = 0; i < P25_SYNC_SYMBOLS; i++)
            corr += (w[i] - c) * (float)(phy.syncPat[i] * bestPol);
        if (corr > bestCorr)
        {
            bestCorr = corr;
            fine = ph;
        }
    }

    /*
     * The cut, from the frame structure.
     *
     * The sync and the NID are 56 symbols whatever the frame is, so those are
     * sliced first, on the levels of the sync field alone (it is all outer
     * levels and sign balanced, so it fixes DC and outer level by itself).
     * The NID says which data unit this is, and the data unit says how long
     * the frame is: 864 symbols for an LDU, 360 for a control-channel TSDU, 396
     * for a header, 72 or 216 for the terminators.
     *
     * Cutting a fixed 864 symbols for every frame is what left the control
     * channel unable to predict its own frames: a TSDU is 360 symbols, so
     * two thirds of every window were the frames that follow it, the levels
     * were fitted over 2.4 frames, and the next sync was predicted 504 symbols
     * (140 %) too far, which the search then had to walk back symbol by symbol.
     */
    for (int k = 0; k < P25_SYNC_NID_SYMBOLS; k++)
        symBuf[k] = ringInterp(fine + (float)k * P25_SPS);

    uint16_t nac = 0;
    uint8_t duid = 0;
    int ec = -1;
    bool nidOk = false;

    for (int attempt = 0; attempt < 2 && !nidOk; attempt++)
    {
        /*
         * The sync field alone fixes DC and outer level (24 symbols, all of
         * them outer).  On a marginal signal that is only 24 symbols of
         * estimate, so the second attempt uses the levels the frame before
         * this one ended up with: the same channel, but 864 symbols' worth of
         * measurement.  Both are free when the first one works.
         */
        struct p25Levels Ls = (attempt == 0 || !phy.haveLevels)
                                  ? frameLevels(symBuf, 0)
                                  : phy.lastLevels;

        for (int k = 0; k < P25_SYNC_NID_SYMBOLS; k++)
        {
            int d = sliceSymbol(symBuf[k], Ls, bestPol);
            bits[2 * k] = (uint8_t)((d >> 1) & 1);
            bits[2 * k + 1] = (uint8_t)(d & 1);
        }
        nidOk = decodeNid(bits, &nac, &duid, &ec);
    }

    if (!nidOk)
        return TRY_NONE;

    const int nSym = p25_frame_symbols((enum p25Duid)duid);

    /*
     * The window can only be as long as the ring is deep: slicing past the
     * newest sample would read what was written a ring ago instead of the
     * frame.  The caller keeps this position and tries it again once the rest
     * of the frame has arrived.
     */
    if ((long)fine + (long)(nSym - 1) * (long)P25_SPS + 3 > phy.ringCount)
        return TRY_WAIT;

    for (int k = P25_SYNC_NID_SYMBOLS; k < nSym; k++)
        symBuf[k] = ringInterp(fine + (float)k * P25_SPS);

    /* Levels refitted on the frame's own span, then the whole cut. */
    struct p25Levels L = frameLevels(symBuf, nSym);
#ifdef P25_PHY_TRACE
    fprintf(stderr, "[sy] start %ld ph %.2f sc %.2f ap %.2f e %d pol %d "
                    "duid %X span %d ec %d\n",
            start, (double)(fine - (float)start), bestScore, bestAp, bestErrs,
            bestPol, (unsigned)duid, nSym, ec);
#else
    (void)bestAp;
#endif
    for (int k = 0; k < nSym; k++)
    {
        int d = sliceSymbol(symBuf[k], L, bestPol);
        bits[2 * k] = (uint8_t)((d >> 1) & 1);
        bits[2 * k + 1] = (uint8_t)(d & 1);
    }

    phy.lastFine = fine;
    f->symbol = start / 5;
    f->spanSymbols = (uint16_t)nSym;
    /*
     * The sign errors of the sync at the phase the frame was actually cut at,
     * on the frame's own levels: the ones the coarse pass counted belong to a
     * candidate up to one sample away and describe the search, not the frame.
     */
    int syncErrs = 0;
    for (int k = 0; k < P25_SYNC_SYMBOLS; k++)
        if (((symBuf[k] - L.dc) > 0.0f) != (phy.syncPat[k] * bestPol > 0))
            syncErrs++;
    f->syncErrors = syncErrs;
    f->inverted = (bestPol < 0);
    f->bchErrors = -1;
    f->lcRsErrors = -1;
    if (!decodeFrame(f, phy.handler, phy.user, nSym))
        return TRY_NONE;

    /* The levels this frame settled on are the best estimate the next one has
     * of the channel, for the NID retry above. */
    phy.lastLevels = L;
    phy.haveLevels = true;
    return TRY_DONE;
}

/* ------------------------------------------------------------------- API */

void p25Phy_init(void)
{
    p25_sync_symbols(phy.syncPat);
    p25Phy_reset();
}

void p25Phy_reset(void)
{
    memset(&phy, 0, sizeof(phy));
    memset(ring, 0, sizeof(ring));
    memset(bits, 0, sizeof(bits));
    memset(symBuf, 0, sizeof(symBuf));
    p25_sync_symbols(phy.syncPat);
    /*
     * The search starts at the first sample the demodulator will ever see, not
     * at the bottom of the ring: a negative position has nothing written in it,
     * so every candidate down there interpolates sample 0 and the acquisition
     * pays a few thousand candidates for nothing (5 ms of audio's worth of
     * them per hunt).  The ring wrap itself is handled by the index mask.
     */
    phy.search = 0;
}

/**
 * A frame decoded at @p start: resume the search after it.
 *
 * The grid prediction is one frame later than the integer position the frame
 * was found at, and that frame is @p nSym symbols long (its DUID says so, not
 * the length of the longest frame: a control channel's TSDUs are 360 symbols
 * and predicting 864 put every one of them off the grid).  The rescan position
 * comes from the interpolated phase of the sync, one sample past it: any start
 * up to a symbol period before that phase would find this same frame again,
 * and a demodulator that decodes the same frame twice shows two calls for one
 * transmission.
 */
static void frameDecoded(long start, bool grid, int nSym)
{
    phy.frames++;
    /*
     * Predict from the phase the sync was found at, not from the integer ring
     * position the search started at: the two differ by up to a symbol, and
     * adding that error to the frame length is what left the +-8 sample grid
     * with almost no margin against the symbol clock's own drift (every frame
     * whose phase sat late was predicted too early and re-acquired by hand).
     */
    phy.search = (long)(phy.lastFine + 0.5f) + (long)nSym * 5;
    phy.rescan = (long)phy.lastFine + 4;
    phy.haveGrid = grid;
    phy.lastFrameAt = phy.ringCount;
}

/**
 * Push one matched-filter output sample: into the ring, and decode the frame
 * that it completes when there is one.
 *
 * One position is tried per decimated sample, moving in whole symbols, so the
 * search costs one correlation per symbol and not one per sample.  After a
 * frame the next one is looked for on the grid it established (frames are
 * contiguous); when that fails, the search restarts from the oldest sample the
 * ring still holds, which is how a frame that starts somewhere else - after a
 * header, after a lost frame, on a late entry - is still found instead of
 * being skipped for ever.
 */
static void pushDecimated(float mf)
{
    float scaled = mf * P25_RING_SCALE;
    if (scaled > 32767.0f)
        scaled = 32767.0f;
    if (scaled < -32768.0f)
        scaled = -32768.0f;

    ring[(unsigned)phy.ringCount & P25_RING_MASK] = (int16_t)lroundf(scaled);
    phy.ringCount++;

    const long frameSamples = (long)P25_FRAME_SYMBOLS * 5;
    const long oldest = phy.ringCount - P25_RING_LEN + 8;
    const long newest = phy.ringCount - frameSamples - 8;

    if (phy.search < oldest)
        phy.search = oldest;
    if (phy.search > newest)
        return;                 /* the frame at that position is not in yet */

    struct p25Frame f;
    memset(&f, 0, sizeof(f));

    /* On the grid the last frame established: a few samples either side. */
    if (phy.haveGrid)
    {
        long from = phy.search - P25_GRID_SAMPLES;
        long to = phy.search + P25_GRID_SAMPLES;
        if (from < oldest)
            from = oldest;
        if (to > newest)
            to = newest;
        for (long pos = from; pos <= to; pos++)
        {
            memset(&f, 0, sizeof(f));
            enum tryResult r = tryFrame(pos, &f);
            if (r == TRY_WAIT)
                return;         /* the frame is longer than the ring holds */
            if (r == TRY_DONE)
            {
                frameDecoded(pos, true, f.spanSymbols);
                return;
            }
        }
        /* Frame missing or somewhere else: scan again from just after the frame
         * that was decoded, never from before it (that would decode the same
         * frame for ever), but not from the grid either (a truncated header
         * before a call starts the next frame early). */
        phy.gridMisses++;
        phy.haveGrid = false;
        phy.search = phy.rescan;
        if (phy.search < oldest)
            phy.search = oldest;
        return;
    }

    enum tryResult r = tryFrame(phy.search, &f);
    if (r == TRY_WAIT)
        return;                 /* the frame is longer than the ring holds */
    if (r == TRY_DONE)
    {
        frameDecoded(phy.search, true, f.spanSymbols);
        return;
    }

    /* Not here: move on by one symbol and wait for the samples to catch up. */
    phy.search += 5;
}

void p25Phy_push(const int16_t *samples, size_t count, p25FrameHandler handler,
                 void *user)
{
    phy.handler = handler;
    phy.user = user;

    for (size_t i = 0; i < count; i++)
    {
        /* Matched filter at 48 kHz, over the history ring. */
        /*
         * The history is written twice (Tk95 style): inHist[pos] and
         * inHist[pos + TAPS] get the same sample, so the taps are read forward
         * from pos with no wrap-around test in the loop.  With the wrap tests
         * the kernel cost 20 instructions per tap pair and 33 M instructions
         * per second of audio in QEMU, a third of the demodulator.
         */
        const float x = (float)samples[i];
        phy.inHist[phy.inPos] = x;
        phy.inHist[phy.inPos + P25_MF_TAPS] = x;
        const float *h = &phy.inHist[phy.inPos];
        float acc0 = 0.0f, acc1 = 0.0f;
        for (int t = 0; t < P25_MF_TAPS - 1; t += 2)
        {
            acc0 += p25_rrc[t] * h[t];
            acc1 += p25_rrc[t + 1] * h[t + 1];
        }
        acc0 += p25_rrc[P25_MF_TAPS - 1] * h[P25_MF_TAPS - 1];
        phy.inPos = (phy.inPos + 1) % P25_MF_TAPS;

        if (++phy.decim < P25_DECIM)
            continue;
        phy.decim = 0;
        pushDecimated(acc0 + acc1);
    }
}

bool p25Phy_synced(void)
{
    return (phy.ringCount - phy.lastFrameAt) <
           (long)P25_DECIM_RATE * P25_SYNC_HOLD_MS / 1000;
}

uint32_t p25Phy_frames(void)
{
    return phy.frames;
}

uint32_t p25Phy_syncTries(void)
{
    return phy.syncTries;
}

uint32_t p25Phy_gridMisses(void)
{
    return phy.gridMisses;
}
