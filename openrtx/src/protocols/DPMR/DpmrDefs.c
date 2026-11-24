/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DpmrDefs.c
 * @brief dPMR coding layer. See DpmrDefs.h for the clause of each piece.
 */

#include "protocols/DPMR/DpmrDefs.h"

#include <string.h>

/* ---------------------------------------------------------------- sync --- */

size_t dpmrDefs_syncLevels(enum dpmrSync sync, int8_t *out)
{
    uint64_t word = 0;
    size_t bits = 0;

    switch (sync) {
        case DPMR_SYNC_FS1: word = DPMR_FS1_BITS; bits = 48; break;
        case DPMR_SYNC_FS2: word = DPMR_FS2_BITS; bits = 24; break;
        case DPMR_SYNC_FS3: word = DPMR_FS3_BITS; bits = 24; break;
        case DPMR_SYNC_FS4: word = DPMR_FS4_BITS; bits = 48; break;
        default: return 0;
    }

    /*
     * Table 12.1: 01 -> +3, 00 -> +1, 10 -> -1, 11 -> -3, in units of 350 Hz.
     * The sync sequences of clause 6.1 contain only 01 and 11 pairs (outer
     * symbols), which is why the run doubles as the level reference.
     */
    for (size_t i = 0; i < bits / 2; i++) {
        unsigned shift = (unsigned)(bits - 2 - 2 * i);
        unsigned dibit = (unsigned)((word >> shift) & 3u);
        out[i] = (dibit == 1) ? 3 : (dibit == 0) ? 1 : (dibit == 2) ? -1 : -3;
    }

    return bits / 2;
}

const char *dpmrDefs_syncName(enum dpmrSync sync)
{
    static const char *const names[] = { "none", "FS1", "FS2", "FS3", "FS4", "CC" };

    if ((unsigned)sync >= (sizeof(names) / sizeof(names[0])))
        return names[0];

    return names[sync];
}

const char *dpmrDefs_headerTypeName(uint8_t ht)
{
    static const char *const names[] = {
        "start", "connect", "disconnect", "ack",
        "system-req", "system-ack", "system-data", "status-resp", "status-req"
    };

    if (ht >= (sizeof(names) / sizeof(names[0])))
        return "reserved";

    return names[ht];
}

const char *dpmrDefs_modeName(uint8_t mode)
{
    static const char *const names[] = { "voice", "voice+sld", "data1",
                                         "data2", "data3", "voice+data" };

    if (mode >= (sizeof(names) / sizeof(names[0])))
        return "reserved";

    return names[mode];
}

/* --------------------------------------------------------- channel code --- */

/*
 * Tables 6.1 and 6.2 of clause 6.1.5.  The standard prints each channel code
 * as the 24 transmitted bits, and clause 7.1 expands every code bit c into the
 * di-bit (c, 1): 0 -> 01, 1 -> 11.  Every one of the 64 printed sequences ends
 * its pairs in 1, so the 12 bit code is the first bit of each pair, and the
 * test checks it against the printed strings rather than against this table.
 *
 * Group A (Initial Services and Facilities) and group B (Configured Services
 * and Facilities) are different codes on the same frequencies: a group A radio
 * on 446,003125 MHz sends 0x145, a group B radio sends 0xD11.  Group A covers
 * 446,003125 to 446,196875 MHz, group B only the lower 16 channels.
 */
struct dpmrChannel
{
    uint32_t freqHz;
    uint16_t codeA;             /* 12 bits, group A */
    uint16_t codeB;             /* 12 bits, group B, 0 if the channel has none */
};

