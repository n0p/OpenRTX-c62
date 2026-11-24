/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host harness for the C62 NXDN receive chain: runs NxdnPhy.c and the
 * firmware AMBE+2 decoder (lib/dmr_vocoder) on 48 kHz discriminator audio, the
 * way OpMode_NXDN does, and writes the decoded 8 kHz PCM.
 *
 *   cmake -S lib/dmr_vocoder -B build_dmr_vocoder
 *   cmake --build build_dmr_vocoder
 *   cc -O2 -I openrtx/include -I lib/dmr_vocoder tests/platform/c62_nxdn_host.c
 *      openrtx/src/protocols/NXDN/{NxdnDefs,NxdnFec,NxdnPhy}.c
 *      openrtx/src/protocols/DMR/DmrDefs.c          # the AMBE+2 bit schedule
 *      build_dmr_vocoder/libdmr_vocoder.a -lm -o c62_nxdn_host
 *   c62_nxdn_host -o out.wav -c frames.csv -a frames.txt -m msgs.txt input.wav
 *   c62_nxdn_host -s tests/unit/assets/nxdn_air_frame.wav   # air self test
 *
 * The input is the discriminator output at 48 kHz (mono s16 WAV).  The CSV has
 * one row per frame: symbol, sync errors, score, EVM, LICH, VCHs of voice,
 * SACCH CRC, message type, source and destination.
 *
 * The AMBE+2 decoder is stateful, so as in the DMR harness the state is kept
 * across gaps shorter than a second (NXDN_NEW_CALL_FRAMES) and the gaps are
 * filled with silence only to keep the file aligned with air time.
 */

#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmr_vocoder.h"
#include "protocols/NXDN/NxdnDefs.h"
#include "protocols/NXDN/NxdnFec.h"
#include "protocols/NXDN/NxdnPhy.h"

#define SAMPLE_RATE 48000
#define PCM_RATE DMR_VOCODER_SAMPLE_RATE
#define BLOCK_SAMPLES 960 /* 20 ms */

/** Frames of silence after which the decoder state is dropped (1 s). */
#define NXDN_NEW_CALL_FRAMES 50

/** Frames the air self test expects, and the AMBE frames it must produce. */
#define AIR_FRAMES_MIN 40

static FILE *pcmFile;
static FILE *csvFile;
static FILE *dumpFile;
static FILE *msgFile;
static FILE *bitsFile;
static FILE *errFile;
static long pcmWritten;
static long lastFrameSymbol = -1;
static unsigned frameCount;
static unsigned ambeCount;
static unsigned voiceFrames;
static unsigned sacchOk;
static unsigned lichBad;
static unsigned invertedFrames;
static unsigned msgVcall;
static unsigned superframeSeg[4];
static unsigned golayHist[8];
static unsigned silentRun;

/* The 72 bits of a superframe's SACCH message, 18 per frame. */
static uint8_t sacchBits[72];
static bool sacchHave[NXDN_SUPERFRAME];

static double audioSumSq;
static long audioSamples;
static int16_t audioPeak;

/* ------------------------------------------------------------------ */
/* WAV                                                               */
/* ------------------------------------------------------------------ */

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

static void wav_header(FILE *f, long samples)
{
    uint32_t dataSize = (uint32_t)(samples * 2);
    uint32_t riffSize = 36 + dataSize;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riffSize, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmtSize = 16;
    uint16_t format = 1;
    uint16_t channels = 1;
    uint16_t bits = 16;
    uint32_t rate = PCM_RATE;
    uint32_t byteRate = PCM_RATE * 2;
    uint16_t blockAlign = 2;

    fwrite(&fmtSize, 4, 1, f);
    fwrite(&format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&blockAlign, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataSize, 4, 1, f);
}

/* ------------------------------------------------------------------ */
/* Audio out                                                          */
/* ------------------------------------------------------------------ */

static void emit_pcm(const int16_t *pcm, int n)
{
    if (pcmFile != NULL)
        fwrite(pcm, 2, (size_t)n, pcmFile);

    for (int i = 0; i < n; i++) {
        int16_t v = pcm[i];

        audioSumSq += (double)v * (double)v;
        if (abs(v) > audioPeak)
            audioPeak = (int16_t)abs(v);
    }

    audioSamples += n;
    pcmWritten += n;
}

