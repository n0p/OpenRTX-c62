/* SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Codec2 stack high-water mark, Cortex-M33 under QEMU (mps2-an505).
 *
 * The codec thread of openrtx/src/core/audio_codec.c runs codec2_encode() or
 * codec2_decode() on a stack of CODEC2_THREAD_STKSIZE bytes.  This harness runs
 * the same call with the same compiler and flags on a stack of exactly that
 * size, painted with 0xA5, and reports how much of it the call used: the
 * distance from the top of the buffer down to the lowest byte the call
 * modified.  It is the number that decides whether the stack can be 8 KiB
 * instead of 16 (reports/codecs.md section 3.3).
 *
 *   -DMODE_ENC  encode path (audio_codec.c's encodeFunc loop body)
 *   -DMODE_DEC  decode path (decodeFunc's, including its 320-sample buffer)
 *   -DSTACK_BYTES=n  stack to paint, default 16384
 *   -DFRAMES=n  frames to run, default 20
 *
 * The call runs on a stack of its own through csk6_call_on_stack(), because the
 * harness's own main() already sits on the mps2 stack and its frames would be
 * counted otherwise.
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "codec2.h"
#include "defines.h"
#include "harness_out.h"

#ifndef STACK_BYTES
#define STACK_BYTES 16384
#endif

#ifndef FRAMES
#define FRAMES 20
#endif

#if !defined(MODE_ENC) && !defined(MODE_DEC)
#error "build with -DMODE_ENC or -DMODE_DEC"
#endif

/*
 * The two codebooks the cmake of codec2 generates and this tree does not
 * carry: mode 3200 never reads them, but codec2_decode()/codec2_encode()
 * reference the other modes' entry points, so the linker needs the symbols.
 * Weak, as in codec2_cost.c, so a real table would win.
 */
__attribute__((weak)) const struct lsp_codebook newamp2vq_cb[1] = {{0, 0, 0, 0}};
__attribute__((weak)) const struct lsp_codebook newamp2_energy_cb[1] =
    {{0, 0, 0, 0}};

#define PAINT 0xA5

static uint32_t threadStack[STACK_BYTES / 4] __attribute__((aligned(8)));
static uint32_t savedSp;

/*
 * r0 = stack top, r1 = function, r2 = where to keep the caller's stack pointer.
 * r4 is callee-saved, so it carries the old stack pointer across the call; the
 * push/pop pair keeps the ABI's promise about it and about lr.
 */
__attribute__((naked, noinline)) static void
csk6_call_on_stack(uint32_t *top, void (*fn)(void), uint32_t *save)
{
    (void)top;
    (void)fn;
    (void)save;
    __asm__ volatile("push {r4, lr}\n"
                     "str  sp, [r2]\n"
                     "mov  r4, sp\n"
                     "mov  sp, r0\n"
                     "mov  r0, r2\n"
                     "blx  r1\n"
                     "mov  sp, r4\n"
                     "pop  {r4, pc}\n");
}

static volatile uint32_t sink;

/**
 * The encode path of audio_codec.c: the codec's own frames plus the loop body.
 */
static void encodePath(void)
{
    struct CODEC2 *codec2 = codec2_create(CODEC2_MODE_3200);
    if (codec2 == NULL) {
        hout_str("codec2_stack: codec2_create failed\n");
        return;
    }

    int16_t audio[160];
    for (unsigned i = 0; i < 160; i++)
        audio[i] = (int16_t)((i * 977) & 0x3fff) - 8192;

    for (unsigned f = 0; f < FRAMES; f++) {
        uint64_t frame = 0;
        codec2_encode(codec2, (uint8_t *)&frame, audio);
        sink += (uint32_t)frame;
    }

    codec2_destroy(codec2);
}

/**
 * The decode path: decodeFunc() declares a 320-sample double buffer on the
 * stack and decodes into one half per call.
 */
static void decodePath(void)
{
    int16_t audioBuf[320];
    memset(audioBuf, 0, sizeof(audioBuf));

    struct CODEC2 *codec2 = codec2_create(CODEC2_MODE_3200);
    if (codec2 == NULL) {
        hout_str("codec2_stack: codec2_create failed\n");
        return;
    }

    for (unsigned f = 0; f < FRAMES; f++) {
        uint64_t frame = 0x0123456789abcdefull ^ f;
        codec2_decode(codec2, audioBuf, (uint8_t *)&frame);
        sink += (uint16_t)audioBuf[f & 1];
    }

    codec2_destroy(codec2);
}

int main(void)
{
    memset(threadStack, PAINT, sizeof(threadStack));

    uint32_t *top = &threadStack[STACK_BYTES / 4];
#ifdef MODE_ENC
    csk6_call_on_stack(top, encodePath, &savedSp);
#else
    csk6_call_on_stack(top, decodePath, &savedSp);
#endif

    /* Lowest byte the call touched: everything below it is still painted */
    const uint8_t *bytes = (const uint8_t *)threadStack;
    size_t bottom = 0;
    for (size_t i = 0; i < sizeof(threadStack); i++) {
        if (bytes[i] != PAINT) {
            bottom = i;
            break;
        }
    }
    size_t used = (bottom == 0) ? sizeof(threadStack)
                                : sizeof(threadStack) - bottom;

    char line[96], *p = line;
#ifdef MODE_ENC
    p = hout_lit(p, "CODEC2-STACK enc stack=");
#else
    p = hout_lit(p, "CODEC2-STACK dec stack=");
#endif
    p = hout_dec(p, STACK_BYTES);
    p = hout_lit(p, " used=");
    p = hout_dec(p, (uint32_t)used);
    p = hout_lit(p, "\n");
    *p = '\0';
    hout_str(line);
    return (int)sink & 0;
}
