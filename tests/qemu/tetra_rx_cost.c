/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Bare-metal Cortex-M33 cost harness for the TETRA receive chain, run under
 * QEMU (see scripts/tetra_rx_cost.sh and the qemu-mcu-instruction-profiling
 * notes): how many instructions one 30 ms block of discriminator audio
 * costs, split into the demodulator and libtetra's decoding.
 *
 * The input is two seconds of a real TMO capture (tests/qemu/tetra_rx_audio.h),
 * so both halves of the chain do the work they do on the radio.
 *
 * Stages per block, one frame_marker() each:
 *   demod  TetraDemod_process(): 1440 samples in, 1080-ish soft bits out
 *   push   tetra_tmo_push_soft(): sync search, LMAC, L2/L3, speech CRC
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <tetra/tetra.h>

#include "protocols/TETRA/TetraDemod.h"
#include "tetra_rx_audio.h"

#define BLOCK 1440 /* 30 ms at 48 kHz */
#define N 60       /* blocks measured (1.8 s of audio) */

volatile uint32_t sink;

static struct tetraDemod demod;
static struct tetra_tmo_rx tmo;
static int8_t soft[BLOCK * 3 / 4 + 8];
static unsigned nTraffic, nCalls;

static void onEvent(void *user, const struct tetra_event *ev)
{
    (void)user;
    if (ev->type == TETRA_EV_TRAFFIC)
        nTraffic++;
    else if (ev->type == TETRA_EV_CALL)
        nCalls++;
}

void __attribute__((noinline)) frame_marker(void)
{
    __asm__ volatile("" ::: "memory");
}

static void block_demod(const int16_t *in, size_t *n)
{
    *n = tetraDemod_process(&demod, in, BLOCK, soft);
    sink += (uint32_t)*n;
}

static void block_push(size_t n)
{
    tetra_tmo_push_soft(&tmo, soft, n);
    sink += (uint32_t)tetra_tmo_locked(&tmo);
}

int main(void)
{
    struct tetra_rx_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cb = onEvent;
    cfg.promiscuous = true;
    tetra_tmo_init(&tmo, &cfg);
    tetraDemod_init(&demod);

    /* init cost, once per mode change */
    frame_marker();
    tetra_tmo_init(&tmo, &cfg);
    tetraDemod_init(&demod);
    frame_marker();

    for (int i = 0; i < N; i++) {
        const int16_t *in = &tetra_rx_audio[(size_t)i * BLOCK];
        size_t n = 0;
        block_demod(in, &n);
        frame_marker();
        block_push(n);
        frame_marker();
    }
    printf("done %u traffic %u calls %u lock %d\n", sink, nTraffic, nCalls,
           tetra_tmo_locked(&tmo));
    return 0;
}