static const struct dpmrChannel dpmrChannels[] = {
    { 446003125u, 0x145, 0xD11 }, { 446009375u, 0x1A4, 0xD61 },
    { 446015625u, 0x1D4, 0xD80 }, { 446021875u, 0x016, 0xDF0 },
    { 446028125u, 0x066, 0xC32 }, { 446034375u, 0x087, 0xC42 },
    { 446040625u, 0x0F7, 0xCA3 }, { 446046875u, 0x303, 0xCD3 },
    { 446053125u, 0x373, 0xF27 }, { 446059375u, 0x392, 0xF57 },
    { 446065625u, 0x3E2, 0xFB6 }, { 446071875u, 0x220, 0xFC6 },
    { 446078125u, 0x250, 0xE04 }, { 446084375u, 0x2B1, 0xE74 },
    { 446090625u, 0x2C1, 0xE95 }, { 446096875u, 0x529, 0xEE5 },
    { 446103125u, 0x145, 0 },     { 446109375u, 0x1A4, 0 },
    { 446115625u, 0x1D4, 0 },     { 446121875u, 0x016, 0 },
    { 446128125u, 0x066, 0 },     { 446134375u, 0x087, 0 },
    { 446140625u, 0x0F7, 0 },     { 446146875u, 0x303, 0 },
    { 446153125u, 0x373, 0 },     { 446159375u, 0x392, 0 },
    { 446165625u, 0x3E2, 0 },     { 446171875u, 0x220, 0 },
    { 446178125u, 0x250, 0 },     { 446184375u, 0x2B1, 0 },
    { 446190625u, 0x2C1, 0 },     { 446196875u, 0x529, 0 },
};

#define DPMR_NUM_CHANNELS (sizeof(dpmrChannels) / sizeof(dpmrChannels[0]))

bool dpmrDefs_channelCodeBits(uint32_t freqHz, enum dpmrChannelGroup group,
                              uint8_t out[12])
{
    for (size_t i = 0; i < DPMR_NUM_CHANNELS; i++) {
        if (dpmrChannels[i].freqHz != freqHz)
            continue;

        uint16_t code = (group == DPMR_GROUP_B) ? dpmrChannels[i].codeB
                                                : dpmrChannels[i].codeA;
        if (code == 0)
            return false;

        for (unsigned b = 0; b < 12; b++)
            out[b] = (uint8_t)((code >> (11 - b)) & 1u);

        return true;
    }

    return false;
}

uint16_t dpmrDefs_channelCode(const uint8_t dibits[DPMR_SYNC_SYMBOLS])
{
    /* Clause 7.1: 0 -> 01, 1 -> 11, so the sign of the symbol is the code bit */
    uint16_t code = 0;

    for (unsigned i = 0; i < DPMR_SYNC_SYMBOLS; i++)
        code = (uint16_t)((code << 1) | ((dibits[i] >> 1) & 1u));

    return code;
}

/* ------------------------------------------------------------ CRC / FEC --- */

uint8_t dpmrFec_crc7(const uint8_t *bits, size_t nbits)
{
    uint8_t work[41 + 7];
    size_t n = nbits > 41 ? 41 : nbits;

    memcpy(work, bits, n);
    memset(work + n, 0, 7);

    for (size_t i = 0; i < n; i++) {
        if (work[i] != 0) {
            work[i] = 0;
            work[i + 4] ^= 1;
            work[i + 7] ^= 1;
        }
    }

    uint8_t crc = 0;

    for (size_t i = 0; i < 7; i++)
        crc = (uint8_t)((crc << 1) | work[n + i]);

    return crc;
}

uint8_t dpmrFec_crc8(const uint8_t *bits, size_t nbits)
{
    uint8_t work[72 + 8];
    size_t n = nbits > 72 ? 72 : nbits;

    memcpy(work, bits, n);
    memset(work + n, 0, 8);

    for (size_t i = 0; i < n; i++) {
        if (work[i] != 0) {
            work[i] = 0;
            work[i + 6] ^= 1;
            work[i + 7] ^= 1;
            work[i + 8] ^= 1;
        }
    }

    uint8_t crc = 0;

    for (size_t i = 0; i < 8; i++)
        crc = (uint8_t)((crc << 1) | work[n + i]);

    return crc;
}

/*
 * Clause 7.3, table 7.1: the eight rows are the data bits X7..1 and their four
 * parity bits C3..C0, so the codeword is data[0..7] followed by C3..C0.  The
 * parity check matrix is therefore [P^T | I4] and every single-bit error has
 * its own syndrome.
 */
static const uint8_t hammingParity[8] = { 0xE, 0x7, 0xA, 0x5,
                                          0xB, 0xC, 0x6, 0x3 };

