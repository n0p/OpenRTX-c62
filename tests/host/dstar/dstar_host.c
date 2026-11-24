/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dstar_host.c
 * @brief Host harness for the D-STAR DV protocol: FEC self-test, a
 * transmitter -> channel -> receiver loopback with a BER curve, and a file
 * decoder for real captures.
 *
 * Build (from the repository root):
 *   gcc -O2 -std=c11 -Iopenrtx/include -o dstar_host tests/host/dstar/dstar_host.c \
 *       openrtx/src/protocols/DSTAR/{DStarFec,DStarPhy,DStarSlowData,DStarTx}.c -lm
 *
 * Usage:
 *   dstar_host selftest
 *   dstar_host loop [snr_db] [frames] [bt]      (default 30 dB, 100 frames)
 *   dstar_host rx <raw s16le 48 kHz> [text]
 *   dstar_host tx <out.raw> [text]
 */

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/DSTAR/DStarPhy.h"
#include "protocols/DSTAR/DStarSlowData.h"
#include "protocols/DSTAR/DStarTx.h"

static int gFrames;
static int gSyncs;
static struct dstarHeader gHeader;
static int gHeaderOk;
static struct dstarSlow gSlow;
static struct dstarFix gFix;
static char gText[DSTAR_TEXT_LEN + 1];
static int gTextOk;
static int gEot;
static uint8_t gLastVoice[DSTAR_VOICE_BYTES];
static long gBitErrors;
static long gBitTotal;
static int gExpectedFrames;
static uint32_t gVoiceSum;

static void onHeader(const struct dstarHeader *h, int syncErrors, void *user)
{
    (void)user;
    gHeader = *h;
    gHeaderOk++;
    printf("  header: ok  sync_err=%d  flags=%02X%02X%02X  rpt2='%s' rpt1='%s' "
           "ur='%s' my='%s' sfx='%s' viterbi=%d\n",
           syncErrors, h->flags[0], h->flags[1], h->flags[2], h->rpt2, h->rpt1,
           h->ur, h->my, h->sfx, (int)h->viterbi);
}

static void onFrame(const struct dstarPhyFrame *f, void *user)
{
    (void)user;
    gFrames++;
    memcpy(gLastVoice, f->voice, DSTAR_VOICE_BYTES);
    /* Payload BER: the transmitter sends the D-STAR null frame in every slot,
     * so every received voice bit can be checked against a known value.  Frames
     * past the transmission (the demodulator keeps slicing the noise floor) are
     * not part of the measurement. */
    if ((int)f->index >= gExpectedFrames)
        return;
    for (int i = 0; i < DSTAR_VOICE_BYTES; i++) {
        uint8_t x = (uint8_t)(f->voice[i] ^ dstarNullVoice[i]);
        while (x) {
            x &= (uint8_t)(x - 1);
            gBitErrors++;
        }
        gBitTotal += 8;
    }
    gVoiceSum = gVoiceSum * 31u + f->voice[0] * 7u + f->voice[4];
    if (f->sync) {
        gSyncs++;
        return;
    }
    dstarSlow_push(&gSlow, f->slow);

    const char *t = dstarSlow_text(&gSlow);
    if (t != NULL) {
        snprintf(gText, sizeof(gText), "%s", t);
        gTextOk++;
        printf("  text: '%s'\n", gText);
    }

    const char *s = dstarSlow_sentence(&gSlow);
    if (s != NULL) {
        if (dstarFix_parse(s, &gFix)) {
            printf("  gps : '%s'\n        lat=%d.%06d lon=%d.%06d%s\n", s,
                   gFix.lat / 1000000, abs(gFix.lat % 1000000),
                   gFix.lon / 1000000, abs(gFix.lon % 1000000),
                   gFix.fromAprs ? " (D-PRS)" : "");
        }
    }
}

static void onEot(void *user)
{
    (void)user;
    gEot++;
}

