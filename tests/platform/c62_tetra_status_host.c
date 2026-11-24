/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host check of the firmware's TETRA status layer (TetraStatus.c) on a real
 * capture: runs the mode's receive chain (TetraDemod.c + libtetra) over
 * discriminator audio and prints what the status structure ends up holding,
 * i.e. what the UI would show after the run.
 *
 *   cc -O2 -I openrtx/include -I <libtetra>/include -I <libtetra>/build/generated
 *      tests/platform/c62_tetra_status_host.c
 *      openrtx/src/protocols/TETRA/TetraStatus.c
 *      openrtx/src/protocols/TETRA/TetraDemod.c <libtetra>/build/libtetra.a
 *      -lpthread -lm -o c62_tetra_status_host
 *   c62_tetra_status_host input.wav|input.s16
 *
 * TetraStatus.c belongs to the firmware, so its two neighbours are stubbed:
 * the SDS store (flash) and the speech decoder (OpenACELP).
 */

#include <stdio.h>
#include <string.h>
#include <tetra/tetra.h>
#include "protocols/TETRA/TetraDemod.h"
#include "protocols/TETRA/TetraStatus.h"

#define HALF_SAMPLES 1440

static struct tetraDemod demod;
static struct tetra_tmo_rx tmo;

/* ---- Stubs for what TetraStatus.c calls out of the mode ---- */

static long long ticks;
static unsigned nSds;
static unsigned nTraffic;
static unsigned nSpeechFrames;

long long getTick(void)
{
    return ticks;
}

int tetraSds_onEvent(const struct tetra_sds *s)
{
    (void)s;
    nSds++;
    return 0;
}

void tetraSpeech_traffic(const struct tetra_traffic *t)
{
    (void)t;
    nTraffic++;
    nSpeechFrames += 2;
}

static void initTmo(void)
{
    struct tetra_rx_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cb = tetraStatus_onEvent; /* the firmware's own callback */
    cfg.promiscuous = true;
    tetra_tmo_init(&tmo, &cfg);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s input.wav|input.s16\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    char hdr[12];
    if (fread(hdr, 1, 12, f) == 12 && !memcmp(hdr, "RIFF", 4)) {
        char id[4];
        uint32_t len;
        while (fread(id, 1, 4, f) == 4 && fread(&len, 4, 1, f) == 1) {
            if (!memcmp(id, "data", 4))
                break;
            fseek(f, len, SEEK_CUR);
        }
    } else {
        fseek(f, 0, SEEK_SET);
    }

    tetraDemod_init(&demod);
    tetraStatus_reset();
    initTmo();

    static int16_t buf[HALF_SAMPLES];
    static int8_t soft[HALF_SAMPLES * 3 / 4 + 8];
    uint32_t jumps = 0;
    uint32_t lastCrc = 0;
    size_t n;
    while ((n = fread(buf, 2, HALF_SAMPLES, f)) > 0) {
        size_t k = tetraDemod_process(&demod, buf, n, soft);
        uint32_t j = tetraDemod_reacquisitions(&demod);
        if (j != jumps) {
            jumps = j;
            initTmo();
            tetraStatus_lostSync(false);
        }
        tetra_tmo_push_soft(&tmo, soft, k);
        uint32_t crc = tetra_tmo_crc_ok(&tmo);
        if (crc != lastCrc) {
            lastCrc = crc;
            tetraDemod_signalOk(&demod);
        }
        ticks += 30; /* one HALF_SAMPLES block is 30 ms minus the symbols */
    }

    struct tetraStatus ts;
    tetra_getStatus(&ts);
    printf("sync %d cell %d %03u-%02u CC %u sysinfo %d LA %u dl %lu Hz "
           "enc %d\n", ts.sync, ts.cell, (unsigned)ts.mcc, (unsigned)ts.mnc,
           (unsigned)ts.colourCode, ts.sysinfo, (unsigned)ts.la,
           (unsigned long)ts.dlFreq, ts.airEncryption);
    printf("registered %d cause %u groups %u detachAll %d\n", ts.registered,
           (unsigned)ts.mmCause, (unsigned)ts.groups, ts.detachAll);
    printf("network %d load %u neighbours %u time %d %u+%dmin\n", ts.haveNet,
           (unsigned)ts.netLoad, (unsigned)ts.neighbours, ts.netTime,
           (unsigned)ts.netYear, (int)ts.netOffsetMin);
    if (ts.netTime) {
        int32_t s = (int32_t)(ts.netUtc % 86400u) + 60 * (int32_t)ts.netOffsetMin;
        s = ((s % 86400) + 86400) % 86400;
        printf("network clock %02d:%02d:%02d local\n", (int)(s / 3600),
               (int)((s / 60) % 60), (int)(s % 60));
    }
    printf("call %d id %u dst %lu talker %lu group %d enc %d voice %d\n",
           ts.call, (unsigned)ts.callId, (unsigned long)ts.callDst,
           (unsigned long)ts.talker, ts.callGroup, ts.callEncrypted, ts.voice);
    printf("uplink %d ssi %lu\n", ts.uplink, (unsigned long)ts.uplinkSs);
    printf("sds %u traffic %u speechFrames %u\n", nSds, nTraffic, nSpeechFrames);
    return 0;
}
