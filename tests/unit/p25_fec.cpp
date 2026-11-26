/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file p25_fec.cpp
 * @brief The P25 channel codes at their limits: what each one must correct,
 * and what it must refuse.
 *
 * The happy path of the FEC is already pinned end to end by
 * tests/host/p25_tx (encode, transmit, demodulate, decode through the
 * firmware's own chain) and by tests/unit/p25_phy (a real LDU1 off the air,
 * bit for bit).  What neither of them does is walk a block past the correcting
 * power of its code: a decoder that "corrects" seven errors in a Reed-Solomon
 * block, or one that hands back a TSBK whose CRC does not check, puts a wrong
 * talkgroup or a wrong source address on the screen, and the radio says the
 * call is fine.
 *
 * So each case here has two halves: inside the radius the information must
 * come back and the number of corrected symbols must be the one the code
 * promises; outside it the block must be refused, either by the decoder (-1) or
 * by the CRC-16 that follows it, and the information must not be returned.
 *
 * The radii are properties of the codes (TIA-102.BAAA/.BAAC/.BABA-A):
 *   Golay(23,12)  3 bit errors, and it never returns a word outside that radius
 *   Hamming(15,11) 1 bit error
 *   Hamming(10,6) 1 bit error
 *   Golay(18,6)   3 bit errors
 *   RS(24,12)     6 symbols, RS(36,20) 8 symbols over GF(64)
 *   BCH(63,16)    11 bit errors (the NID policy that uses it is in P25Phy.c and
 *                 is pinned by tests/unit/p25_phy.cpp)
 *   trellis 1/2   no bound, but the CRC-16 behind it has to refuse
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "protocols/P25/P25Defs.h"
#include "protocols/P25/P25Fec.h"
}

namespace {

/** Number of set bits. */
int weight(uint32_t v)
{
    int n = 0;
    while (v != 0) {
        v &= v - 1;
        n++;
    }
    return n;
}

/** The eleven parity bits of a Golay word, which are what it carries. */
uint32_t golayInfo(uint32_t cw)
{
    return (cw >> 11) & 0xFFF;
}

/**
 * A TSBK block with a valid CRC-16: ten data bytes, then the CRC as the two
 * bytes that make the whole block check out (MSB first, TIA-102.BAAC 7.2.4).
 */
void makeTsbk(uint8_t block[P25_TSBK_BYTES])
{
    for (int i = 0; i < P25_TSBK_BYTES - 2; i++)
        block[i] = (uint8_t)(i * 11 + 3);
    block[P25_TSBK_BYTES - 2] = 0;
    block[P25_TSBK_BYTES - 1] = 0;

    const uint16_t crc = p25Fec_crc16(block, P25_TSBK_BYTES);
    block[P25_TSBK_BYTES - 2] = (uint8_t)(crc >> 8);
    block[P25_TSBK_BYTES - 1] = (uint8_t)crc;
}

} // namespace

TEST_CASE("Golay(23,12) returns every clean codeword untouched", "[p25][fec]")
{
    /* All 4096 information words: the decoder has to be the identity on the
     * code, not just on the ones a transmitter happens to send. */
    for (uint32_t info = 0; info < 0x1000; info++) {
        uint32_t cw = p25Fec_golayEncode(info);
        uint32_t got = cw;
        INFO("info " << info);
        REQUIRE(p25Fec_golayDecode(&got) == 0);
        REQUIRE(got == cw);
        REQUIRE(golayInfo(got) == info);
    }
}

TEST_CASE("Golay(23,12) corrects up to three bits and says how many",
          "[p25][fec]")
{
    /* Every single and double error over eight information words, every triple
     * over one: 8 * (23 + 253) + 1771 words, all of which must come back as the
     * word that was sent, with the count the weight promises. */
    for (uint32_t info = 0; info < 8; info++) {
        const uint32_t cw = p25Fec_golayEncode(info * 0x123u);

        for (int a = 0; a < 23; a++) {
            uint32_t got = cw ^ (1u << a);
            INFO("info " << info << " bit " << a);
            REQUIRE(p25Fec_golayDecode(&got) == 1);
            REQUIRE(golayInfo(got) == info * 0x123u);
        }

        for (int a = 0; a < 23; a++) {
            for (int b = a + 1; b < 23; b++) {
                uint32_t got = cw ^ (1u << a) ^ (1u << b);
                INFO("info " << info << " bits " << a << "," << b);
                REQUIRE(p25Fec_golayDecode(&got) == 2);
                REQUIRE(golayInfo(got) == info * 0x123u);
            }
        }
    }

    for (int a = 0; a < 23; a++) {
        for (int b = a + 1; b < 23; b++) {
            for (int c = b + 1; c < 23; c++) {
                const uint32_t info = 0x555;
                uint32_t got = p25Fec_golayEncode(info) ^ (1u << a) ^
                               (1u << b) ^ (1u << c);
                INFO("bits " << a << "," << b << "," << c);
                REQUIRE(p25Fec_golayDecode(&got) == 3);
                REQUIRE(golayInfo(got) == info);
            }
        }
    }
}

