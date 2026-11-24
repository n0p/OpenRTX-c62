/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost harness for the DMR short message layer on a Cortex-M33, under QEMU
 * (mps2-an505).  Build and run with work/sms-cost/build.sh, which is the same
 * setup as work/dmr-tx/arm: the marker plugin of work/dsp-probe snapshots the
 * instruction counters every time frame_marker() runs, so the difference
 * between two snapshots is exactly one iteration of the operation.
 *
 *   -DMODE=0  dmrSms_encode(), a 4-block message
 *   -DMODE=1  dmrSmsRx_feed() of a whole 4-block packet (header + blocks)
 *   -DMODE=2  dmrTx_dataSlot(), one data burst to baseband samples
 *   -DMODE=3  dmrSms_decodePacket(), payload decode and text conversion
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "protocols/DMR/DmrSms.h"
#include "protocols/DMR/DmrTx.h"

#ifndef ITERS
#define ITERS 64
#endif
#ifndef MODE
#define MODE 0
#endif

extern void initialise_monitor_handles(void);

__attribute__((noinline, used)) void frame_marker(void);
void frame_marker(void) { __asm__ volatile("" ::: "memory"); }

static int16_t slot[DMR_TX_SLOT_SAMPLES];
static volatile int32_t sink;
static struct dmrSmsEncode req;
static struct dmrSmsPacket pkt;
static struct dmrSmsRx rx;
static struct dmrSmsRxResult res;
static struct dmrSmsMessage msg;
static struct dmrTxState st;

static void buildRequest(void)
{
    memset(&req, 0, sizeof(req));
    req.app = DMR_SMS_APP_IP_HC;
    req.src = 1234567;
    req.dst = 1234568;
    req.group = false;
    req.confirmed = false;
    req.tmsSeq = 0;
    req.text = "Mensaje de prueba con cuarenta caracteres";
}

int main(void)
{
    initialise_monitor_handles();

    struct dmrTxConfig cfg;

    cfg.colourCode = 1;
    cfg.sourceId = 1234567;
    cfg.destId = 1234568;
    cfg.group = false;
    cfg.timeslot = 0;
    cfg.lateEntry = false;
    cfg.layout = DMR_TX_LAYOUT_MS;
    cfg.ramp = true;
    cfg.innerLevel = 2731;
    dmrTx_init(&st, &cfg);

    buildRequest();
    (void)dmrSms_encode(&req, &pkt);
    dmrSmsRx_reset(&rx);

    frame_marker();
    for (int i = 0; i < ITERS; i++) {
#if MODE == 1
        dmrSmsRx_reset(&rx);
        for (unsigned b = 0; b <= pkt.blockCount; b++) {
            uint8_t dt;
            uint8_t info[96];
            uint8_t packed[12];

            dmrSms_packetBurst(&pkt, b, &dt, info);
            memset(packed, 0, sizeof(packed));
            for (int k = 0; k < 96; k++)
                if (info[k] != 0)
                    packed[k >> 3] |= (uint8_t)(0x80u >> (k & 7));
            (void)dmrSmsRx_feed(&rx, dt, packed, &res);
        }
        sink += (int32_t)res.msg.textLen;
#elif MODE == 2
        uint8_t dt;
        uint8_t info[96];

        dmrSms_packetBurst(&pkt, 1 + (unsigned)(i % pkt.blockCount), &dt, info);
        dmrTx_dataSlot(&st, dt, info, slot);
        sink += slot[i % DMR_TX_SLOT_SAMPLES];
#elif MODE == 3
        (void)dmrSms_decodePacket(pkt.header, (const uint8_t (*)[12])pkt.blocks,
                                  pkt.blockCount, &msg);
        sink += (int32_t)msg.textLen;
#else
        (void)dmrSms_encode(&req, &pkt);
        sink += (int32_t)pkt.blockCount;
#endif
        frame_marker();
    }

    printf("sms mode=%d iters=%d blocks=%u sink=%ld\n", MODE, ITERS,
           (unsigned)pkt.blockCount, (long)sink);
    return 0;
}
