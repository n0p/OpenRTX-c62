/* imbe_decoder.h -- IMBE 7200x4400 (P25 Phase 1) decoder core, one instance.
 *
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The internal half of lib/imbe_vocoder: the firmware-facing contract is
 * imbe_vocoder.h, which owns the one instance the radio uses.  This interface
 * is separate so the decoder can be instantiated more than once (the host
 * tests decode two streams side by side) and so the QEMU harness can reach the
 * per-instance state.
 *
 * Pure C, no libm, no libstdc++, no TLS, no dynamic allocation, no plan: the
 * whole decoder state lives in one instance the caller allocates (static or on
 * the stack), so there is nothing to malloc and nothing that touches .data at
 * start-up.  Every table is const, in flash.
 *
 *   static imbe_decoder_t voc;
 *   imbe_decoder_init(&voc);
 *   ...
 *   imbe_decoder_decode(&voc, params, total_errors, pcm160);
 *
 * Input: the 88 IMBE parameters of one 20 ms frame, one bit per byte (0/1) in
 * the order TIA-102.BABA chapter 6 numbers them (b0 first), i.e. what
 * mbelib-neo's mbe_decodeImbe4400Parms() consumes and what the P25 receiver
 * gets out of the LDU's Golay/Hamming layer.  Output: 160 samples at 8 kHz.
 */
#ifndef IMBE_DECODER_H
#define IMBE_DECODER_H

#include <stddef.h>
#include <stdint.h>

/** Parameter bits of one frame. */
#define IMBE_PARAM_BITS 88
/** PCM samples of one frame (20 ms at 8 kHz). */
#define IMBE_PCM_SAMPLES 160
/** Highest harmonic count IMBE uses. */
#define IMBE_MAX_HARMONICS 56
/** Frame repeats before the synthesis mutes (TIA-102.BABA 7.7). */
#define IMBE_MAX_FRAME_REPEATS 4
/** Muting error-rate threshold of the IMBE path (TIA-102.BABA 7.8). */
#define IMBE_MUTING_THRESHOLD 0.0875f

/* One frame of MBE model parameters, as the reference keeps them: current,
 * previous and previous-enhanced.  Element 0 of every array is unused by the
 * model, and PHIl[0] is the reference's frame-mark slot (the IMBE path never
 * sets a mark, so it stays 0). */
typedef struct {
    float w0;                              /* fundamental, rad/sample */
    int32_t L;                             /* harmonics */
    int32_t K;                             /* voiced-band allocation */
    int32_t Vl[IMBE_MAX_HARMONICS + 1];    /* voiced/unvoiced per harmonic */
    float Ml[IMBE_MAX_HARMONICS + 1];      /* spectral amplitude */
    float log2Ml[IMBE_MAX_HARMONICS + 1];
    float PHIl[IMBE_MAX_HARMONICS + 1];    /* phase at the frame's end */
    float PSIl[IMBE_MAX_HARMONICS + 1];    /* integrated linear phase */
    float gamma;
    int32_t swn;                           /* tone phase counter (unused) */
    float localEnergy;                     /* adaptive-smoothing state */
    int32_t amplitudeThreshold;
    float errorRate;
    int32_t errorCountTotal;
    int32_t errorCount4;
    int32_t repeatCount;
    float mutingThreshold;
    float previousUw[256];                 /* previous unvoiced block (WOLA) */
    float noiseSeed;                       /* unvoiced noise LCG state */
    float noiseOverlap[96];                /* noise carried between frames */
} imbe_parms_t;

typedef struct {
    imbe_parms_t cur, prev, enh;

    /* per-instance scratch: keeps the stack shallow and the frame small */
    float noise[256];                      /* this frame's noise buffer */
    float uw[256];                         /* inverse-FFT output */
    float fft_re[129], fft_im[129];
    float bin_scale[129];
    float cos_tab[IMBE_MAX_HARMONICS + 1];

    uint64_t cn_state;                     /* muting-noise generator (Java LCG) */
} imbe_decoder_t;

/** Reset the decoder to its start-up state (silence, default model). */
void imbe_decoder_init(imbe_decoder_t *v);

/** Bytes of one decoder instance. */
size_t imbe_decoder_state_size(void);

/**
 * Decode one frame of IMBE parameters into 160 samples of 8 kHz PCM.
 *
 * @param v: decoder instance.
 * @param params: 88 parameter bits, one per byte, 0 or 1, b0 first.
 * @param total_errors: corrected-bit count the FEC layer reported for this
 *        frame (0 for a clean frame).  Drives the repeat and mute decisions
 *        exactly as the reference's error accounting does.
 * @param pcm: destination, IMBE_PCM_SAMPLES samples.
 * @return 0 on a voice frame, 1 if the frame was rejected (bad fundamental,
 *         repeated or muted), -1 on bad arguments.
 */
int imbe_decoder_decode(imbe_decoder_t *v, const uint8_t params[IMBE_PARAM_BITS],
                        int total_errors, int16_t pcm[IMBE_PCM_SAMPLES]);

/**
 * Decode and report what the frame became, for diagnostics and tests:
 * bit 0 = repeated, bit 1 = muted, bit 2 = rejected fundamental.
 */
int imbe_decoder_decode_ex(imbe_decoder_t *v, const uint8_t params[IMBE_PARAM_BITS],
                           int total_errors, int16_t pcm[IMBE_PCM_SAMPLES],
                           unsigned *flags);

/**
 * Unpack 11 bytes (MSB first, as the P25 LDU carries them) into the 88
 * parameter bits this decoder consumes.
 */
void imbe_unpack_params(const uint8_t bytes[11], uint8_t params[IMBE_PARAM_BITS]);

#endif /* IMBE_DECODER_H */
