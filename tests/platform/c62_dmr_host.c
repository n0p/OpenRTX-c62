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
 *   c62_dmr_host -o out.wav -c bursts.csv -a frames.txt input.wav
 *   c62_dmr_host -s tests/unit/assets/dmr_air_bursts.wav   # air self test
 *
 * The input is the discriminator output at 48 kHz (mono s16 WAV), which is
 * what the radio hands the mode; work/dmr-rx/out/discr_*.wav are captures in
 * that form.  The CSV has one row per burst: symbol, sync, voice/data, sync
 * errors, EVM, CACH, colour code, data type, CRC.
 *
 * Two notes on what this tool measures, both learned the hard way:
 *
 *  - The AMBE+2 decoder is stateful (log-gain and pitch prediction, the
 *    synthesis window), so its output level depends on the frames that came
 *    before.  A capture whose bursts are hundreds of ms apart - the sigidwiki
 *    repeater capture has one voice burst every 360 ms - looks 4 times quieter
 *    if the decoder is reset at every gap: each burst then starts from the
 *    decoder's cold fade-in.  The radio never does that: dmrSpeech keeps the
 *    speaker and the decoder open for a second of silence, so only a real
 *    transmission change resets it (DmrSpeech.c, playBurst).  This harness
 *    follows the radio (DMR_NEW_CALL_FRAMES), which is also what DSD and the
 *    host prototype do, and fills the gaps with silence only to keep the file
 *    aligned with air time.
 *  - Frame dumps (-a) are the A/B arbiter against the prototype: the frames
 *    are what the de-interleaver produces, before any decoder state enters.
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

/**
 * Frames of silence after which the decoder state is dropped: 50 frames are
 * one second, the same timeout dmrSpeech uses to release the speaker.
 */
#define DMR_NEW_CALL_FRAMES 50

/** Bursts the air self test expects, and the frames it must produce. */
#define AIR_BURSTS 2
#define AIR_FRAMES (AIR_BURSTS * DMR_FRAMES_PER_BURST)

static FILE *pcmFile;
static FILE *csvFile;
static FILE *dumpFile;
static long pcmWritten;
static long lastFrame = -1;
static unsigned bursts;
static unsigned frames;
static unsigned gapCount;
static unsigned newCallCount;

/* Level of the decoded frames only, so that it can be compared with the
 * prototype irrespective of how much silence the file contains. */
static double audioSumSq;
static long audioSamples;
static long audioPeak;

/* Air self test */
static bool selfTest;
static uint8_t airFrames[AIR_FRAMES][DMR_AMBE_BYTES];
static bool airPresent[AIR_FRAMES];
static unsigned airCount;

/**
 * The frames the reference receiver extracts from
 * tests/unit/assets/dmr_air_bursts.wav: two real over-the-air DMR voice
 * bursts of the sigidwiki repeater capture, de-interleaved by the host
 * prototype (work/dmr-rx) and independently accepted by the AMBE+2 Golay
 * parity of lib/dmr_vocoder (no corrections needed).  A wrong slot order,
 * rW/rX/rY/rZ schedule, EMB/LC position or ambe_layout permutation breaks
 * these bytes, while the synthetic loop cannot: it feeds the generator's own
 * frames back through the same tables.
 */
static const uint8_t airExpected[AIR_FRAMES][DMR_AMBE_BYTES] = {
    { 0xE8, 0x1E, 0x17, 0x9B, 0x17, 0x39, 0xBC, 0xD4, 0xBE },
    { 0xD8, 0x16, 0xCB, 0x6A, 0x18, 0x3A, 0x13, 0x05, 0xB1 },
    { 0xB8, 0x1F, 0x98, 0x64, 0xEB, 0x1D, 0x1C, 0xCD, 0x45 },
    { 0xA8, 0x18, 0x2C, 0x40, 0x31, 0xF6, 0x2E, 0x00, 0x6F },
    { 0xD8, 0x0E, 0x20, 0x32, 0x19, 0x8A, 0x9B, 0x93, 0xE1 },
    { 0xD8, 0x16, 0xCB, 0x66, 0xE0, 0xFC, 0x25, 0x05, 0x4D },
};

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

