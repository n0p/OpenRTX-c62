/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DStarTx.c
 * @brief D-STAR DV transmit chain, see DStarTx.h.
 *
 * Two halves, both streaming:
 *
 *  - a bit generator that walks the transmission of STD-7 4.1.1/4.1.2 and
 *    hands out one bit at a time: bit sync, frame sync, the coded header, then
 *    frames of 72 voice + 24 slow bits, with the resync word every 21st frame
 *    and the end pattern at the end;
 *  - a GMSK shaper: each bit becomes +-A, convolved with the Gaussian pulse
 *    (BT 0.5, 10 samples per symbol at 48 kHz) and handed to the FM path as the
 *    modulating waveform.  Overlap-add into a 41-sample accumulator costs 41
 *    multiply-accumulates per bit, i.e. 200 kMAC/s at 4800 bit/s.
 */

#include "protocols/DSTAR/DStarTx.h"

#include <string.h>

/* Gaussian pulse, BT 0.5, 10 samples/symbol, +-2 symbols, Q15, sum 32767. */
#define MOD_TAPS 41
static const int16_t gauss10[MOD_TAPS] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 4, 15, 52, 151, 380, 832, 1579, 2599,
    3710, 4594, 4933, 4594, 3710, 2599, 1579, 832, 380, 151, 52, 15, 4, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0,
};

enum txStage {
    TX_IDLE = 0,
    TX_BITSYNC,
    TX_FRAMESYNC,
    TX_HEADER,
    TX_FRAMES,
    TX_END,
    TX_DONE,
};

static enum txStage stage;
static uint8_t headerBits[DSTAR_CODED_BITS];
static uint32_t bitIndex;
static uint32_t frameIndex;

/* Slow data: a stream of 6-byte blocks, split into two 3-byte halves each. */
#define SLOW_QUEUE 24
static uint8_t slowBlocks[SLOW_QUEUE][DSTAR_SLOW_BLOCK];
static uint8_t slowCount;
static uint8_t slowBlockIdx;
static uint8_t slowHalf;
static uint8_t slowByte; /* index inside the current half, 0..2 */
static uint8_t slowField[DSTAR_SLOW_BYTES];

static uint8_t voiceFrame[DSTAR_VOICE_BYTES];
static dstarVoiceSource voiceSrc;
static void *voiceUser;
static bool useNullVoice;

/* Modulator state: overlap-add accumulator as a 64-sample ring.  A pulse spans
 * MOD_TAPS samples and is only ever added for a bit whose first sample is at or
 * before the sample being emitted, so 64 slots always suffice (40 < 64). */
#define MOD_RING 64
static int16_t modAcc[MOD_RING];
static uint32_t modOut;   /* absolute index of the next output sample */
static uint32_t modBits;  /* bits already shaped */

static void slowReset(void)
{
    slowCount = 0;
    slowBlockIdx = 0;
    slowHalf = 0;
    slowByte = 0;
    memset(slowField, 0, sizeof(slowField));
}

/**
 * @brief Queue one 6-byte slow-data block.
 *
 * @param type: type nibble (0x40 text, 0x50 header copy, ...).
 * @param low: low nibble: the length for most types, the fragment number for
 *             the free text (whose length is always five), see G4KLX's slow
 *             data description.
 */
static void slowPushBlock(uint8_t type, uint8_t low, const uint8_t *payload, uint8_t len)
{
    if (slowCount >= SLOW_QUEUE)
        return;
    uint8_t *b = slowBlocks[slowCount++];
    memset(b, DSTAR_SLOW_FILL, DSTAR_SLOW_BLOCK);
    b[0] = (uint8_t)(type | (low & DSTAR_SLOW_LEN_MASK));
    for (uint8_t i = 0; i < len && i < 5; i++)
        b[1 + i] = payload[i];
}

/* Next 3 slow-data bytes (still unscrambled). */
static void slowNextField(void)
{
    if (slowBlockIdx >= slowCount)
        slowBlockIdx = 0;

    const uint8_t *b = slowBlocks[slowBlockIdx];
    for (int i = 0; i < 3; i++)
        slowField[i] = b[slowHalf * 3 + i];

    slowHalf ^= 1;
    if (slowHalf == 0)
        slowBlockIdx = (uint8_t)((slowBlockIdx + 1) % slowCount);
}

void dstarTx_setVoiceSource(dstarVoiceSource src, void *user)
{
    voiceSrc = src;
    voiceUser = user;
}

void dstarTx_begin(const struct dstarTxConfig *cfg)
{
    uint8_t hdr[DSTAR_HEADER_BYTES];
    struct dstarHeader h = cfg->hdr;

    /* A general call has no destination station; a simplex call has no
     * repeater, and STD-7 4.1.1 f/g spell that "DIRECT". */
    if (h.ur[0] == '\0')
        strcpy(h.ur, "CQCQCQ");
    if (h.rpt1[0] == '\0' && h.rpt2[0] == '\0') {
        h.flags[0] = (uint8_t)(h.flags[0] & (uint8_t)~DSTAR_FLAG1_REPEATER);
    }

    dstarHeaderPack(&h, hdr);
    dstarHeaderEncode(hdr, headerBits);

    slowReset();
    if (cfg->text != NULL && cfg->text[0] != '\0') {
        char text[DSTAR_TEXT_LEN];
        memset(text, ' ', sizeof(text));
        for (int i = 0; i < DSTAR_TEXT_LEN && cfg->text[i] != '\0'; i++)
            text[i] = cfg->text[i];
        for (int frag = 0; frag < 4; frag++)
            slowPushBlock(DSTAR_SLOW_TEXT, (uint8_t)frag,
                          (const uint8_t *)text + frag * 5, 5);
    }
    if (cfg->repeatHeader) {
        for (int i = 0; i < DSTAR_HEADER_BYTES; i += 5) {
            uint8_t len = (uint8_t)((i + 5 <= DSTAR_HEADER_BYTES) ? 5 : DSTAR_HEADER_BYTES - i);
            slowPushBlock(DSTAR_SLOW_HEADER, len, hdr + i, len);
        }
    }
    if (slowCount == 0) {
        /* Nothing to say: filler keeps the channel defined. */
        static const uint8_t fill[5] = {0x66, 0x66, 0x66, 0x66, 0x66};
        slowPushBlock(DSTAR_SLOW_FILLER, 5, fill, 5);
    }

    useNullVoice = cfg->useNullVoice;
    memcpy(voiceFrame, dstarNullVoice, DSTAR_VOICE_BYTES);

    stage = TX_BITSYNC;
    bitIndex = 0;
    frameIndex = 0;
    modOut = 0;
    modBits = 0;
    memset(modAcc, 0, sizeof(modAcc));
}

