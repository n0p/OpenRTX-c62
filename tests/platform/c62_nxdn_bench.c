/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Sensitivity bench for the NXDN receive PHY: a real off-air frame becomes a
 * family of captures at known Eb/N0, and the demodulator's counters are read
 * back for each of them.
 *
 * The input is the 48 kHz discriminator asset tests/unit/assets/nxdn_air_frame.wav
 * (a 1.5 s slice of the NXDN48 capture documented in docs/RX_NXDN.md).  Signal
 * power is the mean square of the file with its mean removed; white Gaussian
 * noise is added in the time domain at a variance that puts the result at a
 * chosen Eb/N0:
 *
 *     Eb/N0 = Ps * Fs / (4 * sigma^2 * Rs)   ->   sigma^2 = Ps * Fs / (4 Rs Eb/N0)
 *
 * with Rs = 2400 symbols/s and two bits per symbol, so sigma^2 = 5 * Ps / EbN0
 * at 48 kHz.  The noise is generated from a fixed xorshift64* stream and a
 * Box-Muller pair, so a (Eb/N0, seed) pair is the same file on every machine.
 *
 *   cc -O2 -ffp-contract=off -I openrtx/include tests/platform/c62_nxdn_bench.c \
 *      openrtx/src/protocols/NXDN/{NxdnDefs,NxdnFec,NxdnPhy}.c -lm -o nxdn_bench
 *   ./nxdn_bench tests/unit/assets/nxdn_air_frame.wav
 *   ./nxdn_bench --seeds 8 --csv /tmp/nxdn_bench.csv capture.wav
 *
 * One CSV row per run: Eb/N0, seed, frames the sync detector accepted, frames
 * whose LICH parity passed, LICH failures, SACCH blocks that passed CRC-6,
 * voice frames, AMBE+2 frames, mean EVM of the accepted frames and mean sync
 * error count.  The table on stdout is the per-Eb/N0 mean over the seeds,
 * which is what docs/RX_NXDN.md quotes.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/NXDN/NxdnDefs.h"
#include "protocols/NXDN/NxdnPhy.h"

#define SAMPLE_RATE 48000
#define SYMBOL_RATE NXDN_SYMBOL_RATE
#define BITS_PER_SYMBOL 2

/** Frames of one run's counters. */
struct runResult {
    double ebn0;
    unsigned seed;
    unsigned frames;
    unsigned lichOk;
    unsigned lichBad;
    unsigned sacchOk;
    unsigned voice;
    unsigned ambe;
    double evmMean;
    double syncErrMean;
};

static int wav_read_mono_s16(const char *path, int16_t **out, long *count)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;

    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0
        || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return -1;
    }

    int channels = 0;
    int bits = 0;
    long rate = 0;
    long dataSize = 0;
    long dataOffset = 0;

    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8)
            break;

        uint32_t size = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8)
                      | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);

        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            if (size > sizeof(fmt) || fread(fmt, 1, size, f) != size)
                break;
            channels = fmt[2] | (fmt[3] << 8);
            rate = (long)(fmt[4] | (fmt[5] << 8) | (fmt[6] << 16)
                          | ((uint32_t)fmt[7] << 24));
            bits = fmt[14] | (fmt[15] << 8);
        } else if (memcmp(ch, "data", 4) == 0) {
            dataSize = (long)size;
            dataOffset = ftell(f);
            break;
        } else {
            if (fseek(f, (long)((size + 1u) & ~1u), SEEK_CUR) != 0)
                break;
        }
    }

    if ((channels != 1) || (bits != 16) || (rate != SAMPLE_RATE)
        || (dataSize <= 0)) {
        fprintf(stderr, "%s: need 48 kHz mono s16, got %ld Hz %d ch %d bit\n",
                path, rate, channels, bits);
        fclose(f);
        return -1;
    }

    *count = dataSize / 2;
    *out = malloc((size_t)dataSize);
    if (*out == NULL) {
        fclose(f);
        return -1;
    }

    fseek(f, dataOffset, SEEK_SET);
    if (fread(*out, 2, (size_t)*count, f) != (size_t)*count) {
        free(*out);
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

/* xorshift64* and a Box-Muller pair: a (Eb/N0, seed) pair must give the same
 * noise everywhere, so no library RNG is used. */
static uint64_t rngState;

static uint64_t rng_next(void)
{
    uint64_t x = rngState;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rngState = x;

    return x * 0x2545F4914F6CDD1DULL;
}

static double rng_uniform(void)
{
    /* (0, 1): the log below cannot see zero */
    return ((double)(rng_next() >> 11) + 1.0) / 9007199254740994.0;
}

static double rng_gauss(void)
{
    const double u1 = rng_uniform();
    const double u2 = rng_uniform();

    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static void seed_rng(unsigned ebn0x10, unsigned seed)
{
    rngState = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)ebn0x10 << 32)
             ^ ((uint64_t)seed * 0xBF58476D1CE4E5B9ULL);
    for (int i = 0; i < 8; i++)
        rng_next();
}

