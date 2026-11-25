/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost of the P25 and System Fusion transmitters: frame building (FEC, CRC,
 * interleaving, vocoder packing) plus the C4FM shaper, per frame, bare metal
 * on QEMU's mps2-an505 (Cortex-M33, as the CSK6011B).  Every executed
 * instruction is counted by the TCG plugin in tests/host/dmr_enc_qemu/.
 *
 * Built and run by c62_c4fm_tx_cost.sh, three ELFs with ITERS = 0, 1 and N:
 * start-up, one frame, N frames.  (cN - c1) / (N - 1) is one frame.
 *
 * The call is started once, before the loop, the way the PTT path starts it,
 * so the steady-state figure is a voice frame (LDU1/LDU2 for P25, V/D mode 1
 * for System Fusion) and not a header.  The header is the first frame, and it
 * only shows up in the ITERS = 1 figure.
 *
 *   -DP25      the P25 chain, one 180 ms frame (8640 samples)
 *   -DYSF      the System Fusion chain, one 100 ms frame (4800 samples)
 *   -DVOICE    install a voice source, so the IMBE/AMBE packing is included
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(P25)
#include "protocols/P25/P25Tx.h"
#define FRAME_SAMPLES 8640
#elif defined(YSF)
#include "protocols/YSF/YsfTx.h"
#define FRAME_SAMPLES 4800
#else
#error "define P25 or YSF"
#endif

#ifndef ITERS
#define ITERS 20
#endif

#ifndef GAIN
#define GAIN 5460.0f
#endif

extern void initialise_monitor_handles(void);

static int16_t out[FRAME_SAMPLES];
static volatile uint32_t sink;

#if defined(VOICE) && defined(P25)
/* 11 bytes: the 88-bit IMBE parameters the transmitter packs into a word */
static int voice(uint8_t params[11], void *user)
{
    (void)user;

    for (int i = 0; i < 11; i++)
        params[i] = (uint8_t)(i * 37 + 11);
    return 0;
}
#elif defined(VOICE) && defined(YSF)
/* 9 bytes: the canonical 72-bit AMBE+2 channel frame */
static int voice(uint8_t frame[9], void *user)
{
    (void)user;

    for (int i = 0; i < 9; i++)
        frame[i] = (uint8_t)(i * 37 + 11);
    return 0;
}
#endif

int main(void)
{
    initialise_monitor_handles();

#if defined(P25)
    struct p25TxConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.nac = 0x293;
    cfg.group = 1;
    cfg.source = 0x123456;
    cfg.service = 0;

    p25Tx_begin(&cfg);
#elif defined(YSF)
    struct ysfTxConfig cfg;

    memset(&cfg, 0, sizeof(cfg));
    for (int i = 0; i < 10; i++) {
        cfg.source[i] = (char)('A' + i);
        cfg.dest[i] = 'C';
        cfg.uplink[i] = 'U';
        cfg.downlink[i] = 'D';
    }
    cfg.source[10] = cfg.dest[10] = cfg.uplink[10] = cfg.downlink[10] = '\0';
    cfg.dgid = 42;
    cfg.text = "COSTE DE TRANSMISION SYSTEM FUSION";

    ysfTx_begin(&cfg);
#endif

#if defined(VOICE) && defined(P25)
    p25Tx_setVoiceSource(voice, NULL);
#elif defined(VOICE) && defined(YSF)
    ysfTx_setVoiceSource(voice, NULL);
#elif defined(P25)
    p25Tx_setVoiceSource(NULL, NULL);
#else
    ysfTx_setVoiceSource(NULL, NULL);
#endif

    for (int it = 0; it < ITERS; it++) {
#if defined(P25)
        p25Tx_samples(out, FRAME_SAMPLES, GAIN);
#else
        ysfTx_samples(out, FRAME_SAMPLES, GAIN);
#endif
        sink += (uint32_t)out[0] + (uint32_t)out[FRAME_SAMPLES - 1];
    }

    printf("c4fm tx cost done (iters %d), sink %lu\n", ITERS,
           (unsigned long)sink);
    return 0;
}