static void resetStats(void)
{
    gFrames = 0;
    gSyncs = 0;
    gHeaderOk = 0;
    gEot = 0;
    gTextOk = 0;
    gVoiceSum = 0;
    gBitErrors = 0;
    gBitTotal = 0;
    dstarSlow_reset(&gSlow);
}

static void defaultHeader(struct dstarHeader *h)
{
    memset(h, 0, sizeof(*h));
    h->flags[0] = 0x40; /* voice, via repeater, normal, no interruption */
    strcpy(h->rpt2, "ED1ZAR G");
    strcpy(h->rpt1, "ED1ZAR B");
    strcpy(h->ur, "CQCQCQ");
    strcpy(h->my, "EA5TEST");
    strcpy(h->sfx, "C62");
}

/* ------------------------------------------------------------------ tx --- */

static int16_t *makeTransmission(int voiceFrames, const char *text, int *outLen)
{
    struct dstarTxConfig cfg;
    defaultHeader(&cfg.hdr);
    cfg.text = text;
    cfg.repeatHeader = true;
    cfg.useNullVoice = false;

    dstarTx_begin(&cfg);

    /* bit sync + frame sync + header + frames, then the end pattern */
    int frameSamples = (int)(((int64_t)(64 + 15 + 660 + voiceFrames * 96) * 48000) / 4800) + 64;
    /* The end pattern, then 50 ms of receiver noise floor: a real receiver
     * keeps sampling after the carrier drops, and the demodulator needs a few
     * symbols after the last bit to finish its timing block. */
    int endSamples = (48 * 48000) / 4800 + 4800;
    int totalSamples = frameSamples + endSamples;
    int16_t *buf = calloc((size_t)totalSamples, sizeof(int16_t));
    dstarTx_samples(buf, frameSamples, 24000);
    dstarTx_end();
    dstarTx_samples(buf + frameSamples, endSamples, 24000);
    *outLen = totalSamples;
    return buf;
}

/* ------------------------------------------------------------- self test --- */

