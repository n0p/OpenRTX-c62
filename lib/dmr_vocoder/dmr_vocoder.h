/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_vocoder.h
 * @brief DMR AMBE+2 vocoder, decode only, for the C62 target.
 *
 * Public contract of lib/dmr_vocoder: one 72-bit AMBE+2 voice frame in, 160
 * samples of 8 kHz PCM out.  The backend (dmr_decoder.c, GPL-2.0-or-later) is
 * a pure C reimplementation of the decoder half of OpenDMR (mbelib-neo),
 * restructured for a small ARM MCU:
 *
 *  - decode path only: the OP25-derived encoder is not part of the library and
 *    does not fit in the CSK6011B's Cortex-M33 anyway;
 *  - single static instance: no heap, no thread-local storage, no libc stdio;
 *  - no per-frame allocation and no start-up computation: every table (DCT
 *    cosines, windows, FFT twiddles, Golay syndromes, quantisers) is in flash;
 *  - no C++, no libm: sin/cos/exp2/log2 come from dmr_math.h and sqrt is the
 *    FPv5 VSQRT instruction, so the object only references memcpy/memset;
 *  - FPU single precision only, no double-precision arithmetic anywhere.
 *
 * One DMR voice frame is 72 bits (9 bytes) in the canonical, de-interleaved
 * order A(24) | B(23) | C(25) used by OpenDMR, and synthesises 160 samples of
 * 16-bit PCM at 8 kHz, i.e. 20 ms of audio.  Undoing the over-the-air
 * interleaving of the 36 dibits of a burst is the caller's job: see
 * docs/DMR_VOCODER.md and the DMR mode in openrtx/src/protocols.
 *
 * Typical use:
 *
 *     dmr_vocoder_reset();                       // start of a new transmission
 *     for (;;) {
 *         if (dmr_vocoder_decode(frame, pcm) == 0)
 *             play(pcm, 160);
 *     }
 */

#ifndef DMR_VOCODER_H
#define DMR_VOCODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Size of one AMBE+2 voice frame, in bytes. */
#define DMR_VOCODER_FRAME_BYTES 9

/** Number of PCM samples produced per frame (20 ms at 8 kHz). */
#define DMR_VOCODER_PCM_SAMPLES 160

/** Sample rate of the decoded audio, in Hz. */
#define DMR_VOCODER_SAMPLE_RATE 8000

/**
 * @brief Decode one AMBE+2 voice frame to PCM.
 *
 * The decoder keeps state between calls, so consecutive frames of the same
 * transmission must be decoded in order.  The first frame after
 * dmr_vocoder_reset() produces a fade-in from silence (the synthesis window
 * overlaps two frames); this matches the reference implementation.  The state
 * is initialised on the first call, so an explicit reset before the first
 * frame is optional.
 *
 * @param frame AMBE+2 frame, 9 bytes, canonical order A|B|C.
 * @param pcm   Output buffer, 160 samples of signed 16-bit PCM at 8 kHz.
 * @return 0 on success, -1 on invalid arguments.
 */
int dmr_vocoder_decode(const uint8_t frame[DMR_VOCODER_FRAME_BYTES],
                       int16_t pcm[DMR_VOCODER_PCM_SAMPLES]);

/**
 * @brief Decode one 49-bit AMBE+2 parameter frame to PCM.
 *
 * System Fusion's V/D mode 2 carries the parameters with its own repetition
 * and whitening, so no Golay/Hamming layer is left for the vocoder: this entry
 * takes what dmr_vocoder_decode() recovers internally from a 72-bit frame.
 * Same instance and state, so a decoder started on one mode continues on the
 * other without a reset.
 *
 * @param d: 49 parameter bits, one per byte, MSB first.
 * @param pcm: 160 samples of 16-bit PCM at 8 kHz.
 * @return 0 on success, -1 on bad arguments.
 */
int dmr_vocoder_decode_ambe(const uint8_t d[49],
                            int16_t pcm[DMR_VOCODER_PCM_SAMPLES]);

/**
 * @brief Reset the decoder state.
 *
 * Call at the start of a new transmission, or when synchronisation is lost, so
 * that the synthesis does not carry the previous talker's pitch and phase.
 * Does not free or reallocate anything.
 */
void dmr_vocoder_reset(void);

/**
 * @brief Number of bit errors corrected by the Golay FEC in the last frame.
 *
 * Diagnostic only: values above 3 in either block mean the frame was probably
 * corrupted beyond repair.  Valid after a successful dmr_vocoder_decode().
 */
unsigned int dmr_vocoder_last_errors(void);

/**
 * @brief Static RAM used by the vocoder: state plus per-frame scratch.
 *
 * On the target 8 544 B (4 492 B of state and 4 052 B of scratch); the host
 * adds the wider pointer and reports 8 548 B.
 */
size_t dmr_vocoder_state_bytes(void);

/**
 * @brief Bytes taken from the internal allocation arena: always 0.
 *
 * Kept for API compatibility with the first backend, which needed a static
 * arena for the decoder's one-off allocations.  This decoder allocates
 * nothing: its state is one object and its tables are in flash.
 */
size_t dmr_vocoder_arena_used(void);

#ifdef __cplusplus
}
#endif

#endif /* DMR_VOCODER_H */
