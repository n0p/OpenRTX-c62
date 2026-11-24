/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file main.c
 * @brief P25 Phase 1 transmit chain, closed through the firmware's own
 * receiver.
 *
 * The transmitter is the one the radio runs (protocols/P25/P25Tx.c and
 * protocols/C4FM/C4fmMod.c), the channel is additive white Gaussian noise with
 * a settable SNR, and the receiver is protocols/P25/P25Phy.c, i.e. the mode's
 * own demodulator and FEC.  Every field the receiver publishes is compared
 * with what went in, so an encoder and a decoder that disagree cannot both
 * pass: the NID (NAC, DUID), the link control (format, service, talkgroup,
 * source), all nine IMBE words bit for bit, the TSBK bytes, the HDU group and
 * the TDU that closes the call.
 *
 * Modes:
 *   p25_tx_test fec                      FEC encoder/decoder self-tests
 *   p25_tx_test loop [snr_db] [frames]   voiced call (HDU, LDU1/2, TDU)
 *   p25_tx_test idle [snr_db] [frames]   HDU, TSBK frames, TDU (no voice)
 *   p25_tx_test sweep                    errors against the SNR
 *   p25_tx_test wave <out.raw>           the modulating waveform, for dsd-neo
 *
 * SNR is in the occupied band: P25 C4FM with an RRC alpha 0.2 occupies
 * 4800 * 1.2 = 5760 Hz, and the noise is scaled to that bandwidth, so the
 * number is the one a BER curve is usually quoted against.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/C4FM/C4fmMod.h"
#include "protocols/P25/P25Fec.h"
#include "protocols/P25/P25Phy.h"
#include "protocols/P25/P25Tx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** Outer C4FM level at 1800 Hz of deviation; the radio's gain is the REG_40
 *  calibration and only the ratio matters here (the slicer is level blind). */
#define TX_GAIN 5460.0f

#define NAC 0x293u
#define GROUP 1u
#define SOURCE 0x123456u

/** Occupied bandwidth of the C4FM signal, for the SNR definition. */
#define BAND_HZ 5760.0
#define NYQUIST_HZ 24000.0

/** Longest call the harness builds. */
#define MAX_FRAMES 96
#define MAX_LDUS 256

static uint32_t rngState = 12345;

static uint32_t rnd(void)
{
    rngState = rngState * 1103515245u + 12345u;

    return rngState >> 8;
}

