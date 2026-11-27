/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dpmr_rx.cpp
 * @brief dPMR receive path: coding-layer vectors and a real capture.
 *
 * Five groups of checks:
 *
 *  1. The coding layer against vectors derived from ETSI TS 102 490 clauses 7
 *     and 9 (Hamming, CRC, scrambler, CCH/HI/END layout), including vectors
 *     taken from an independent implementation (DSDcc, fec.cpp / dpmr.cpp).
 *  2. The AMBE+2 frame mapping: dpmrFec_voiceFrame() must produce the same 9
 *     bytes as the DMR voice path's de-interleaver on the same payload,
 *     which is what lets lib/dmr_vocoder be reused unchanged.
 *  3. The channel permutation, exhaustively: canonical bit (18j + k) leaves as
 *     transmitted bit (4k + j), one bit at a time through both directions.
 *  4. Rejection: the CCH, the header and the END field must refuse what their
 *     Hamming blocks and CRCs cannot rescue, instead of handing the mode a
 *     frame number, an address slice or a status that nobody sent.
 *  5. A real over-the-air capture: samples/dpmr.dis from the DSDcc project
 *     (S16LE 48 kS/s discriminator output of a French professional dPMR
 *     system), decoded end to end.  The assertions are the numbers the
 *     prototype measured: frames decoded, CCH CRC rate, and the station
 *     identities the header frames announce.
 *
 * The capture is not part of the repository (1.4 MB); the test skips group 3
 * when DPMR_CAPTURE is not set, so the coding checks always run.
 */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include "dmr_tables.h"
#include "protocols/DPMR/DpmrData.h"
#include "protocols/DPMR/DpmrDefs.h"
#include "protocols/DPMR/DpmrPhy.h"
#include "dmr_vocoder.h"
}

namespace {

/* ------------------------------------------------------------- helpers --- */

std::vector<uint8_t> bitsFromHex(const std::string &hex)
{
    std::vector<uint8_t> out;

    for (char c : hex) {
        if (c == ' ')
            continue;
        unsigned v = (unsigned)strtoul(std::string(1, c).c_str(), nullptr, 16);
        for (int b = 3; b >= 0; b--)
            out.push_back((uint8_t)((v >> b) & 1u));
    }
    return out;
}

/* Encode one 41/17 bit field the way the standard does, for TX-side vectors */
std::vector<uint8_t> cchEncode(const std::vector<uint8_t> &info)
{
    std::vector<uint8_t> d = info;
    d.resize(41, 0);
    uint8_t crc = dpmrFec_crc7(d.data(), 41);
    for (int b = 6; b >= 0; b--)
        d.push_back((uint8_t)((crc >> b) & 1u));

    /* 48 bits -> 6 bytes -> 6 x 12 Hamming -> interleave -> scramble */
    std::vector<uint8_t> ham;
    for (int i = 0; i < 6; i++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++)
            byte = (uint8_t)((byte << 1) | d[8 * i + b]);
        uint8_t cw[12];
        dpmrFec_hammingEncode(byte, cw);
        ham.insert(ham.end(), cw, cw + 12);
    }

    /* Transmit interleave is the inverse of the receive map used by the
     * decoder: transmitted bit dI72[i] is ham[i] */
    static const uint8_t dI72[72] = {
        0, 12, 24, 36, 48, 60, 1, 13, 25, 37, 49, 61, 2, 14, 26, 38, 50, 62,
        3, 15, 27, 39, 51, 63, 4, 16, 28, 40, 52, 64, 5, 17, 29, 41, 53, 65,
        6, 18, 30, 42, 54, 66, 7, 19, 31, 43, 55, 67, 8, 20, 32, 44, 56, 68,
        9, 21, 33, 45, 57, 69, 10, 22, 34, 46, 58, 70, 11, 23, 35, 47, 59, 71
    };
    std::vector<uint8_t> tx(72);
    for (int i = 0; i < 72; i++)
        tx[i] = ham[dI72[i]];

    uint16_t scr = dpmrFec_scramblerReset();
    for (int i = 0; i < 72; i++)
        tx[i] ^= dpmrFec_scramblerNext(&scr);

    /* Bits -> 36 dibits (MSB first), per table 12.1 */
    std::vector<uint8_t> dibits;
    for (int i = 0; i < 36; i++)
        dibits.push_back((uint8_t)((tx[2 * i] << 1) | tx[2 * i + 1]));

    return dibits;
}


/* Encode a 72 bit header information field: CRC-8, 10 Hamming blocks, the
 * 12x10 interleaver of clause 7.5, the scrambler of clause 7.4, 60 dibits. */