static int selfTest(void)
{
    int fails = 0;

    /* 1. header FEC round trip through soft values with bit errors */
    uint8_t hdr[DSTAR_HEADER_BYTES];
    struct dstarHeader h, back;
    defaultHeader(&h);
    dstarHeaderPack(&h, hdr);
    if (!dstarHeaderCheck(hdr)) {
        printf("FAIL: header CRC on a freshly packed header\n");
        fails++;
    }
    uint8_t bits[DSTAR_CODED_BITS];
    dstarHeaderEncode(hdr, bits);
    for (int trial = 0; trial < 3; trial++) {
        int8_t soft[DSTAR_CODED_BITS];
        int errs = (trial == 0) ? 0 : (trial == 1 ? 6 : 20);
        for (int i = 0; i < DSTAR_CODED_BITS; i++)
            soft[i] = (int8_t)(bits[i] ? 96 : -96);
        for (int i = 0; i < errs; i++)
            soft[(i * 37) % DSTAR_CODED_BITS] = (int8_t)-soft[(i * 37) % DSTAR_CODED_BITS];
        bool ok = dstarHeaderDecode(soft, &back);
        bool same = ok && strcmp(back.my, "EA5TEST") == 0 && strcmp(back.rpt1, "ED1ZAR B") == 0 &&
                    strcmp(back.ur, "CQCQCQ") == 0;
        printf("%s: header FEC with %2d bit errors -> %s\n", same ? "ok  " : "FAIL",
               errs, ok ? "CRC ok" : "CRC bad");
        if (!same)
            fails++;
    }

    /* 2. slow data: text and GPS reassembly */
    struct dstarSlow sd;
    dstarSlow_reset(&sd);
    const char *msg = "HOLA DESDE EL C62  ";
    uint8_t blocks[4][DSTAR_SLOW_BLOCK];
    for (int f = 0; f < 4; f++) {
        memset(blocks[f], DSTAR_SLOW_FILL, DSTAR_SLOW_BLOCK);
        blocks[f][0] = (uint8_t)(DSTAR_SLOW_TEXT | f);
        memcpy(blocks[f] + 1, msg + f * 5, 5);
    }
    for (int f = 0; f < 4; f++) {
        dstarSlow_push(&sd, blocks[f]);
        dstarSlow_push(&sd, blocks[f] + 3);
    }
    const char *t = dstarSlow_text(&sd);
    printf("%s: slow-data text -> '%s'\n", (t && strcmp(t, msg) == 0) ? "ok  " : "FAIL",
           t ? t : "(none)");
    if (!t || strcmp(t, msg) != 0)
        fails++;

    /* 3. GPS: a GGA sentence in five-character fragments */
    dstarSlow_reset(&sd);
    /* Build the sentence with a real NMEA checksum */
    char gga[80];
    snprintf(gga, sizeof(gga), "$GPGGA,120000,5437.232,N,02516.782,E,1,08,1.0,112.0,M,,M,,");
    {
        uint8_t ck = 0;
        for (int i = 1; gga[i] != '\0'; i++)
            ck ^= (uint8_t)gga[i];
        int n = (int)strlen(gga);
        snprintf(gga + n, sizeof(gga) - (size_t)n, "*%02X", ck);
    }
    int glen = (int)strlen(gga);
    for (int off = 0; off < glen; off += 5) {
        uint8_t b[DSTAR_SLOW_BLOCK];
        memset(b, DSTAR_SLOW_FILL, sizeof(b));
        int n = (glen - off < 5) ? glen - off : 5;
        b[0] = (uint8_t)(DSTAR_SLOW_GPS | n);
        memcpy(b + 1, gga + off, (size_t)n);
        if (n == 5 && off + 5 <= glen && gga[off + 5] == '$')
            b[0] = (uint8_t)(DSTAR_SLOW_GPS | n);
        dstarSlow_push(&sd, b);
        dstarSlow_push(&sd, b + 3);
    }
    /* terminator so the sentence is handed over */
    {
        uint8_t b[DSTAR_SLOW_BLOCK] = {DSTAR_SLOW_GPS | 5, '\r', '\n', DSTAR_SLOW_FILL,
                                       DSTAR_SLOW_FILL, DSTAR_SLOW_FILL};
        dstarSlow_push(&sd, b);
        dstarSlow_push(&sd, b + 3);
    }
    const char *s = dstarSlow_sentence(&sd);
    struct dstarFix fix;
    bool fixOk = s != NULL && dstarFix_parse(s, &fix);
    printf("%s: slow-data GPS -> '%s'\n", fixOk ? "ok  " : "FAIL", s ? s : "(none)");
    if (fixOk)
        printf("      lat=%d.%06d lon=%d.%06d alt=%d\n", fix.lat / 1000000,
               abs(fix.lat % 1000000), fix.lon / 1000000, abs(fix.lon % 1000000),
               fix.alt);
    else
        fails++;

    return fails;
}

/* -------------------------------------------------------------- loopback --- */

