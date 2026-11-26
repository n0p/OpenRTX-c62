/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_CODEC_H
#define AUDIO_CODEC_H

#include "core/audio_path.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise audio codec manager, allocating data buffers.
 *
 * This function allows recursive calls. However the number of times this
 * function is called must be balanced by an equal number of calls to the
 * terminate() function.
 */
void codec_init();

/**
 * Shutdown audio codec manager and deallocate data buffers.
 */
void codec_terminate();

/**
 * Start encoding of audio data from a given audio source.
 * Only an encoding or decoding operation at a time is possible: in case there
 * is already an operation in progress, this function returns false.
 *
 * @param path: audio path for encoding source.
 * @return true on success, false on failure.
 */
bool codec_startEncode(const pathId path);

/**
 * Start dencoding of audio data sending the uncompressed samples to a given
 * audio destination.
 * Only an encoding or decoding operation at a time is possible: in case there
 * is already an operation in progress, this function returns false.
 *
 * @param path: audio path for decoded audio.
 * @return true on success, false on failure.
 */
bool codec_startDecode(const pathId path);

/**
 * Stop an ongoing encoding or decoding operation.
 *
 * The call is bounded: it asks the codec thread to exit, cancels the audio
 * stream it is reading or writing -- which is what releases it if it is blocked
 * there -- and waits a bounded time for it. A thread that does not come back is
 * left to exit on its own, because the alternative is a caller -- the RTX thread
 * with the PA keyed -- waiting for ever.
 *
 * @param path: audio path on which the encoding or decoding operation was
 * started.
 */
void codec_stop(const pathId path);

/**
 * Cancel the running operation from another thread, without waiting.
 *
 * Sets the stop request and terminates the stream the codec thread is blocked
 * on. Safe from any thread; the thread then unwinds and marks itself stopped.
 * Used by the transmit supervisor, whose job is to release a blocked RTX
 * thread.
 */
void codec_abort();

/**
 * Get current oprational status of the codec thread.
 *
 * @return true if the codec thread is active.
 */
bool codec_running();

/**
 * Check if the codec has no frames queued for a given audio path.
 *
 * @param path: audio path on which the operation was started.
 * @return true if the codec is not running on that path or its queue is empty.
 */
bool codec_queueEmpty(const pathId path);

/**
 * Get a compressed audio frame from the internal queue. Each frame is composed
 * of 8 bytes.
 *
 * @param frame: pointer to a destination buffer where to put the encoded frame.
 * @param blocking: if true the execution flow will be blocked whenever the
 * internal buffer is empty and resumed as soon as an encoded frame is available.
 * @return zero on success, -EAGAIN if the queue is empty and the function is
 * nonblocking or -EPERM if there is no encoding operation ongoing.
 */
int codec_popFrame(uint8_t *frame, const bool blocking);

/**
 * Push a a compressed audio frame to the internal queue for decoding.
 * Each frame is composed of 8 bytes.
 *
 * @param frame: frame to be pushed to the queue.
 * @param blocking: if true the execution flow will be blocked whenever the
 * internal buffer is full and resumed as soon as space for an encoded frame is
 * available.
 * @return zero on success, -EAGAIN if the queue is full and the function is
 * nonblocking or -EPERM if there is no decoding operation ongoing.
 */
int codec_pushFrame(const uint8_t *frame, const bool blocking);

#ifdef __cplusplus
}
#endif

#endif