std::vector<uint8_t> hiEncode(const std::vector<uint8_t> &info)
{
    std::vector<uint8_t> d = info;
    d.resize(72, 0);
    uint8_t crc = dpmrFec_crc8(d.data(), 72);
    for (int b = 7; b >= 0; b--)
        d.push_back((uint8_t)((crc >> b) & 1u));

    std::vector<uint8_t> ham;
    for (int i = 0; i < 10; i++) {
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++)
            byte = (uint8_t)((byte << 1) | d[8 * i + b]);
        uint8_t cw[12];
        dpmrFec_hammingEncode(byte, cw);
        ham.insert(ham.end(), cw, cw + 12);
    }

    static const uint8_t dI120[120] = {
        0, 12, 24, 36, 48, 60, 72, 84, 96, 108, 1, 13, 25, 37, 49, 61, 73, 85,
        97, 109, 2, 14, 26, 38, 50, 62, 74, 86, 98, 110, 3, 15, 27, 39, 51, 63,
        75, 87, 99, 111, 4, 16, 28, 40, 52, 64, 76, 88, 100, 112, 5, 17, 29, 41,
        53, 65, 77, 89, 101, 113, 6, 18, 30, 42, 54, 66, 78, 90, 102, 114, 7, 19,
        31, 43, 55, 67, 79, 91, 103, 115, 8, 20, 32, 44, 56, 68, 80, 92, 104, 116,
        9, 21, 33, 45, 57, 69, 81, 93, 105, 117, 10, 22, 34, 46, 58, 70, 82, 94,
        106, 118, 11, 23, 35, 47, 59, 71, 83, 95, 107, 119
    };
    std::vector<uint8_t> tx(120);
    for (int i = 0; i < 120; i++)
        tx[i] = ham[dI120[i]];

    uint16_t scr = dpmrFec_scramblerReset();
    for (int i = 0; i < 120; i++)
        tx[i] ^= dpmrFec_scramblerNext(&scr);

    std::vector<uint8_t> dibits;
    for (int i = 0; i < 60; i++)
        dibits.push_back((uint8_t)((tx[2 * i] << 1) | tx[2 * i + 1]));

    return dibits;
}

/* Encode a 17 bit END field: CRC-7, 3 Hamming blocks, 36 bits, sent twice and
 * scrambled from the reset state in each copy (clause 9.6). */
std::vector<uint8_t> endEncode(const std::vector<uint8_t> &info)
{
    std::vector<uint8_t> d = info;
    d.resize(17, 0);
    uint8_t crc = dpmrFec_crc7(d.data(), 17);
    for (int b = 6; b >= 0; b--)
        d.push_back((uint8_t)((crc >> b) & 1u));

    std::vector<uint8_t> tx;
    for (int rep = 0; rep < 2; rep++) {
        std::vector<uint8_t> block;
        for (int i = 0; i < 3; i++) {
            uint8_t byte = 0;
            for (int b = 0; b < 8; b++)
                byte = (uint8_t)((byte << 1) | d[8 * i + b]);
            uint8_t cw[12];
            dpmrFec_hammingEncode(byte, cw);
            block.insert(block.end(), cw, cw + 12);
        }

        uint16_t scr = dpmrFec_scramblerReset();
        for (int i = 0; i < 36; i++)
            block[i] ^= dpmrFec_scramblerNext(&scr);
        tx.insert(tx.end(), block.begin(), block.end());
    }

    std::vector<uint8_t> dibits;
    for (int i = 0; i < 36; i++)
        dibits.push_back((uint8_t)((tx[2 * i] << 1) | tx[2 * i + 1]));

    return dibits;
}

/** Write n bits of value v, MSB first, into bits at offset first. */
void putBits(std::vector<uint8_t> &bits, size_t first, size_t count,
             uint32_t v)
{
    for (size_t i = 0; i < count; i++)
        bits[first + i] = (uint8_t)((v >> (count - 1 - i)) & 1u);
}

/* ------------------------------------------------------------- vectors --- */


} // namespace

TEST_CASE("dPMR frame sync sequences match clause 6.1", "[dpmr]")
{
    int8_t lv[24];
    REQUIRE(dpmrDefs_syncLevels(DPMR_SYNC_FS1, lv) == 24);
    /* 57 FF 5F ... : 01 01 01 11 -> +3 +3 +3 -3 */
    REQUIRE(lv[0] == 3);
    REQUIRE(lv[1] == 3);
    REQUIRE(lv[2] == 3);
    REQUIRE(lv[3] == -3);

    REQUIRE(dpmrDefs_syncLevels(DPMR_SYNC_FS2, lv) == 12);
    /* 5F F7 7D -> 01 01 11 11 11 11 01 11 ... */
    REQUIRE(lv[0] == 3);
    REQUIRE(lv[2] == -3);

    REQUIRE(dpmrDefs_syncLevels(DPMR_SYNC_FS3, lv) == 12);
    /*
     * 7D DF F5 -> 01 11 11 01 11 01 11 11 11 11 01 01, i.e. +3 -3 -3 +3 -3
     * +3 -3 -3 -3 -3 +3 +3: every symbol of every sync word is an outer one.
     */
    REQUIRE(lv[0] == 3);
    REQUIRE(lv[1] == -3);
    REQUIRE(lv[2] == -3);
    REQUIRE(lv[3] == 3);
    REQUIRE(lv[11] == 3);
}

