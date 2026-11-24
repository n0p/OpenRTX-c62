/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The destination of a DMR call has to reach the link control, which is what
 * the receiver reads to decide whether the call is for it: a talkgroup call is
 * FLCO 0x00 and every radio on that talkgroup opens, a unit call is FLCO 0x03
 * and only the addressed subscriber opens.  The source ID travels in the same
 * PDU: it is the radio's own DMR ID from the settings.
 *
 * The package builds the LC (dmr_tx_setup, dmr_tx.h) and puts it in the air
 * twice: whole in the voice LC header and terminator bursts, and as four
 * fragments of an embedded LC in every superframe, which is what a receiver
 * uses to keep tracking the call.  Both are read back here from the bursts
 * themselves, through the same de-interleaving and BPTC(196,96) a receiver
 * runs, so the test fails if the LC only exists in the call state.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

extern "C" {
#include "protocols/DMR/dmr_fec.h"
#include "protocols/DMR/dmr_tx.h"
}

/* Read n bits, MSB first, from a packed 12-byte LC */
static uint32_t bytesBits(const uint8_t lc[12], int off, int n)
{
    uint32_t v = 0;

    for (int i = 0; i < n; i++) {
        int b = off + i;
        v = (v << 1) | ((lc[b >> 3] >> (7 - (b & 7))) & 1u);
    }

    return v;
}

/* Read n bits, MSB first, from an unpacked bit array (one bit per byte) */
static uint32_t bitBits(const uint8_t *bits, int off, int n)
{
    uint32_t v = 0;

    for (int i = 0; i < n; i++)
        v = (v << 1) | (bits[off + i] & 1u);

    return v;
}

/*
 * Recover the 196 info bits of a data burst (header, terminator: info 0..97,
 * slot type, sync, slot type, info 166..263; see dmr_tx.c), BPTC-decode them
 * and hand back the LC octets and the slot type.
 */
struct DataBurst {
    uint8_t lc[12];   /* LC octets, RS parity in 9..11 */
    uint8_t cc;
    uint8_t dt;
};

static DataBurst decodeDataBurst(const uint8_t dibits[DMR_TX_BURST_DIBITS])
{
    uint8_t bits[264];

    for (int i = 0; i < 264; i++)
        bits[i] = (uint8_t)((dibits[i / 2] >> (1 - (i % 2))) & 1u);

    uint8_t info[196], decoded[96];
    memcpy(info, bits, 98);
    memcpy(&info[98], &bits[166], 98);

    DataBurst out;
    memset(&out, 0, sizeof(out));
    REQUIRE(dmr_bptc196_decode(info, decoded) == 0);

    for (int i = 0; i < 72; i++)
        out.lc[i / 8] |= (uint8_t)(decoded[i] << (7 - i % 8));

    /* Slot type: the same 10-bit Golay codeword twice, 8 data bits */
    uint32_t cw = (bitBits(bits, 98, 10) << 10) | bitBits(bits, 156, 10);
    REQUIRE(dmr_golay_20_8_decode(&cw));
    const uint32_t d = (cw >> 12) & 0xFFu; /* data bits 19..12 */
    out.cc = (uint8_t)((d >> 4) & 0x0Fu);
    out.dt = (uint8_t)(d & 0x0Fu);
    return out;
}

TEST_CASE("A group call is addressed to its talkgroup", "[dmr-tx]")
{
    dmr_tx_t tx;
    uint8_t dibits[DMR_TX_BURST_DIBITS];

    dmr_tx_setup(&tx, 2142123, 2142, true, 5);

    /* The call state, in bits, as the receiver reads it */
    REQUIRE(bitBits(tx.lc, 0, 1) == 0);        /* PF */
    REQUIRE(bitBits(tx.lc, 2, 6) == 0x00);     /* FLCO: group voice channel user */
    REQUIRE(bitBits(tx.lc, 8, 8) == 0x00);     /* FID: standard */
    REQUIRE(bitBits(tx.lc, 16, 8) == 0x00);    /* service options */
    REQUIRE(bitBits(tx.lc, 24, 24) == 2142);   /* destination: the talkgroup */
    REQUIRE(bitBits(tx.lc, 48, 24) == 2142123);/* source: our DMR ID */

    /* ... and on the air, in the header burst */
    dmr_tx_header(&tx, dibits);
    DataBurst h = decodeDataBurst(dibits);
    REQUIRE(h.dt == 1);                        /* voice LC header */
    REQUIRE(h.cc == 5);                        /* the channel's colour code */
    REQUIRE(bytesBits(h.lc, 2, 6) == 0x00);
    REQUIRE(bytesBits(h.lc, 24, 24) == 2142);
    REQUIRE(bytesBits(h.lc, 48, 24) == 2142123);
}

