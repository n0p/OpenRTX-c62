/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_TX_H
#define DMR_TX_H

/*
 * DMR voice transmission, mobile station in direct mode (simplex, to another
 * radio or a hotspot): the bursts of a call and their 4FSK baseband.
 *
 * A call is a voice LC header (sent twice), voice superframes of six bursts
 * (A with the voice sync, B..E with the four fragments of the embedded LC,
 * F with a null one) and a terminator with LC. Bursts are 132 dibits in the
 * receiver's format (dmr_frame_t), so they can be checked by dmr_proto.
 *
 * The modulator turns dibits into 4800 symbols/s (+1, +3, -1, -3) shaped by
 * a root raised cosine (alpha 0.2), at 48 kHz, ten samples per symbol.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMR_TX_BURST_DIBITS 132
#define DMR_TX_SPS 10
#define DMR_TX_RRC_TAPS 81

/* A call being built */
typedef struct {
    uint8_t lc[72];     /* Full link control, one bit per byte */
    uint8_t embLc[128]; /* Embedded LC, the B..E fragments */
    uint8_t cc;         /* Colour code */
    /* Encrypted, as a record of what dmr_tx_privacy() did. It must NOT
     * reach the EMB: the PI bit there is the pre-emption and power
     * control indicator (clause 9.3.2), not a privacy flag, and setting
     * it made conforming receivers read the embedded LC fragment as a
     * Reverse Channel command. The encryption travels in the service
     * options of the LC, bit 6, which dmr_tx_privacy() sets. */
    bool privacy;
    int voiceIdx;       /* Next voice burst, 0..5 = A..F */
} dmr_tx_t;

/**
 * Start a call.
 *
 * @param tx: call state.
 * @param src: own DMR ID (24 bits).
 * @param dst: talkgroup or called ID (24 bits).
 * @param group: true for a group call, false for a private (unit) call.
 * @param cc: colour code, 0..15.
 */
void dmr_tx_setup(dmr_tx_t *tx, uint32_t src, uint32_t dst, bool group,
                  uint8_t cc);

/**
 * Mark the call as encrypted the Motorola way: feature set ID 0x10 and the
 * privacy bit in the service options of the LC (header, embedded,
 * terminator), as Basic and Enhanced Privacy radios send it. After
 * dmr_tx_setup().
 */
void dmr_tx_privacy(dmr_tx_t *tx);
/** The same with another feature set ID: 0x68 for Hytera's privacy */
void dmr_tx_privacy_fid(dmr_tx_t *tx, uint8_t fid);
/**
 * Hytera Enhanced's PI header (dmr_hytera.h): algorithm 0x02, feature set
 * ID 0x68, the key ID, the 40-bit MI, a zero, byte 9 minus the sum of
 * bytes 0..8, and the CRC-CCITT masked 0x6969 over bytes 0..9 as the
 * other PI headers.
 */
void dmr_tx_pi_hytera(const dmr_tx_t *tx, uint8_t keyId, uint64_t mi,
                      uint8_t dibits[DMR_TX_BURST_DIBITS]);

/**
 * Privacy indicator (PI) header burst of Motorola Enhanced Privacy, sent
 * after the voice LC headers: ARC4 (algorithm 0x21), feature set ID 0x10,
 * key ID, the 32-bit message indicator of the first superframe and the
 * destination, CRC-CCITT masked 0x6969 (see dmr_ep.h).
 */
void dmr_tx_pi(const dmr_tx_t *tx, uint8_t keyId, uint32_t mi, uint32_t dst,
               uint8_t dibits[DMR_TX_BURST_DIBITS]);
/** The same with another algorithm: 0x21 ARC4, 0x25 AES-256 */
void dmr_tx_pi_alg(const dmr_tx_t *tx, uint8_t alg, uint8_t keyId,
                   uint32_t mi, uint32_t dst,
                   uint8_t dibits[DMR_TX_BURST_DIBITS]);

/** Voice LC header burst. */
void dmr_tx_header(const dmr_tx_t *tx, uint8_t dibits[DMR_TX_BURST_DIBITS]);

/**
 * Repeater wake-up: a CSBK BS_Dwn_Act burst, which a sleeping repeater
 * takes at any timing and answers by transmitting.
 *
 * @param tx: call state (colour code).
 * @param bs: repeater address (0xFFFFFF: any).
 * @param src: own DMR ID.
 */
/**
 * A data burst of 12 octets as they are, CRC included (a CSBK, a data
 * header, a rate 1/2 block: BPTC(196,96)), MS sourced.
 * @param cc: colour code.
 * @param dt: data type of the slot type (3 CSBK, 6 data header, 7 rate
 * 1/2 data).
 */
void dmr_tx_block(uint8_t cc, uint8_t dt, const uint8_t bytes[12],
                  uint8_t dibits[DMR_TX_BURST_DIBITS]);

void dmr_tx_wakeup(const dmr_tx_t *tx, uint32_t bs, uint32_t src,
                   uint8_t dibits[DMR_TX_BURST_DIBITS]);

/** Terminator with LC burst. */
void dmr_tx_terminator(const dmr_tx_t *tx,
                       uint8_t dibits[DMR_TX_BURST_DIBITS]);

/**
 * Next voice burst of the superframe.
 *
 * @param tx: call state, advances to the next burst.
 * @param ambe: three AMBE+2 frames of 72 bits, 9 bytes each, first bit
 * transmitted in the MSB of the first byte (DMR interleaved order).
 * @param dibits: the burst.
 */
void dmr_tx_voice(dmr_tx_t *tx, const uint8_t ambe[3][9],
                  uint8_t dibits[DMR_TX_BURST_DIBITS]);

/* 4FSK modulator */
typedef struct {
    float taps[DMR_TX_RRC_TAPS];
    float hist[16]; /* Last symbols, newest at histPos */
    unsigned histPos;
    float gain;
    bool invert;
} dmr_mod_t;

/**
 * Initialise the modulator.
 *
 * @param mod: modulator.
 * @param gain: output for a long run of symbols +1 (a run of +3 gives three
 * times this). Peaks of the filtered signal are saturated to int16.
 * @param invert: negate the output (for a transmitter that inverts).
 */
void dmr_mod_init(dmr_mod_t *mod, float gain, bool invert);

/**
 * Modulate dibits, DMR_TX_SPS samples each.
 *
 * @param mod: modulator.
 * @param dibits: dibits to send, or NULL for silence (symbol 0, no
 * deviation: the gaps between the bursts, and the end of the filter tail).
 * @param n: number of dibits (or silent symbols).
 * @param out: n * DMR_TX_SPS samples.
 */
void dmr_mod_dibits(dmr_mod_t *mod, const uint8_t *dibits, int n,
                    int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DMR_TX_H */