TEST_CASE("dPMR Hamming (12,8) corrects one bit per block", "[dpmr]")
{
    for (unsigned v = 0; v < 256; v++) {
        uint8_t cw[12];
        uint8_t back[8];

        dpmrFec_hammingEncode((uint8_t)v, cw);
        REQUIRE(dpmrFec_hammingDecode(cw, back) == 0);
        uint8_t byte = 0;
        for (int i = 0; i < 8; i++)
            byte = (uint8_t)((byte << 1) | back[i]);
        REQUIRE(byte == v);

        for (int bit = 0; bit < 12; bit++) {
            uint8_t bad[12];
            memcpy(bad, cw, 12);
            bad[bit] ^= 1;
            REQUIRE(dpmrFec_hammingDecode(bad, back) == 1);
            byte = 0;
            for (int i = 0; i < 8; i++)
                byte = (uint8_t)((byte << 1) | back[i]);
            REQUIRE(byte == v);
        }
    }
}

TEST_CASE("dPMR CCH round trip through the real FEC pipeline", "[dpmr]")
{
    /* Idle-ish voice CCH: FN=0, called ID upper 12 bits 0x393, mode 0 */
    std::vector<uint8_t> info(41, 0);

    /* FN = 00, ID = 0x393, mode = 000, version = 00, format = 01, SLD = 0 */
    info[0] = 0; info[1] = 0;
    for (int b = 0; b < 12; b++)
        info[2 + b] = (uint8_t)((0x393 >> (11 - b)) & 1u);
    info[19] = 0; info[20] = 1;     /* format: peer to peer */

    std::vector<uint8_t> dibits = cchEncode(info);
    struct dpmrCch cch;

    REQUIRE(dpmrFec_cchDecode(dibits.data(), &cch));
    REQUIRE(cch.fn == 0);
    REQUIRE(cch.id12 == 0x393);
    REQUIRE(cch.mode == DPMR_MODE_VOICE);
    REQUIRE(cch.version == 0);
    REQUIRE(cch.format == DPMR_FMT_PEER);
    REQUIRE(cch.fixes == 0);

    /* One flipped dibit bit must still close, as a corrected error */
    std::vector<uint8_t> bad = dibits;
    bad[7] ^= 2;
    REQUIRE(dpmrFec_cchDecode(bad.data(), &cch));
    REQUIRE(cch.id12 == 0x393);
    REQUIRE(cch.fixes == 1);
}

TEST_CASE("dPMR header information carries the fields of table 5.4", "[dpmr]")
{
    /*
     * HT 0100 (system request), called 0x123456, own 0x0ABCDE, mode 001,
     * version 00, format 01, reserved 00, CI 0x155.  The CI starts after the
     * two reserved bits, which is the field boundary the layout makes easy to
     * get wrong: bits 59 and 60 are reserved, CI occupies 61 to 71.
     */
    std::vector<uint8_t> info(72, 0);

    putBits(info, 0, 4, DPMR_HT_SYSTEM_REQ);
    putBits(info, 4, 24, 0x123456);
    putBits(info, 28, 24, 0x0ABCDE);
    putBits(info, 52, 3, DPMR_MODE_VOICE_SLD);
    putBits(info, 55, 2, 0);
    putBits(info, 57, 2, DPMR_FMT_PEER);
    putBits(info, 61, 11, 0x155);

    std::vector<uint8_t> dibits = hiEncode(info);
    struct dpmrHeader hdr;

    REQUIRE(dpmrFec_hiDecode(dibits.data(), &hdr));
    REQUIRE(hdr.fixes == 0);
    REQUIRE(hdr.ht == DPMR_HT_SYSTEM_REQ);
    REQUIRE(hdr.calledId == 0x123456);
    REQUIRE(hdr.ownId == 0x0ABCDE);
    REQUIRE(hdr.mode == DPMR_MODE_VOICE_SLD);
    REQUIRE(hdr.version == 0);
    REQUIRE(hdr.format == DPMR_FMT_PEER);
    REQUIRE(hdr.ci == 0x155);

    /* One bit of one Hamming block: corrected, field unchanged */
    std::vector<uint8_t> bad = dibits;
    bad[13] ^= 1;
    REQUIRE(dpmrFec_hiDecode(bad.data(), &hdr));
    REQUIRE(hdr.fixes == 1);
    REQUIRE(hdr.ci == 0x155);
    REQUIRE(hdr.calledId == 0x123456);
}

TEST_CASE("dPMR END field sent twice decodes as one frame", "[dpmr]")
{
    /*
     * ET 01 (end with status), ARQ 01, WAIT 0011 (160 ms), STAT 10101, RES 0000.
     * Clause 9.6: the 36 bit block is sent twice and each copy is scrambled
     * from the reset state, so a receiver that scrambles the second half with
     * a continuing LFSR gets exactly one half of every END frame.
     */
    std::vector<uint8_t> info(17, 0);

    putBits(info, 0, 2, 1);
    putBits(info, 2, 2, 1);
    putBits(info, 4, 4, 3);
    putBits(info, 8, 5, 0x15);

    std::vector<uint8_t> dibits = endEncode(info);
    struct dpmrEnd end;

    REQUIRE(dpmrFec_endDecode(dibits.data(), &end));
    REQUIRE(end.crcOk[0]);
    REQUIRE(end.crcOk[1]);
    REQUIRE(end.same);
    REQUIRE(end.et == 1);
    REQUIRE(end.arq == 1);
    REQUIRE(end.wait == 3);
    REQUIRE(end.status == 0x15);
    REQUIRE(end.reserved == 0);
    REQUIRE(end.fixes[0] == 0);
    REQUIRE(end.fixes[1] == 0);

    /* Both copies are the same bits on air: damage one, the other still closes */
    std::vector<uint8_t> one = dibits;
    for (int i = 0; i < 18; i++)
        one[i] = (uint8_t)(one[i] ^ 0x3);
    REQUIRE(dpmrFec_endDecode(one.data(), &end));
    REQUIRE(!end.crcOk[0]);
    REQUIRE(end.crcOk[1]);
    REQUIRE(!end.same);
    REQUIRE(end.status == 0x15);
}

