/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file P25Tx.c
 * @brief P25 Phase 1 transmit chain, see P25Tx.h.
 *
 * Two layers: a frame builder that fills a 1728-bit buffer (one byte per bit,
 * so a table of positions can be applied directly), and a symbol pump that
 * reads that buffer two bits at a time into the C4FM shaper.  The builder is
 * the exact inverse of decodeFrame() in P25Phy.c, position table by position
 * table, and tests/host/p25_tx round-trips every frame type through the
 * receiver to prove it.
 */

#include "protocols/P25/P25Tx.h"

#include <string.h>

#include "protocols/C4FM/C4fmMod.h"
#include "protocols/P25/P25Fec.h"

/** Zeroed by p25Tx_begin(): the section is NOLOAD on the radio. */
#ifdef __ZEPHYR__
#define P25TX_PSRAM __attribute__((section(".psram_section")))
#else
#define P25TX_PSRAM
#endif

enum p25TxStage
{
    TX_IDLE = 0,
    TX_HDU,
    TX_VOICE,
    TX_SIGNALLING,
    TX_TDU,
    TX_DONE,
};

static enum p25TxStage stage;
static struct p25TxConfig config;
static uint8_t tsbkData[P25_TSBK_BYTES];
static bool tsbkSet;
static p25VoiceSource voiceSrc;
static void *voiceUser;
static bool nextIsLdu1;
static uint32_t framesSent;

/** The frame being shaped, one byte per bit. */
static uint8_t frameBits[P25_FRAME_BITS] P25TX_PSRAM;
/** Symbol index inside the frame, 0..863. */
static unsigned symbolInFrame;

/** Shaper and the sample carry, for calls whose sample count is not a whole
 *  number of symbols (the mode hands 30 ms halves, which always are). */
static c4fmMod_t mod;
static int16_t carry[C4FM_MOD_SPS];
static unsigned carryLen;
static bool modReady;

/* ------------------------------------------------------------ bit writer --- */

/** Write a value, MSB first, into the frame at the given positions. */
static void putField(const uint16_t *pos, int n, uint64_t value, int width)
{
    for (int i = 0; i < n; i++)
        frameBits[pos[i]] = (uint8_t)((value >> (width - 1 - i)) & 1u);
}

/** Copy a bit array into the frame at the given positions. */
static void putBits(const uint16_t *pos, int n, const uint8_t *src)
{
    for (int i = 0; i < n; i++)
        frameBits[pos[i]] = (uint8_t)(src[i] & 1u);
}

/** Append a value to a bit array, MSB first. */
static void appendBits(uint8_t *dst, int *n, uint64_t value, int width)
{
    for (int i = width - 1; i >= 0; i--)
        dst[(*n)++] = (uint8_t)((value >> i) & 1u);
}

/** The DUID parity bit of the NID (TIA-102.BAAC). */
static uint8_t duidParity(enum p25Duid duid)
{
    switch (duid)
    {
        case P25_DUID_LDU1:
        case P25_DUID_LDU2:
            return 1;
        default:
            return 0;
    }
}

/**
 * Start a frame: sync, NID and the fixed status symbols.
 *
 * The status symbol conveys nothing; TIA-102.BAAA transmits it as the fixed
 * dibit 01 and every receiver discards it (the position table skips it).
 */
static void frameStart(enum p25Duid duid)
{
    memset(frameBits, 0, sizeof(frameBits));

    for (int i = 0; i < P25_SYNC_BITS; i++)
        frameBits[i] = (uint8_t)((P25_SYNC_WORD >> (P25_SYNC_BITS - 1 - i)) & 1u);

    uint64_t cw = p25Fec_bchEncode((uint16_t)(((config.nac & 0xFFFu) << 4)
                                              | (uint8_t)duid));
    uint64_t nid = (cw << 1) | duidParity(duid);

    putField(p25_imbe_nid_bits, P25_NID_BITS, nid, P25_NID_BITS);

    for (int s = 0; s < P25_STATUS_SYMBOLS; s++)
    {
        /* dibit 01: first bit 0, second 1 */
        frameBits[p25_status_bits[2 * s]] = 0;
        frameBits[p25_status_bits[2 * s + 1]] = 1;
    }
}

/* ---------------------------------------------------------------- voice --- */