void dpmrFec_hammingEncode(uint8_t byte, uint8_t out[12])
{
    uint8_t par = 0;

    for (unsigned i = 0; i < 8; i++) {
        out[i] = (uint8_t)((byte >> (7 - i)) & 1u);
        if (out[i])
            par ^= hammingParity[i];
    }

    for (unsigned i = 0; i < 4; i++)
        out[8 + i] = (uint8_t)((par >> (3 - i)) & 1u);
}

int dpmrFec_hammingDecode(const uint8_t in[12], uint8_t out[8])
{
    uint8_t syndrome = 0;
    uint8_t cw[12];

    memcpy(cw, in, 12);

    for (unsigned row = 0; row < 4; row++) {
        uint8_t s = 0;

        for (unsigned i = 0; i < 8; i++)
            s ^= (uint8_t)(cw[i] & ((hammingParity[i] >> (3 - row)) & 1u));

        s ^= cw[8 + row];
        syndrome = (uint8_t)((syndrome << 1) | s);
    }

    if (syndrome != 0) {
        int pos = -1;

        for (unsigned i = 0; i < 8; i++) {
            if (hammingParity[i] == syndrome)
                pos = (int)i;
        }
        if (pos < 0) {
            uint8_t par = (uint8_t)(syndrome & 0xF);

            if ((par & (par - 1)) != 0)
                return -1;      /* two or more errors: uncorrectable */
            for (unsigned i = 0; i < 4; i++) {
                if (par == (1u << (3 - i)))
                    pos = (int)(8 + i);
            }
        }
        if (pos < 0)
            return -1;

        cw[pos] ^= 1;
    }

    for (unsigned i = 0; i < 8; i++)
        out[i] = cw[i];

    return syndrome != 0 ? 1 : 0;
}

uint16_t dpmrFec_scramblerReset(void)
{
    return 0x3FF;               /* clause 7.4: preset all ones */
}

uint8_t dpmrFec_scramblerNext(uint16_t *state)
{
    uint16_t sr = *state;

    sr >>= 1;
    uint8_t res = (uint8_t)(sr & 1u);
    uint16_t feedback = (uint16_t)((((sr >> 4) & 1u) ^ res) << 9);

    *state = (uint16_t)((sr & 0x1FFu) | feedback);

    return res;
}

/*
 * Clause 7.5: the receive de-interleaver writes the transmitted bits into the
 * matrix in rows and reads it out in columns, which is this index map.
 */
static const uint8_t dI72[72] = {
    0, 12, 24, 36, 48, 60, 1, 13, 25, 37, 49, 61, 2, 14, 26, 38, 50, 62,
    3, 15, 27, 39, 51, 63, 4, 16, 28, 40, 52, 64, 5, 17, 29, 41, 53, 65,
    6, 18, 30, 42, 54, 66, 7, 19, 31, 43, 55, 67, 8, 20, 32, 44, 56, 68,
    9, 21, 33, 45, 57, 69, 10, 22, 34, 46, 58, 70, 11, 23, 35, 47, 59, 71
};

static const uint8_t dI120[120] = {
    0, 12, 24, 36, 48, 60, 72, 84, 96, 108, 1, 13, 25, 37, 49, 61, 73, 85,
    97, 109, 2, 14, 26, 38, 50, 62, 74, 86, 98, 110, 3, 15, 27, 39, 51, 63,
    75, 87, 99, 111, 4, 16, 28, 40, 52, 64, 76, 88, 100, 112, 5, 17, 29, 41,
    53, 65, 77, 89, 101, 113, 6, 18, 30, 42, 54, 66, 78, 90, 102, 114, 7, 19,
    31, 43, 55, 67, 79, 91, 103, 115, 8, 20, 32, 44, 56, 68, 80, 92, 104, 116,
    9, 21, 33, 45, 57, 69, 81, 93, 105, 117, 10, 22, 34, 46, 58, 70, 82, 94,
    106, 118, 11, 23, 35, 47, 59, 71, 83, 95, 107, 119
};

/**
 * De-interleave and descramble a field: transmitted symbol s carries bits
 * 2s (MSB) and 2s+1, each XORed with the scrambler in transmission order, and
 * lands at index map[i].
 */
