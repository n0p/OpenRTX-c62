/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file sim_png.h
 * @brief PNG writer of the host simulator.
 *
 * SDL2 can only save BMP by itself; the screenshots of the simulator are PNGs
 * because they are read by tools, committed to a directory per run and
 * compared, and a BMP is neither small nor reviewable in a diff viewer. zlib
 * is already a dependency of every host build of this tree, so the encoder is
 * thirty lines: an 8-bit RGB image, one IDAT, no interlacing.
 */

#ifndef SIM_PNG_H
#define SIM_PNG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write an 8-bit RGB image as PNG.
 *
 * @param path: destination file.
 * @param rgb: pixels, 3 bytes per pixel, row-major, no padding.
 * @param w, h: image size.
 * @return true on success.
 */
bool sim_png_write(const char *path, const uint8_t *rgb, int w, int h);

#ifdef __cplusplus
}
#endif

#endif /* SIM_PNG_H */