/** Silence for the air time between two frames. */
static void pad_to(long symbol)
{
    if (lastFrameSymbol < 0) {
        lastFrameSymbol = symbol;
        return;
    }

    long gap = symbol - lastFrameSymbol;

    if (gap <= 16) {
        lastFrameSymbol = symbol;
        return;
    }

    if (gap > (long)NXDN_FRAME_SYMBOLS * NXDN_NEW_CALL_FRAMES) {
        /* a new transmission ends the previous one */
        dmr_vocoder_reset();
        memset(sacchHave, 0, sizeof(sacchHave));
        silentRun = NXDN_NEW_CALL_FRAMES;
    }

    /* symbols are 20 ms of voice each; fill the hole with silence */
    long samples = (gap - 16) * PCM_RATE / (long)NXDN_SYMBOL_RATE;

    if (samples > (long)PCM_RATE * 2)
        samples = (long)PCM_RATE * 2;

    while (samples > 0) {
        int16_t zeros[160];

        memset(zeros, 0, sizeof(zeros));
        int n = (samples > 160) ? 160 : (int)samples;

        emit_pcm(zeros, n);
        samples -= n;
    }

    lastFrameSymbol = symbol;
}

/* ------------------------------------------------------------------ */
/* Frame callback                                                     */
/* ------------------------------------------------------------------ */

static void on_frame(const struct nxdnFrame *f, void *user)
{
    (void)user;

    frameCount++;
    if (f->inverted)
        invertedFrames++;
    if (!f->lichOk) {
        lichBad++;
        return;
    }

    if (csvFile != NULL) {
        fprintf(csvFile,
                "%ld,%d,%.3f,%.3f,%02X,%d,%d,%d,%d,%d\n",
                f->symbol, f->syncErrors, (double)f->score, (double)f->evm,
                f->lich.raw, f->lich.rfType, f->lich.usc, f->voiceCount,
                f->sacchOk ? 1 : 0, f->frameCount);
    }

    /* Superframe assembly: 18 layer 3 bits per frame, position in bits 0..1
     * of the SR field. */
    if (f->sacchOk) {
        /* The 2-bit superframe position in the SR field counts down: 3 is the
         * first frame of the superframe (NXDN TS 1-A 4.1.1). */
        int part = 3 - ((f->sacchSr >> 6) & 0x03);

        sacchOk++;
        superframeSeg[part]++;
        memcpy(&sacchBits[part * 18], f->sacchBits, 18);
        sacchHave[part] = true;

        if ((part == 3) && sacchHave[0] && sacchHave[1] && sacchHave[2]
            && sacchHave[3]) {
            struct nxdnSacchMessage msg;

            nxdnFecSacchParse(sacchBits, &msg);
            msg.valid = true;

            if (msgFile != NULL) {
                fprintf(msgFile, "%s src=%u dst=%u type=%02X octets=",
                        nxdnFecMsgName(msg.type), msg.source, msg.dest,
                        msg.type);
                for (int o = 0; o < 9; o++)
                    fprintf(msgFile, "%02X", msg.octets[o]);
                fprintf(msgFile, "\n");
            }

            if (msg.type == NXDN_MSG_VCALL)
                msgVcall++;

            memset(sacchHave, 0, sizeof(sacchHave));
        }
    }

    if (bitsFile != NULL) {
        fprintf(bitsFile, "%ld %d ", f->symbol, f->sacchOk);
        for (int i = 0; i < NXDN_SACCH_BITS; i++) {
            int dibit = f->dibits[NXDN_OFF_SACCH + (i >> 1)];
            fprintf(bitsFile, "%d", (i & 1) ? (dibit & 1) : ((dibit >> 1) & 1));
        }
        fprintf(bitsFile, "\n");
    }

    if (dumpFile != NULL) {
        fprintf(dumpFile, "%ld", f->symbol);
        for (int v = 0; v < f->frameCount; v++) {
            fprintf(dumpFile, " ");
            for (int b = 0; b < NXDN_AMBE_BYTES; b++)
                fprintf(dumpFile, "%02X", f->frame[v][b]);
        }
        fprintf(dumpFile, "\n");
    }

    pad_to(f->symbol);

    if (f->frameCount == 0)
        return;

    voiceFrames++;

    for (int v = 0; v < f->frameCount; v++) {
        int16_t pcm[DMR_VOCODER_PCM_SAMPLES];

        if (dmr_vocoder_decode(f->frame[v], pcm) == 0) {
            unsigned e = dmr_vocoder_last_errors();
            if (e < 8)
                golayHist[e]++;
            if (errFile != NULL) {
                fprintf(errFile, "%ld %u", f->symbol, e);
                for (int b = 0; b < NXDN_AMBE_BYTES; b++)
                    fprintf(errFile, " %02X", f->frame[v][b]);
                fprintf(errFile, "\n");
            }
            ambeCount++;
            emit_pcm(pcm, DMR_VOCODER_PCM_SAMPLES);
        }
    }

    silentRun = 0;
    lastFrameSymbol = f->symbol + (long)NXDN_FRAME_SYMBOLS;
}

