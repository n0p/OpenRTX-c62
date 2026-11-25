/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file YsfTx.c
 * @brief System Fusion transmit chain, see YsfTx.h.
 *
 * The FEC is the mirror of YsfFrame.c: the same CRC-16 (polynomial 0x1021,
 * register zero, result inverted, closed by putting the CRC of the block with
 * the field left at zero), the same Golay(24,12), the same rate-1/2 K=5
 * convolutional code with four tail bits, the same 20-row block interleaver
 * and the same PN(9,5) whitening.  tests/host/ysf_tx transmits, demodulates
 * and decodes through the firmware's own receiver and compares every field,
 * so the two halves cannot drift.
 */

#include "protocols/YSF/YsfTx.h"

#include <string.h>

#include "protocols/C4FM/C4fmMod.h"

/** Zeroed by ysfTx_begin(): the section is NOLOAD on the radio. */
#ifdef __ZEPHYR__
#define YSFTX_PSRAM __attribute__((section(".psram_section")))
#else
#define YSFTX_PSRAM
#endif

/** Bits of one frame, and the parts of it. */
#define FRAME_BITS 960
#define SYNC_BITS 40
#define FICH_BITS 200
#define PAYLOAD_BITS 720

/** Dibits of a data channel block (180 for the 160-bit blocks). */
#define DCH_DIBITS 180
/** Dibits of one CSD or voice slot inside a V/D mode 1 frame. */
#define SLOT_DIBITS 36

enum ysfTxStage {
    YTX_IDLE = 0,
    YTX_HEADER,
    YTX_VOICE,
    YTX_TERM,
    YTX_DONE,
};

/** The frame sync: 20 dibits, 0x7FFD... in the receiver's terms. */
static const uint8_t syncDibits[YSF_SYNC_SYMBOLS] = { 3, 1, 1, 0, 1, 3, 0,
                                                      1, 3, 0, 2, 1, 1, 2,
                                                      0, 3, 1, 0, 3, 1 };

/** Parity of a five-bit window, for the convolutional encoder. */
static const uint8_t parity32[32] = { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0,
                                      1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1,
                                      1, 0, 0, 1, 1, 0, 1, 0, 0, 1 };

static enum ysfTxStage stage;
static struct ysfTxConfig config;
static char text[YSF_TX_TEXT_MAX];
static uint8_t textFrags; /* fragments of the message, 0 when there is none */
static ysfVoiceSource voiceSrc;
static void *voiceUser;
static uint32_t framesSent;

/** The frame being shaped, one dibit per symbol. */
static uint8_t frameDibits[YSF_FRAME_DIBITS] YSFTX_PSRAM;
static unsigned symbolInFrame;

static c4fmMod_t mod;
static int16_t carry[C4FM_MOD_SPS];
static unsigned carryLen;
static bool modReady;

/* ----------------------------------------------------------------- FEC --- */

/** CRC-16 of System Fusion: 0x1021, zero register, inverted result. */
static uint16_t crc16(const uint8_t *bits, int len)
{
    uint32_t crc = 0;

    for (int i = 0; i < len; i++) {
        crc = ((crc << 1) | (bits[i] & 1u)) & 0x1FFFFu;
        if (crc & 0x10000u)
            crc = (crc & 0xFFFFu) ^ 0x1021u;
    }

    return (uint16_t)((crc ^ 0xFFFFu) & 0xFFFFu);
}

/** Systematic extended Golay(24,12): 12 data bits, 11 remainder, 1 parity. */
static uint32_t golay24(uint32_t m)
{
    uint32_t cw = (m & 0xFFFu) << 11;
    uint32_t word;
    int ones = 0;

    for (int i = 22; i >= 11; i--)
        if (cw & (1u << i))
            cw ^= 0xC75u << (i - 11);

    word = ((m & 0xFFFu) << 11) | (cw & 0x7FFu);
    for (uint32_t v = word; v != 0; v >>= 1)
        ones += (int)(v & 1u);

    return (word << 1) | (uint32_t)(ones & 1);
}