static void deinterleave(const uint8_t *dibits, size_t nsymbols,
                         const uint8_t *map, uint8_t *bits)
{
    uint16_t scr = dpmrFec_scramblerReset();

    for (size_t s = 0; s < nsymbols; s++) {
        uint8_t msb = (uint8_t)(((dibits[s] >> 1) & 1u) ^ dpmrFec_scramblerNext(&scr));
        uint8_t lsb = (uint8_t)((dibits[s] & 1u) ^ dpmrFec_scramblerNext(&scr));

        bits[map[2 * s]] = msb;
        bits[map[2 * s + 1]] = lsb;
    }
}

/** Hamming-decode nblocks of 12 bits into nblocks bytes; returns fixes or -1. */
static int hammingDecodeBlocks(const uint8_t *bits, size_t nblocks,
                               uint8_t *data)
{
    int fixes = 0;

    for (size_t b = 0; b < nblocks; b++) {
        int r = dpmrFec_hammingDecode(bits + 12 * b, data + 8 * b);

        if (r < 0)
            fixes += 2;         /* uncorrectable: reported, CRC will fail */
        else
            fixes += r;
    }

    return fixes;
}

/** Pack n bits (MSB first) into a value. */
static uint32_t packBits(const uint8_t *bits, size_t first, size_t count)
{
    uint32_t v = 0;

    for (size_t i = 0; i < count; i++)
        v = (v << 1) | (uint32_t)(bits[first + i] & 1u);

    return v;
}

bool dpmrFec_cchDecode(const uint8_t dibits[DPMR_CCH_SYMBOLS],
                       struct dpmrCch *out)
{
    uint8_t bits[72];
    uint8_t data[48];

    memset(out, 0, sizeof(*out));
    deinterleave(dibits, DPMR_CCH_SYMBOLS, dI72, bits);
    out->fixes = (uint8_t)hammingDecodeBlocks(bits, 6, data);

    /* Clause 7.6: 41 information bits, then the 7 bit CRC */
    uint8_t crc = dpmrFec_crc7(data, 41);
    uint8_t rx = (uint8_t)packBits(data, 41, 7);

    out->crcOk = (crc == rx);
    out->fn = (uint8_t)packBits(data, 0, 2);
    out->id12 = (uint16_t)packBits(data, 2, 12);
    out->mode = (uint8_t)packBits(data, 14, 3);
    out->version = (uint8_t)packBits(data, 17, 2);
    out->format = (uint8_t)packBits(data, 19, 2);
    out->sld = packBits(data, 23, 18);

    return out->crcOk;
}

bool dpmrFec_hiDecode(const uint8_t dibits[DPMR_HI_SYMBOLS],
                      struct dpmrHeader *out)
{
    uint8_t bits[120];
    uint8_t data[80];

    memset(out, 0, sizeof(*out));
    deinterleave(dibits, DPMR_HI_SYMBOLS, dI120, bits);
    out->fixes = (uint8_t)hammingDecodeBlocks(bits, 10, data);

    uint8_t crc = dpmrFec_crc8(data, 72);
    uint8_t rx = (uint8_t)packBits(data, 72, 8);

    out->crcOk = (crc == rx);
    out->ht = (uint8_t)packBits(data, 0, 4);
    out->calledId = packBits(data, 4, 24);
    out->ownId = packBits(data, 28, 24);
    out->mode = (uint8_t)packBits(data, 52, 3);
    out->version = (uint8_t)packBits(data, 55, 2);
    out->format = (uint8_t)packBits(data, 57, 2);
    /* Table 5.4: two reserved bits sit between F and CI */
    out->ci = (uint16_t)packBits(data, 61, 11);

    return out->crcOk;
}

bool dpmrFec_endDecode(const uint8_t dibits[DPMR_END_FIELD_SYMBOLS],
                       struct dpmrEnd *out)
{
    /*
     * Clause 7.8: 17 information bits + CRC-7 -> 3 bytes -> 3 Hamming blocks
     * -> 36 bits, and the 36 bit block is sent twice inside one 72 bit field.
     * Clause 9.6 is explicit that the scrambler restarts for each 36 bit
     * block ("each block of 36 bits is scrambled"), so both copies decode
     * with the same LFSR run from all ones and each carries its own CRC.
     */
    uint8_t half[2][24];

    memset(out, 0, sizeof(*out));