/* Per-run accumulators, reset by on_frame(). */
static struct runResult current;
static double evmAcc;
static double syncAcc;
static bool dumpFrames;
static const char *writeDir;

static void on_frame(const struct nxdnFrame *f, void *user)
{
    (void)user;

    current.frames++;
    if (f->lichOk)
        current.lichOk++;
    else
        current.lichBad++;
    if (f->sacchOk)
        current.sacchOk++;
    if (f->frameCount > 0) {
        current.voice++;
        current.ambe += f->frameCount;
    }
    evmAcc += (double)f->evm;
    syncAcc += (double)f->syncErrors;

    if (dumpFrames) {
        printf("  sym %6ld  err %d  score %.3f  evm %.3f  lich %s (%02X)  "
               "sacch %s  vch %d\n", f->symbol, f->syncErrors, (double)f->score,
               (double)f->evm, f->lichOk ? "ok  " : "FAIL",
               f->lich.raw, f->sacchOk ? "ok  " : "FAIL", f->frameCount);
    }
}

/** Write one noisy capture, of the family the reference decoder is run on. */
static void wav_write_mono_s16(const char *path, const int16_t *samples,
                               long count)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }

    uint32_t dataSize = (uint32_t)(count * 2);
    uint32_t riffSize = 36 + dataSize;
    uint32_t fmtSize = 16;
    uint16_t format = 1, channels = 1, bits = 16;
    uint16_t blockAlign = 2;
    uint32_t rate = SAMPLE_RATE;
    uint32_t byteRate = SAMPLE_RATE * 2;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtSize, 4, 1, f);
    fwrite(&format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataSize, 4, 1, f);
    fwrite(samples, 2, (size_t)count, f);
    fclose(f);
}

/* Mean signal power of the file, DC removed. */
static double signal_power(const int16_t *samples, long count)
{
    double sum = 0.0;
    double sumSq = 0.0;

    for (long i = 0; i < count; i++) {
        sum += (double)samples[i];
        sumSq += (double)samples[i] * (double)samples[i];
    }

    const double mean = sum / (double)count;

    return sumSq / (double)count - mean * mean;
}

static struct runResult run_capture(const int16_t *samples, long count,
                                    double power, double ebn0, unsigned seed)
{
    /*
     * sigma^2 = Ps * Fs / (4 Rs Eb/N0): the noise is white over the 48 kHz
     * band, so a variance of sigma^2 means a one-sided density
     * N0 = 2 sigma^2 / Fs, and Eb/N0 = (Ps/Rs) / (2 N0) / 2 bits.
     */
    const double ebn0Lin = pow(10.0, ebn0 / 10.0);
    const double sigma = sqrt(power * (double)SAMPLE_RATE
                              / (4.0 * (double)SYMBOL_RATE * ebn0Lin));

    memset(&current, 0, sizeof(current));
    evmAcc = 0.0;
    syncAcc = 0.0;
    current.ebn0 = ebn0;
    current.seed = seed;

    seed_rng((unsigned)lround(ebn0 * 10.0), seed);

    int16_t *noisy = malloc((size_t)count * sizeof(int16_t));
    if (noisy == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }

    for (long i = 0; i < count; i++) {
        double v = (double)samples[i] + sigma * rng_gauss();

        if (v > 32767.0)
            v = 32767.0;
        if (v < -32768.0)
            v = -32768.0;
        noisy[i] = (int16_t)lround(v);
    }

    if (writeDir != NULL) {
        char path[512];

        snprintf(path, sizeof(path), "%s/nxdn_ebn0_%04d_seed%u.wav", writeDir,
                 (int)lround(ebn0 * 10.0), seed);
        wav_write_mono_s16(path, noisy, count);
    }

    nxdnPhy_init();

    const long block = 960; /* 20 ms, the size the radio hands over */
    for (long off = 0; off < count; off += block) {
        long n = (count - off > block) ? block : (count - off);

        nxdnPhy_push(&noisy[off], (size_t)n, on_frame, NULL);
    }

    free(noisy);

    current.evmMean = (current.frames > 0) ? evmAcc / (double)current.frames : 0.0;
    current.syncErrMean = (current.frames > 0) ? syncAcc / (double)current.frames
                                               : 0.0;

    return current;
}