/** Rate 1/2, K = 5: one dibit per input bit, generators 0o31 / 0o27. */
static void convEncode(const uint8_t *in, int steps, uint8_t *out)
{
    int reg = 0;

    for (int i = 0; i < steps; i++) {
        reg = ((reg << 1) | (in[i] & 1)) & 0x1F;
        out[i] = (uint8_t)((parity32[reg & 0x19] << 1) | parity32[reg & 0x17]);
    }
}

/** Block interleaver: written row by row, read column by column. */
static void interleave(const uint8_t *in, int rows, int cols, uint8_t *out)
{
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < cols; j++)
            out[i + j * rows] = in[j + i * cols];
}

/** PN(9,5) whitening: 9-bit LFSR, S0 ^ S4 feedback, output S0. */
static void whiten(uint8_t *bits, int n)
{
    uint16_t reg = 0x1C9;

    for (int i = 0; i < n; i++) {
        const uint8_t bit = (uint8_t)(reg & 1u);
        const uint8_t feedback = (uint8_t)(((reg >> 4) ^ reg) & 1u);

        reg = (uint16_t)((reg >> 1) | ((uint16_t)feedback << 8));
        bits[i] ^= bit;
    }
}

/** Write a value, MSB first, into a bit array. */
static void put(uint8_t *bits, int offset, int length, uint32_t value)
{
    for (int i = 0; i < length; i++)
        bits[offset + i] = (uint8_t)((value >> (length - 1 - i)) & 1u);
}

/** Copy a callsign field, padded with spaces, as the air carries it. */
static void putCallsign(uint8_t *bits, int offset, const char *call)
{
    for (int i = 0; i < YSF_CALLSIGN_LEN; i++) {
        const char c = (call[i] != '\0') ? call[i] : ' ';

        put(bits, offset + 8 * i, 8, (uint8_t)c);
    }
}

/* --------------------------------------------------------------- frames --- */

/**
 * Build the FICH: the 32 information bits the frame announces, coded.
 *
 * @param fi: frame indicator (0 header, 1 communication, 2 terminator)
 * @param bn: block number of the data channel
 * @param fn: frame number inside the superframe
 * @param ft: last frame number of the superframe
 * @param dt: data type (0 = V/D mode 1)
 */
static void buildFich(uint8_t fi, uint8_t bn, uint8_t fn, uint8_t ft,
                      uint8_t dt, uint8_t *dibits)
{
    uint8_t info[48];
    uint8_t coded[96];
    uint8_t payload[100];
    uint8_t air[100];

    memset(info, 0, sizeof(info));
    put(info, 0, 2, fi); /* frame indicator */
    put(info, 2, 2, 2);  /* callsigns assigned */
    put(info, 4, 2, 0);  /* call mode: group */
    put(info, 6, 2, bn);
    put(info, 8, 2, 0);  /* block type */
    put(info, 10, 3, fn);
    put(info, 13, 3, ft);
    put(info, 18, 3, 1); /* message/repeater */
    put(info, 21, 1, 1); /* voice present */
    put(info, 22, 2, dt);
    /* The squelch flag is inverted on the air: clear means a DG-ID follows. */
    put(info, 24, 1, (config.dgid <= 99) ? 0 : 1);
    put(info, 25, 7, (config.dgid <= 99) ? config.dgid : 0);

    put(info, 32, 16, crc16(info, 48)); /* over the bits and sixteen zeros */

    for (int g = 0; g < 4; g++) {
        uint32_t m = 0;

        for (int i = 0; i < 12; i++)
            m = (m << 1) | (info[g * 12 + i] & 1u);

        const uint32_t w = golay24(m);

        for (int i = 0; i < 24; i++)
            coded[g * 24 + i] = (uint8_t)((w >> (23 - i)) & 1u);
    }

    memcpy(payload, coded, 96);
    memset(&payload[96], 0, 4); /* tail bits */

    convEncode(payload, 100, air);
    interleave(air, 20, 5, dibits);
}

