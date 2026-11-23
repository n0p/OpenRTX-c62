/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_vocoder.c
 * @brief DMR AMBE+2 decode: 9-byte frame to 160 PCM samples.
 *
 * The frame is unpacked and FEC-decoded here, in C, and the resulting 49 voice
 * parameter bits are handed to mbelib-neo's AMBE 3600x2450 synthesiser.  This
 * mirrors OpenDMR's opendmr.cpp decode path, which cannot be used as is on an
 * MCU because it is C++, pulls in the encoder and allocates on the heap.
 */

#include <string.h>

#include "dmr_vocoder.h"
#include "dmr_golay.h"

#include "mbelib.h" /* GPL-2.0-or-later, lib/opendmr/decoder */

/**
 * @brief Static arena for the one-off allocations of the decoder.
 *
 * mbelib allocates the 256-point FFT plan (about 11 KiB) on first use and the
 * AMBE DCT tables live in .bss; there is no allocation per frame.  Routing the
 * few startup allocations through this bump allocator keeps the vocoder off the
 * libc heap entirely: it is deterministic, and the firmware's heap is left for
 * the rest of the system.  Nothing is ever freed during normal operation
 * (dmr_vocoder_free() is a no-op) because the plan is created once and reused.
 */
#define DMR_VOCODER_ARENA_BYTES 20480u
#define DMR_VOCODER_ARENA_ALIGN 64u

static uint8_t dmr_arena[DMR_VOCODER_ARENA_BYTES] __attribute__((aligned(DMR_VOCODER_ARENA_ALIGN)));
static size_t dmr_arena_used;
static int dmr_arena_failed;

/**
 * @brief Decoder state: the three AMBE parameter sets mbelib interpolates between.
 */
static struct {
    mbe_parms cur_mp;
    mbe_parms prev_mp;
    mbe_parms prev_mp_enhanced;
} dmr_state;

static int dmr_initialised;
static unsigned int dmr_last_errors;

/* Scratch buffers kept out of the caller's stack. */
static char dmr_ambe_d[49];
static char dmr_err_str[64];

void *dmr_vocoder_alloc(size_t size)
{
    size_t start = (dmr_arena_used + (DMR_VOCODER_ARENA_ALIGN - 1u)) & ~(size_t)(DMR_VOCODER_ARENA_ALIGN - 1u);

    if (size == 0u || start > DMR_VOCODER_ARENA_BYTES || size > DMR_VOCODER_ARENA_BYTES - start) {
        dmr_arena_failed = 1;
        return NULL;
    }
    dmr_arena_used = start + size;
    return &dmr_arena[start];
}

void dmr_vocoder_free(void *ptr)
{
    (void)ptr; /* bump allocator: everything lives until reset of the MCU */
}

/**
 * @brief Scrambling mask for the B block.
 *
 * The B block is XORed with a 23-bit mask produced by the LCG defined by the
 * AMBE+2 specification, seeded with the decoded 12 bits of the A block.
 *
 * @param a_orig Decoded A block (12 bits).
 * @return 23-bit scrambling mask.
 */
static uint32_t prng_mask_23bit(uint32_t a_orig)
{
    uint16_t pr[24];
    uint32_t mask = 0;

    pr[0] = (uint16_t)(16u * a_orig);
    for (int i = 1; i < 24; i++)
        pr[i] = (uint16_t)((173u * (uint32_t)pr[i - 1] + 13849u) % 65536u);

    for (int i = 1; i < 24; i++) {
        pr[i] = (uint16_t)(pr[i] / 32768u);
        if (pr[i] != 0u)
            mask |= 1u << (23 - i);
    }
    return mask;
}

/**
 * @brief Unpack and FEC-decode a 72-bit frame into the 49 mbelib parameter bits.
 *
 * Frame layout, as produced by OpenDMR and by the DVSI convention:
 *   bits 0..23   A block, Golay(24,12)   -> C0, 12 bits
 *   bits 24..46  B block, Golay(23,12)   -> C1, 12 bits (scrambled with C0)
 *   bits 47..71  C block, unprotected    -> C2 (11 bits) + C3 (14 bits)
 *
 * Output layout expected by mbelib's AMBE 2450 decoder:
 *   ambe_d[0..11]   C0 (MSB first)
 *   ambe_d[12..23]  C1 (MSB first)
 *   ambe_d[24..48]  C2 + C3 (MSB first)
 *
 * @param frame  9-byte frame.
 * @param ambe_d Output parameter bits.
 * @return Number of bit errors corrected by the Golay decoders.
 */
static unsigned int unpack_frame(const uint8_t frame[DMR_VOCODER_FRAME_BYTES], char ambe_d[49])
{
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    unsigned int errors;

    for (int i = 0; i < 24; i++) {
        if ((frame[i / 8] >> (7 - (i % 8))) & 1u)
            a |= 0x800000u >> i;
    }

    for (int i = 0; i < 23; i++) {
        int pos = 24 + i;
        if ((frame[pos / 8] >> (7 - (pos % 8))) & 1u)
            b |= 0x400000u >> i;
    }

    for (int i = 0; i < 25; i++) {
        int pos = 47 + i;
        if ((frame[pos / 8] >> (7 - (pos % 8))) & 1u)
            c |= 0x1000000u >> i;
    }

    uint32_t c0 = dmr_golay24128_decode(a);
    errors = dmr_golay_last_errors;

    uint32_t c1 = dmr_golay23127_decode(b ^ prng_mask_23bit(c0));
    errors += dmr_golay_last_errors;

    memset(ambe_d, 0, 49);
    for (int i = 0; i < 12; i++) {
        ambe_d[i] = (char)((c0 >> (11 - i)) & 1u);
        ambe_d[12 + i] = (char)((c1 >> (11 - i)) & 1u);
    }
    for (int i = 0; i < 25; i++)
        ambe_d[24 + i] = (char)((c >> (24 - i)) & 1u);

    return errors;
}

int dmr_vocoder_decode(const uint8_t frame[DMR_VOCODER_FRAME_BYTES],
                       int16_t pcm[DMR_VOCODER_PCM_SAMPLES])
{
    int err_count = 0;
    int err_count2 = 0;

    if (frame == NULL || pcm == NULL)
        return -1;

    if (dmr_initialised == 0) {
        mbe_initMbeParms(&dmr_state.cur_mp, &dmr_state.prev_mp, &dmr_state.prev_mp_enhanced);
        dmr_initialised = 1;
    }

    dmr_last_errors = unpack_frame(frame, dmr_ambe_d);

    dmr_err_str[0] = 0;
    mbe_processAmbe2450Data(pcm, &err_count, &err_count2, dmr_err_str, dmr_ambe_d,
                            &dmr_state.cur_mp, &dmr_state.prev_mp,
                            &dmr_state.prev_mp_enhanced, 3);

    return dmr_arena_failed ? -2 : 0;
}

void dmr_vocoder_reset(void)
{
    mbe_initMbeParms(&dmr_state.cur_mp, &dmr_state.prev_mp, &dmr_state.prev_mp_enhanced);
    dmr_initialised = 1;
    dmr_last_errors = 0;
}

unsigned int dmr_vocoder_last_errors(void)
{
    return dmr_last_errors;
}

size_t dmr_vocoder_state_bytes(void)
{
    return sizeof(dmr_state);
}

size_t dmr_vocoder_arena_used(void)
{
    return dmr_arena_used;
}