void dstarTx_end(void)
{
    if (stage != TX_IDLE && stage != TX_DONE) {
        /* The end pattern has its own bit counter: bitIndex still holds the
         * position inside the last frame, and TX_END tests it against the
         * pattern length. */
        bitIndex = 0;
        stage = TX_END;
    }
}

bool dstarTx_done(void)
{
    return stage == TX_DONE || stage == TX_IDLE;
}

/* Next bit of the transmission, or -1 when it is over. */
static int nextBit(void)
{
    switch (stage) {
    case TX_BITSYNC:
        /* 64 bits "1010..." (STD-7 4.1.1 a), MSB-first per byte */
        if (bitIndex >= DSTAR_BITSYNC_BITS) {
            stage = TX_FRAMESYNC;
            bitIndex = 0;
            return nextBit();
        }
        return (bitIndex++ & 1) ? 0 : 1;

    case TX_FRAMESYNC:
        if (bitIndex >= DSTAR_FRAME_SYNC_BITS) {
            stage = TX_HEADER;
            bitIndex = 0;
            return nextBit();
        }
        return (DSTAR_FRAME_SYNC >> (14 - bitIndex++)) & 1;

    case TX_HEADER:
        if (bitIndex >= DSTAR_CODED_BITS) {
            stage = TX_FRAMES;
            bitIndex = 0;
            return nextBit();
        }
        return headerBits[bitIndex++];

    case TX_FRAMES: {
        uint16_t inFrame = (uint16_t)(bitIndex % DSTAR_FRAME_BITS);

        if (bitIndex != 0 && inFrame == 0) {
            /* New frame: refresh the voice frame and, every 21st, the resync */
            frameIndex++;
            if (useNullVoice || voiceSrc == NULL) {
                memcpy(voiceFrame, dstarNullVoice, DSTAR_VOICE_BYTES);
            } else if (voiceSrc(voiceFrame, voiceUser) != 0) {
                memcpy(voiceFrame, dstarNullVoice, DSTAR_VOICE_BYTES);
            }
        }

        if (inFrame < DSTAR_VOICE_BITS) {
            bitIndex++;
            return (voiceFrame[inFrame >> 3] >> (inFrame & 7)) & 1;
        }

        uint16_t k = (uint16_t)(inFrame - DSTAR_VOICE_BITS);
        if (k == 0) {
            if ((frameIndex % DSTAR_SUPERFRAME) == 0) {
                for (int i = 0; i < 3; i++)
                    slowField[i] = (uint8_t)((DSTAR_DATA_SYNC >> (8 * i)) & 0xFF);
            } else {
                slowNextField();
                for (int i = 0; i < 3; i++)
                    slowField[i] ^= dstarSlowScrambler[i];
            }
        }
        bitIndex++;
        return (slowField[k >> 3] >> (k & 7)) & 1;
    }

    case TX_END:
        if (bitIndex >= DSTAR_END_BYTES * 8) {
            stage = TX_DONE;
            return -1;
        }
        uint16_t b = bitIndex++;
        return (dstarEndPattern[b >> 3] >> (b & 7)) & 1;

    default:
        return -1;
    }
}

/* Add the pulse of one NRZ symbol (+-1) starting at absolute sample modBits*10. */
static void shapeBit(int bit, int amplitude)
{
    int32_t sign = bit ? +1 : -1;
    uint32_t start = modBits * 10u;

    for (int i = 0; i < MOD_TAPS; i++) {
        uint32_t pos = start + (uint32_t)i;
        int32_t v = modAcc[pos & (MOD_RING - 1u)] +
                    (sign * ((int32_t)gauss10[i] * amplitude >> 15));
        if (v > 32767)
            v = 32767;
        if (v < -32768)
            v = -32768;
        modAcc[pos & (MOD_RING - 1u)] = (int16_t)v;
    }
    modBits++;
}

int dstarTx_samples(int16_t *out, int count, int amplitude)
{
    for (int n = 0; n < count; n++) {
        /* Shape every bit whose pulse reaches this sample; a finished
         * transmission feeds zeros so the shaper settles instead of stopping
         * mid-pulse. */
        while (modBits * 10u <= modOut) {
            int bit = nextBit();
            shapeBit(bit > 0 ? 1 : 0, amplitude);
        }

        out[n] = modAcc[modOut & (MOD_RING - 1u)];
        modAcc[modOut & (MOD_RING - 1u)] = 0;
        modOut++;
    }
    return count;
}