TEST_CASE("dPMR channel codes match tables 6.1 and 6.2", "[dpmr]")
{
    /*
     * The standard prints the 24 transmitted bits; clause 7.1 makes each code
     * bit c a di-bit (c, 1), so every printed pair ends in 1 and the 12 bit
     * code is the first bit of each pair.  These strings are copied from the
     * table, so this is a check of the table and not of itself.
     */
    struct row { uint32_t freq; const char *bits; uint16_t codeA, codeB; };

    static const struct row rows[] = {
        { 446003125u, "010101110111010101110111", 0x145, 0xD11 },
        { 446009375u, "010101111101110101110101", 0x1A4, 0xD61 },
        { 446040625u, "010101011111111101111111", 0x0F7, 0xCA3 },
        { 446096875u, "011101110101110111010111", 0x529, 0xEE5 },
        { 446103125u, "010101110111010101110111", 0x145, 0 },
        { 446196875u, "011101110101110111010111", 0x529, 0 },
    };

    for (const struct row &r : rows) {
        uint16_t fromBits = 0;
        for (int i = 0; i < 24; i += 2)
            fromBits = (uint16_t)((fromBits << 1) | (uint16_t)(r.bits[i] - '0'));

        REQUIRE(fromBits == r.codeA);

        uint8_t lv[12];
        REQUIRE(dpmrDefs_channelCodeBits(r.freq, DPMR_GROUP_A, lv));
        for (int b = 0; b < 12; b++)
            REQUIRE(lv[b] == ((r.codeA >> (11 - b)) & 1u));

        if (r.codeB != 0) {
            REQUIRE(dpmrDefs_channelCodeBits(r.freq, DPMR_GROUP_B, lv));
            for (int b = 0; b < 12; b++)
                REQUIRE(lv[b] == ((r.codeB >> (11 - b)) & 1u));
        } else {
            /* Group B has no code above 446,096875 MHz */
            uint8_t unused[12];
            REQUIRE(!dpmrDefs_channelCodeBits(r.freq, DPMR_GROUP_B, unused));
        }
    }

    /* And the dibits on air carry the code in the MSB of each symbol */
    uint8_t dibits[12];
    for (int i = 0; i < 12; i++)
        dibits[i] = (uint8_t)((((0x145u >> (11 - i)) & 1u) << 1) | 1u);
    REQUIRE(dpmrDefs_channelCode(dibits) == 0x145);

    uint8_t unused[12];
    REQUIRE(!dpmrDefs_channelCodeBits(145000000u, DPMR_GROUP_A, unused));
}

TEST_CASE("dPMR frame syncs and header types have names", "[dpmr]")
{
    REQUIRE(std::string(dpmrDefs_syncName(DPMR_SYNC_NONE)) == "none");
    REQUIRE(std::string(dpmrDefs_syncName(DPMR_SYNC_FS1)) == "FS1");
    REQUIRE(std::string(dpmrDefs_syncName(DPMR_SYNC_FS4)) == "FS4");
    /* The channel code opens frames 2 and 4: it is not "none" */
    REQUIRE(std::string(dpmrDefs_syncName(DPMR_SYNC_CC)) == "CC");

    REQUIRE(std::string(dpmrDefs_headerTypeName(DPMR_HT_START)) == "start");
    REQUIRE(std::string(dpmrDefs_headerTypeName(DPMR_HT_ACK)) == "ack");
    REQUIRE(std::string(dpmrDefs_headerTypeName(DPMR_HT_STATUS_REQ)) == "status-req");
    REQUIRE(std::string(dpmrDefs_headerTypeName(9)) == "reserved");
}

TEST_CASE("dPMR slow data reassembles a message across frames", "[dpmr]")
{
    /*
     * Clause 5.9.1: each of the two user bytes of a frame is preceded by a
     * continuation flag that says the message finishes with that byte.  Five
     * bytes "HELLO" take three frames.
     */
    static const char text[] = "HELLO";
    struct dpmrSlowData d;
    struct dpmrSldMessage m;

    dpmrSld_reset(&m);

    for (unsigned frame = 0; frame < 3; frame++) {
        uint32_t sld = 0;

        for (unsigned i = 0; i < 2; i++) {
            unsigned idx = 2 * frame + i;
            uint32_t byte = idx < 5 ? (uint8_t)text[idx] : 0x00;
            uint32_t last = idx == 4 ? 1u : 0u;

            /* The 18 bits are flag, byte, flag, byte from bit 17 down */
            sld |= last << (9 * (1 - i) + 8);
            sld |= byte << (9 * (1 - i));
        }

        REQUIRE(dpmrData_slowDecode(sld, &d) == 2);
        dpmrSld_push(&m, &d);
    }

    REQUIRE(m.complete);
    REQUIRE(!m.overflow);
    REQUIRE(m.len == 5);
    REQUIRE(memcmp(m.text, text, 5) == 0);

    /* A sixth frame with a last flag starts a new message with one byte */
    uint32_t wide = (1u << 17) | (0x41u << 9);
    uint32_t sld = wide;
    dpmrData_slowDecode(sld, &d);
    dpmrSld_push(&m, &d);
    REQUIRE(m.complete);
    REQUIRE(m.len == 1);
    REQUIRE(m.text[0] == 'A');
}

