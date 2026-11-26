/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Minimal RIFF/WAVE reader and writer for the host tools: 16-bit PCM mono,
 * any sample rate. Handles the usual odd chunk layout of recorder output and
 * refuses compressed formats rather than guessing.
 */
#ifndef WAV_H
#define WAV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    int16_t *samples;    /**< mono, interleaved input is averaged       */
    size_t   count;
    uint32_t rate;
} wav_t;

/** Load a 16-bit PCM WAV (mono or stereo, averaged to mono). */
bool wav_load(const char *path, wav_t *w);

void wav_free(wav_t *w);

/** Write 16-bit PCM mono. */
bool wav_save(const char *path, const int16_t *samples, size_t count,
              uint32_t rate);

/** Append 16-bit mono samples to a mono WAV (no seeking, file must exist). */
bool wav_append(const char *path, const int16_t *samples, size_t count,
                uint32_t rate);

#ifdef __cplusplus
}
#endif

#endif /* WAV_H */
