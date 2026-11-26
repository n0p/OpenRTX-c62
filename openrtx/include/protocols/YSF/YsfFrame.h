/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_YSF_FRAME_H
#define OPENRTX_YSF_FRAME_H

/**
 * @file YsfFrame.h
 * @brief System Fusion frame layer: FICH, the data channel and the voice slots.
 *
 * One 100 ms frame is 960 bits: a 40-bit sync, a 200-bit FICH and 720 bits of
 * payload (system description, see docs/RX_FUSION.md).  The FICH coding chain
 * is 32 information bits + CRC-16, four Golay(24,12) codewords, a rate-1/2
 * constraint-length-5 convolutional code, 4 tail bits and a 20x5 dibit
 * interleaver -- the P25 Phase 1 primitives, which is why the Golay decoder of
 * lib/dmr_vocoder is reused here instead of a second table in flash.
 *
 * The payload depends on the FICH data type (DT):
 *
 *   DT 0, V/D mode 1: 5 x (36 dibits CSD + 36 dibits voice)
 *   DT 1, data FR:    10 x 36 dibits of CSD
 *   DT 2, V/D mode 2: 5 x (20 dibits DCH + 52 dibits voice)
 *   DT 3, voice FR:   5 x 72 dibits of IMBE full rate (not decoded)
 *
 * Voice in V/D mode 1 is a 72-bit AMBE+2 channel frame per 20 ms slot, the
 * same canonical A(24)|B(23)|C(25) frame lib/dmr_vocoder already takes; in
 * V/D mode 2 it is 49 AMBE parameter bits, three times repeated then
 * whitened.
 */

#include <stdbool.h>
#include <stdint.h>

#include "protocols/YSF/YsfPhy.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Characters in a System Fusion callsign field. */
#define YSF_CALLSIGN_LEN 10

/** Voice slots in one frame: five 20 ms frames. */
#define YSF_VOICE_SLOTS 5

/** Longest voice slot: the 72-bit AMBE+2 channel frame of V/D mode 1. */
#define YSF_VOICE_MAX_BITS 72

/** Longest text message kept, in characters. */
#define YSF_TEXT_MAX 80

/**
 * One voice slot, as bits (one per byte) so that the two modes' lengths stay
 * apart: 72 for V/D mode 1, 49 for V/D mode 2.
 */
struct ysfVoiceSlot
{
    uint8_t nbits;
    uint8_t bits[YSF_VOICE_MAX_BITS];
};

/** FICH fields of the last good frame, and what the transmission says. */
struct ysfCallInfo
{
    uint8_t fi;   ///< Frame indicator: 0 header, 1 communication, 2 terminator
    uint8_t cm;   ///< Call mode: 0 group/CQ, 1 radio ID, 3 private
    uint8_t bn;   ///< Data channel block number
    uint8_t bt;   ///< Data channel block type
    uint8_t fn;   ///< Frame number inside the superframe
    uint8_t ft;   ///< Last frame number of the superframe
    uint8_t dt;   ///< Data type, see the table above
    bool dgidValid;    ///< The FICH carries a DG-ID (squelch code enabled)
    uint8_t dgid;      ///< DG-ID, 0..99

    char dest[YSF_CALLSIGN_LEN + 1];     ///< Destination, or the DG-ID text
    char source[YSF_CALLSIGN_LEN + 1];   ///< Sending station
    char uplink[YSF_CALLSIGN_LEN + 1];   ///< Wires-X uplink node
    char downlink[YSF_CALLSIGN_LEN + 1]; ///< Wires-X downlink node

    uint16_t frames;      ///< Frames with a good FICH
    uint16_t badFich;     ///< Frames whose FICH failed Golay or CRC
    uint16_t voices;      ///< Voice slots handed on
    uint16_t unsupported; ///< Voice slots of a mode this build cannot decode
    uint16_t dataBlocks;  ///< Data channel blocks with a good CRC
};

/** A received text message, reassembled across frames. */
struct ysfText
{
    char text[YSF_TEXT_MAX + 1];
    uint8_t length;
    uint8_t expected;   ///< fn of the last block, from the FICH
    uint8_t received;   ///< blocks assembled so far
    bool complete;
    uint16_t messages;  ///< complete messages seen since the last reset
};

/** Everything one frame decodes to. */
struct ysfDecoded
{
    bool fichOk;
    struct ysfCallInfo call;
    uint8_t voiceCount;
    struct ysfVoiceSlot voice[YSF_VOICE_SLOTS];
    bool textChanged;
};

/** Clear the per-transmission state. */
void ysfFrame_reset(void);

/**
 * Decode one frame.
 *
 * The returned structure lives in .bss and is valid until the next call: the
 * RTX thread is the only caller, and the mode copies what it needs.
 *
 * @param frame: 480 dibits, sync first.
 * @return the decoded frame, or NULL if the frame carries nothing usable.
 */
const struct ysfDecoded *ysfFrame_decode(const struct ysfFrame *frame);

/** The current call information, valid even between frames. */
const struct ysfCallInfo *ysfFrame_call(void);

/** The text message being assembled, or the last one completed. */
const struct ysfText *ysfFrame_text(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_YSF_FRAME_H */