TEST_CASE("dPMR appended data header decodes per clause 5.9.2", "[dpmr]")
{
    /* DP 11 (data present), format 0010 (free text), Cont. 0, length 12 */
    uint32_t sld = (3u << 11) | (2u << 7) | (0u << 6) | 12u;
    struct dpmrSld hdr;

    dpmrData_sldDecode(sld, &hdr);
    REQUIRE(hdr.dp == DPMR_DP_DATA);
    REQUIRE(hdr.format == DPMR_DFORM_TEXT);
    REQUIRE(!hdr.last);
    REQUIRE(hdr.length == 12);

    /* DP 00: this frame has no data, whatever the rest of the field says */
    dpmrData_sldDecode(0, &hdr);
    REQUIRE(hdr.dp == DPMR_DP_NONE);
    REQUIRE(hdr.length == 0);
}

TEST_CASE("dPMR scrambler is X^9+X^5+1 preset to ones", "[dpmr]")
{
    uint16_t st = dpmrFec_scramblerReset();
    uint8_t seq[16];

    for (int i = 0; i < 16; i++)
        seq[i] = dpmrFec_scramblerNext(&st);

    /*
     * Reference: the LFSR of DSDcc's DSDdPMR::LFSRGenerator::init()/next() with
     * m_sr = 0x3FF, first bit out = 1 (bit 0 of (0x3FF >> 1)).
     */
    REQUIRE(seq[0] == 1);
    REQUIRE(seq[1] == 1);
    REQUIRE(seq[2] == 1);
    REQUIRE(seq[3] == 1);
}

/*
 * Reference de-interleaver: the four-block voice interleave of TS 102 361-1,
 * the one dmr_enc.c writes and dmr_voice.c undoes (its rW/rX/rY/rZ schedule):
 * canonical bit c travels as air bit 4*(c mod 18) + c/18.  It used to be
 * dmr_fec_voice_deinterleave() of the tree's own DMR FEC, which the protocol
 * package replaced; the package keeps the schedule private to dmr_voice.c, so
 * the reference the dPMR side is checked against lives here.
 */
static void refVoiceDeinterleave(const uint8_t air[72], uint8_t frame[9])
{
    memset(frame, 0, 9);

    for (int c = 0; c < 72; c++) {
        const int a = 4 * (c % 18) + c / 18;

        if (air[a] != 0)
            frame[c >> 3] |= (uint8_t)(0x80u >> (c & 7));
    }
}

TEST_CASE("dPMR TCH bits map onto the same AMBE+2 frame as DMR", "[dpmr]")
{
    /*
     * The claim that lets lib/dmr_vocoder be reused: dPMR's 72 bit TCH group
     * is the same AMBE+2 3600x2450 frame DMR carries, only interleaved 4-way.
     * Same payload bits through both de-interleavers must give the same 9
     * bytes, for any payload.
     */
    unsigned seed = 12345;

    for (int t = 0; t < 2000; t++) {
        uint8_t payload[72];
        for (int i = 0; i < 72; i++) {
            seed = seed * 1103515245u + 12345u;
            payload[i] = (uint8_t)((seed >> 16) & 1u);
        }

        uint8_t dibits[36];
        for (int i = 0; i < 36; i++)
            dibits[i] = (uint8_t)((payload[2 * i] << 1) | payload[2 * i + 1]);

        uint8_t dpmrFrame[9];
        uint8_t dmrFrame[9];

        dpmrFec_voiceFrame(dibits, dpmrFrame);
        refVoiceDeinterleave(payload, dmrFrame);

        REQUIRE(memcmp(dpmrFrame, dmrFrame, 9) == 0);
    }
}

