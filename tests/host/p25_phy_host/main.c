/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host harness for the firmware P25 PHY (openrtx/src/protocols/P25/P25Phy.c).
 *
 * Feeds a 48 kHz mono signed 16-bit discriminator capture to the exact code the
 * radio runs, in the block size the C62 audio stream delivers, and prints one
 * line per frame.  It exists to tell a PHY defect from a capture/audio-path
 * defect: the same file is decoded by the host prototype (work/p25-rx), which
 * is the version validated against dsd-neo and op25 off the air.
 *
 * Build and run:
 *   make -C tests/host/p25_phy_host
 *   ./tests/host/p25_phy_host/build/p25_phy_host capture.wav [block]
 *
 * A WAV (16-bit PCM mono) or raw s16le file is accepted.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/P25/P25Defs.h"
#include "protocols/P25/P25Phy.h"

static long frames, ldu, ldu1, ldu2, tsbk, tsbkOk, imbe, imbeBad, imbeErrs;
static long lcValid, syncErrors;

static void onFrame(const struct p25Frame *f, void *user)
{
    (void)user;
    frames++;
    syncErrors += f->syncErrors;
    if (f->duid == P25_DUID_LDU1)
    {
        ldu1++;
        ldu++;
    }
    if (f->duid == P25_DUID_LDU2)
    {
        ldu2++;
        ldu++;
    }
    if (f->duid == P25_DUID_TSBK)
        tsbk++;
    if (f->tsbkValid)
        tsbkOk++;
    if (f->duid == P25_DUID_LDU1 || f->duid == P25_DUID_LDU2)
    {
        imbe += f->voiceCount;
        imbeBad += f->voiceBad;
        imbeErrs += f->imbeErrors;
    }
    if (f->lcValid)
        lcValid++;

    printf("%6ld %-6s NAC=%03X bch=%d sync=%d", f->symbol, p25_duid_name(f->duid),
           f->nac, f->bchErrors, f->syncErrors);
    if (f->lcValid)
        printf(" LC(tg=%u src=%u rs=%d)", f->talkgroup, f->source, f->lcRsErrors);
    if (f->duid == P25_DUID_LDU1 || f->duid == P25_DUID_LDU2)
        printf(" imbe=%u bad=%u errs=%d", f->voiceCount, f->voiceBad,
               f->imbeErrors);
    if (f->tsbkValid)
        printf(" TSBK op=%02X mfid=%02X", f->tsbk[0] & 0x3F, f->tsbk[1]);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s capture.wav|raw [block_samples]\n", argv[0]);
        return 2;
    }

    const size_t block = (argc > 2) ? (size_t)atoi(argv[2]) : 960;
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL)
    {
        perror("fopen");
        return 1;
    }

    /* Skip a RIFF header if there is one: find the "data" chunk */
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) == 12 && memcmp(hdr, "RIFF", 4) == 0)
    {
        for (;;)
        {
            unsigned char ch[8];
            if (fread(ch, 1, 8, f) != 8)
                goto done;
            unsigned len = (unsigned)ch[4] | ((unsigned)ch[5] << 8) |
                           ((unsigned)ch[6] << 16) | ((unsigned)ch[7] << 24);
            if (memcmp(ch, "data", 4) == 0)
                break;
            fseek(f, (long)((len + 1) & ~1u), SEEK_CUR);
        }
    }
    else
    {
        rewind(f);
    }

    static int16_t samples[8192];
    static int16_t silence[17280];     /* two frames of zeros at 48 kHz */
    size_t have = 0;

    p25Phy_init();

    for (;;)
    {
        size_t got = fread(samples + have, sizeof(int16_t), block - have, f);
        have += got;
        if (have == block)
        {
            p25Phy_push(samples, have, onFrame, NULL);
            have = 0;
        }
        if (got == 0)
            break;
    }

    if (have > 0)
        p25Phy_push(samples, have, onFrame, NULL);

    /*
     * The end of a file is not the end of a stream: the last frames of a
     * capture are cut before their last symbol, so the demodulator never gets
     * to slice them.  Feed one frame of silence (what a live radio always has
     * after the transmission) so the tail decodes; without it the last three
     * frames of the control-channel capture are reported missing.
     */
    memset(silence, 0, sizeof(silence));
    p25Phy_push(silence, sizeof(silence) / sizeof(silence[0]), onFrame, NULL);

done:
    fclose(f);

    printf("\n%ld frames: %ld LDU1 %ld LDU2 %ld TSBK | LC %ld | TSBK CRC %ld | "
           "IMBE %ld bad %ld errs %ld | sync errors %ld | synced %d\n",
           frames, ldu1, ldu2, tsbk, lcValid, tsbkOk, imbe, imbeBad, imbeErrs,
           syncErrors, p25Phy_synced() ? 1 : 0);
    return 0;
}
