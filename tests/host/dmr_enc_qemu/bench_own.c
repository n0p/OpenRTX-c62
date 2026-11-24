/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost of one transmitted voice burst (30 ms slot) with our vocoders:
 * three frames through dmrEnc_encode() (lib/dmr_encoder), dmr_tx_voice() and
 * the 4FSK modulator of the package.  Bare metal on QEMU's mps2-an505, with
 * every executed instruction counted by the TCG plugin (insn_count.c), as
 * enc_bench.c does for OpenDMR's encoder alone.
 *
 * Built and run by run_own.sh, three ELFs with ITERS = 0, 1 and N: start-up,
 * init + first burst, and N bursts.  (cN - c1) / (N - 1) is one burst; a
 * 20 ms frame of speech is a third of it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "protocols/DMR/dmr_enc.h"
#include "protocols/DMR/dmr_tx.h"

#ifndef ITERS
#define ITERS 60
#endif

extern void initialise_monitor_handles(void);

#define FRAME_SAMPLES 160            /* 20 ms at 8 kHz */
#define MAX_SAMPLES (30 * 8000)
#define HALF_SAMPLES 1440            /* 30 ms at 48 kHz, one slot */

static int16_t pcm[MAX_SAMPLES];
static int16_t out[HALF_SAMPLES];

int main(void)
{
    initialise_monitor_handles();

    FILE *f = fopen("pcm8k.raw", "rb");
    if (f == NULL) {
        printf("bench: no pcm8k.raw\n");
        return 1;
    }
    const size_t n = fread(pcm, sizeof(int16_t), MAX_SAMPLES, f);
    fclose(f);
    if (n < FRAME_SAMPLES) {
        printf("bench: pcm8k.raw shorter than one frame\n");
        return 1;
    }

    dmr_tx_t tx;
    dmr_mod_t mod;
    uint8_t ambe[3][9], dibits[DMR_TX_BURST_DIBITS];
    size_t off = 0;

    dmr_tx_setup(&tx, 1234567, 9, true, 1);
    dmr_mod_init(&mod, 1000.0f, false);

    for (int it = 0; it < ITERS; it++) {
        for (int k = 0; k < 3; k++) {
            if (off + FRAME_SAMPLES > n)
                off = 0;
            if (!dmrEnc_encode(&pcm[off], ambe[k])) {
                printf("bench: encode failed\n");
                return 1;
            }
            off += FRAME_SAMPLES;
        }
        dmr_tx_voice(&tx, (const uint8_t (*)[9])ambe, dibits);
        dmr_mod_dibits(&mod, dibits, DMR_TX_BURST_DIBITS, out);
    }

    printf("bench: %u samples, %d bursts, out[0..2] %d %d %d\n",
           (unsigned)n, (int)ITERS, out[0], out[1], out[2]);
    return 0;
}