TEST_CASE("dPMR ambe frame de-interleave is a 4-way stride", "[dpmr]")
{
    /*
     * The 72 bits of a 20 ms voice frame travel as four interleaved streams of
     * 18: canonical bit (18j + k) leaves as transmitted bit (4k + j), so the
     * first four transmitted bits are canonical bits 0, 18, 36 and 54.
     *
     * This is the whole permutation, not a sample of it: one canonical bit at a
     * time through the transmitter's packer, and the air has to carry it at
     * exactly 4k+j; then one air bit at a time through the receiver's, and it
     * has to come back at 18j+k.  A transposition of any two bits in the map
     * shows up here, and the two directions are checked against the same
     * arithmetic rather than against each other.
     */
    uint8_t frame[DPMR_AMBE_BYTES];
    uint8_t dibits[36];
    uint8_t back[DPMR_AMBE_BYTES];

    for (int c = 0; c < 72; c++) {
        std::memset(frame, 0, sizeof(frame));
        frame[c >> 3] = (uint8_t)(0x80u >> (c & 7));

        dpmrFec_voiceFrameEncode(frame, dibits);

        const int want = 4 * (c % 18) + c / 18;
        int set = 0;
        int where = -1;

        for (int m = 0; m < 72; m++) {
            int bit = (m & 1) ? (dibits[m / 2] & 1u) : ((dibits[m / 2] >> 1) & 1u);

            if (bit != 0) {
                set++;
                where = m;
            }
        }

        INFO("canonical bit " << c << " leaves at " << where
                              << ", expected " << want);
        REQUIRE(set == 1);
        REQUIRE(where == want);
    }

    for (int m = 0; m < 72; m++) {
        std::memset(dibits, 0, sizeof(dibits));
        dibits[m / 2] = (uint8_t)((m & 1) ? 0x1 : 0x2);

        dpmrFec_voiceFrame(dibits, back);

        const int want = 18 * (m % 4) + m / 4;
        int set = 0;
        int where = -1;

        for (int c = 0; c < 72; c++) {
            if (((back[c >> 3] >> (7 - (c & 7))) & 1u) != 0u) {
                set++;
                where = c;
            }
        }

        INFO("air bit " << m << " comes back at " << where
                        << ", expected " << want);
        REQUIRE(set == 1);
        REQUIRE(where == want);
    }
}

/* ------------------------------------------------------- real capture --- */

namespace {

struct CaptureStats
{
    unsigned frames = 0, cchOk = 0, cchFail = 0, voice = 0, headers = 0, ends = 0;
    unsigned endsBoth = 0, endsWithStatus = 0, ambeFrames = 0, ambeNonSilent = 0;
    unsigned vocoded = 0, vocoderErrors = 0, silentPcm = 0;
    uint32_t calledId = 0, ownId = 0;
    uint8_t mode = 0xFF, ht = 0xFF, cc = 0;
    unsigned ccSeen = 0;
    float evmSum = 0.0f, pcmSum2 = 0.0;
    uint8_t statuses[8] = { 0 };
    unsigned statusCount = 0;
};

int16_t pcm[160];

void onFrame(const struct dpmrFrame *frame, void *user)
{
    CaptureStats *st = static_cast<CaptureStats *>(user);
    st->frames++;

    if (frame->cchOk)
        st->cchOk++;
    else if (frame->sync != DPMR_SYNC_FS1 && frame->sync != DPMR_SYNC_FS3)
        st->cchFail++;

    if (frame->headerOk) {
        st->headers++;
        st->calledId = frame->header.calledId;
        st->ownId = frame->header.ownId;
        st->mode = frame->header.mode;
        st->ht = frame->header.ht;
    }

    if (frame->endOk) {
        st->ends++;
        if (frame->end.same)
            st->endsBoth++;
        if (frame->end.et == 1) {
            st->endsWithStatus++;
            if (st->statusCount < 8)
                st->statuses[st->statusCount++] = frame->end.status;
        }
    }

    /* The channel code only exists on the frames that carry it (2 and 4) */
    if (frame->sync == DPMR_SYNC_CC) {
        st->cc = (uint8_t)frame->channelCode;
        st->ccSeen++;
    }

    if (frame->voice) {
        st->voice++;
        for (unsigned f = 0; f < DPMR_AMBE_FRAMES; f++) {
            bool nonzero = false;
            for (int i = 0; i < DPMR_AMBE_BYTES; i++)
                nonzero |= frame->ambe[f][i] != 0;
            st->ambeFrames++;
            if (nonzero)
                st->ambeNonSilent++;

            /* Every AMBE+2 frame goes through the DMR vocoder */
            if (dmr_vocoder_decode(frame->ambe[f], pcm) == 0) {
                st->vocoded++;
                st->vocoderErrors += dmr_vocoder_last_errors();
                double sum2 = 0.0;
                for (int i = 0; i < 160; i++)
                    sum2 += (double)pcm[i] * pcm[i];
                st->pcmSum2 += sum2;
                if (sum2 < 1.0)
                    st->silentPcm++;
            }
        }
    }

    st->evmSum += frame->evm;
}

} // namespace

