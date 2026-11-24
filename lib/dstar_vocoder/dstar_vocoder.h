/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dstar_vocoder.h
 * @brief AMBE 3600x2400 voice decoder for the D-STAR DV mode (C62 target).
 *
 * Public contract of lib/dstar_vocoder: one 72-bit D-STAR voice frame in, 160
 * samples of 8 kHz PCM out.  The backend (dstar_decoder.c, GPL-2.0-or-later)
 * is a C port of mbelib-neo v2.6.2's D-STAR path, restructured for a small ARM
 * MCU:
 *
 *  - decode path only, single static instance: no heap, no thread-local
 *    storage, no C++ and no libc stdio;
 *  - no libm: sin/cos/exp2/log2/sqrt come from lib/dmr_vocoder's dmr_math.h,
 *    the 256-point real FFT from dmr_fft.c, every quantiser table from
 *    dstar_tables.h in flash;
 *  - single precision throughout, with the state in PSRAM on the C62 (see
 *    DSTAR_VOCODER_STATE_ATTR below), so the radio's 256 KB of internal RAM
 *    does not grow;
 *  - one frame costs no stack beyond a few scalars: the scratch buffers live
 *    in the instance.
 *
 * The D-STAR front end (openrtx/src/protocols/DSTAR/DStarVocoder.c) owns the
 * air-bit map, the Golay/PN correction and the error counts; this library owns
 * everything after the 49 parameter bits.
 *
 * This file is compiled into the application (not into an archive) so that it
 * overrides the weak dstarVocoder_decode()/dstarVocoder_available() hooks of
 * the protocol layer.
 */

#ifndef DSTAR_VOCODER_H
#define DSTAR_VOCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Size of one D-STAR voice frame on air, in bytes (72 bits). */
#define DSTAR_VOCODER_FRAME_BYTES 9

/** PCM samples produced per frame (20 ms at 8 kHz). */
#define DSTAR_VOCODER_PCM_SAMPLES 160

/** Sample rate of the decoded audio, in Hz. */
#define DSTAR_VOCODER_SAMPLE_RATE 8000

/**
 * @brief Where the decoder's state lives.
 *
 * The C62 maps 1.75 MB of PSRAM at 0x30200000 and its linker script puts
 * `.psram_section` there as NOLOAD, so the state is not zeroed by the
 * start-up code; dstarVocoder_reset() writes every field before the first
 * frame.  Anything else (the host tests, the QEMU harness) gets a plain
 * object.
 */
#ifndef DSTAR_VOCODER_STATE_ATTR
#define DSTAR_VOCODER_STATE_ATTR
#endif

/**
 * @brief Decode one D-STAR voice frame to PCM.
 *
 * The decoder keeps state between calls, so the frames of one transmission
 * must be decoded in order; the first frame after a reset fades in from
 * silence (the synthesis window overlaps two frames).
 *
 * @param air the 72 voice bits of a DV frame, in air order (bit 0 of byte 0 is
 *            the first bit on air)
 * @param pcm output, 160 samples of signed 16-bit PCM at 8 kHz
 * @return 0 on success, -1 on invalid arguments
 */
int dstarVocoder_decode(const uint8_t air[DSTAR_VOCODER_FRAME_BYTES],
                        int16_t pcm[DSTAR_VOCODER_PCM_SAMPLES]);

/**
 * @brief Forget the previous talker: pitch, phases, noise and error history.
 *
 * Call at the start of a transmission, and when synchronisation is lost for
 * longer than a frame or two.  Resetting on every gap, however short, makes a
 * healthy stream sound quiet (each reset fades in from silence).
 */
void dstarVocoder_reset(void);

/** @return true: a 2400 decoder is linked in, so audio plays. */
bool dstarVocoder_available(void);

/**
 * @brief Frames decoded since the reset, and how many were repeats, erasures,
 * tone frames and error-rate mutes.  Diagnostics for the status screen.
 */
void dstarVocoder_stats(uint16_t *frames, uint16_t *bad, uint16_t *erasures,
                        uint16_t *tones, uint16_t *muted);

/** @brief Static RAM used by the decoder state, in bytes. */
size_t dstarVocoder_state_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* DSTAR_VOCODER_H */
