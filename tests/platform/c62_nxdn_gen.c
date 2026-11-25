/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Synthetic NXDN48 call generator: preamble, then N frames of a call through
 * the transmitter of this tree (NxdnTx.c + the 4FSK modulator), written as the
 * 48 kHz discriminator WAV the receiver takes.
 *
 *   cc -O2 -I openrtx/include tests/platform/c62_nxdn_gen.c \
 *      openrtx/src/protocols/{Fsk4Tx.c,NXDN/NxdnTx.c,NXDN/NxdnFec.c,
 *                             NXDN/NxdnDefs.c} -lm -o nxdn_gen
 *   ./nxdn_gen -n 150 -o /tmp/call.wav
 *
 * It exists for two measurements: a *locked* capture -- every frame decodes,
 * no acquisition, no dropouts -- for the cost harness, where it is the
 * steady-state regime docs/RX_NXDN.md quotes; and a signal whose payload is
 * known, so a bit error rate can be read off it.
 *
 * The AMBE+2 payload is a counter, one distinctive word per frame and VCH, so
 * a receiver can tell which frame and which VCH a wrong byte came from.
 */

#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/Fsk4Tx.h"
#include "protocols/NXDN/NxdnDefs.h"
#include "protocols/NXDN/NxdnTx.h"

#define SAMPLE_RATE 48000
#define LEVEL_12K5 3189.0f /* outer deviation +-1050 Hz */

static void wav_write(const char *path, const int16_t *samples, long count)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        exit(1);
    }

    uint32_t dataSize = (uint32_t)(count * 2);
    uint32_t riffSize = 36 + dataSize;
    uint32_t fmtSize = 16;
    uint16_t format = 1, channels = 1, bits = 16, blockAlign = 2;
    uint32_t rate = SAMPLE_RATE, byteRate = SAMPLE_RATE * 2;

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

int main(int argc, char **argv)
{
    int frames = 150;
    const char *out = "nxdn_call.wav";
    float level = LEVEL_12K5;
    int opt;

    while ((opt = getopt(argc, argv, "n:o:l:h")) != -1) {
        switch (opt) {
            case 'n':
                frames = atoi(optarg);
                break;
            case 'o':
                out = optarg;
                break;
            case 'l':
                level = (float)atof(optarg);
                break;
            default:
                printf("usage: %s [-n frames] [-o out.wav] [-l level]\n",
                       argv[0]);
                return 1;
        }
    }

    if (frames < 1)
        frames = 1;

    nxdn_tx_t tx;
    fsk4_mod_t mod;
    int8_t levels[NXDN_FRAME_SYMBOLS];
    int16_t audio[NXDN_TX_FRAME_SAMPLES];
    long total = (long)(frames + 1) * NXDN_TX_FRAME_SAMPLES;
    int16_t *pcm = malloc((size_t)total * sizeof(int16_t));

    if (pcm == NULL)
        return 1;

    nxdnTx_setup(&tx, 901, 0, 1, true);
    fsk4Mod_init(&mod, NXDN_TX_SPS, level, false, true);

    long at = 0;

    /* The preamble: one frame of acquisition pattern before the call */
    nxdnTx_preamble(levels);
    fsk4Mod_levels(&mod, levels, NXDN_FRAME_SYMBOLS, &pcm[at]);
    at += NXDN_TX_FRAME_SAMPLES;

    for (int n = 0; n < frames; n++) {
        uint8_t ambe[NXDN_VCH_PER_FRAME][NXDN_AMBE_BYTES];

        for (int v = 0; v < NXDN_VCH_PER_FRAME; v++)
            for (int b = 0; b < NXDN_AMBE_BYTES; b++)
                ambe[v][b] = (uint8_t)((n * 73 + v * 29 + b * 11 + 0x35)
                                       & 0xFF);

        nxdnTx_frame(&tx, ambe, levels);
        fsk4Mod_levels(&mod, levels, NXDN_FRAME_SYMBOLS, audio);
        memcpy(&pcm[at], audio, sizeof(audio));
        at += NXDN_TX_FRAME_SAMPLES;
    }

    wav_write(out, pcm, at);
    printf("%s: %d frames, %.2f s, level %.0f\n", out, frames,
           (double)at / SAMPLE_RATE, (double)level);

    free(pcm);
    return 0;
}