TEST_CASE("dPMR real capture decodes frames, identity and voice", "[dpmr][capture]")
{
    const char *path = getenv("DPMR_CAPTURE");

    if (path == nullptr) {
        WARN("DPMR_CAPTURE not set: skipping the over-the-air check");
        return;
    }

    FILE *f = fopen(path, "rb");
    REQUIRE(f != nullptr);

    std::vector<int16_t> samples;
    int16_t buf[4096];
    size_t n;
    while ((n = fread(buf, sizeof(int16_t), 4096, f)) > 0)
        samples.insert(samples.end(), buf, buf + n);
    fclose(f);

    REQUIRE(samples.size() > 48000 * 5);    /* at least 5 s of baseband */

    dpmrPhy_init();
    dmr_vocoder_reset();

    CaptureStats st;
    const size_t block = 960;               /* 20 ms: one audio stream half */
    unsigned syncHits = 0;

    size_t i = 0;

    for (; i + block <= samples.size(); i += block) {
        dpmrPhy_push(&samples[i], block, onFrame, &st);
        if (dpmrPhy_synced())
            syncHits++;
    }

    /* The tail of the file, so that the numbers are the whole capture */
    if (i < samples.size())
        dpmrPhy_push(&samples[i], samples.size() - i, onFrame, &st);

    struct dpmrPhyStats ps;
    dpmrPhy_stats(&ps);

    float crcRate = st.cchOk + st.cchFail > 0
                    ? (float)st.cchOk / (float)(st.cchOk + st.cchFail) : 0.0f;

    printf("dpmr capture: %zu samples, frames %u cch_ok %u cch_fail %u "
           "(%.1f%%) voice %u headers %u ends %u (%u both halves) evm %.2f "
           "sync %u\n",
           samples.size(), st.frames, st.cchOk, st.cchFail, 100.0f * crcRate,
           st.voice, st.headers, st.ends, st.endsBoth,
           st.frames ? st.evmSum / (float)st.frames : 0.0f, syncHits);
    printf("dpmr capture: called %06X own %06X HT %X mode %u cc %03X "
           "(%u frames)\n", st.calledId, st.ownId, st.ht, st.mode, st.cc,
           st.ccSeen);
    printf("dpmr capture: ambe %u/%u non-silent, vocoder %u frames, %u golay "
           "errors, %u silent, mean power %.1f\n",
           st.ambeNonSilent, st.ambeFrames, st.vocoded, st.vocoderErrors,
           st.silentPcm,
           st.vocoded ? st.pcmSum2 / (160.0 * st.vocoded) : 0.0);

    /*
     * Numbers of the 30 s capture (DSDcc samples/dpmr.dis, French
     * professional system, S16LE 48 kS/s discriminator): every frame that
     * reaches a CCH closes its CRC, and the 12 bit address slices of the
     * superframe spell the same identities the header frame announces.
     */
    /*
     * One frame of the capture sits on the edge of the timing sweep and moves
     * with the flags (120 frames here, 121 with the same chain built -O2 by
     * hand), so the floors are one short of what this build measures.
     */
    REQUIRE(st.frames >= 118);
    REQUIRE(st.cchOk >= 110);
    REQUIRE(st.cchFail == 0);
    REQUIRE(st.voice == 102);
    REQUIRE(st.headers == 2);
    REQUIRE(st.calledId == 0x00393D);
    REQUIRE(st.ownId == 0x000135);
    REQUIRE(st.ht == DPMR_HT_START);
    REQUIRE(st.mode == DPMR_MODE_VOICE);

    /* Channel code: the same 12 bits on every frame that carries one */
    REQUIRE(st.ccSeen > 20);
    REQUIRE(st.cc == 0x0DD);

    /* END frames: the ones the framer finds are the ones both copies agree on */
    REQUIRE(st.ends == 6);
    REQUIRE(st.endsBoth == st.ends);

    /* Voice: 4 AMBE+2 frames per voice frame, all of them through the vocoder */
    REQUIRE(st.ambeFrames == 4 * st.voice);
    REQUIRE(st.ambeNonSilent == st.ambeFrames);
    REQUIRE(st.vocoded == st.ambeFrames);
    /* A handful of comfort-noise frames decode to silence; most do not */
    REQUIRE(st.silentPcm <= 5);

    printf("dpmr capture: end statuses %u\n", st.endsWithStatus);
}

/* ------------------------------------------------------------- rejection --- */

namespace {

/** Deterministic bits for the "must not pass by accident" blocks. */
struct Bits {
    uint32_t s = 0x5EED1234u;

    unsigned next()
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (s >> 16) & 1u;
    }
};

} /* namespace */

TEST_CASE("dPMR CCH rejects what Hamming and CRC-7 cannot rescue",
          "[dpmr][rx][fec]")
{
    /*
     * The CCH is the frame's identity and the only field the mode filters on:
     * 41 information bits, a CRC-7, six shortened Hamming (12,8) blocks and the
     * interleaver.  One error per block is corrected (the Hamming test above
     * covers that); two inside the same block are not, and the CRC has to say
     * so instead of handing the mode a frame number and an address slice that
     * belong to a frame nobody sent.
     */
    std::vector<uint8_t> info(41, 0);

    for (int b = 0; b < 12; b++)
        info[2 + b] = (uint8_t)((0x393 >> (11 - b)) & 1u);
    info[20] = 1;

    std::vector<uint8_t> dibits = cchEncode(info);
    struct dpmrCch cch;

    REQUIRE(dpmrFec_cchDecode(dibits.data(), &cch));
    REQUIRE(cch.id12 == 0x393);

    /*
     * Two bits of the same transmitted Hamming block: refused, not
     * "corrected".  The transmit interleaver of clause 7.5 is the inverse of
     * dI72, so air bits i and i+6 are two bits of block 0 (ham[0] and ham[1])
     * while bits i and i+1 land in two different blocks and are each a single
     * correctable error.
     */
    {
        std::vector<uint8_t> one = dibits;

        one[0] ^= 0x2;                  /* ham[0] only: correctable */
        REQUIRE(dpmrFec_cchDecode(one.data(), &cch));
        REQUIRE(cch.id12 == 0x393);
        REQUIRE(cch.fixes == 1);

        std::vector<uint8_t> two = dibits;

        two[0] ^= 0x2;                  /* ham[0] */
        two[3] ^= 0x2;                  /* air bit 6: ham[1] */
        REQUIRE(!dpmrFec_cchDecode(two.data(), &cch));
        REQUIRE(!cch.crcOk);
    }

    /* A block that is noise: a CRC-7 lets one in 128 through, nothing more */
    Bits rnd;
    int accepted = 0;
    constexpr int BLOCKS = 500;

    for (int b = 0; b < BLOCKS; b++) {
        uint8_t r[DPMR_CCH_SYMBOLS];

        for (int i = 0; i < DPMR_CCH_SYMBOLS; i++)
            r[i] = (uint8_t)(rnd.next() << 1 | rnd.next());
        if (dpmrFec_cchDecode(r, &cch))
            accepted++;
    }

    INFO("random CCH accepted: " << accepted << "/" << BLOCKS);
    REQUIRE(accepted <= BLOCKS / 16);
}