/**
 * Encode one IMBE frame: 88 parameter bits to the 144 bits on the air.
 *
 * Four Golay(23,12) words (the first one carries the 12 bits that seed the
 * PN sequence, the other three are scrambled with it), three Hamming(15,11)
 * words and seven unprotected bits: TIA-102.BABA-A, the inverse of
 * decodeImbe() in P25Phy.c.
 */
static void imbeEncode(const uint8_t params[P25_IMBE_BYTES], uint8_t air[P25_IMBE_BITS])
{
    uint32_t u[7];
    uint32_t u7;
    int p = 0;

    for (int i = 0; i < 4; i++)
    {
        u[i] = 0;
        for (int b = 0; b < 12; b++, p++)
            u[i] = (u[i] << 1) | ((params[p >> 3] >> (7 - (p & 7))) & 1u);
    }
    for (int i = 4; i <= 6; i++)
    {
        u[i] = 0;
        for (int b = 0; b < 11; b++, p++)
            u[i] = (u[i] << 1) | ((params[p >> 3] >> (7 - (p & 7))) & 1u);
    }
    u7 = 0;
    for (int b = 0; b < 7; b++, p++)
        u7 = (u7 << 1) | ((params[p >> 3] >> (7 - (p & 7))) & 1u);

    uint32_t pn = u[0] << 4;    /* 16-bit LFSR, advanced per word */

    for (int i = 0; i < 4; i++)
    {
        uint32_t w = p25Fec_golayEncode(u[i]);

        if (i > 0)
        {
            uint32_t m = 0;

            for (int b = 22; b >= 0; b--)
            {
                pn = (173u * pn + 13849u) & 0xFFFFu;
                if (pn & 0x8000u)
                    m |= 1u << b;
            }
            w ^= m;
        }

        for (int b = 0; b < 23; b++)
            air[23 * i + b] = (uint8_t)((w >> (22 - b)) & 1u);
    }

    for (int i = 4; i <= 6; i++)
    {
        uint32_t m = 0;

        for (int b = 14; b >= 0; b--)
        {
            pn = (173u * pn + 13849u) & 0xFFFFu;
            if (pn & 0x8000u)
                m |= 1u << b;
        }

        uint32_t w = (uint32_t)p25Fec_hamEncode((uint16_t)u[i]) ^ m;

        for (int b = 0; b < 15; b++)
            air[92 + 15 * (i - 4) + b] = (uint8_t)((w >> (14 - b)) & 1u);
    }

    for (int b = 0; b < 7; b++)
        air[137 + b] = (uint8_t)((u7 >> (6 - b)) & 1u);
}

/** Build one voice frame (LDU1 or LDU2). */
static void buildLdu(enum p25Duid duid)
{
    frameStart(duid);

    for (int n = 0; n < P25_IMBE_PER_LDU; n++)
    {
        uint8_t params[P25_IMBE_BYTES];
        uint8_t air[P25_IMBE_BITS];
        bool ok = false;

        if (voiceSrc != NULL && voiceSrc(params, voiceUser) == 0)
            ok = true;

        if (!ok)
        {
            /* No encoder (or it failed): a frame of zero parameters.  It is
             * not a defined silence frame -- P25 has none in the public
             * standard -- it is the FEC-valid encoding of "no parameters",
             * which is why a receiver will not hear speech in it. */
            memset(params, 0, sizeof(params));
        }

        imbeEncode(params, air);
        putBits(p25_voice_bits[n], P25_IMBE_BITS, air);
    }

    /* Low-speed data: 24 Hamming(10,6) words.  LDU1 carries the link control
     * (RS(24,12) over GF(64)); LDU2 carries the encryption sync, which this
     * transmitter sends as zeros because the call is in the clear and no
     * receiver in this tree reads that field. */
    uint8_t sym[24];
    memset(sym, 0, sizeof(sym));

    if (duid == P25_DUID_LDU1)
    {
        uint8_t lc[9];
        uint8_t cw[24];

        /* LCF = 0x00 (group voice user), MFID = 0x00 (standard), service
         * options, a reserved octet, the group and the source address:
         * TIA-102.AABF-D 7.3.1 */
        lc[0] = 0x00;
        lc[1] = 0x00;
        lc[2] = config.service;
        lc[3] = 0x00;
        lc[4] = (uint8_t)(config.group >> 8);
        lc[5] = (uint8_t)(config.group & 0xFFu);
        lc[6] = (uint8_t)((config.source >> 16) & 0xFFu);
        lc[7] = (uint8_t)((config.source >> 8) & 0xFFu);
        lc[8] = (uint8_t)(config.source & 0xFFu);

        /* 72 bits -> 12 six-bit symbols, MSB first */
        uint8_t bits[72];
        int n = 0;

        for (int i = 0; i < 9; i++)
            appendBits(bits, &n, lc[i], 8);

        for (int i = 0; i < 12; i++)
        {
            uint8_t s = 0;

            for (int b = 0; b < 6; b++)
                s = (uint8_t)((s << 1) | (bits[6 * i + b] & 1u));
            sym[i] = s;
        }

        p25Fec_rsEncode(sym, 12, cw, 24);
        memcpy(sym, cw, sizeof(cw));
    }

    for (int i = 0; i < P25_LC_HEXBITS; i++)
    {
        uint16_t w = p25Fec_ham10Encode(sym[i]);

        putField(&p25_ldu_ls_bits[10 * i], 10, w, 10);
    }
}