    for (unsigned h = 0; h < 2; h++) {
        uint8_t bits[36];
        uint8_t data[24];
        uint16_t scr = dpmrFec_scramblerReset();
        const uint8_t *d = dibits + 18 * h;

        for (unsigned s = 0; s < 18; s++) {
            bits[2 * s] = (uint8_t)(((d[s] >> 1) & 1u) ^ dpmrFec_scramblerNext(&scr));
            bits[2 * s + 1] = (uint8_t)((d[s] & 1u) ^ dpmrFec_scramblerNext(&scr));
        }

        out->fixes[h] = (uint8_t)hammingDecodeBlocks(bits, 3, data);
        out->crcOk[h] = dpmrFec_crc7(data, 17) == (uint8_t)packBits(data, 17, 7);
        memcpy(half[h], data, sizeof(data));
    }

    /* Fields of the first half that closed, or of the second one if alone */
    const uint8_t *data = out->crcOk[0] ? half[0] : half[1];

    out->et = (uint8_t)packBits(data, 0, 2);
    out->arq = (uint8_t)packBits(data, 2, 2);
    out->wait = (uint8_t)packBits(data, 4, 4);
    out->status = (uint8_t)packBits(data, 8, 5);
    out->reserved = (uint8_t)packBits(data, 13, 4);

    /* Two identical copies that both closed: the frame is what it says */
    out->same = out->crcOk[0] && out->crcOk[1]
                && (memcmp(half[0], half[1], 17) == 0);

    return out->crcOk[0] || out->crcOk[1];
}

/* ------------------------------------------------------- transmitting --- */

/*
 * The three field encoders below are the exact inverse of the decoders above:
 * the information bits are framed with their CRC, cut into (12,8) Hamming
 * blocks, written into the de-interleaver's index map backwards, scrambled
 * from the reset state and packed two bits per dibit, MSB first.
 */

/** Write n bits of v, MSB first, at bits[first..]. */
static void putBits(uint8_t *bits, size_t first, size_t count, uint32_t v)
{
    for (size_t i = 0; i < count; i++)
        bits[first + i] = (uint8_t)((v >> (count - 1 - i)) & 1u);
}

/**
 * Turn nmap bits of a coded-and-hamming'd field into scrambled dibits:
 * air[i] = bits[map[i]] XOR scrambler bit i, dibit s = air[2s], air[2s+1].
 */
static void interleaveAndScramble(const uint8_t *bits, size_t nbits,
                                  const uint8_t *map, uint8_t *dibits)
{
    uint16_t scr = dpmrFec_scramblerReset();
    uint8_t air[144];

    for (size_t i = 0; i < nbits; i++)
        air[i] = (uint8_t)(bits[map[i]] ^ dpmrFec_scramblerNext(&scr));

    for (size_t s = 0; s < nbits / 2; s++)
        dibits[s] = (uint8_t)((air[2 * s] << 1) | air[2 * s + 1]);
}

/** Hamming-encode nblocks octets, packed from the bit array, into 12*n bits. */
static void hammingEncodeBlocks(const uint8_t *bits, size_t nblocks,
                                uint8_t *out)
{
    for (size_t b = 0; b < nblocks; b++) {
        uint8_t byte = 0;

        for (size_t i = 0; i < 8; i++)
            byte = (uint8_t)((byte << 1) | (bits[8 * b + i] & 1u));

        dpmrFec_hammingEncode(byte, out + 12 * b);
    }
}

void dpmrFec_cchEncode(uint8_t fn, uint16_t id12, uint8_t mode,
                       uint8_t version, uint8_t format, uint32_t sld,
                       uint8_t out[DPMR_CCH_SYMBOLS])
{
    uint8_t data[48];
    uint8_t ham[72];

    memset(data, 0, sizeof(data));
    putBits(data, 0, 2, (uint32_t)(fn & 3u));
    putBits(data, 2, 12, (uint32_t)(id12 & 0xFFFu));
    putBits(data, 14, 3, mode);
    putBits(data, 17, 2, version);
    putBits(data, 19, 2, format);
    putBits(data, 23, 18, sld & 0x3FFFFu);

    putBits(data, 41, 7, dpmrFec_crc7(data, 41));

    hammingEncodeBlocks(data, 6, ham);
    interleaveAndScramble(ham, 72, dI72, out);
}