TEST_CASE("Golay(23,12) never answers from outside its correction radius",
          "[p25][fec]")
{
    /*
     * A perfect code cannot detect a fourth error: some weight-4 patterns are
     * within three bits of another codeword, and the right answer there is that
     * other codeword.  What the decoder must never do is return a word that is
     * not a codeword, or a codeword more than three bits away from what it was
     * given: that would be a correction it invents.  Every weight-4 pattern on
     * one word is checked against this, which is what "refused or honestly
     * corrected" means for this code.
     */
    const uint32_t info = 0x123;
    const uint32_t sent = p25Fec_golayEncode(info);
    int refused = 0;

    for (int a = 0; a < 23; a++) {
        for (int b = a + 1; b < 23; b++) {
            for (int c = b + 1; c < 23; c++) {
                for (int d = c + 1; d < 23; d++) {
                    const uint32_t rx = sent ^ (1u << a) ^ (1u << b) ^
                                        (1u << c) ^ (1u << d);
                    uint32_t got = rx;

                    if (p25Fec_golayDecode(&got) < 0) {
                        refused++;
                        continue;
                    }

                    INFO("bits " << a << "," << b << "," << c << "," << d);
                    /* It is a codeword ... */
                    REQUIRE(p25Fec_golayEncode(golayInfo(got)) == got);
                    /* ... and it is within the radius of what arrived */
                    REQUIRE(weight(got ^ rx) <= 3);
                }
            }
        }
    }

    /* Golay(23,12) is perfect: every syndrome belongs to a pattern of three
     * bits or fewer, so a fourth error is always answered as the neighbour it
     * lands next to and never refused.  The invariant above is therefore the
     * only thing that keeps a broken decoder from answering with anything it
     * likes, and it is what this case exists for. */
    REQUIRE(refused == 0);
}

TEST_CASE("Hamming(15,11) corrects one bit of every word", "[p25][fec]")
{
    for (uint32_t info = 0; info < 0x800; info++) {
        const uint16_t cw = p25Fec_hamEncode((uint16_t)info);
        uint16_t got = cw;

        REQUIRE(p25Fec_hamDecode(&got) == 0);
        REQUIRE(got == cw);

        for (int bit = 0; bit < 15; bit++) {
            got = (uint16_t)(cw ^ (1u << bit));
            INFO("info " << info << " bit " << bit);
            REQUIRE(p25Fec_hamDecode(&got) == 1);
            REQUIRE(got == cw);
        }
    }
}

TEST_CASE("Hamming(10,6) flags what it cannot correct instead of guessing",
          "[p25][fec]")
{
    /*
     * The low-speed data of an LDU1: 24 of these words carry the twelve
     * Reed-Solomon symbols of the link control.  The published (10,6) table
     * (op25's op25_hamming.h, TIA-102.BAAA) does not correct every single-bit
     * error: an error in the two most significant data bits cannot be told
     * apart from an erasure and comes back as -1, which P25Phy.c passes on to
     * the RS(24,12) as the raw symbol.  What must never happen is a symbol
     * that decodes to *different* data without saying so -- that is a talkgroup
     * nobody transmitted.
     */
    int corrected = 0;
    int flagged = 0;

    for (uint32_t data = 0; data < 64; data++) {
        const uint16_t w = p25Fec_ham10Encode(data);

        REQUIRE(p25Fec_ham10Decode(w >> 4, w & 0xF) == (int)data);

        for (int bit = 0; bit < 10; bit++) {
            const uint16_t got = (uint16_t)(w ^ (1u << bit));
            const int rc = p25Fec_ham10Decode(got >> 4, got & 0xF);

            INFO("data " << data << " bit " << bit << " rc " << rc);
            REQUIRE((rc == (int)data || rc == -1));

            if (rc < 0)
                flagged++;
            else
                corrected++;
        }
    }

    /* Eight of the ten positions per word, and not one of the 128 others is
     * turned into a wrong symbol. */
    REQUIRE(corrected == 512);
    REQUIRE(flagged == 128);
}

