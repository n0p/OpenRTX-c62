/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host harness for the firmware DMR PHY (openrtx/src/protocols/DMR/DmrPhy.c).
 *
 * Feeds a 48 kHz mono signed 16-bit discriminator capture to the exact code the
 * radio runs, in the block size the C62 audio stream delivers (1440 samples =
 * 30 ms), and prints one line per burst.  It exists to tell a PHY defect from
 * a capture/audio-path defect: the same file is decoded by the host prototype
 * (work/dmr-rx/build/dmr_rx), which is the version validated off the air.
 *
 * Build and run:
 *   make -C tests/host/dmr_phy_host
 *   ./tests/host/dmr_phy_host/build/dmr_phy_host capture.wav [block]
 *
 * A WAV (16-bit PCM mono) or raw s16le file is accepted.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/DMR/DmrDefs.h"
#include "protocols/DMR/DmrPhy.h"

static long bursts, voice, data, cach, golay, crcOk, crcChecked;
static long syncErrors;

static void onBurst(const struct dmrBurst *b, void *user)
{
    (void)user;
    bursts++;
    if (b->voice)
        voice++;
    else
        data++;
    if (b->cachValid)
        cach++;
    if (b->slotTypeValid)
        golay++;
    if (b->crcChecked) {
        crcChecked++;
        if (b->crcOk)
            crcOk++;
    }
    syncErrors += b->syncErrors;

    printf("%6ld sym=%ld %s%s%s err=%d score=%.2f evm=%.2f ap=%.2f "
           "cc=%2u ts=%u %s%s",
           bursts, b->symbol, b->voice ? "VOICE" : "DATA",
           b->relaxed ? "/grid" : "", b->coasted ? "/coast" : "",
           b->syncErrors, b->score, b->evm, b->aperture, b->colourCode,
           b->timeslot2 ? 2u : 1u, b->cachValid ? "cach" : "----",
           b->slotTypeValid ? " st" : " --");
    if (b->crcChecked)
        printf(" crc=%s", b->crcOk ? "OK" : "BAD");
    if (b->voice)
        printf(" frames=%u", b->frameCount);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s capture.wav|raw [block_samples]\n", argv[0]);
        return 2;
    }

    const size_t block = (argc > 2) ? (size_t)atoi(argv[2]) : 1440;
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror("fopen");
        return 1;
    }

    /* Skip a RIFF header if there is one: find the "data" chunk */
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) == 12 && memcmp(hdr, "RIFF", 4) == 0) {
        for (;;) {
            unsigned char ch[8];
            if (fread(ch, 1, 8, f) != 8)
                goto done;
            unsigned len = (unsigned)ch[4] | ((unsigned)ch[5] << 8)
                         | ((unsigned)ch[6] << 16) | ((unsigned)ch[7] << 24);
            if (memcmp(ch, "data", 4) == 0)
                break;
            fseek(f, (long)((len + 1) & ~1u), SEEK_CUR);
        }
    } else {
        rewind(f);
    }

    static int16_t samples[8192];
    size_t have = 0;

    dmrPhy_init();

    for (;;) {
        size_t got = fread(samples + have, sizeof(int16_t), block - have, f);
        have += got;
        if (have == block) {
            dmrPhy_push(samples, have, onBurst, NULL);
            have = 0;
        }
        if (got == 0)
            break;
    }

done:
    fclose(f);

    printf("\n%ld bursts: %ld voice, %ld data | CACH %ld | slot-type %ld | "
           "sync errors %ld | CRC16 %ld/%ld\n",
           bursts, voice, data, cach, golay, syncErrors, crcOk, crcChecked);
    return 0;
}