void dpmrFec_hiEncode(uint8_t ht, uint32_t calledId, uint32_t ownId,
                      uint8_t mode, uint8_t version, uint8_t format,
                      uint16_t ci, uint8_t out[DPMR_HI_SYMBOLS])
{
    uint8_t data[80];
    uint8_t ham[120];

    memset(data, 0, sizeof(data));
    putBits(data, 0, 4, ht);
    putBits(data, 4, 24, calledId);
    putBits(data, 28, 24, ownId);
    putBits(data, 52, 3, mode);
    putBits(data, 55, 2, version);
    putBits(data, 57, 2, format);
    /* Table 5.4: two reserved bits between F and CI */
    putBits(data, 61, 11, (uint32_t)(ci & 0x7FFu));

    putBits(data, 72, 8, dpmrFec_crc8(data, 72));

    hammingEncodeBlocks(data, 10, ham);
    interleaveAndScramble(ham, 120, dI120, out);
}

void dpmrFec_endEncode(uint8_t et, uint8_t arq, uint8_t wait, uint8_t status,
                       uint8_t out[DPMR_END_FIELD_SYMBOLS])
{
    uint8_t data[24];
    uint8_t ham[36];
    uint8_t air[36];
    uint8_t block[18];

    memset(data, 0, sizeof(data));
    putBits(data, 0, 2, (uint32_t)(et & 3u));
    putBits(data, 2, 2, (uint32_t)(arq & 3u));
    putBits(data, 4, 4, (uint32_t)(wait & 0xFu));
    putBits(data, 8, 5, (uint32_t)(status & 0x1Fu));

    putBits(data, 17, 7, dpmrFec_crc7(data, 17));

    hammingEncodeBlocks(data, 3, ham);

    /*
     * Clause 9.6: each 36-bit block is scrambled on its own from the reset
     * state, and the same block is sent twice.
     */
    uint16_t scr = dpmrFec_scramblerReset();

    for (int i = 0; i < 36; i++)
        air[i] = (uint8_t)(ham[i] ^ dpmrFec_scramblerNext(&scr));

    for (int s = 0; s < 18; s++)
        block[s] = (uint8_t)((air[2 * s] << 1) | air[2 * s + 1]);

    for (int rep = 0; rep < 2; rep++)
        memcpy(&out[18 * rep], block, 18);
}

void dpmrFec_voiceFrameEncode(const uint8_t frame[DPMR_AMBE_BYTES],
                              uint8_t dibits[36])
{
    uint8_t air[72];

    for (int m = 0; m < 72; m++) {
        /* canonical bit t = 18*(m%4) + m/4 leaves as air bit m */
        const int t = 18 * (m & 3) + m / 4;

        air[m] = (uint8_t)((frame[t >> 3] >> (7 - (t & 7))) & 1u);
    }

    for (int s = 0; s < 36; s++)
        dibits[s] = (uint8_t)((air[2 * s] << 1) | air[2 * s + 1]);
}

void dpmrFec_voiceFrame(const uint8_t dibits[36], uint8_t frame[DPMR_AMBE_BYTES])
{
    /*
     * The 72 TCH bits of one 20 ms group are sent as four interleaved streams
     * of 18 bits: canonical bit (4k + j) leaves as transmitted bit (18j + k).
     * Undoing that puts the bits back in the canonical A(24)|B(23)|C(25) order
     * the AMBE+2 3600x2450 decoder of lib/dmr_vocoder takes, which is the same
     * frame DMR carries (there through a 36-dibit plane schedule, see
     * DmrFec.c/dmr_ambe_layout -- the unit test checks both agree).
     */
    memset(frame, 0, DPMR_AMBE_BYTES);

    for (unsigned m = 0; m < 72; m++) {
        unsigned s = m / 2u;
        uint8_t bit = (m & 1u) ? (uint8_t)(dibits[s] & 1u)
                               : (uint8_t)((dibits[s] >> 1) & 1u);

        if (bit != 0) {
            unsigned t = 18u * (m & 3u) + m / 4u;
            frame[t >> 3] |= (uint8_t)(0x80u >> (t & 7u));
        }
    }
}