/* ----------------------------------------------------------------- HDU --- */

/** Build the header data unit: the 120-bit header, RS and Golay coded. */
static void buildHdu(void)
{
    frameStart(P25_DUID_HDU);

    uint8_t bits[120];
    int n = 0;

    /* Message indicator: 72 bits.  A clear call has none, and ALGID says so. */
    appendBits(bits, &n, 0, 72);
    appendBits(bits, &n, 0x00, 8);      /* MFID: standard */
    appendBits(bits, &n, 0x80, 8);      /* ALGID: unencrypted */
    appendBits(bits, &n, 0x0000, 16);   /* Key ID */
    appendBits(bits, &n, config.group, 16);

    uint8_t sym[20];
    for (int i = 0; i < 20; i++)
    {
        uint8_t s = 0;

        for (int b = 0; b < 6; b++)
            s = (uint8_t)((s << 1) | (bits[6 * i + b] & 1u));
        sym[i] = s;
    }

    uint8_t rs[36];
    p25Fec_rsEncode(sym, 20, rs, 36);

    for (int i = 0; i < 36; i++)
    {
        /* Six information bits and the twelve parity bits of the (18,6) code:
         * TIA-102.BAAC, the inverse of p25Fec_golay18Decode(). */
        uint8_t air[18];

        p25Fec_golay18Encode(rs[i], air);
        putBits(&p25_hdu_bits[18 * i], 18, air);
    }
}

/* ---------------------------------------------------------------- TSBK --- */

/** Build a signalling frame: three trellis-coded 196-bit blocks. */
static void buildTsbk(void)
{
    frameStart(P25_DUID_TSBK);

    for (int b = 0; b < 3; b++)
    {
        uint8_t air[P25_TSBK_BITS];

        p25Fec_trellisEncode(tsbkData, air);

        int n = 0;
        int s = 0;
        const int start = 57 + 101 * b;

        while ((s < 101) && (n < P25_TSBK_BITS))
        {
            const int sym = start + s;

            if (sym >= P25_FRAME_SYMBOLS)
                break;
            if ((sym % 36) == 35)
            {
                s++;    /* status symbol, not payload */
                continue;
            }

            frameBits[2 * sym] = air[n++];
            if (n < P25_TSBK_BITS)
                frameBits[2 * sym + 1] = air[n++];
            s++;
        }
    }
}

/* --------------------------------------------------------------- pump ---- */

static void buildNextFrame(void)
{
    switch (stage)
    {
        case TX_IDLE:
            break;

        case TX_HDU:
            buildHdu();
            stage = (voiceSrc != NULL) ? TX_VOICE : TX_SIGNALLING;
            nextIsLdu1 = true;
            break;

        case TX_VOICE:
            buildLdu(nextIsLdu1 ? P25_DUID_LDU1 : P25_DUID_LDU2);
            nextIsLdu1 = !nextIsLdu1;
            break;

        case TX_SIGNALLING:
            buildTsbk();
            break;

        case TX_TDU:
            /* The terminator is the last frame: the silence starts once it has
             * been pumped out, not when it is built. */
            frameStart(P25_DUID_TDU);
            stage = TX_DONE;
            break;

        case TX_DONE:
            break;
    }

    framesSent++;
}

