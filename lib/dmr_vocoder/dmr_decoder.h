/* dmr_decoder.h -- AMBE+2 (DMR 3600x2450) decoder core, one instance.
 *
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Internal header of lib/dmr_vocoder: the firmware-facing contract is
 * dmr_vocoder.h, which owns the one instance the radio uses.  This interface
 * exists separately so that the decoder can be instantiated more than once
 * (the host tests decode two streams side by side) and so that the QEMU
 * harnesses can reach the per-instance state.
 *
 * Pure C, no libm, no libstdc++, no TLS, no dynamic allocation.  The whole
 * decoder state lives in one instance structure that the caller allocates
 * (static or on the stack), so there is nothing to malloc and nothing that
 * touches .data at start-up: every table is const, in flash.
 *
 *   static dmr_vocoder_t voc;
 *   dmr_vocoder_init(&voc);
 *   ...
 *   dmr_vocoder_decode_frame(&voc, frame9, pcm160, &errs);
 *
 * Input frame format: the 9-byte / 72-bit "canonical" AMBE+2 frame
 * (A: 24 bits Golay(24,12) | B: 23 bits Golay(23,12) scrambled | C: 25 raw
 * bits), MSB first, i.e. what OpenRTX gets after de-interleaving the DMR burst.
 * Output: 160 little-endian int16 samples at 8 kHz.
 */
#ifndef DMR_DECODER_H
#define DMR_DECODER_H

#include <stddef.h>
#include <stdint.h>

#define DMR_FRAME_BYTES 9
#define DMR_PCM_SAMPLES 160
#define DMR_MAX_BANDS 56 /* 56 harmonics at 8 kHz */

/* One frame of MBE model parameters (the four sets kept by the reference
 * decoder: current, previous and previous-enhanced). */
typedef struct {
    float w0;          /* fundamental frequency, rad/sample */
    int16_t L;         /* number of harmonics */
    int16_t swn;       /* tone generator phase counter */
    int16_t repeat;    /* repeated frame flag */
    int16_t repeatCount;
    int16_t amplitudeThreshold;
    int16_t errorCountTotal;
    int16_t errorCount4;
    float gamma;       /* gain */
    float errorRate;   /* smoothed bit error rate */
    float localEnergy; /* adaptive smoothing state */
    float mutingThreshold;
    /* Indexed 0..56; the model's predictor reads log2Ml[L+1] with L up to 56,
     * so one extra slot is kept allocated (and zeroed) to keep that read
     * inside the object instead of running into the next field. */
    uint8_t Vl[DMR_MAX_BANDS + 2]; /* voiced/unvoiced per harmonic */
    float Ml[DMR_MAX_BANDS + 2];   /* spectral amplitude */
    float log2Ml[DMR_MAX_BANDS + 2];
    float PHIl[DMR_MAX_BANDS + 2]; /* synthesis phase */
    float PSIl[DMR_MAX_BANDS + 2]; /* phase accumulator */
} dmr_parms_t;

typedef struct {
    dmr_parms_t cur, prev, enh;

    float last_uw[256];      /* previous unvoiced synthesis block (WOLA) */
    float noise_overlap[96]; /* LCG noise overlap between frames */
    float noise_seed;

    /* per-instance scratch (keeps the stack shallow and the frame small) */
    float noise[256];
    float uw[256];
    float fft_re[129], fft_im[129];
    float bin_scale[129];
    float cos_tab[DMR_MAX_BANDS + 1];
    int16_t a_min[DMR_MAX_BANDS + 1], b_max[DMR_MAX_BANDS + 1];

    uint32_t cn_seed; /* comfort-noise RNG */
} dmr_vocoder_t;

/* Reset an instance to the decoder's initial state (no allocation). */
void dmr_vocoder_init(dmr_vocoder_t *v);

/* Decode one 72-bit frame to 160 PCM samples.
 * Returns 0 on success, -1 on bad arguments.  *errs (optional) receives the
 * number of corrected FEC bits for this frame. */
int dmr_vocoder_decode_frame(dmr_vocoder_t *v, const uint8_t frame[DMR_FRAME_BYTES],
                             int16_t pcm[DMR_PCM_SAMPLES], int *errs);

/* The two halves of dmr_vocoder_decode_frame(), for a caller that has to take
 * a keystream off the coded bits between them (DMR privacy, NXDN):
 *
 *   n = dmr_vocoder_unpack(frame, d);        // FEC, n = corrected bits
 *   ... descramble d[0..48] ...
 *   dmr_vocoder_decode_data(v, d, pcm);      // synthesis, no FEC
 *
 * dmr_vocoder_decode_data() is the mirror of mbelib's
 * mbe_processAmbe2450Dataf(); dmr_vocoder_unpack() of its
 * mbe_eccAmbe3600x2450C0() + mbe_demodulateAmbe3600x2450Data(). */
int dmr_vocoder_unpack(const uint8_t frame[DMR_FRAME_BYTES], uint8_t d[49]);
int dmr_vocoder_decode_data(dmr_vocoder_t *v, const uint8_t d[49],
                            int16_t pcm[DMR_PCM_SAMPLES]);

/* Size of one instance in bytes (for .bss placement decisions). */
size_t dmr_vocoder_state_size(void);

#endif /* DMR_DECODER_H */
