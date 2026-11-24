/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host harness for the dPMR receive path: feed a discriminator capture through
 * the demodulator and print what it decodes.  Same shape as c62_dmr_host.c.
 *
 * Meson builds it as c62_dpmr_host in the Linux build and runs it on the seven
 * seconds of the real capture kept in tests/unit/assets (dpmr_air_frame.dis,
 * S16LE at 48 kS/s, the second transmission of the DSDcc sample): 40 frames,
 * 37 of them with a valid CCH, 37 voice, one header announcing 00393D called
 * and 000135 own, two END frames, EVM 0.14.  A shorter result fails the test.
 * By hand:
 *
 *   ./build_linux/c62_dpmr_host tests/unit/assets/dpmr_air_frame.dis 1
 *                                                  # second argument: verbose
 *
 * Any S16LE discriminator file works, the 30 s DSDcc capture included.
 *
 * To hear the voice, add lib/dmr_vocoder and write the PCM out as a WAV; the
 * unit test (tests/unit/dpmr_rx.cpp) already decodes every AMBE+2 frame of the
 * capture through the vocoder, so this harness only reports the frames.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/DPMR/DpmrDefs.h"
#include "protocols/DPMR/DpmrPhy.h"

static unsigned frames, cchOk, cchFail, voice, headers, ends, endsBoth;
static unsigned ambeFrames, ambeNonSilent;
static uint32_t calledId, ownId;
static unsigned char mode = 0xFF, ht = 0xFF;
static unsigned char code;
static unsigned codeSeen;
static float evmSum;
static int verbose;

static void onFrame(const struct dpmrFrame *f, void *user)
{
    (void)user;
    frames++;

    if (f->cchOk)
        cchOk++;
    else if (f->sync != DPMR_SYNC_FS1 && f->sync != DPMR_SYNC_FS3)
        cchFail++;

    if (f->headerOk) {
        headers++;
        calledId = f->header.calledId;
        ownId = f->header.ownId;
        mode = f->header.mode;
        ht = f->header.ht;
        if (verbose)
            printf("HI %s HT=%u(%s) CID=%06X OID=%06X M=%u V=%u F=%u CI=%03X "
                   "fixes=%u evm=%.2f\n",
                   f->header.fromHi1 ? "HI1" : "HI0", f->header.ht,
                   dpmrDefs_headerTypeName(f->header.ht), f->header.calledId,
                   f->header.ownId, f->header.mode, f->header.version,
                   f->header.format, f->header.ci, f->fixes, f->evm);
    }

    if (f->endOk) {
        ends++;
        if (f->end.same)
            endsBoth++;
        if (verbose)
            printf("END et=%u arq=%u wait=%u stat=%u crc=%d/%d same=%d evm=%.2f\n",
                   f->end.et, f->end.arq, f->end.wait, f->end.status,
                   (int)f->end.crcOk[0], (int)f->end.crcOk[1],
                   (int)f->end.same, f->evm);
    }

    if (f->sync == DPMR_SYNC_CC) {
        code = (unsigned char)f->channelCode;
        codeSeen++;
    }

    if (f->voice) {
        voice++;
        for (unsigned i = 0; i < DPMR_AMBE_FRAMES; i++) {
            int nz = 0;
            for (int b = 0; b < DPMR_AMBE_BYTES; b++)
                nz |= f->ambe[i][b];
            ambeFrames++;
            if (nz)
                ambeNonSilent++;
        }
        if (verbose)
            printf("VOICE %s fn=%u id=%03X m=%u cc=%03X fixes=%u evm=%.2f "
                   "ambe0=%02X%02X%02X%02X%02X%02X%02X%02X%02X\n",
                   dpmrDefs_syncName(f->sync), f->cch.fn, f->cch.id12,
                   f->cch.mode, f->channelCode, f->fixes, f->evm,
                   f->ambe[0][0], f->ambe[0][1], f->ambe[0][2], f->ambe[0][3],
                   f->ambe[0][4], f->ambe[0][5], f->ambe[0][6], f->ambe[0][7],
                   f->ambe[0][8]);
    }

    evmSum += f->evm;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "samples/dpmr.dis";
    verbose = argc > 2 ? atoi(argv[2]) : 0;

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        perror(path);
        return 1;
    }

    static int16_t buf[960];    /* 20 ms */
    size_t n;
    unsigned blocks = 0, syncBlocks = 0;

    dpmrPhy_init();

    while ((n = fread(buf, sizeof(int16_t), 960, fp)) > 0) {
        dpmrPhy_push(buf, n, onFrame, NULL);
        blocks++;
        if (dpmrPhy_synced())
            syncBlocks++;
    }
    fclose(fp);

    struct dpmrPhyStats s;
    dpmrPhy_stats(&s);

    unsigned total = cchOk + cchFail;

    printf("frames %u cch_ok %u cch_fail %u voice %u headers %u ends %u "
           "(%u both halves)\n", frames, cchOk, cchFail, voice, headers, ends,
           endsBoth);
    printf("crc rate %.1f%%  ambe %u/%u non-silent  evm %.2f  synced %u/%u "
           "blocks\n", 100.0 * cchOk / (double)(total ? total : 1),
           ambeNonSilent, ambeFrames, frames ? evmSum / frames : 0.0,
           syncBlocks, blocks);
    printf("ids: called %06X own %06X HT %u(%s) mode %u cc %03X (%u frames)\n",
           calledId, ownId, ht, dpmrDefs_headerTypeName(ht), mode, code,
           codeSeen);
    printf("phy: frames %u cchOk %u cchFail %u voice %u headers %u ends %u "
           "resyncs %u lost %u evm %.2f\n", s.frames, s.cchOk, s.cchFail,
           s.voice, s.headers, s.ends, s.resyncs, s.syncLost, s.evm);

    /*
     * Exit status for the build: `meson test` runs this harness on the seven
     * seconds of the DSDcc capture kept in tests/unit/assets, and a capture
     * that stops decoding has to fail the run, not just print smaller numbers.
     * The floors are what that file gives; see the header.
     */
    bool ok = (frames >= 38) && (cchOk >= 35) && (voice >= 35)
              && (headers == 1) && (ends >= 2) && (calledId == 0x00393D)
              && (ownId == 0x000135);

    return ok ? 0 : 1;
}