/* ------------------------------------------------------------------ */
/* Self test                                                          */
/* ------------------------------------------------------------------ */

static int self_test(const char *path)
{
    int16_t *samples = NULL;
    long count = 0;

    if (wav_read_mono_s16(path, &samples, &count) != 0)
        return 1;

    nxdnPhy_init();
    frameCount = 0;
    ambeCount = 0;

    for (long i = 0; i < count; i += BLOCK_SAMPLES) {
        long n = (count - i > BLOCK_SAMPLES) ? BLOCK_SAMPLES : (count - i);

        nxdnPhy_push(&samples[i], (size_t)n, on_frame, NULL);
    }

    free(samples);

    printf("self test: %u frames, %u with voice, %u AMBE frames, "
           "%u SACCH ok, %u LICH errors\n",
           frameCount, voiceFrames, ambeCount, sacchOk, lichBad);

    if ((frameCount < 2u) || (ambeCount < AIR_FRAMES_MIN)) {
        printf("self test FAILED\n");
        return 1;
    }

    printf("self test OK\n");
    return 0;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *outPath = NULL;
    const char *csvPath = NULL;
    const char *dumpPath = NULL;
    const char *msgPath = NULL;
    const char *testPath = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "o:c:a:m:s:h")) != -1) {
        switch (opt) {
        case 'o':
            outPath = optarg;
            break;
        case 'c':
            csvPath = optarg;
            break;
        case 'a':
            dumpPath = optarg;
            break;
        case 'm':
            msgPath = optarg;
            break;
        case 's':
            testPath = optarg;
            break;
        default:
            printf("usage: %s [-o out.wav] [-c frames.csv] [-a frames.txt]\n"
                   "          [-m msgs.txt] [-s air.wav] input.wav\n", argv[0]);
            return 1;
        }
    }

    if (testPath != NULL)
        return self_test(testPath);

    if (optind >= argc) {
        fprintf(stderr, "no input file\n");
        return 1;
    }

    int16_t *samples = NULL;
    long count = 0;

    if (wav_read_mono_s16(argv[optind], &samples, &count) != 0)
        return 1;

    if (outPath != NULL) {
        pcmFile = fopen(outPath, "wb");
        if (pcmFile == NULL)
            return 1;
        wav_header(pcmFile, 0);
    }
    if (csvPath != NULL)
        csvFile = fopen(csvPath, "w");
    if (dumpPath != NULL)
        dumpFile = fopen(dumpPath, "w");
    if (msgPath != NULL)
        msgFile = fopen(msgPath, "w");
    /* Debug dumps used to validate the SACCH chain against the reference
     * decoder: the 60 raw bits of every block, and the Golay error count of
     * every decoded frame next to its 9 bytes. */
    if (getenv("NXDN_DUMP") != NULL) {
        bitsFile = fopen("/tmp/sacch_bits.txt", "w");
        errFile = fopen("/tmp/frames_err.txt", "w");
    }

    nxdnPhy_init();

    for (long i = 0; i < count; i += BLOCK_SAMPLES) {
        long n = (count - i > BLOCK_SAMPLES) ? BLOCK_SAMPLES : (count - i);

        nxdnPhy_push(&samples[i], (size_t)n, on_frame, NULL);
    }

    free(samples);

    if (pcmFile != NULL) {
        fseek(pcmFile, 0, SEEK_SET);
        wav_header(pcmFile, pcmWritten);
        fclose(pcmFile);
    }
    if (csvFile != NULL)
        fclose(csvFile);
    if (dumpFile != NULL)
        fclose(dumpFile);
    if (msgFile != NULL)
        fclose(msgFile);
    if (bitsFile != NULL)
        fclose(bitsFile);
    if (errFile != NULL)
        fclose(errFile);

    double rms = (audioSamples > 0) ? sqrt(audioSumSq / (double)audioSamples)
                                    : 0.0;

    printf("frames %u (inverted %u, LICH errors %u), voice frames %u, "
           "AMBE %u, SACCH ok %u\n",
           frameCount, invertedFrames, lichBad, voiceFrames, ambeCount,
           sacchOk);
    printf("superframe segments 1..4: %u %u %u %u, VCALL messages %u\n",
           superframeSeg[0], superframeSeg[1], superframeSeg[2],
           superframeSeg[3], msgVcall);
    printf("Golay corrections per frame 0..7: %u %u %u %u %u %u %u %u\n",
           golayHist[0], golayHist[1], golayHist[2], golayHist[3],
           golayHist[4], golayHist[5], golayHist[6], golayHist[7]);
    printf("PCM %ld samples (%.2f s) RMS %.0f peak %d\n", pcmWritten,
           (double)pcmWritten / PCM_RATE, rms, audioPeak);

    return 0;
}