/**
 * Build one data channel block: 160 bits of content and a CRC-16, whitened.
 *
 * @param data: 160 bits (20 characters, or two callsigns).
 * @param dibits: receives the 180 interleaved dibits.
 */
static void buildDch(const uint8_t *data, uint8_t *dibits)
{
    uint8_t info[180];
    uint8_t air[180];

    memset(info, 0, sizeof(info));
    memcpy(info, data, 160);
    whiten(info, 160);
    put(info, 160, 16, crc16(info, 176)); /* CRC of the block, field zero */
    memset(&info[176], 0, 4);             /* tail bits */

    convEncode(info, 180, air);
    interleave(air, 20, 9, dibits);
}

/**
 * Header and terminator payload: two data channel blocks interleaved in
 * 36-dibit pieces, as the receiver un-interleaves them.
 */
static void putCallsignFrame(uint8_t fi)
{
    uint8_t data[160];
    uint8_t block[2][DCH_DIBITS];

    /* Block 0: destination and source, block 1: the two nodes */
    memset(data, 0, sizeof(data));
    putCallsign(data, 0, config.dest);
    putCallsign(data, 80, config.source);
    buildDch(data, block[0]);

    memset(data, 0, sizeof(data));
    putCallsign(data, 0, config.downlink);
    putCallsign(data, 80, config.uplink);
    buildDch(data, block[1]);

    buildFich(fi, 0, 0, 0, 0, &frameDibits[YSF_SYNC_SYMBOLS]);

    uint8_t *payload = &frameDibits[YSF_SYNC_SYMBOLS + 100];

    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 5; i++)
            memcpy(&payload[(i * 2 + b) * SLOT_DIBITS],
                   &block[b][i * SLOT_DIBITS], SLOT_DIBITS);
}

/** One communication frame of a V/D mode 1 call. */
static void putVoiceFrame(uint8_t fn)
{
    uint8_t csd[160];
    uint8_t block[DCH_DIBITS];
    uint8_t bn;

    /*
     * The data channel rotates through its fields with the frame number: the
     * destination and source, then the nodes, then the text in three
     * fragments.  A receiver needs no more than that to show the call and the
     * message.
     */
    if (textFrags > 0 && fn >= 2) {
        const int frag = (int)fn - 2;

        bn = 2;
        memset(csd, 0, sizeof(csd));
        for (int i = 0; i < 20; i++)
            put(csd, 8 * i, 8,
                (uint8_t)((frag < textFrags) ? text[frag * 20 + i] : ' '));
    } else if (fn & 1) {
        /* A voice frame with block number 1 carries the nodes in this order:
         * the receiver reads uplink first (YsfFrame.c, decode of a bn = 1
         * block), which is the opposite of the header's block 1. */
        bn = 1;
        memset(csd, 0, sizeof(csd));
        putCallsign(csd, 0, config.uplink);
        putCallsign(csd, 80, config.downlink);
    } else {
        bn = 0;
        memset(csd, 0, sizeof(csd));
        putCallsign(csd, 0, config.dest);
        putCallsign(csd, 80, config.source);
    }

    buildDch(csd, block);
    buildFich(1, bn, fn, YSF_VOICE_SLOTS - 1, 0,
              &frameDibits[YSF_SYNC_SYMBOLS]);

    uint8_t *payload = &frameDibits[YSF_SYNC_SYMBOLS + 100];

    for (int i = 0; i < YSF_VOICE_SLOTS; i++) {
        uint8_t frame[9];

        memcpy(&payload[i * 72], &block[i * SLOT_DIBITS], SLOT_DIBITS);

        if ((voiceSrc == NULL) || (voiceSrc(frame, voiceUser) != 0))
            memset(frame, 0, sizeof(frame));

        /* The 72 bits go on the air as 36 dibits, MSB first, unprotected:
         * the receiver hands exactly these bits to the vocoder. */
        for (int b = 0; b < 36; b++) {
            const int b0 = 2 * b;
            const uint8_t bit0 = (uint8_t)((frame[b0 >> 3] >> (7 - (b0 & 7)))
                                           & 1u);
            const uint8_t bit1 =
                (uint8_t)((frame[(b0 + 1) >> 3] >> (7 - ((b0 + 1) & 7))) & 1u);

            payload[i * 72 + SLOT_DIBITS + b] = (uint8_t)((bit0 << 1) | bit1);
        }
    }
}

