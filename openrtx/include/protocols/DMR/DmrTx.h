/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_TX_H
#define OPENRTX_DMR_TX_H

/**
 * @file DmrTx.h
 * @brief DMR transmit chain: link control, CACH, FEC, burst framing and the
 * 4FSK modulator that turns 72-bit vocoder frames into a 48 kHz baseband.
 *
 * The chain is the mirror image of DmrPhy.c and shares its tables and FEC
 * definitions, so the receiver of the same build decodes what this produces
 * bit for bit (measured in work/dmr-tx: 100 % of the voice frames, see
 * docs/DMR_TX.md).
 *
 *   frames[3][9] --> interleave 3 x 72 bits --> 216 payload bits
 *                 --> [CACH 24 b] + 108 b + sync 48 b + 108 b, 144 symbols
 *                 --> RRC alpha=0.2, 10 samples/symbol at 48 kHz  (Q14 table)
 *                 --> DMR_TX_SLOT_SAMPLES, PA envelope applied
 *
 * Data bursts (voice LC header, terminator) go through the full chain:
 * LC PDU -> RS(12,9) -> BPTC(196,96) + Golay(20,8) slot type.
 *
 * @note The modulator is integer (Q14/Q15): no floating point, no
 * transcendentals, no tables beyond 138 int16.  One 30 ms voice slot costs
 * 117 669 instructions of a Cortex-M33 (measured with work/dmr-tx/arm, QEMU
 * instruction counting at -O2), 1.6-2.1 % of a 300 MHz core; an empty slot
 * costs 1 644.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocols/DMR/DmrDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Q15 amplitude of an inner symbol on the radio's baseband path.
 *
 * 2731/32768 = 8.3 % of full scale for the inner level, i.e. 25 % for the
 * outer one.  The deviation that comes out of it is set by the BK4819
 * (REG_40, see radio_C62.cpp): the ETSI value is 1944 Hz for the outer level,
 * 648 Hz for the inner one, and the factory calibration of the C62 is a
 * starting point, not a measurement.  Calibrate with the analyser (a +3/-3
 * alternating pattern) and then scale this constant.
 */
#define DMR_TX_INNER_LEVEL 2731

/** Samples per 30 ms timeslot at 48 kHz. */
#define DMR_TX_SLOT_SAMPLES 1440

/** Samples per symbol at 48 kHz (4800 Bd). */
#define DMR_TX_SPS 10

/** Polyphase RRC: phases x taps.  Four symbols each side of the peak, the
 *  same span as the receiver's matched filter and as MMDVM's TX filter. */
#define DMR_TX_RRC_PHASES 10
#define DMR_TX_RRC_HALF 4
#define DMR_TX_RRC_SPAN (2 * DMR_TX_RRC_HALF + 1)

/** Slot geometry: the 132 symbols of a burst, in samples. */
#define DMR_TX_BURST_SAMPLES (DMR_BURST_SYMBOLS * DMR_TX_SPS)

/**
 * Where the burst sits in the 30 ms slot.
 */
enum dmrTxLayout {
    /**
     * Handheld / simplex (TS 102 361-1 clause 4.6.2): the 132 symbols are
     * centred in the slot, 1,25 ms of guard on each side, and the PA ramps
     * live inside that guard.  No CACH: on the inbound channel the 2,5 ms
     * between bursts is guard time, the CACH belongs to the base station.
     */
    DMR_TX_LAYOUT_MS = 0,

    /**
     * Base station / repeater (clause 4.6.3): 24-bit CACH in the first
     * 2,5 ms, then the burst, continuous carrier, no ramping.
     */
    DMR_TX_LAYOUT_BS = 1,
};

/** Kinds of slot the caller can ask for. */
enum dmrTxBurst {
    DMR_TXB_EMPTY = 0, ///< No transmission (silence, PA ramps down)
    DMR_TXB_IDLE,      ///< Idle data burst (BPTC payload, DT=idle)
    DMR_TXB_VOICE_LC,  ///< Voice LC header data burst (RS + BPTC)
    DMR_TXB_VOICE,     ///< Voice burst: three AMBE+2 frames
    DMR_TXB_TERMINATOR,///< Terminator with LC data burst
    DMR_TXB_DATA       ///< Data burst of our own, see dmrTx_dataSlot()
};