TEST_CASE("Golay(18,6) corrects three bit errors and refuses four",
          "[p25][fec]")
{
    /* The header words of an HDU: 18 bits on the air, six information bits.
     * Every symbol the transmitter sends comes from this encoder, so all 64 of
     * them are checked at every weight the code corrects. */
    for (uint32_t hex = 0; hex < 64; hex++) {
        uint8_t air[18];
        p25Fec_golay18Encode(hex, air);

        uint32_t got = 0;
        REQUIRE(p25Fec_golay18Decode(air, &got) == 0);
        REQUIRE(got == hex);

        for (int a = 0; a < 18; a++) {
            uint8_t w[18];
            memcpy(w, air, sizeof(w));
            w[a] ^= 1;
            REQUIRE(p25Fec_golay18Decode(w, &got) == 1);
            REQUIRE(got == hex);
        }

        for (int a = 0; a < 18; a++) {
            for (int b = a + 1; b < 18; b++) {
                for (int c = b + 1; c < 18; c++) {
                    uint8_t w[18];
                    memcpy(w, air, sizeof(w));
                    w[a] ^= 1;
                    w[b] ^= 1;
                    w[c] ^= 1;
                    REQUIRE(p25Fec_golay18Decode(w, &got) == 3);
                    REQUIRE(got == hex);
                }
            }
        }
    }

    /*
     * Four errors: minimum distance 8 puts every weight-4 pattern outside the
     * radius of every codeword, so all of them must be refused -- this is the
     * word that carries the encryption algorithm and the talkgroup of a header,
     * and a header the radio cannot trust must not be shown as a call.
     */
    uint8_t air[18];
    p25Fec_golay18Encode(0x2B, air);
    int refused = 0;
    int total = 0;
    for (int a = 0; a < 18; a++) {
        for (int b = a + 1; b < 18; b++) {
            for (int c = b + 1; c < 18; c++) {
                for (int d = c + 1; d < 18; d++) {
                    uint8_t w[18];
                    memcpy(w, air, sizeof(w));
                    w[a] ^= 1;
                    w[b] ^= 1;
                    w[c] ^= 1;
                    w[d] ^= 1;

                    uint32_t got = 0;
                    const int rc = p25Fec_golay18Decode(w, &got);
                    INFO("bits " << a << "," << b << "," << c << "," << d
                                 << " rc " << rc);
                    REQUIRE(rc == -1);
                    REQUIRE(got != 0x2B);
                    refused++;
                    total++;
                }
            }
        }
    }
    REQUIRE(total == 3060);
    REQUIRE(refused == total);
}

TEST_CASE("Reed-Solomon (24,12) refuses a block past six symbols",
          "[p25][fec]")
{
    /* The link control of an LDU1: 12 symbols of information, six of parity.
     * Up to six wrong symbols must be corrected; seven or more must come back
     * as -1 with the block left alone, so the mode reports "no link control"
     * instead of a talkgroup nobody said. */
    uint8_t info[12];
    uint8_t cw[24];
    for (int i = 0; i < 12; i++)
        info[i] = (uint8_t)(i * 5 + 1);
    p25Fec_rsEncode(info, 12, cw, 24);

    REQUIRE(p25Fec_rsCheck(cw, 24, 12) == 0);

    for (int errors = 1; errors <= 6; errors++) {
        uint8_t rx[24];
        memcpy(rx, cw, sizeof(rx));
        for (int k = 0; k < errors; k++)
            rx[k] ^= (uint8_t)(1u << (k % 6));

        INFO("errors " << errors);
        REQUIRE(p25Fec_rsDecode(rx, 24, 12) == errors);
        REQUIRE(memcmp(rx, cw, 24) == 0);
    }

    for (int errors = 7; errors <= 12; errors++) {
        uint8_t rx[24];
        memcpy(rx, cw, sizeof(rx));
        for (int k = 0; k < errors; k++)
            rx[k] ^= (uint8_t)(1u << (k % 6));

        INFO("errors " << errors);
        REQUIRE(p25Fec_rsDecode(rx, 24, 12) == -1);
        /* Nothing was "corrected": the block is not a codeword either */
        REQUIRE(p25Fec_rsCheck(rx, 24, 12) == -1);
        /* And the information is not the one that was sent */
        REQUIRE(memcmp(rx, info, 12) != 0);
    }
}

