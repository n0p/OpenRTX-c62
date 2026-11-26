/* dstar_decoder.h -- AMBE 3600x2400 (D-STAR DV) decoder core, one instance.
 *
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Internal header of lib/dstar_vocoder: the firmware-facing contract is
 * dstar_vocoder.h.  This interface exists separately so the decoder can be
 * instantiated more than once (the host comparison decodes the same frames
 * twice) and so the QEMU harness can size the state.
 *
 * Pure C, no libm, no libstdc++, no TLS, no dynamic allocation, every table in
 * flash, all arithmetic single precision (the one exception is the low-band
 * roll-off's phase, which the reference computes in double and this port
 * approximates in float -- see docs/RX_DSTAR.md).
 *
 *   static dstar_decoder_t dec;
 *   dstar_decoder_init(&dec);
 *   ...
 *   dstar_decoder_frame(&dec, params49, c0errs, proterrs, pcm160);
 *
 * Input is the 49 parameter bits of an AMBE 3600x2400 frame (mbelib's
 * mbe_decodeAmbe3600x2400Frame() layout, one bit per byte, MSB first) plus the
 * two error counts the frame's FEC produced, because the decoder's
 * repeat/erasure/mute decisions are driven by them.  Output is 160 samples of
 * 16-bit PCM at 8 kHz, i.e. 20 ms.
 */
#ifndef DSTAR_DECODER_H
#define DSTAR_DECODER_H

#include <stddef.h>
#include <stdint.h>

#define DSTAR_FRAME_SAMPLES 160
#define DSTAR_MAX_HARMONICS 56
#define DSTAR_FFT_SIZE 256

/* One frame of MBE model parameters.  Field for field what mbelib-neo's
 * mbe_parms carries for the D-STAR path, with the types narrowed where the
 * value cannot leave the narrow type (Vl is 0/1, L is 1..56). */
typedef struct {
    float w0;      /* fundamental, rad/sample */
    int16_t L;     /* harmonic count */
    float gamma;   /* gain */
    uint32_t tonePhase;
    uint32_t swn; /* tone phase accumulator (a 32-bit phase, as the reference) */

    float Ml[57];      /* spectral amplitude per harmonic */
    float log2Ml[57];
    float PHIl[57];    /* absolute phase per harmonic */
    float PSIl[57];    /* integrated linear (pulse position) phase */
    uint8_t Vl[57];    /* voiced/unvoiced per harmonic */

    float localEnergy;      /* adaptive smoothing state */
    int32_t amplitudeThreshold;
    float errorRate;
    int16_t errorCountTotal;
    int16_t errorCount4;
    int16_t repeatCount;
    float mutingThreshold;

    float previousUw[DSTAR_FFT_SIZE]; /* previous unvoiced frame (WOLA) */
    float noiseSeed;                  /* unvoiced LCG state */
    float noiseOverlap[96];           /* LCG overlap between frames */
} dstar_parms_t;

/* Decoder state.  All of it is touched per frame, not per sample, so on the
 * C62 it lives in PSRAM (see dstar_vocoder.c). */
typedef struct {
    dstar_parms_t cur, prev, enh;

    /* Scratch, kept in the object so a frame uses no stack. */
    float noise[DSTAR_FFT_SIZE]; /* unvoiced noise buffer */
    float scratch[DSTAR_FFT_SIZE]; /* FFT input / IFFT output */
    float cosTab[57];              /* harmonic cosines of w0 */
    float phi[57];                 /* regenerated envelope phase */
    float phaseB[56 + 2 * 19 + 1]; /* kernel input of the phase regeneration */
    float fbuf[DSTAR_FRAME_SAMPLES]; /* float PCM of this frame */
    float fftRe[DSTAR_FFT_SIZE / 2 + 1];
    float fftIm[DSTAR_FFT_SIZE / 2 + 1];
    float binScale[DSTAR_FFT_SIZE / 2 + 1];
    float savedMl[57];
    float savedPHIl[57];
    float savedMlPrev[57];
    float savedPHIlPrev[57];
    int16_t intkl[57];
    float flokl[57], deltal[57], Tl[57];

    uint64_t comfortState; /* comfort-noise LCG (48 bit), as the reference's */

    /* Diagnostics, read by dstar_vocoder_frame(). */
    uint16_t frames, badFrames, erasures, toneFrames, mutedFrames;
    uint16_t lastErrors;
} dstar_decoder_t;

/* Reset the decoder to the reference's initial state (mbe_initMbeParms()). */
void dstar_decoder_init(dstar_decoder_t *d);

/* Decode one frame.  params is the 49 parameter bits (one per byte, MSB
 * first); c0Errors and protectedErrors are the frame's FEC error counts, used
 * exactly as mbelib's process path uses them (repeat above three, erasure and
 * tone classification).  pcm receives 160 samples. */
void dstar_decoder_frame(dstar_decoder_t *d, const uint8_t params[49],
                         int c0Errors, int protectedErrors,
                         int16_t pcm[DSTAR_FRAME_SAMPLES]);

/* Size of one instance in bytes (for .bss / PSRAM placement decisions). */
size_t dstar_decoder_state_size(void);

#endif /* DSTAR_DECODER_H */