int main(int argc, char **argv)
{
    const char *wavPath = NULL;
    const char *csvPath = NULL;
    unsigned seeds = 5;
    const char *snrList = "14,12,10,8,6,4,2,0,-2";
    bool clean = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seeds") == 0 && (i + 1) < argc)
            seeds = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--snr") == 0 && (i + 1) < argc)
            snrList = argv[++i];
        else if (strcmp(argv[i], "--csv") == 0 && (i + 1) < argc)
            csvPath = argv[++i];
        else if (strcmp(argv[i], "--no-clean") == 0)
            clean = false;
        else if (strcmp(argv[i], "--dump") == 0)
            dumpFrames = true;
        else if (strcmp(argv[i], "--write-dir") == 0 && (i + 1) < argc)
            writeDir = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        } else
            wavPath = argv[i];
    }

    if (wavPath == NULL) {
        fprintf(stderr,
                "usage: %s [--seeds N] [--snr a,b,c] [--csv out.csv] "
                "[--no-clean] capture.wav\n", argv[0]);
        return 2;
    }

    int16_t *samples = NULL;
    long count = 0;

    if (wav_read_mono_s16(wavPath, &samples, &count) != 0)
        return 1;

    const double power = signal_power(samples, count);
    const double seconds = (double)count / (double)SAMPLE_RATE;

    printf("capture %s: %.2f s, signal power %.1f (%.1f dBFS rms)\n", wavPath,
           seconds, power, 10.0 * log10(power / (32768.0 * 32768.0)));

    FILE *csv = NULL;
    if (csvPath != NULL) {
        csv = fopen(csvPath, "w");
        if (csv == NULL)
            return 1;
        fprintf(csv, "ebn0_db,seed,frames,lich_ok,lich_bad,sacch_ok,voice,"
                     "ambe,evm,sync_errors\n");
    }

    /* The list of Eb/N0 points to sweep: "clean" first, then descending. */
    double snrs[32];
    int nSnr = 0;

    {
        char *copy = strdup(snrList);
        char *tok = strtok(copy, ",");

        while ((tok != NULL) && (nSnr < 32)) {
            snrs[nSnr++] = atof(tok);
            tok = strtok(NULL, ",");
        }
        free(copy);
    }

    printf("\n%s %s %s\n", "Eb/N0", "frames  LICHok  LICHbad  SACCHok "
           " voice  AMBE", "EVM     sync");

    if (clean) {
        struct runResult r = run_capture(samples, count, power, 99.0, 0);

        /* 99 dB is "no noise added" only if the quantization is invisible;
         * at this power it is, and the run is the same as pushing the file. */
        printf("%6s %6u  %6u  %7u  %7u %6u %5u  %5.3f  %4.2f\n", "clean",
               r.frames, r.lichOk, r.lichBad, r.sacchOk, r.voice, r.ambe,
               r.evmMean, r.syncErrMean);
        if (csv != NULL)
            fprintf(csv, "%.1f,%u,%u,%u,%u,%u,%u,%u,%.4f,%.2f\n", 99.0, 0u,
                    r.frames, r.lichOk, r.lichBad, r.sacchOk, r.voice, r.ambe,
                    r.evmMean, r.syncErrMean);
    }

    for (int s = 0; s < nSnr; s++) {
        double acc[7] = { 0 };
        double evm = 0.0;
        double sync = 0.0;

        for (unsigned seed = 0; seed < seeds; seed++) {
            struct runResult r = run_capture(samples, count, power, snrs[s],
                                             seed);

            acc[0] += r.frames;
            acc[1] += r.lichOk;
            acc[2] += r.lichBad;
            acc[3] += r.sacchOk;
            acc[4] += r.voice;
            acc[5] += r.ambe;
            evm += r.evmMean;
            sync += r.syncErrMean;

            if (csv != NULL)
                fprintf(csv, "%.1f,%u,%u,%u,%u,%u,%u,%u,%.4f,%.2f\n", snrs[s],
                        seed, r.frames, r.lichOk, r.lichBad, r.sacchOk,
                        r.voice, r.ambe, r.evmMean, r.syncErrMean);
        }

        printf("%5.1f %6.1f  %6.1f  %7.1f  %7.1f %6.1f %5.1f  %5.3f  %4.2f\n",
               snrs[s], acc[0] / seeds, acc[1] / seeds, acc[2] / seeds,
               acc[3] / seeds, acc[4] / seeds, acc[5] / seeds, evm / seeds,
               sync / seeds);
    }

    if (csv != NULL)
        fclose(csv);

    free(samples);
    return 0;
}