struct dmrTxConfig {
    uint8_t colourCode; ///< 0..15
    uint32_t sourceId;  ///< 24-bit radio ID
    uint32_t destId;    ///< 24-bit talkgroup or unit ID
    bool group;         ///< true: group call (talkgroup), false: unit call
    uint8_t timeslot;   ///< 0 = TS1, 1 = TS2 (CACH TC bit)
    bool lateEntry;     ///< CACH LS bit
    enum dmrTxLayout layout;
    bool ramp;          ///< PA envelope on the MS layout (1 ms raised cosine)
    int16_t innerLevel; ///< Q15 audio amplitude of a +/-1 symbol
};

/** Samples of pulse tail kept from one slot to the next: 8 symbols. */
#define DMR_TX_CARRY_SAMPLES (DMR_TX_RRC_HALF * DMR_TX_SPS)

struct dmrTxState {
    struct dmrTxConfig cfg;
    uint8_t voiceBurst; ///< Position in the 6-burst voice superframe
    /** Q14 pulse tail of the previous slot, in units of the inner level. */
    int32_t carry[DMR_TX_CARRY_SAMPLES];
};

/**
 * Initialise the state from a configuration.  Call once, then dmrTx_slot()
 * per 30 ms timeslot.
 */
void dmrTx_init(struct dmrTxState *state, const struct dmrTxConfig *cfg);

/** Change talkgroup, source or colour code without losing the slot phase. */
void dmrTx_setCall(struct dmrTxState *state, uint8_t colourCode,
                   uint32_t sourceId, uint32_t destId, bool group);

/**
 * Render one 30 ms timeslot at 48 kHz.
 *
 * @param state: transmitter state.
 * @param kind: what the slot carries.
 * @param frames: DMR_TXB_VOICE only: three AMBE+2 frames, 9 bytes each in the
 *                canonical A|B|C order of dmr_vocoder_decode().
 * @param out: DMR_TX_SLOT_SAMPLES samples.  Always fully written.
 * @return the number of samples of the slot that carry energy, 0 for an empty
 *         slot (so the caller can skip keying the PA).
 */
size_t dmrTx_slot(struct dmrTxState *state, enum dmrTxBurst kind,
                  const uint8_t frames[DMR_FRAMES_PER_BURST][DMR_AMBE_BYTES],
                  int16_t out[DMR_TX_SLOT_SAMPLES]);

/**
 * Render one data burst of our own: a data header or a rate 1/2 block of a
 * short message.  Same chain and same slot cadence as dmrTx_slot(), which
 * this does not replace: it takes the 96 information bits and the burst data
 * type instead of the call frames.
 *
 * @param state: transmitter state, configured by dmrTx_init().
 * @param dataType: burst data type, DMR_DT_DATA_HEADER or DMR_DT_RATE_12.
 * @param info: 96 information bits, one per element, as dmrSms_packetBurst()
 *              hands them out.
 * @param out: DMR_TX_SLOT_SAMPLES samples.
 * @return DMR_TX_SLOT_SAMPLES.
 */
size_t dmrTx_dataSlot(struct dmrTxState *state, uint8_t dataType,
                      const uint8_t info[96],
                      int16_t out[DMR_TX_SLOT_SAMPLES]);

/**
 * Build the 12-byte Terminator Data Link Control of a data call (FLCO
 * 110000, G/I, FMF set) with its RS(12,9) parity, ready for the BPTC encoder.
 * Send it in a DMR_DT_TERMINATOR_LC burst after the last data block.
 *
 * @param lc: destination, 12 bytes.
 * @param srcId: source radio ID.
 * @param dstId: destination unit or talkgroup.
 * @param group: the call was addressed to a talkgroup.
 */
void dmrTx_dataTerminator(uint8_t lc[12], uint32_t srcId, uint32_t dstId,
                          bool group);

/**
 * Build the 12-byte link control of a group or unit voice call (FLCO 0x00,
 * FID 0x00, service options 0x00) with its RS(12,9) parity, ready for the BPTC
 * encoder.  Exposed for tests and for the terminator.
 */
void dmrTx_linkControl(uint8_t lc[12], uint32_t srcId, uint32_t dstId,
                       bool group, uint8_t dataType);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_TX_H */
