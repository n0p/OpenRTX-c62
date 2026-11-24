/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Receive cost of the DMR package with our vocoder, in instructions per
 * second of audio: 48 kHz discriminator in, dmr_demod_feed_disc() ->
 * dmr_proto (FEC, LC, CSBK, Tier III feed) -> dmr_voice's queue ->
 * dmr_voice_decode_next() with lib/dmr_vocoder.  Bare metal on QEMU's
 * mps2-an505 (Cortex-M33), every instruction counted by the TCG plugin.
 *
 * Built and run by run_rx.sh: the ELF with SECS=0 measures start-up and the
 * mode's own init, the one with SECS=N the same plus N seconds of a real
 * capture; the difference is what the receiver costs.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocols/DMR/dmr_demod.h"
#include "protocols/DMR/dmr_proto.h"
#include "protocols/DMR/dmr_voice.h"
#include "protocols/DMR/dmr_glue.h"
#include "protocols/DMR/dmr_sms.h"
#include "protocols/DMR/trunk/dmr_trunk.h"

/*
 * dmr_sms.c is left out of this bench (it locks with pthread, which bare
 * metal has not): dmr_proto still calls this one hook for a data packet.
 */
bool dmrSms_rxPacket(const dmr_packet_t *pk, uint32_t t_ms)
{
    (void)pk;
    (void)t_ms;
    return false;
}

#ifndef SECS
#define SECS 12
#endif

extern void initialise_monitor_handles(void);

#define HALF 1440 /* 30 ms at 48 kHz */

static long long tickMs;

long long getTick(void)
{
    return tickMs;
}

static uint32_t pcmSamples;

static void sink(const float *pcm, int n)
{
    /* Keep the synthesis alive: one value out is enough to stop the link
     * from dropping it */
    pcmSamples += (uint32_t)n + (uint32_t)(pcm[0] != 0.0f);
}

int main(void)
{
    initialise_monitor_handles();

    FILE *f = NULL;
    if (SECS > 0) {
        f = fopen("discr.raw", "rb");
        if (f == NULL) {
            printf("bench: no discr.raw\n");
            return 1;
        }
    }

    if (!dmr_demod_init()) {
        printf("bench: dmr_demod_init failed\n");
        return 1;
    }
    dmr_proto_reset();
    dmr_voice_reset();
    dmr_voice_set_sink(sink);
    dmrTrunk_start(0, 0);

    static int16_t raw[HALF];
    float block[HALF];
    long blocks = (long)SECS * 48000 / HALF;

    for (long b = 0; b < blocks; b++) {
        if (fread(raw, sizeof(int16_t), HALF, f) != HALF)
            break;
        for (int k = 0; k < HALF; k++)
            block[k] = (float)raw[k];
        tickMs += 30;
        dmr_demod_feed_disc(block, HALF);
        while (dmr_voice_decode_next())
            ;
    }
    if (f != NULL)
        fclose(f);

    dmr_demod_status_t ds;
    dmr_proto_stats_t st;
    uint32_t frames, avgUs;
    dmr_demod_get_status(&ds);
    dmr_proto_get_stats(&st);
    dmr_voice_get_timing(&frames, &avgUs);
    printf("bench: %d s, blocks %ld, syncs %u, bursts %u, voice %u frames, "
           "%u pcm samples\n", (int)SECS, blocks, ds.syncs, st.frames,
           frames, pcmSamples);
    return 0;
}