TEST_CASE("A unit call is addressed to one subscriber", "[dmr-tx]")
{
    dmr_tx_t tx;
    uint8_t dibits[DMR_TX_BURST_DIBITS];

    dmr_tx_setup(&tx, 2142123, 1234567, false, 9);

    REQUIRE(bitBits(tx.lc, 2, 6) == 0x03);     /* FLCO: unit-to-unit */
    REQUIRE(bitBits(tx.lc, 24, 24) == 1234567);
    REQUIRE(bitBits(tx.lc, 48, 24) == 2142123);

    dmr_tx_header(&tx, dibits);
    DataBurst h = decodeDataBurst(dibits);
    REQUIRE(h.dt == 1);
    REQUIRE(h.cc == 9);
    REQUIRE(bytesBits(h.lc, 2, 6) == 0x03);
    REQUIRE(bytesBits(h.lc, 24, 24) == 1234567);
}

TEST_CASE("The embedded LC of every superframe repeats the destination",
          "[dmr-tx]")
{
    dmr_tx_t tx;
    uint8_t lc[72];

    dmr_tx_setup(&tx, 2142123, 2142, true, 5);

    /*
     * The embedded LC is the same 72 bits, Golay-coded in 128 bits of four
     * fragments: a receiver tracks the call with it when the header is over.
     */
    REQUIRE(dmr_emb_lc_decode(tx.embLc, lc));
    REQUIRE(memcmp(lc, tx.lc, sizeof(lc)) == 0);

    dmr_tx_setup(&tx, 2142123, 1234567, false, 5);
    REQUIRE(dmr_emb_lc_decode(tx.embLc, lc));
    REQUIRE(bitBits(lc, 2, 6) == 0x03);
    REQUIRE(bitBits(lc, 24, 24) == 1234567);
}

TEST_CASE("The terminator carries the same link control", "[dmr-tx]")
{
    dmr_tx_t tx;
    uint8_t dibits[DMR_TX_BURST_DIBITS];

    dmr_tx_setup(&tx, 2142123, 2142, true, 1);
    dmr_tx_terminator(&tx, dibits);

    DataBurst t = decodeDataBurst(dibits);
    REQUIRE(t.dt == 2);                        /* terminator with LC */
    REQUIRE(bytesBits(t.lc, 2, 6) == 0x00);    /* group voice channel user */
    REQUIRE(bytesBits(t.lc, 24, 24) == 2142);
    REQUIRE(bytesBits(t.lc, 48, 24) == 2142123);
}

TEST_CASE("The LC carries the identifiers whole and no more", "[dmr-tx]")
{
    dmr_tx_t tx;

    /* The largest 24-bit identifiers */
    dmr_tx_setup(&tx, 0xFFFFFF, 0xFFFFFF, true, 0);
    REQUIRE(bitBits(tx.lc, 24, 24) == 0xFFFFFF);
    REQUIRE(bitBits(tx.lc, 48, 24) == 0xFFFFFF);

    /* A 32-bit value must be cut to its 24 bits, not shifted into the CRC */
    dmr_tx_setup(&tx, 0x01234567, 0x0089ABCD, true, 0);
    REQUIRE(bitBits(tx.lc, 24, 24) == 0x89ABCD);
    REQUIRE(bitBits(tx.lc, 48, 24) == 0x234567);
}
