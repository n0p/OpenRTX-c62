/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_vocoder.h
 * @brief DMR AMBE+2 vocoder, decode only, for the C62 target.
 *
 * Thin C layer around the decoder half of OpenDMR (lib/opendmr, GPL-2.0-or-later
 * mbelib-neo sources), built for a small ARM MCU:
 *
 *  - decode path only: the OP25-derived encoder is not compiled (it does not fit
 *    in the CSK6011B's Cortex-M33, see docs/DMR_VOCODER.md);
 *  - single static instance: no heap, no thread-local storage, no libc printf;
 *  - no per-frame allocation: the FFT plan and the DCT tables are built once, in
 *    a static arena, on the first decode;
 *  - no C++: the Golay decoders and the frame unpacking are reimplemented in C
 *    (dmr_golay.c);
 *  - FPU single precision only.
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
 * overlaps two frames); this matches the reference implementation.
 *
 * @param frame AMBE+2 frame, 9 bytes, canonical order A|B|C.
 * @param pcm   Output buffer, 160 samples of signed 16-bit PCM at 8 kHz.
 * @return 0 on success, -1 on invalid arguments, -2 if the internal arena is
 *         exhausted (cannot happen with the sizes in this build).
 */
int dmr_vocoder_decode(const uint8_t frame[DMR_VOCODER_FRAME_BYTES],
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
 * @brief Static RAM used by the vocoder state (decoder parameters, DCT tables).
 */
size_t dmr_vocoder_state_bytes(void);

/**
 * @brief Static arena high-water mark, valid after the first decode.
 */
size_t dmr_vocoder_arena_used(void);

/*
 * Internal: allocator handed to the OpenDMR sources (MBE_MALLOC/PFFFT_MALLOC)
 * to keep every allocation inside this library's static arena.  Not for use by
 * application code.
 */
void *dmr_vocoder_alloc(size_t size);
void dmr_vocoder_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* DMR_VOCODER_H */
