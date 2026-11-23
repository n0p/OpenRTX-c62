/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host harness for the C62 DMR receive chain: runs DmrPhy.c and the firmware
 * AMBE+2 decoder (lib/dmr_vocoder) on 48 kHz discriminator audio, the way
 * OpMode_DMR does, and writes the decoded 8 kHz PCM.
 *
 *   cmake -S lib/dmr_vocoder -B build_dmr_vocoder
 *   cmake --build build_dmr_vocoder
 *   cc -O2 -I openrtx/include -I lib/dmr_vocoder tests/platform/c62_dmr_host.c
 *      openrtx/src/protocols/DMR/DmrDefs.c openrtx/src/protocols/DMR/DmrFec.c
 *      openrtx/src/protocols/DMR/DmrPhy.c build_dmr_vocoder/libdmr_vocoder.a
 *      -lm -o c62_dmr_host
 *   c62_dmr_host -o out.wav -c bursts.csv input.wav
 *
 * The input is the discriminator output at 48 kHz (mono s16 WAV), which is
 * what the radio hands the mode; work/dmr-rx/out/discr_*.wav are captures in
 * that form.  The CSV has one row per burst: symbol, sync, voice/data, sync
 * errors, EVM, CACH, colour code, data type, CRC.
 */

#include <getopt.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dmr_vocoder.h"
#include "protocols/DMR/DmrDefs.h"
#include "protocols/DMR/DmrPhy.h"

#define SAMPLE_RATE 48000
#define PCM_RATE DMR_VOCODER_SAMPLE_RATE
#define BLOCK_SAMPLES 1440 /* 30 ms, the mode's audio block */

static FILE *pcmFile;
static FILE *csvFile;
static long pcmWritten;
static long lastFrame = -1;
static unsigned bursts;
static unsigned frames;
static unsigned gapCount;

static void putSilence(long samples)
{
    static const int16_t zero[160] = { 0 };

    while (samples > 0) {
        long n = (samples > 160) ? 160 : samples;
        if (pcmFile != NULL) {
            fwrite(zero, sizeof(int16_t), (size_t)n, pcmFile);
            pcmWritten += n;
        }
        samples -= n;
    }
}

static void onBurst(const struct dmrBurst *burst, void *user)
{
    (void)user;
    bursts++;

    if (csvFile != NULL) {
        fprintf(csvFile, "%u,%ld,%s,%s,%d,%.4f,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d\n",
                bursts, burst->symbol, dmr_sync_name(burst->sync),
                burst->voice ? "V" : "D", burst->syncErrors, burst->evm,
                burst->cachValid, burst->accessType, burst->timeslot2,
                burst->lateEntry, burst->slotTypeValid, burst->colourCode,
                burst->dataType, burst->crcChecked, burst->crcOk,
                burst->frameCount);
    }

    if ((!burst->voice) || (burst->frameCount == 0))
        return;

    frames += burst->frameCount;
    if (pcmFile == NULL)
        return;

    long frameIndex = burst->symbol / DMR_FRAME_SYMBOLS;

    if (lastFrame < 0) {
        lastFrame = frameIndex;
        dmr_vocoder_reset();
    } else if (frameIndex > lastFrame + 1) {
        putSilence((frameIndex - lastFrame - 1) * DMR_PCM_PER_BURST);
        gapCount++;
        dmr_vocoder_reset();
    } else if (frameIndex < lastFrame) {
        return;
    }
    lastFrame = frameIndex;

    for (unsigned f = 0; f < burst->frameCount; f++) {
        int16_t pcm[DMR_VOCODER_PCM_SAMPLES];

        if (dmr_vocoder_decode(burst->frame[f], pcm) != 0) {
            putSilence(DMR_VOCODER_PCM_SAMPLES);
            continue;
        }
        fwrite(pcm, sizeof(pcm), 1, pcmFile);
        pcmWritten += DMR_VOCODER_PCM_SAMPLES;
    }
}

