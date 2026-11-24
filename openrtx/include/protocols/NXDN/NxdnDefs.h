/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_NXDN_DEFS_H
#define OPENRTX_NXDN_DEFS_H

/**
 * @file NxdnDefs.h
 * @brief NXDN constants: symbol rate, frame layout, frame sync word, the
 * default data scrambler and the LICH field decoding.
 *
 * References (NXDN Forum, TS 1-A/1-B v1.3, November 2011):
 *  - 4.3/4.4.1: one frame is 192 symbols (384 bits) at 4800 bit/s, 2400
 *    symbols/s, 80 ms; 9600 bit/s is 4800 symbols/s and 40 ms.
 *  - 4.4.4 Table 4.4-2: frame sync word (FSW), 10 symbols, hex CDF59.
 *  - 4.4.3 Table 4.4-1: preamble `5775FD` and the +3+3-3-3 pattern.
 *  - 4.1.2/Figure 4.1-4: one frame carries FSW(10) + LICH(8 symbols) +
 *    SACCH(30 symbols) + 4 x VCH(36 symbols); a VCH is 72 bits, i.e. 20 ms of
 *    AMBE+2 at 3600 bit/s.
 *  - 4.6: every symbol but the FSW is scrambled by sign inversion with a 9-bit
 *    LFSR (X^9 + X^4 + 1), default seed 0b011100100.
 *  - 5.2.1 Table 5.2-1: LICH = 7 bits, [6:5] RF channel type, [4:3] functional
 *    channel type, [2:1] option (steal flag), [0] direction (1 = outbound).
 *  - 4.5.3 Figure 4.5-7: LICH is 8 coding bits, each sent as one dibit whose
 *    sign bit is the data bit; 1 bit of even parity covers the 4 MSBs.
 *  - 4.1.1 Figure 4.1-1: a voice call's superframe is four frames and the
 *    SACCH carries one 18-bit segment of a 72-bit layer 3 message per frame.
 *
 * What the C62 receives is the discriminator audio of a 4FSK signal, so the
 * symbol values below are the ideal deviations in units of the outer level:
 * +-3 for the outer symbols and +-1 for the inner ones, which is the mapping
 * of ETSI-style dibits (sign bit, then a "three" bit).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Symbols per second: 4800 bit/s, two bits per symbol. NXDN48. */
#define NXDN_SYMBOL_RATE 2400

/** Symbols per frame: 80 ms at 2400 symbols/s. */
#define NXDN_FRAME_SYMBOLS 192

/** Bits per frame. */
#define NXDN_FRAME_BITS 384

/** Symbols of the frame sync word. */
#define NXDN_FSW_SYMBOLS 10

/** Dibits of the LICH (one coding bit each, in the sign bit). */
#define NXDN_LICH_DIBITS 8

/** Bits of the SACCH: a punctured convolutional block. */
#define NXDN_SACCH_BITS 60

/** Dibits of the SACCH. */
#define NXDN_SACCH_DIBITS 30

/** Bits of one voice channel (VCH): one AMBE+2 frame per 20 ms. */
#define NXDN_VCH_BITS 72

/** Dibits of one VCH. */
#define NXDN_VCH_DIBITS 36

/** Voice channels per frame at 4800 bit/s. */
#define NXDN_VCH_PER_FRAME 4

/** Octets of one AMBE+2 3600x2450 frame. */
#define NXDN_AMBE_BYTES 9

/** Frames of a voice superframe. */
#define NXDN_SUPERFRAME 4

/** Bit offsets inside the 384-bit frame (FSW excluded from the payload). */
#define NXDN_OFF_LICH 0    ///< 8 dibits after the FSW
#define NXDN_OFF_SACCH 8   ///< 30 dibits
#define NXDN_OFF_VCH1 38   ///< 36 dibits each, then VCH2..VCH4

/**
 * Ideal frame sync word, 10 symbols, transmitted as -3 +1 -3 +3 -3 -3 +3 +3 -1
 * +3 (TS 1-A Table 4.4-2, hex CDF59 read two bits per symbol).  Its sum is
 * zero, so a residual carrier offset does not bias the correlation.
 */
extern const int8_t nxdnFsw[NXDN_FSW_SYMBOLS];

/**
 * Which symbols of the 182 payload dibits the default scrambler inverts, as a
 * bit mask: bit (i & 7) of byte (i >> 3) set means dibit i has its sign bit
 * flipped.  Generated once from the LFSR of TS 1-A 4.6 with the default seed;
 * the mask is the same for every frame of a transmission.
 */
extern const uint8_t nxdnScramble[23];

/** RF channel type field of the LICH (bits 6:5). */
enum nxdnRfType {
    NXDN_RF_RCCH = 0x00,
    NXDN_RF_RTCH = 0x01,   ///< Traffic channel, trunked
    NXDN_RF_RDCH = 0x02,   ///< Traffic channel, conventional / direct
    NXDN_RF_RTCH_C = 0x03, ///< Composite control channel (Type-C)
};

/** Functional channel type of the LICH (bits 4:3) on a traffic channel. */
enum nxdnUsc {
    NXDN_USC_SACCH_NS = 0x00, ///< SACCH, non-superframe (first/last frame)
    NXDN_USC_UDCH = 0x01,     ///< User data channel
    NXDN_USC_SACCH_SF = 0x02, ///< SACCH, superframe (voice frames)
    NXDN_USC_SACCH_IDLE = 0x03, ///< SACCH superframe with an idle FACCH1
};

/** Steal flag of the LICH (bits 2:1), for USC = SACCH. */
enum nxdnSteal {
    NXDN_STEAL_BOTH = 0x00,  ///< Both VCH halves are FACCH1
    NXDN_STEAL_FIRST = 0x01, ///< VCH1/VCH2 are FACCH1
    NXDN_STEAL_SECOND = 0x02, ///< VCH3/VCH4 are FACCH1
    NXDN_STEAL_NONE = 0x03,  ///< VCH only
};

/** Decoded LICH: the 7 control bits split into their fields. */
struct nxdnLich {
    uint8_t raw;       ///< The 7 control bits
    uint8_t rfType;    ///< enum nxdnRfType
    uint8_t usc;       ///< enum nxdnUsc
    uint8_t steal;     ///< enum nxdnSteal
    bool outbound;     ///< Direction bit: true = outbound
    bool parityOk;     ///< Even parity over the 4 MSBs matched
};

/**
 * @brief Split the 7 control bits of a LICH into fields.
 */
void nxdnLichParse(uint8_t raw, struct nxdnLich *lich);

/**
 * @brief Number of VCHs of a frame that carry voice, given its LICH.
 *
 * Voice sits in the half (or halves) the steal flag does not hand to FACCH1,
 * and only on a traffic channel whose functional channel is a SACCH.
 *
 * @return first VCH index (0 or 2) in *first, count (0, 2 or 4) in *count.
 */
void nxdnLichVoice(const struct nxdnLich *lich, int *first, int *count);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_NXDN_DEFS_H */