static uint8_t currentDibit(void)
{
    const uint8_t b0 = frameBits[2 * symbolInFrame] & 1u;
    const uint8_t b1 = frameBits[2 * symbolInFrame + 1] & 1u;

    return (uint8_t)((b0 << 1) | b1);
}

/* ------------------------------------------------------------------ API -- */

void p25Tx_setVoiceSource(p25VoiceSource src, void *user)
{
    voiceSrc = src;
    voiceUser = user;
}

void p25Tx_setTsbk(const uint8_t block[P25_TSBK_BYTES])
{
    memcpy(tsbkData, block, P25_TSBK_BYTES);
    tsbkSet = true;
}

void p25Tx_begin(const struct p25TxConfig *cfg)
{
    config = *cfg;

    if (!tsbkSet)
    {
        /* Null information: no data, and the CRC field filled in the way this
         * CRC closes to zero, which is the CRC of the block with the field
         * left at zero (P25Fec.c, crc16). */
        memset(tsbkData, 0, sizeof(tsbkData));
        uint16_t crc = p25Fec_crc16(tsbkData, P25_TSBK_BYTES);

        tsbkData[P25_TSBK_BYTES - 2] = (uint8_t)(crc >> 8);
        tsbkData[P25_TSBK_BYTES - 1] = (uint8_t)(crc & 0xFFu);
    }

    memset(frameBits, 0, sizeof(frameBits));
    framesSent = 0;
    symbolInFrame = P25_FRAME_SYMBOLS;   /* forces the first frame on demand */
    carryLen = 0;
    nextIsLdu1 = true;
    stage = TX_HDU;
    modReady = false;
}

void p25Tx_end(void)
{
    if ((stage != TX_IDLE) && (stage != TX_DONE))
        stage = TX_TDU;
}

/** @return true when the TDU frame has been pumped out (feed a few ms more to
 *          let the shaper settle, then stop). */
bool p25Tx_done(void)
{
    return ((stage == TX_DONE) && (symbolInFrame >= P25_FRAME_SYMBOLS)) ||
           (stage == TX_IDLE);
}

uint32_t p25Tx_frames(void)
{
    return framesSent;
}

int p25Tx_samples(int16_t *out, int count, float gain)
{
    if (!modReady)
    {
        c4fmMod_init(&mod, gain, false);
        modReady = true;
    }

    int n = 0;

    while (n < count)
    {
        /* Finish a symbol the previous call left half-emitted */
        if (carryLen > 0)
        {
            const int take = (count - n < (int)carryLen) ? count - n : (int)carryLen;

            memcpy(&out[n], carry, (size_t)take * sizeof(int16_t));
            memmove(carry, &carry[take], (carryLen - (size_t)take) * sizeof(int16_t));
            carryLen -= (unsigned)take;
            n += take;
            continue;
        }

        int16_t sym[C4FM_MOD_SPS];

        /* Silence before a call starts, and once the TDU has left the shaper */
        const bool over = ((stage == TX_DONE) &&
                           (symbolInFrame >= P25_FRAME_SYMBOLS)) ||
                          (stage == TX_IDLE);

        if (over)
        {
            /* The transmission is over: keep the shaper fed with silence so
             * the last pulse leaves the filter instead of being cut. */
            c4fmMod_dibits(&mod, NULL, 1, sym);
        }
        else
        {
            if (symbolInFrame >= P25_FRAME_SYMBOLS)
            {
                buildNextFrame();
                symbolInFrame = 0;
            }

            const uint8_t d = currentDibit();

            symbolInFrame++;
            c4fmMod_dibits(&mod, &d, 1, sym);
        }

        const int take = (count - n < C4FM_MOD_SPS) ? count - n : C4FM_MOD_SPS;

        memcpy(&out[n], sym, (size_t)take * sizeof(int16_t));
        if (take < C4FM_MOD_SPS)
        {
            memcpy(carry, &sym[take], (C4FM_MOD_SPS - take) * sizeof(int16_t));
            carryLen = (unsigned)(C4FM_MOD_SPS - take);
        }
        n += take;
    }

    return count;
}