static double gauss(void)
{
    const double u1 = ((double)(rnd() & 0xFFFFFF) + 1.0) / 16777217.0;
    const double u2 = (double)(rnd() & 0xFFFFFF) / 16777216.0;

    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

/* ------------------------------------------------------------- reference -- */

/** IMBE parameters the transmitter fed, per LDU: 9 words of 11 bytes. */
static uint8_t sentImbe[MAX_LDUS][9][11];
static int lduFed;


/** One frame of the call, in the order the transmitter sends them. */
struct expect
{
    enum p25Duid duid;
};

static struct expect sent[MAX_FRAMES];
static int sentFrames;

/* --------------------------------------------------------------- results -- */

static struct
{
    int frames;
    int nacBad;
    int duidBad;
    int lcBad;
    int imbeWords;
    int imbeBad;
    int tsbk;
    int tsbkBad;
    int hdu;
    int hduBad;
    int tdu;
    int syncErrors;
    int bchErrors;
    int unexpected;
} res;

static int expectIdx;
static int lduCheck;

/** Where the call starts in the generated waveform (samples) and how long a
 *  frame is: the receiver reports the symbol position of each sync, so a frame
 *  is identified by where it sits, not by the order it arrived in.  A frame
 *  lost to the noise then costs that frame instead of shifting every
 *  expectation after it. */
static double callStart;
static double frameSamples;

static const char *duidName(enum p25Duid d)
{
    switch (d)
    {
        case P25_DUID_HDU: return "HDU";
        case P25_DUID_LDU1: return "LDU1";
        case P25_DUID_LDU2: return "LDU2";
        case P25_DUID_TSBK: return "TSBK";
        case P25_DUID_TDU: return "TDU";
        default: return "?";
    }
}

static int voiceSource(uint8_t params[11], void *user)
{
    (void)user;

    if (lduFed >= MAX_LDUS)
        return -1;

    /* A deterministic pseudo-random 88-bit pattern, different every frame:
     * every parameter bit changes between frames, so a packing mistake cannot
     * survive the comparison. */
    uint32_t s = (uint32_t)(lduFed * 2654435761u) ^ 0x5A5A1234u;

    memset(params, 0, 11);
    for (int i = 0; i < 88; i++)
    {
        s = s * 1664525u + 1013904223u;
        if (s & 0x80000000u)
            params[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
    }

    memcpy(sentImbe[lduFed / 9][lduFed % 9], params, 11);
    lduFed++;

    return 0;
}

static void onFrame(const struct p25Frame *f, void *user)
{
    (void)user;

    res.frames++;
    res.syncErrors += f->syncErrors;
    if (f->bchErrors >= 0)
        res.bchErrors += f->bchErrors;

    const long idx = lround(((double)f->symbol * 10.0 - callStart) / frameSamples);

    if (idx < 0 || idx >= sentFrames)
    {
        res.unexpected++;
        printf("  frame at sample %.0f: outside the call\n", (double)f->symbol * 10.0);
        return;
    }

    const struct expect *e = &sent[idx];

    if (f->nac != NAC)
    {
        res.nacBad++;
        printf("  frame %ld: NAC 0x%03X, expected 0x%03X\n", idx, f->nac, NAC);
    }
    if (f->duid != e->duid)
    {
        res.duidBad++;
        printf("  frame %ld: DUID %s, expected %s\n", idx, duidName(f->duid),
               duidName(e->duid));
        return;
    }

    switch (f->duid)
    {
        case P25_DUID_HDU:
            res.hdu++;
            if (f->hduGroup != GROUP)
            {
                res.hduBad++;
                printf("  HDU: talkgroup %u, expected %u\n", f->hduGroup, GROUP);
            }
            if (f->hduAlgid != 0x80 || f->hduKeyid != 0x0000)
            {
                res.hduBad++;
                printf("  HDU: algorithm 0x%02X key 0x%04X, expected 0x80/0x0000\n",
                       f->hduAlgid, f->hduKeyid);
            }
            break;

        case P25_DUID_TDU:
            res.tdu++;
            break;

        case P25_DUID_LDU1:
        case P25_DUID_LDU2:
        {
            /* The reference is grouped by voice frame, nine IMBE words each,
             * which is what the transmitter pulls: frame 1 is the first LDU. */
            const int ldu = (int)idx - 1;

            if (f->duid == P25_DUID_LDU1)
            {
                if (!f->lcValid || (f->talkgroup != GROUP) ||
                    (f->source != SOURCE) || (f->lc[2] != 0x00))
                {
                    res.lcBad++;
                    printf("  LDU1: lcValid=%d tg=%u src=%06X (expected %u/%06X)\n",
                           (int)f->lcValid, f->talkgroup, f->source, GROUP, SOURCE);
                }
            }
            if (ldu >= 0 && ldu < lduFed / 9)
            {
                if (f->voiceCount != 9)
                    printf("  LDU %d: %u/9 IMBE words decoded\n", ldu, f->voiceCount);
                for (int n = 0; n < 9; n++)
                {
                    res.imbeWords++;
                    if (!f->voiceValid[n] ||
                        memcmp(f->voice[n], sentImbe[ldu][n], 11) != 0)
                    {
                        res.imbeBad++;
                        /* Which reference frame does it match?  An off-by-one
                         * in either side shows up here. */
                        int match = -100;

                        for (int d = -2; d <= 2; d++)
                            for (int m = 0; m < 9; m++)
                                if ((ldu + d >= 0) && (ldu + d < 4 * 64) &&
                                    memcmp(f->voice[n], sentImbe[ldu + d][m], 11) == 0)
                                    match = d * 10 + m;

                        printf("  LDU %d: IMBE word %d differs (matches %d, "
                               "valid %d, rx %02X%02X tx %02X%02X)\n", ldu, n,
                               match, (int)f->voiceValid[n], f->voice[n][0],
                               f->voice[n][1], sentImbe[ldu][n][0],
                               sentImbe[ldu][n][1]);
                    }
                }
            }
            break;
        }

        case P25_DUID_TSBK:
        {
            /* The idle block: all-zero data, CRC-16 of ten zero bytes, i.e.
             * the complement of zero as this CRC is defined (P25Fec.c) */
            static const uint8_t zero[12] =
                { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };

            res.tsbk++;
            if (!f->tsbkValid || memcmp(f->tsbk, zero, 12) != 0)
            {
                res.tsbkBad++;
                printf("  TSBK differs (valid=%d)\n", (int)f->tsbkValid);
            }
            break;
        }

        default:
            break;
    }
}

/* ------------------------------------------------------------------ run --- */

static int run(double snrDb, int voiceFrames, bool withVoice, bool quiet)
{
    struct p25TxConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.nac = NAC;
    cfg.group = GROUP;
    cfg.source = SOURCE;
    cfg.service = 0x00;

    memset(&res, 0, sizeof(res));
    memset(sentImbe, 0, sizeof(sentImbe));
    lduFed = 0;
    lduCheck = 0;
    expectIdx = 0;
    sentFrames = 0;

    p25Tx_setVoiceSource(withVoice ? voiceSource : NULL, NULL);

    /* The call: HDU, then the voice or signalling frames, then the TDU */
    sent[sentFrames++].duid = P25_DUID_HDU;
    for (int i = 0; i < voiceFrames; i++)
    {
        if (withVoice)
            sent[sentFrames++].duid = (i & 1) ? P25_DUID_LDU2 : P25_DUID_LDU1;
        else
            sent[sentFrames++].duid = P25_DUID_TSBK;
    }
    sent[sentFrames++].duid = P25_DUID_TDU;

    p25Tx_begin(&cfg);

    const size_t data = (size_t)sentFrames * P25_FRAME_SYMBOLS * C4FM_MOD_SPS;
    const size_t lead = 2400;

    callStart = (double)lead;
    frameSamples = (double)P25_FRAME_SYMBOLS * C4FM_MOD_SPS;
    const size_t tail = 2 * (size_t)P25_FRAME_SYMBOLS * C4FM_MOD_SPS;
    int16_t *wave = calloc(lead + data + tail, sizeof(int16_t));

    if (wave == NULL)
        return -1;

    /* Everything but the last frame, then the request to end: the TDU is
     * built when its first symbol is asked for, which is why the call to
     * p25Tx_end() has to come before that frame, not after. */
    const size_t lastFrame = (size_t)P25_FRAME_SYMBOLS * C4FM_MOD_SPS;

    p25Tx_samples(&wave[lead], (int)(data - lastFrame), TX_GAIN);
    p25Tx_end();
    p25Tx_samples(&wave[lead + data - lastFrame], (int)lastFrame, TX_GAIN);

    /* Signal power over the whole burst, then the noise for the wanted SNR in
     * the occupied band (the samples are at 48 kHz, so the noise is spread
     * over 24 kHz and scaled to BAND_HZ). */
    double power = 0.0;

    for (size_t i = lead; i < lead + data; i++)
        power += (double)wave[i] * (double)wave[i];
    power /= (double)data;

    if (snrDb < 90.0)
    {
        const double sigma = sqrt(power / pow(10.0, snrDb / 10.0) * NYQUIST_HZ / BAND_HZ);

        for (size_t i = 0; i < lead + data; i++)
        {
            double v = (double)wave[i] + sigma * gauss();

            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            wave[i] = (int16_t)lrint(v);
        }
    }

    if (p25Tx_frames() != (uint32_t)sentFrames)
        printf("  transmitter built %u frames, the call has %d\n",
               (unsigned)p25Tx_frames(), sentFrames);

    p25Phy_init();
    p25Phy_push(wave, lead + data + tail, onFrame, NULL);

    free(wave);

    if (!quiet)
    {
        printf("snr=%5.1f dB frames=%d/%d nac_bad=%d duid_bad=%d lc_bad=%d "
               "imbe=%d/%d bad tsbk=%d/%d hdu=%d(%d bad) tdu=%d "
               "sync_err=%d bch=%d extra=%d\n",
               snrDb, res.frames, sentFrames, res.nacBad, res.duidBad, res.lcBad,
               res.imbeBad, res.imbeWords, res.tsbkBad, res.tsbk, res.hdu,
               res.hduBad, res.tdu, res.syncErrors, res.bchErrors,
               res.unexpected);
    }

    return res.nacBad + res.duidBad + res.lcBad + res.imbeBad + res.tsbkBad +
           res.hduBad + res.unexpected + ((res.frames < sentFrames) ? 1 : 0);
}

/* ---------------------------------------------------------------- tests --- */

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL: ");                                                  \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/** The FEC encoders against their decoders, with no radio in between. */
static int fecSelftest(void)
{
    for (uint32_t v = 0; v < 4096; v += 37)
    {
        uint32_t cw = p25Fec_golayEncode(v);
        uint32_t w = cw;
        int e = p25Fec_golayDecode(&w);

        CHECK(e == 0 && w == cw, "golay(0x%03X) decoded clean=%d", v, e);
        CHECK((cw >> 11) == v, "golay(0x%03X) data", v);

        w = cw;
        for (int k = 0; k < 3; k++)
            w ^= 1u << (k * 7 + 1);
        e = p25Fec_golayDecode(&w);
        CHECK(e == 3 && w == cw, "golay 3 errors: e=%d", e);
    }

    for (uint32_t v = 0; v < 2048; v += 19)
    {
        uint16_t cw = p25Fec_hamEncode((uint16_t)v);
        uint16_t w = cw;
        int e = p25Fec_hamDecode(&w);

        CHECK(e == 0 && ((uint32_t)(w >> 4) == v), "hamming(0x%03X)", v);
        w = (uint16_t)(cw ^ (1u << 6));
        e = p25Fec_hamDecode(&w);
        CHECK(e == 1 && w == cw, "hamming 1 error: e=%d", e);
    }

    for (uint32_t v = 0; v < 64; v++)
    {
        const uint16_t w = p25Fec_ham10Encode(v);
        const int d = p25Fec_ham10Decode(w >> 4, w & 0xF);

        CHECK(d == (int)v, "ham10(0x%02X) -> %d", v, d);
    }

    for (uint32_t v = 0; v < 65536; v += 251)
    {
        uint64_t cw = p25Fec_bchEncode((uint16_t)v);
        uint64_t w = cw;
        int e = p25Fec_bchDecode(&w);

        CHECK(e == 0 && w == cw, "bch(0x%04X): e=%d", v, e);
        CHECK((cw >> 47) == v, "bch data(0x%04X)", v);

        /* five errors, the correction radius of the code as decoded */
        w = cw ^ (1ull << 3) ^ (1ull << 17) ^ (1ull << 29) ^ (1ull << 41) ^
            (1ull << 55);
        e = p25Fec_bchDecode(&w);
        CHECK(e == 5 && w == cw, "bch 5 errors: e=%d", e);
    }

    for (int k = 12; k <= 20; k += 8)
    {
        const int n = (k == 12) ? 24 : 36;
        uint8_t info[20], cw[36], w[36];

        for (int i = 0; i < k; i++)
            info[i] = (uint8_t)((i * 7 + k) & 0x3F);

        p25Fec_rsEncode(info, k, cw, n);
        CHECK(p25Fec_rsCheck(cw, n, k) == 0, "rs(%d,%d) not a codeword", n, k);
        CHECK(memcmp(cw, info, (size_t)k) == 0, "rs(%d,%d) systematic", n, k);

        memcpy(w, cw, (size_t)n);
        CHECK(p25Fec_rsDecode(w, n, k) == 0, "rs(%d,%d) clean", n, k);

        memcpy(w, cw, (size_t)n);
        w[1] ^= 0x15;
        w[n - 2] ^= 0x3C;
        int e = p25Fec_rsDecode(w, n, k);
        CHECK(e == 2 && memcmp(w, cw, (size_t)n) == 0, "rs(%d,%d) 2 errors: %d",
              n, k, e);
    }

    for (uint32_t v = 0; v < 256; v++)
    {
        uint8_t in[12], bits[196], out[12];

        memset(in, 0, sizeof(in));
        for (int i = 0; i < 10; i++)
            in[i] = (uint8_t)(v * (i + 3) + i);

        /* The CRC field is what makes the block close to zero, i.e. the CRC of
         * the block with the field left at zero (P25Fec.c) */
        const uint16_t crc = p25Fec_crc16(in, 12);
        in[10] = (uint8_t)(crc >> 8);
        in[11] = (uint8_t)(crc & 0xFFu);
        CHECK(p25Fec_crc16(in, 12) == 0, "crc residue");

        p25Fec_trellisEncode(in, bits);
        int e = p25Fec_trellisDecode(bits, out);
        CHECK(e == 0 && memcmp(in, out, 12) == 0, "trellis: e=%d", e);
    }

    printf("fec: %s (%d failures)\n", failures == 0 ? "ok" : "FAILED", failures);

    return failures;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("usage: %s fec|loop|idle|sweep|wave [args]\n", argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "fec") == 0)
        return fecSelftest() == 0 ? 0 : 1;

    if (strcmp(argv[1], "loop") == 0 || strcmp(argv[1], "idle") == 0)
    {
        const bool voice = strcmp(argv[1], "loop") == 0;
        const double snr = (argc > 2) ? atof(argv[2]) : 60.0;
        const int frames = (argc > 3) ? atoi(argv[3]) : (voice ? 18 : 6);

        const int bad = run(snr, frames, voice, false);

        printf("%s: %s\n", argv[1], bad == 0 ? "PASS" : "FAILED");
        return bad == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "sweep") == 0)
    {
        static const double snr[] = { 4, 8, 12, 16, 20, 24 };

        printf("snr_db  frames/sent  errors  detail\n");
        for (unsigned i = 0; i < sizeof(snr) / sizeof(snr[0]); i++)
        {
            const int bad = run(snr[i], 18, true, true);

            printf("%5.0f   %2d/%-2d        %3d     nac=%d duid=%d lc=%d "
                   "imbe=%d/%d tsbk=%d hdu=%d tdu=%d\n",
                   snr[i], res.frames, sentFrames, bad, res.nacBad, res.duidBad,
                   res.lcBad, res.imbeBad, res.imbeWords, res.tsbkBad,
                   res.hduBad, res.tdu);
        }
        return 0;
    }

    if (strcmp(argv[1], "wave") == 0 && argc > 2)
    {
        struct p25TxConfig cfg;
        const int voiceFrames = (argc > 3) ? atoi(argv[3]) : 18;
        const bool idle = (argc > 4) && (strcmp(argv[4], "idle") == 0);

        memset(&cfg, 0, sizeof(cfg));
        cfg.nac = NAC;
        cfg.group = GROUP;
        cfg.source = SOURCE;
        cfg.service = 0x00;

        p25Tx_setVoiceSource(idle ? NULL : voiceSource, NULL);
        p25Tx_begin(&cfg);

        FILE *f = fopen(argv[2], "wb");

        if (f == NULL)
            return 1;

        const size_t lastFrame = (size_t)P25_FRAME_SYMBOLS * C4FM_MOD_SPS;
        const size_t data = (size_t)(1 + voiceFrames) * lastFrame;
        int16_t *buf = calloc(data + 2 * lastFrame, sizeof(int16_t));

        if (buf == NULL)
            return 1;

        p25Tx_samples(buf, (int)(data - lastFrame), TX_GAIN);
        p25Tx_end();
        p25Tx_samples(&buf[data - lastFrame], (int)(3 * lastFrame), TX_GAIN);
        fwrite(buf, sizeof(int16_t), data + 2 * lastFrame, f);
        fclose(f);
        printf("wrote %s: %u frames, %u samples\n", argv[2],
               (unsigned)p25Tx_frames(), (unsigned)(data + 2 * lastFrame));
        return 0;
    }

    return 2;
}