static void buildNextFrame(void)
{
    memcpy(frameDibits, syncDibits, sizeof(syncDibits));

    switch (stage) {
        case YTX_HEADER:
            putCallsignFrame(0);
            stage = YTX_VOICE;
            break;

        case YTX_VOICE:
            putVoiceFrame((uint8_t)(framesSent % YSF_VOICE_SLOTS));
            break;

        case YTX_TERM:
            putCallsignFrame(2);
            stage = YTX_DONE;
            break;

        default:
            break;
    }

    framesSent++;
}

/* ------------------------------------------------------------------ API -- */

void ysfTx_setVoiceSource(ysfVoiceSource src, void *user)
{
    voiceSrc = src;
    voiceUser = user;
}

void ysfTx_begin(const struct ysfTxConfig *cfg)
{
    config = *cfg;

    memset(text, ' ', sizeof(text));

    int n = 0;
    if (cfg->text != NULL)
        for (; (n < YSF_TX_TEXT_MAX) && (cfg->text[n] != '\0'); n++)
            text[n] = cfg->text[n];

    textFrags = (uint8_t)((n + 19) / 20);

    memset(frameDibits, 0, sizeof(frameDibits));
    symbolInFrame = YSF_FRAME_DIBITS; /* forces the first frame on demand */
    carryLen = 0;
    framesSent = 0;
    stage = YTX_HEADER;
    modReady = false;
}

void ysfTx_end(void)
{
    if ((stage != YTX_IDLE) && (stage != YTX_DONE))
        stage = YTX_TERM;
}

bool ysfTx_done(void)
{
    return ((stage == YTX_DONE) && (symbolInFrame >= YSF_FRAME_DIBITS))
        || (stage == YTX_IDLE);
}

uint32_t ysfTx_frames(void)
{
    return framesSent;
}

const uint8_t *ysfTx_frameDibits(void)
{
    return frameDibits;
}

int ysfTx_samples(int16_t *out, int count, float gain)
{
    if (!modReady) {
        c4fmMod_init(&mod, gain, false, C4FM_PULSE_RC);
        modReady = true;
    }

    int n = 0;

    while (n < count) {
        if (carryLen > 0) {
            const int take = (count - n < (int)carryLen) ? count - n :
                                                           (int)carryLen;

            memcpy(&out[n], carry, (size_t)take * sizeof(int16_t));
            memmove(carry, &carry[take],
                    (carryLen - (size_t)take) * sizeof(int16_t));
            carryLen -= (unsigned)take;
            n += take;
            continue;
        }

        int16_t sym[C4FM_MOD_SPS];

        /* Silence before the call and once the terminator is out */
        const bool over = ((stage == YTX_DONE)
                           && (symbolInFrame >= YSF_FRAME_DIBITS))
                       || (stage == YTX_IDLE);

        if (over) {
            c4fmMod_dibits(&mod, NULL, 1, sym);
        } else {
            if (symbolInFrame >= YSF_FRAME_DIBITS) {
                buildNextFrame();
                symbolInFrame = 0;
            }

            const uint8_t d = frameDibits[symbolInFrame++];

            c4fmMod_dibits(&mod, &d, 1, sym);
        }

        const int take = (count - n < C4FM_MOD_SPS) ? count - n : C4FM_MOD_SPS;

        memcpy(&out[n], sym, (size_t)take * sizeof(int16_t));
        if (take < C4FM_MOD_SPS) {
            memcpy(carry, &sym[take], (C4FM_MOD_SPS - take) * sizeof(int16_t));
            carryLen = (unsigned)(C4FM_MOD_SPS - take);
        }
        n += take;
    }

    return count;
}