static void playFrame(const uint8_t frame[DMR_AMBE_BYTES])
{
    int16_t pcm[DMR_VOCODER_PCM_SAMPLES];

    if (dmr_vocoder_decode(frame, pcm) != 0) {
        putSilence(DMR_VOCODER_PCM_SAMPLES);
        return;
    }

    for (int i = 0; i < DMR_VOCODER_PCM_SAMPLES; i++) {
        int v = pcm[i];
        audioSumSq += (double)v * (double)v;
        if (v < 0)
            v = -v;
        if (v > audioPeak)
            audioPeak = v;
    }
    audioSamples += DMR_VOCODER_PCM_SAMPLES;

    if (pcmFile != NULL) {
        fwrite(pcm, sizeof(pcm), 1, pcmFile);
        pcmWritten += DMR_VOCODER_PCM_SAMPLES;
    }
}

static void onBurst(const struct dmrBurst *burst, void *user)
{
    (void)user;
    bursts++;

    if (csvFile != NULL) {
        fprintf(csvFile,
                "%u,%ld,%s,%s,%d,%.4f,%.3f,%.3f,%d,%d,%d,%u,%d,%d,%d,%d,%d,%d,"
                "%d,%d\n",
                bursts, burst->symbol, dmr_sync_name(burst->sync),
                burst->voice ? "V" : "D", burst->syncErrors, burst->evm,
                burst->score, burst->aperture, burst->relaxed ? 1 : 0,
                burst->coasted ? 1 : 0, burst->cachValid, burst->accessType,
                burst->timeslot2, burst->lateEntry, burst->slotTypeValid,
                burst->colourCode, burst->dataType, burst->crcChecked,
                burst->crcOk, burst->frameCount);
    }

    if (dumpFile != NULL) {
        fprintf(dumpFile, "%ld %s %s %d %d", burst->symbol,
                dmr_sync_name(burst->sync), burst->voice ? "V" : "D",
                burst->frameCount, burst->syncErrors);
        for (unsigned f = 0; burst->voice && (f < burst->frameCount); f++) {
            fputc(' ', dumpFile);
            for (int i = 0; i < DMR_AMBE_BYTES; i++)
                fprintf(dumpFile, "%02X", burst->frame[f][i]);
        }
        fputc('\n', dumpFile);
    }

    if ((!burst->voice) || (burst->frameCount == 0))
        return;

    frames += burst->frameCount;

    if (selfTest) {
        for (unsigned f = 0; (f < burst->frameCount) && (airCount < AIR_FRAMES);
             f++) {
            memcpy(airFrames[airCount], burst->frame[f], DMR_AMBE_BYTES);
            airPresent[airCount] = true;
            airCount++;
        }
        return;
    }

    if (pcmFile == NULL)
        return;

    /*
     * Keep the file aligned with air time, but only drop the decoder state
     * for a real transmission change: a shorter gap is a lost burst inside
     * the same call and the AMBE+2 prediction must survive it.
     */
    long frameIndex = burst->symbol / DMR_FRAME_SYMBOLS;
    long gap = 0;

    if (lastFrame >= 0) {
        if (frameIndex < lastFrame)
            return;
        gap = frameIndex - lastFrame - 1;
    }

    if (gap > 0) {
        putSilence(gap * DMR_PCM_PER_BURST);
        gapCount++;
    }

    if ((lastFrame < 0) || (gap >= DMR_NEW_CALL_FRAMES)) {
        if (lastFrame >= 0)
            newCallCount++;
        dmr_vocoder_reset();
    }

    lastFrame = frameIndex;

    for (unsigned f = 0; f < burst->frameCount; f++)
        playFrame(burst->frame[f]);
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

/** Decode a whole capture with the firmware chain. */
static int run(const char *path)
{
    size_t count = 0;
    int16_t *samples = readWav(path, &count);
    if (samples == NULL)
        return -1;

    dmrPhy_init();
    for (size_t offset = 0; offset < count; offset += BLOCK_SAMPLES) {
        size_t n = count - offset;
        if (n > BLOCK_SAMPLES)
            n = BLOCK_SAMPLES;
        dmrPhy_push(samples + offset, n, onBurst, NULL);
    }

    free(samples);
    return 0;
}

/**
 * Real-air regression: the frames the chain extracts from a committed
 * recording must be the ones the reference receiver extracts, and the AMBE+2
 * Golay parity must accept them without corrections.
 */
static int runSelfTest(const char *path)
{
    int rc;

    selfTest = true;
    if (run(path) != 0)
        return -1;
    selfTest = false;

    printf("air self test: %u bursts, %u voice frames\n", bursts, frames);

    if (airCount != AIR_FRAMES) {
        printf("FAIL: expected %d frames, got %u\n", AIR_FRAMES, airCount);
        return -1;
    }

    rc = 0;
    unsigned same = 0;
    unsigned clean = 0;

    for (unsigned i = 0; i < AIR_FRAMES; i++) {
        if (memcmp(airFrames[i], airExpected[i], DMR_AMBE_BYTES) == 0)
            same++;
        else {
            printf("FAIL: frame %u is not the reference frame\n", i);
            rc = -1;
        }

        int16_t pcm[DMR_VOCODER_PCM_SAMPLES];
        dmr_vocoder_reset();
        if ((dmr_vocoder_decode(airFrames[i], pcm) != 0)
            || (dmr_vocoder_last_errors() != 0)) {
            printf("FAIL: frame %u fails the AMBE+2 Golay parity (%u errors)\n",
                   i, dmr_vocoder_last_errors());
            rc = -1;
        } else {
            clean++;
        }
    }

    printf("%s: %u/%d air frames identical to the reference, %u/%d accepted "
           "by the AMBE+2 Golay parity\n", (rc == 0) ? "PASS" : "FAIL", same,
           AIR_FRAMES, clean, AIR_FRAMES);
    return rc;
}

static void printHelp(const char *name)
{
    puts("Host harness for the C62 DMR receive chain.");
    puts("");
    printf("usage: %s [-o out.wav] [-c bursts.csv] [-a frames.txt] [-b samples]"
           " input.wav\n", name);
    printf("       %s -s input.wav\n", name);
    puts("");
    puts("  -o  write the decoded 8 kHz PCM here");
    puts("  -c  write one CSV row per burst here");
    puts("  -a  write the de-interleaved AMBE+2 frames, one burst per line");
    puts("  -b  audio block size, in samples of 48 kHz (default 1440)");
    puts("  -s  real-air self test: decode input.wav and check the frames");
    puts("      against tests/unit/assets/dmr_air_bursts.wav's reference");
    puts("  -h  this help");
    puts("");
    puts("The input is mono signed 16-bit discriminator audio at 48 kHz.");
}

int main(int argc, char **argv)
{
    const char *pcmPath = NULL;
    const char *csvPath = NULL;
    const char *dumpPath = NULL;
    int block = BLOCK_SAMPLES;
    int option;

    while ((option = getopt(argc, argv, "o:c:a:b:sh")) != -1) {
        switch (option) {
            case 'o':
                pcmPath = optarg;
                break;
            case 'c':
                csvPath = optarg;
                break;
            case 'a':
                dumpPath = optarg;
                break;
            case 'b':
                block = atoi(optarg);
                break;
            case 's':
                selfTest = true;
                break;
            case 'h':
                printHelp(argv[0]);
                return 0;
            default:
                printHelp(argv[0]);
                return -1;
        }
    }

    if ((optind >= argc) || (block <= 0) || (selfTest && (pcmPath != NULL))) {
        printHelp(argv[0]);
        return -1;
    }

    if (selfTest)
        return (runSelfTest(argv[optind]) == 0) ? 0 : 1;

    if (pcmPath != NULL) {
        pcmFile = fopen(pcmPath, "wb");
        if (pcmFile == NULL) {
            perror(pcmPath);
            return -1;
        }
        writeWavHeader(pcmFile, 0);
    }
    if (csvPath != NULL) {
        csvFile = fopen(csvPath, "w");
        if (csvFile == NULL) {
            perror(csvPath);
            return -1;
        }
        fputs("burst,symbol,sync,voice,errors,evm,score,aperture,filled,"
              "coasted,cach,at,ts2,late,slotType,cc,type,crcChecked,crcOk,"
              "frames\n", csvFile);
    }
    if (dumpPath != NULL) {
        dumpFile = fopen(dumpPath, "w");
        if (dumpFile == NULL) {
            perror(dumpPath);
            return -1;
        }
    }

    if (run(argv[optind]) != 0)
        return -1;

    double rms = (audioSamples > 0) ? sqrt(audioSumSq / (double)audioSamples) : 0.0;
    printf("bursts %u, voice frames %u, gaps %u, new transmissions %u, "
           "%.2f s of audio, sampling phase %d\n", bursts, frames, gapCount,
           newCallCount, (double)pcmWritten / PCM_RATE, dmrPhy_phase());
    printf("decoded audio: %ld samples, rms %.1f, peak %ld (no silence "
           "included)\n", audioSamples, rms, audioPeak);

    if (pcmFile != NULL) {
        fseek(pcmFile, 0, SEEK_SET);
        writeWavHeader(pcmFile, (uint32_t)pcmWritten);
        fclose(pcmFile);
    }
    if (csvFile != NULL)
        fclose(csvFile);
    if (dumpFile != NULL)
        fclose(dumpFile);

    return 0;
}
