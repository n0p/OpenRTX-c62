/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * What the Tier III engine needs to hear, checked without a radio.
 *
 * The capture the host harness decodes (sigidwiki's DMR control channel,
 * out/discr_dmr_t3_cc.wav) carries CSBKs whose second octet is 0x06, not the
 * ETSI FID 0x00: the engine counts them (good CRC) and drops them, because
 * only an ETSI C_ALOHA confirms a control channel.  This program builds the
 * same PDU the standard describes, feeds it to dmrTrunk_block() the way
 * dmr_proto.c does, and prints what the engine makes of it: the state, the
 * confirmation, the system code and the timeslot it camped on.  It is the
 * reference for "the engine is not the thing that is missing".
 */

#include <stdio.h>
#include <string.h>

#include "protocols/DMR/trunk/dmr_trunk.h"
#include "protocols/DMR/trunk/t3_ms.h"
#include "protocols/DMR/trunk/t3_pdu.h"

long long getTick(void);

/* Audio time, as the host harness runs it: advanced by hand below */
static long long tickMs;
long long getTick(void)
{
    return tickMs;
}

static void show(const char *what)
{
    dmrTrunkStatus_t st;

    dmrTrunk_getStatus(&st);
    printf("%-28s state %u (%s) confirmed %d tscc %u Hz cc %u syscode %04x "
           "csbk %u grants %u\n",
           what, st.state,
           st.state == 0 ? "off" : st.state == 1 ? "hunt" : st.state == 2
                                                          ? "camped"
                                                          : "follow",
           (int)st.confirmed, st.tsccHz, st.tsccCc, st.syscode, st.csbk,
           st.grants);
}

int main(void)
{
    /* An ETSI Aloha: opcode C_ALOHA (1), FID 0, system code 0x1234 >> 2 = the
     * net/site, PAR bits in the low two */
    t3_csbk_t aloha;

    memset(&aloha, 0, sizeof(aloha));
    aloha.lb = 1;
    aloha.pf = 0;
    aloha.op = T3_OP_C_ALOHA;
    aloha.fid = 0;
    aloha.u.aloha.tsccas = 1;
    aloha.u.aloha.version = 1;
    aloha.u.aloha.mask = 1;
    aloha.u.aloha.reg = 1;
    aloha.u.aloha.backoff = 0;
    aloha.u.aloha.syscode = 0x1236; /* model 0, net/site 0x48D, PAR 2 */
    aloha.u.aloha.ms = 0;

    uint8_t pdu[T3_CSBK_BYTES];
    if (!t3_csbk_build(&aloha, T3_CRC_MASK_CSBK, pdu)) {
        printf("t3_csbk_build failed\n");
        return 1;
    }
    printf("aloha pdu:");
    for (int i = 0; i < T3_CSBK_BYTES; i++)
        printf(" %02x", pdu[i]);
    printf("\n");

    /* Two TSCC slots: the engine takes the first slot an Aloha shows */
    dmrTrunk_start(438000000u, 438000000u);
    show("after start");

    tickMs += 500; /* past T3_SETTLE_MS */
    dmrTrunk_block(0, T3_DT_CSBK, 1, pdu);
    show("after one ETSI aloha");

    /* Once camped, an Aloha on the other slot is the second TSCC slot */
    tickMs += 60;
    dmrTrunk_block(1, T3_DT_CSBK, 1, pdu);
    show("after aloha on TS2");

    /* The same PDU with a vendor FID must be ignored, not camped on */
    dmrTrunk_stop();
    dmrTrunk_start(438000000u, 438000000u);
    tickMs += 500;
    uint8_t vendor[T3_CSBK_BYTES];
    memcpy(vendor, pdu, sizeof(vendor));
    vendor[1] = 0x06;
    t3_crc_append(vendor, T3_CRC_MASK_CSBK); /* a valid CRC over a vendor PDU */
    dmrTrunk_block(0, T3_DT_CSBK, 1, vendor);
    show("after a FID 0x06 CSBK");

    dmrTrunk_stop();
    return 0;
}
