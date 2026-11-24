/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_alloc.h
 * @brief Declarations for the allocation hooks passed to the OpenDMR sources.
 *
 * The OpenDMR decoder sources are compiled with
 * -DPFFFT_MALLOC=dmr_vocoder_alloc -DPFFFT_FREE=dmr_vocoder_free and this header
 * is force-included (-include), so that the hook names are declared before the
 * macro is expanded inside lib/opendmr/decoder/pffft.c, which knows nothing
 * about this layer.
 */

#ifndef DMR_ALLOC_H
#define DMR_ALLOC_H

#include <stddef.h>

/** Allocate @p size bytes, 64-byte aligned, from the library's static arena. */
void *dmr_vocoder_alloc(size_t size);

/** Release a block from the static arena (no-op: the arena is bump-allocated). */
void dmr_vocoder_free(void *ptr);

#endif /* DMR_ALLOC_H */