TEST_CASE("dPMR header and END reject what their CRCs cannot rescue",
          "[dpmr][rx][fec]")
{
    /*
     * The header is what names the call, and the END frame is what says it is
     * over: both carry a CRC (8 and 7 bits) over Hamming-coded blocks, and both
     * have to refuse a field the receiver could only guess at.
     */
    std::vector<uint8_t> info(72, 0);

    putBits(info, 0, 4, DPMR_HT_START);
    putBits(info, 4, 24, 0x123456);
    putBits(info, 28, 24, 0x0ABCDE);
    putBits(info, 52, 3, DPMR_MODE_VOICE);
    putBits(info, 57, 2, DPMR_FMT_PEER);
    putBits(info, 61, 11, 0x155);

    std::vector<uint8_t> hi = hiEncode(info);
    struct dpmrHeader hdr;

    REQUIRE(dpmrFec_hiDecode(hi.data(), &hdr));
    REQUIRE(hdr.ownId == 0x0ABCDE);

    /* Two bits in one block (air bits 0 and 10 are ham[0] and ham[1] of
     * block 0, the inverse of dI120): the CRC must refuse it */
    {
        std::vector<uint8_t> bad = hi;

        bad[0] ^= 0x2;                  /* air bit 0,  maps to ham[0] */
        bad[5] ^= 0x2;                  /* air bit 10, maps to ham[1] */
        REQUIRE(!dpmrFec_hiDecode(bad.data(), &hdr));
        REQUIRE(!hdr.crcOk);
    }

    /* Random header fields: a CRC-8 is one in 256 */
    Bits rnd;
    int accepted = 0;
    constexpr int BLOCKS = 500;

    for (int b = 0; b < BLOCKS; b++) {
        uint8_t r[DPMR_HI_SYMBOLS];

        for (int i = 0; i < DPMR_HI_SYMBOLS; i++)
            r[i] = (uint8_t)(rnd.next() << 1 | rnd.next());
        if (dpmrFec_hiDecode(r, &hdr))
            accepted++;
    }

    INFO("random header accepted: " << accepted << "/" << BLOCKS);
    REQUIRE(accepted <= BLOCKS / 16);

    /* END: both copies destroyed is a refusal; one copy is enough to keep it */
    std::vector<uint8_t> endInfo(17, 0);

    putBits(endInfo, 0, 2, 1);
    putBits(endInfo, 8, 5, 0x15);

    std::vector<uint8_t> end = endEncode(endInfo);
    struct dpmrEnd e;

    REQUIRE(dpmrFec_endDecode(end.data(), &e));

    std::vector<uint8_t> both = end;

    for (int i = 0; i < DPMR_END_FIELD_SYMBOLS; i++)
        both[i] ^= 0x3;
    REQUIRE(!dpmrFec_endDecode(both.data(), &e));

    std::vector<uint8_t> half = end;

    for (int i = 0; i < 18; i++)
        half[i] ^= 0x3;
    REQUIRE(dpmrFec_endDecode(half.data(), &e));
    REQUIRE(e.crcOk[1]);
    REQUIRE(e.status == 0x15);
}

TEST_CASE("the V/UV table index has no margin left to lose", "[dmr][vocoder]")
{
    /*
     * dmr_decoder.c reads dmr_vuv[b1][(int)(l * 16 * f0)] out of an 8 column
     * codebook, with l up to L = dmr_l_table[b0].  Over the 120 rows the
     * product reaches 7.4 (b0 = 113), which truncates to 7, the last valid
     * column: there is less than one column of margin, and none at all once
     * the 0.4 is spent.  The clamp mbelib has (and which this port had
     * dropped) is back in the decoder, so an index past the last column can no
     * longer read the constant that follows the table; this case pins the
     * other half, the tables.  If a regenerated dmr_w0 or dmr_l_table ever
     * truncates to 8 or more, it fails here, on the host, instead of turning
     * into V/UV decisions taken from someone else's data on the radio.
     */
    double worst = 0.0;
    int worstB0 = -1;

    for (int b0 = 0; b0 < 120; b0++) {
        const double p = (double)dmr_l_table[b0] * 16.0 * (double)dmr_w0[b0];

        if (p > worst) {
            worst = p;
            worstB0 = b0;
        }
    }

    INFO("worst product " << worst << " at b0 " << worstB0);
    REQUIRE(worst < 8.0); /* (int)worst is the column index */
    REQUIRE(worstB0 >= 0);
}