static double gaussNoise(void)
{
    double u1 = (rand() + 1.0) / (RAND_MAX + 2.0);
    double u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static int loopback(double snrDb, int frames, int verbose)
{
    resetStats();
    dstarPhy_reset();

    int len = 0;
    gExpectedFrames = frames;
    int16_t *sig = makeTransmission(frames, "PRUEBA D-STAR C62", &len);

    /* Channel: AWGN at the given SNR in the 6.25 kHz channel, and a timing
     * offset by leaving a fraction of a symbol of silence in front. */
    /* SNR is quoted over the 6.25 kHz channel, against the measured signal
     * power (a GMSK waveform's peak deviation only appears on long runs, so
     * its RMS is far below the shaper's peak).  The noise we add spans 48 kHz:
     * sigma^2 = rms^2 * (48000/6250) / SNR. */
    double sigSq0 = 0;
    for (int i = 0; i < len; i++)
        sigSq0 += (double)sig[i] * sig[i];
    double rms = sqrt(sigSq0 / len);
    double sigma = rms * sqrt(48000.0 / (6250.0 * pow(10.0, snrDb / 10.0)));
    double sum = 0;
    double sigSq = 0, noiseSq = 0;
    for (int i = 0; i < len; i++) {
        /* The last 4800 samples are the receiver's noise-free tail: they let
         * the demodulator finish its timing block without inventing frames. */
        double noise = (i < len - 4800) ? sigma * gaussNoise() : 0.0;
        double v = sig[i] + noise;
        sigSq += (double)sig[i] * sig[i];
        noiseSq += noise * noise;
        sum += v * v;
        int32_t q = (int32_t)lrint(v);
        if (q > 32767)
            q = 32767;
        if (q < -32768)
            q = -32768;
        sig[i] = (int16_t)q;
    }
    printf("  rms signal %.0f, rms noise %.0f (in band %.0f), wideband snr %.1f dB, "
           "in-band %.1f dB\n",
           sqrt(sigSq / len), sqrt(noiseSq / len), sqrt(noiseSq / len * 5000.0 / 48000.0),
           10 * log10(sigSq / noiseSq), 10 * log10(sigSq / noiseSq * 48000.0 / 5000.0));
    (void)sum;

    /* Push in 160-sample blocks like the audio driver does */
    for (int off = 0; off < len; off += 160) {
        int n = (len - off < 160) ? len - off : 160;
        dstarPhy_push(sig + off, (size_t)n, onHeader, onFrame, onEot, NULL);
    }

    free(sig);

    printf("snr=%5.1f dB  header=%d  frames=%d  resync=%d  text=%d  eot=%d  "
           "ber=%.2e (%ld/%ld)  phy=%d\n",
           snrDb, gHeaderOk, gFrames, gSyncs, gTextOk, gEot,
           gBitTotal ? (double)gBitErrors / (double)gBitTotal : 0.0, gBitErrors,
           gBitTotal, dstarPhy_state());
    if (verbose && gHeaderOk > 0)
        printf("           my='%s' text='%s'\n", gHeader.my, gTextOk ? gText : "");
    return gHeaderOk;
}

/* --------------------------------------------------------------- stress --- */

/**
 * @brief Loopback with a clock error, a fractional delay and/or an inverted
 * discriminator, which is what a real receiver adds on top of the noise.
 */
static int stress(double snrDb, double ppm, double fracDelay, int invert, int frames)
{
    resetStats();
    dstarPhy_reset();

    int len = 0;
    gExpectedFrames = frames;
    int16_t *sig = makeTransmission(frames, "PRUEBA D-STAR C62", &len);

    /* Resample by (1 + ppm) with linear interpolation and shift by fracDelay
     * samples, then add noise on the resampled signal. */
    double step = 1.0 + ppm * 1e-6;
    int outLen = (int)((double)len / step);
    int16_t *out = calloc((size_t)outLen, sizeof(int16_t));
    for (int i = 0; i < outLen; i++) {
        double pos = (double)i * step + fracDelay;
        int i0 = (int)pos;
        double f = pos - i0;
        double y0 = (i0 >= 0 && i0 < len) ? sig[i0] : 0;
        double y1 = (i0 + 1 >= 0 && i0 + 1 < len) ? sig[i0 + 1] : 0;
        double v = y0 + (y1 - y0) * f;
        if (invert)
            v = -v;
        out[i] = (int16_t)lrint(v);
    }
    free(sig);

    double sigSq = 0;
    for (int i = 0; i < outLen; i++)
        sigSq += (double)out[i] * out[i];
    double rms = sqrt(sigSq / outLen);
    double sigma = rms * sqrt(48000.0 / (6250.0 * pow(10.0, snrDb / 10.0)));
    srand(1234);
    for (int i = 0; i < outLen; i++) {
        double v = out[i] + sigma * gaussNoise();
        int32_t q = (int32_t)lrint(v);
        if (q > 32767)
            q = 32767;
        if (q < -32768)
            q = -32768;
        out[i] = (int16_t)q;
    }

    for (int off = 0; off < outLen; off += 160) {
        int n = (outLen - off < 160) ? outLen - off : 160;
        dstarPhy_push(out + off, (size_t)n, onHeader, onFrame, onEot, NULL);
    }
    free(out);

    printf("  ppm=%+6.1f delay=%.2f inv=%d snr=%.0f dB -> header=%d frames=%d "
           "resync=%d ber=%.2e\n",
           ppm, fracDelay, invert, snrDb, gHeaderOk, gFrames, gSyncs,
           gBitTotal ? (double)gBitErrors / (double)gBitTotal : 0.0);
    return gHeaderOk;
}

/* ------------------------------------------------------------------ file --- */

static int decodeFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("cannot open %s\n", path);
        return 1;
    }
    resetStats();
    dstarPhy_reset();

    int16_t buf[4800];
    size_t n;
    long total = 0;
    while ((n = fread(buf, sizeof(int16_t), 4800, f)) > 0) {
        dstarPhy_push(buf, n, onHeader, onFrame, onEot, NULL);
        total += (long)n;
    }
    fclose(f);
    printf("%s: %ld samples (%.2f s), headers=%d frames=%d resync=%d text=%d eot=%d\n",
           path, total, (double)total / 48000.0, gHeaderOk, gFrames, gSyncs, gTextOk,
           gEot);
    return gHeaderOk > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s selftest|loop|rx|tx [args]\n", argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "selftest") == 0)
        return selfTest() == 0 ? 0 : 1;

    if (strcmp(argv[1], "loop") == 0) {
        double snr = (argc > 2) ? atof(argv[2]) : 30.0;
        int frames = (argc > 3) ? atoi(argv[3]) : 100;
        return loopback(snr, frames, 1) > 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "stress") == 0) {
        int f = 100;
        printf("clean reference:\n");
        stress(20.0, 0.0, 0.0, 0, f);
        printf("fractional delay (timing offset):\n");
        stress(20.0, 0.0, 1.5, 0, f);
        stress(20.0, 0.0, 0.7, 0, f);
        printf("clock error:\n");
        stress(20.0, 100.0, 0.0, 0, f);
        stress(20.0, -100.0, 0.0, 0, f);
        stress(20.0, 500.0, 0.0, 0, f);
        stress(20.0, -500.0, 0.0, 0, f);
        printf("inverted discriminator:\n");
        stress(20.0, 0.0, 0.0, 1, f);
        printf("worst case, 8 dB:\n");
        stress(8.0, 200.0, 1.3, 0, f);
        stress(8.0, -200.0, 0.4, 1, f);
        return 0;
    }

    if (strcmp(argv[1], "sweep") == 0) {
        int frames = (argc > 2) ? atoi(argv[2]) : 100;
        for (double snr = 3.0; snr <= 15.0; snr += 1.0) {
            int ok = 0;
            int runs = 3;
            for (int r = 0; r < runs; r++)
                ok += loopback(snr, frames, 0) > 0;
            printf("        -> header decoded in %d/%d runs\n", ok, runs);
        }
        return 0;
    }

    if (strcmp(argv[1], "rx") == 0 && argc > 2)
        return decodeFile(argv[2]);

    if (strcmp(argv[1], "tx") == 0 && argc > 2) {
        int len = 0;
        int16_t *sig = makeTransmission(200, argc > 3 ? argv[3] : "TEST D-STAR", &len);
        FILE *f = fopen(argv[2], "wb");
        fwrite(sig, sizeof(int16_t), (size_t)len, f);
        fclose(f);
        free(sig);
        printf("wrote %s: %d samples (%.2f s)\n", argv[2], len, len / 48000.0);
        return 0;
    }

    printf("unknown command\n");
    return 2;
}