static void writeWavHeader(FILE *f, uint32_t samples)
{
    uint32_t data = samples * 2;
    uint32_t riff = 36 + data;
    uint32_t rate = PCM_RATE;
    uint32_t byteRate = PCM_RATE * 2;
    uint16_t format = 1, channels = 1, align = 2, bits = 16;
    uint32_t fmtLen = 16;

    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtLen, 4, 1, f);
    fwrite(&format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
}

/**
 * Read a mono 48 kHz signed 16-bit WAV.  Assumes the canonical 44-byte
 * header, which is what the baseband dumps and the prototype write.
 */
static int16_t *readWav(const char *path, size_t *count)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return NULL;
    }

    uint8_t header[44];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return NULL;
    }

    unsigned channels = header[22] | (header[23] << 8);
    unsigned rate = header[24] | (header[25] << 8) | (header[26] << 16)
                  | (header[27] << 24);
    if ((memcmp(header, "RIFF", 4) != 0) || (memcmp(header + 8, "WAVE", 4) != 0)
        || (channels != 1) || (rate != SAMPLE_RATE)) {
        fprintf(stderr, "%s: need a mono 48 kHz WAV\n", path);
        fclose(f);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f) - (long)sizeof(header);
    fseek(f, (long)sizeof(header), SEEK_SET);

    *count = (size_t)(size / 2);
    int16_t *samples = malloc(*count * sizeof(int16_t));
    if ((samples == NULL) || (fread(samples, 2, *count, f) != *count)) {
        free(samples);
        fclose(f);
        return NULL;
    }

    fclose(f);
    return samples;
}

static void printHelp(const char *name)
{
    puts("Host harness for the C62 DMR receive chain.");
    puts("");
    printf("usage: %s [-o out.wav] [-c bursts.csv] [-b samples] input.wav\n",
           name);
    puts("");
    puts("  -o  write the decoded 8 kHz PCM here");
    puts("  -c  write one CSV row per burst here");
    puts("  -b  audio block size, in samples of 48 kHz (default 1440)");
    puts("  -h  this help");
    puts("");
    puts("The input is mono signed 16-bit discriminator audio at 48 kHz.");
}

int main(int argc, char **argv)
{
    const char *pcmPath = NULL;
    const char *csvPath = NULL;
    int block = BLOCK_SAMPLES;
    int option;

    while ((option = getopt(argc, argv, "o:c:b:h")) != -1) {
        switch (option) {
            case 'o':
                pcmPath = optarg;
                break;
            case 'c':
                csvPath = optarg;
                break;
            case 'b':
                block = atoi(optarg);
                break;
            case 'h':
                printHelp(argv[0]);
                return 0;
            default:
                printHelp(argv[0]);
                return -1;
        }
    }

    if ((optind >= argc) || (block <= 0)) {
        printHelp(argv[0]);
        return -1;
    }

    size_t count = 0;
    int16_t *samples = readWav(argv[optind], &count);
    if (samples == NULL)
        return -1;

    if (pcmPath != NULL) {
        pcmFile = fopen(pcmPath, "wb");
        if (pcmFile == NULL) {
            perror(pcmPath);
            free(samples);
            return -1;
        }
        writeWavHeader(pcmFile, 0);
    }
    if (csvPath != NULL) {
        csvFile = fopen(csvPath, "w");
        if (csvFile == NULL) {
            perror(csvPath);
            free(samples);
            return -1;
        }
        fputs("burst,symbol,sync,voice,errors,evm,cach,at,ts2,late,"
              "slotType,cc,type,crcChecked,crcOk,frames\n",
              csvFile);
    }

    dmrPhy_init();
    for (size_t offset = 0; offset < count; offset += (size_t)block) {
        size_t n = count - offset;
        if (n > (size_t)block)
            n = (size_t)block;
        dmrPhy_push(samples + offset, n, onBurst, NULL);
    }

    printf("bursts %u, voice frames %u, gaps %u, %.2f s of audio\n", bursts,
           frames, gapCount, (double)pcmWritten / PCM_RATE);

    if (pcmFile != NULL) {
        fseek(pcmFile, 0, SEEK_SET);
        writeWavHeader(pcmFile, (uint32_t)pcmWritten);
        fclose(pcmFile);
    }
    if (csvFile != NULL)
        fclose(csvFile);

    free(samples);
    return 0;
}
