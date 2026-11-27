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
 * Pure C, no libm, no libstdc++, no TLS, no dynamic allocation.  The
 * frame-to-frame state lives in one instance structure that the caller
 * allocates (static or on the stack) and the per-frame working set in a
 * second one whose storage the caller also owns and may place in slower
 * memory, so there is nothing to malloc and nothing that touches .data at
 * start-up: every table is const, in flash.
 *
 *   static dmr_vocoder_t voc;
 *   static dmr_scratch_t scratch;   // may live in slower memory
 *   dmr_vocoder_init(&voc, &scratch);
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

/*
 * Per-frame working set (4 056 B): filled and consumed inside one call to
 * dmr_vocoder_decode_frame()/decode_data(), never carried between frames.
 *
 * It is a separate object so that a target can put it where the cheapest
 * memory is.  On the C62 the frame-to-frame state below is in DTCM (zero wait
 * states, 16 KB, the region that limits the whole firmware) but this scratch
 * goes to PSRAM: it is written and read once or twice per 20 ms frame, never
 * inside a sample loop, so the slower memory costs nothing measurable.  That
 * takes 4 056 B off DTCM and is what makes two decoder instances (the two DMR
 * slots) fit there.
 *
 * The caller owns the storage and must keep it alive for as long as the
 * instance: dmr_vocoder_init() only stores the pointer.
 */
typedef struct {
    float noise[256];
    float uw[256];
    float fft_re[129], fft_im[129];
    float bin_scale[129];
    float cos_tab[DMR_MAX_BANDS + 1];
    int16_t a_min[DMR_MAX_BANDS + 1], b_max[DMR_MAX_BANDS + 1];
} dmr_scratch_t;

typedef struct {
    dmr_parms_t cur, prev, enh;

    float last_uw[256];      /* previous unvoiced synthesis block (WOLA) */
    float noise_overlap[96]; /* LCG noise overlap between frames */
    float noise_seed;

    uint32_t cn_seed; /* comfort-noise RNG */

    /* Per-frame working set, owned by the caller (see dmr_scratch_t).  Kept
     * out of this structure so that the hot state stays compact and can live
     * in fast memory on its own.  Last, so the 8-byte alignment of the pointer
     * leaves no padding in the middle. */
    dmr_scratch_t *scratch;
} dmr_vocoder_t;

/* Size of the per-frame scratch, in bytes. */
size_t dmr_vocoder_scratch_size(void);

/* Reset an instance to the decoder's initial state (no allocation).  The
 * scratch storage is caller-owned and only referenced, not cleared: every
 * field is written before it is read, once per frame. */
void dmr_vocoder_init(dmr_vocoder_t *v, dmr_scratch_t *scratch);

/* Decode one 72-bit frame to 160 PCM samples.
 * Returns 0 on success, -1 on bad arguments.  *errs (optional) receives the
 * number of corrected FEC bits for this frame. */
int dmr_vocoder_decode_frame(dmr_vocoder_t *v, const uint8_t frame[DMR_FRAME_BYTES],
                             int16_t pcm[DMR_PCM_SAMPLES], int *errs);

/**
 * Decode one 49-bit AMBE parameter frame to 160 PCM samples.
 *
 * System Fusion's V/D mode 2 hands the vocoder the 49 parameter bits
 * directly -- it protects them with its own repetition and whitening -- so
 * this entry takes what dmr_vocoder_decode_frame() recovers internally.
 * Same state, same bit-exactness, no FEC in this path.
 *
 * @param d: 49 parameter bits, one per byte, MSB first, in the order
 *           dmr_vocoder_decode_frame() produces.
 */
int dmr_vocoder_decode_ambe_params(dmr_vocoder_t *v, const uint8_t d[49],
                            int16_t pcm[DMR_PCM_SAMPLES], int *errs);

/**
 * Correct one extended Golay(24,12) codeword: 12 information bits, 11 parity
 * bits and one overall parity bit, most significant first.
 *
 * Shared with the System Fusion FICH, which uses the same code, so that only
 * one syndrome table lives in flash.
 *
 * @param w24: the received 24-bit word.
 * @param info12: receives the 12 information bits.
 * @return the number of corrected bits (0 to 3), or -1 if the word is too
 *         far from any codeword.
 */
int dmr_golay24_correct(uint32_t w24, uint32_t *info12);

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