TEST_CASE("Reed-Solomon (36,20) refuses a block past eight symbols",
          "[p25][fec]")
{
    /* The HDU header.  Same contract at its own radius. */
    uint8_t info[20];
    uint8_t cw[36];
    for (int i = 0; i < 20; i++)
        info[i] = (uint8_t)(i * 3 + 2);
    p25Fec_rsEncode(info, 20, cw, 36);

    REQUIRE(p25Fec_rsCheck(cw, 36, 20) == 0);

    for (int errors = 1; errors <= 8; errors++) {
        uint8_t rx[36];
        memcpy(rx, cw, sizeof(rx));
        for (int k = 0; k < errors; k++)
            rx[k] ^= (uint8_t)(1u << (k % 6));

        INFO("errors " << errors);
        REQUIRE(p25Fec_rsDecode(rx, 36, 20) == errors);
        REQUIRE(memcmp(rx, cw, 36) == 0);
    }

    for (int errors = 9; errors <= 14; errors++) {
        uint8_t rx[36];
        memcpy(rx, cw, sizeof(rx));
        for (int k = 0; k < errors; k++)
            rx[k] ^= (uint8_t)(1u << (k % 6));

        INFO("errors " << errors);
        REQUIRE(p25Fec_rsDecode(rx, 36, 20) == -1);
        REQUIRE(p25Fec_rsCheck(rx, 36, 20) == -1);
    }
}

TEST_CASE("Reed-Solomon check rejects a single wrong symbol", "[p25][fec]")
{
    /* The cheap test the HDU path and the reference prototype use before the
     * expensive decode: one wrong symbol is not a codeword. */
    uint8_t info[12];
    uint8_t cw[24];
    for (int i = 0; i < 12; i++)
        info[i] = (uint8_t)(i + 0x21);
    p25Fec_rsEncode(info, 12, cw, 24);

    for (int pos = 0; pos < 24; pos++) {
        uint8_t rx[24];
        memcpy(rx, cw, sizeof(rx));
        rx[pos] ^= 0x15;
        INFO("symbol " << pos);
        REQUIRE(p25Fec_rsCheck(rx, 24, 12) == -1);
    }
}

TEST_CASE("CRC-16 of a TSBK catches a single flipped bit", "[p25][fec]")
{
    uint8_t block[P25_TSBK_BYTES];
    makeTsbk(block);

    REQUIRE(p25Fec_crc16(block, P25_TSBK_BYTES) == 0);

    for (int bit = 0; bit < 8 * P25_TSBK_BYTES; bit++) {
        uint8_t rx[P25_TSBK_BYTES];
        memcpy(rx, block, sizeof(rx));
        rx[bit >> 3] ^= (uint8_t)(0x80u >> (bit & 7));
        INFO("bit " << bit);
        REQUIRE(p25Fec_crc16(rx, P25_TSBK_BYTES) != 0);
    }
}

TEST_CASE("a trellis block with a broken CRC is refused", "[p25][fec]")
{
    /*
     * The TSBK path of the demodulator accepts a block only when the Viterbi
     * decoded it AND the CRC-16 behind it checks out (P25Phy.c, decodeTsbk).
     * The code corrects small channel errors on its own; past that the block
     * has to be thrown away, and the CRC is the only thing that can say so.
     */
    uint8_t block[P25_TSBK_BYTES];
    makeTsbk(block);

    uint8_t air[P25_TSBK_BITS];
    p25Fec_trellisEncode(block, air);

    uint8_t out[P25_TSBK_BYTES];
    const int errs = p25Fec_trellisDecode(air, out);
    REQUIRE(errs == 0);
    REQUIRE(memcmp(out, block, sizeof(out)) == 0);

    /* A handful of flips is inside the code's reach and must be corrected into
     * the very same block. */
    for (int flips = 4; flips <= 16; flips += 4) {
        uint8_t rx[P25_TSBK_BITS];
        memcpy(rx, air, sizeof(rx));
        for (int k = 0; k < flips; k++)
            rx[(k * 13) % P25_TSBK_BITS] ^= 1;

        INFO("flips " << flips);
        REQUIRE(p25Fec_trellisDecode(rx, out) >= 0);
        REQUIRE(p25Fec_crc16(out, P25_TSBK_BYTES) == 0);
        REQUIRE(memcmp(out, block, sizeof(out)) == 0);
    }

    /* Past it, the CRC must not answer zero: the pair is the acceptance rule */
    for (int flips = 24; flips <= 96; flips += 8) {
        uint8_t rx[P25_TSBK_BITS];
        memcpy(rx, air, sizeof(rx));
        for (int k = 0; k < flips; k++)
            rx[(k * 13) % P25_TSBK_BITS] ^= 1;

        const int rc = p25Fec_trellisDecode(rx, out);
        INFO("flips " << flips << " rc " << rc);
        REQUIRE((rc < 0 || p25Fec_crc16(out, P25_TSBK_BYTES) != 0));
    }
}
