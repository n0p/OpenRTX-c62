/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost harness for the NXDN and dPMR transmit chains: bare metal on QEMU
 * (mps2-an505, Cortex M33), one frame marker per transmitted frame,
 * instructions counted by the TCG plugin of work/dsp-probe/common/insnframe.c.
 *
 * What it measures is the per-frame work the mode does in the rtx thread
 * besides the vocoder: the frame builder (LICH, SACCH with its punctured
 * convolutional code, the four VCH packings for NXDN; CCH, HI and TCH for
 * dPMR) plus the 4FSK modulator over the whole frame, which is the polyphase
 * root raised cosine at 20 samples per symbol.  The AMBE+2 encoder is not in
 * here: it has its own figure (1.43 M instructions per 20 ms frame, see
 * docs/DMR_PKG.md) and NXDN and dPMR use it unchanged.
 *
 * tests/platform/run_tx_4fsk_cost.sh builds it and reports instructions per
 * frame; it skips (exit 77) without arm-none-eabi-gcc or qemu-system-arm.
 *
 *   --mode nxdn : preamble + 40 voice frames, 80 ms each
 *   --mode dpmr : 2 preamble frames + header + 40 payload frames + END frame
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "protocols/Fsk4Tx.h"
#include "protocols/NXDN/NxdnTx.h"
#include "protocols/DPMR/DpmrTx.h"

extern void initialise_monitor_handles(void);

#ifdef MARK_FRAMES
__attribute__((noinline, used)) void frame_marker(void);
void frame_marker(void) { __asm__ volatile("" ::: "memory"); }
#define MARK() frame_marker()
#else
#define MARK() do { } while (0)
#endif

/** One 80 ms frame of baseband at 48 kHz, the largest the two modes build. */
static int16_t audio[192 * 20];
static int8_t levels[192];

static volatile uint32_t sink;

static void nxdnRun(int frames)
{
    static fsk4_mod_t mod;
    nxdn_tx_t tx;
    uint8_t ambe[4][9];

    for (int v = 0; v < 4; v++)
        for (int b = 0; b < 9; b++)
            ambe[v][b] = (uint8_t)(v * 17 + b * 7 + 0x21);

    nxdnTx_setup(&tx, 0x0123, 0x4567, 17, true);
    fsk4Mod_init(&mod, NXDN_TX_SPS, 3189.0f, true, true);

    MARK();   /* preamble is part of the call, not per frame */
    nxdnTx_preamble(levels);
    fsk4Mod_levels(&mod, levels, 192, audio);

    for (int f = 0; f < frames; f++) {
        nxdnTx_frame(&tx, (const uint8_t (*)[9])ambe, levels);
        fsk4Mod_levels(&mod, levels, 192, audio);
        MARK();
        sink += (uint32_t)audio[0] + (uint32_t)audio[1919];
    }
}

static void dpmrRun(int frames)
{
    static fsk4_mod_t mod;
    dpmr_tx_t tx;
    uint8_t ambe[4][9];

    for (int v = 0; v < 4; v++)
        for (int b = 0; b < 9; b++)
            ambe[v][b] = (uint8_t)(v * 17 + b * 7 + 0x21);

    dpmrTx_setup(&tx, 0x000135, 0x00393D, 0x0DD);
    fsk4Mod_init(&mod, DPMR_TX_SPS, 3189.0f, true, false);

    MARK();
    dpmrTx_preamble(levels);
    fsk4Mod_levels(&mod, levels, 192, audio);
    MARK();
    dpmrTx_header(&tx, DPMR_HT_START, 0x123, levels);
    fsk4Mod_levels(&mod, levels, 192, audio);

    for (int f = 0; f < frames; f++) {
        dpmrTx_frame(&tx, (const uint8_t (*)[9])ambe, levels);
        fsk4Mod_levels(&mod, levels, 192, audio);
        MARK();
        sink += (uint32_t)audio[0] + (uint32_t)audio[1919];
    }

    MARK();
    dpmrTx_end(&tx, -1, levels);
    fsk4Mod_levels(&mod, nullptr, 0, audio);
}

int main(int argc, char **argv)
{
    initialise_monitor_handles();

    /* The mode is a compile-time choice: semihosting passes no arguments */
#ifdef TX4FSK_DPMR
    const int nxdn = 0;
#else
    const int nxdn = 1;
#endif
    const int frames = 40;

    (void)argc;
    (void)argv;

    if (nxdn) {
        nxdnRun(frames);
        printf("tx cost: nxdn %d frames of 80 ms, sink %u\n", frames,
               (unsigned)sink);
    } else {
        dpmrRun(frames);
        printf("tx cost: dpmr %d frames of 80 ms, sink %u\n", frames,
               (unsigned)sink);
    }

    return 0;
}
