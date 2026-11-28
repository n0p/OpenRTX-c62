/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Frame harness for the RNNoise DSP port.
 *
 * The same source is built three ways so that the three numbers mean the same
 * thing:
 *
 *   host  (x86, cc)            the reference the DSP build is checked against
 *                              when the M33 emulator is not in the loop
 *   m33   (arm-none-eabi-gcc)  the firmware's own build of lib/rnnoise: this is
 *                              what "bit-exact against the M33" is measured on
 *   dsp   (xt-clang, venus_hifi4) the port under test
 *
 * Two modes, chosen with -DNS_DUMP:
 *
 *   cost  (default)  2 warm-up frames then NS_ITERS frames, each bracketed by
 *                    frame_marker() so a profiler can attribute cost to one
 *                    10 ms frame.  No I/O at all.
 *   dump  (NS_DUMP=1) the same loop, then the output of every frame as raw
 *                    float bits, one frame per line, for a frame-by-frame
 *                    comparison against another build.
 *
 * int16 goes in and int16 comes out, through the same conversion the firmware
 * does in core/noise_suppress.c, because that conversion is part of what the
 * DSP service will have to reproduce.
 */

#include <stdint.h>
#include <stddef.h>

#include "rnnoise.h"
#include "pcm_frames.h"

#if NS_DUMP
#include <stdio.h>
#endif
#ifdef NS_RDIMON
extern void initialise_monitor_handles(void);
#endif

#ifndef NS_ITERS
#define NS_ITERS 8
#endif
#define NS_WARM 2
#define NS_FRAME 480

/* rnnoise_init() calloc()s the three GRU states (672 B in total).  No target
 * of this bench has a heap worth the name, so _sbrk hands out a static bump
 * arena.  The M33 build overrides this with newlib's, see common/mps2.ld. */
#ifndef NS_OWN_SBRK
static unsigned char ns_heap[4096] __attribute__((aligned(8)));
static size_t ns_heap_used;
void *_sbrk(ptrdiff_t incr)
{
    void *p = &ns_heap[ns_heap_used];
    if (ns_heap_used + (size_t)incr > sizeof(ns_heap))
        return (void *)-1;
    ns_heap_used += ((size_t)incr + 7u) & ~(size_t)7u;
    return p;
}
#endif

/* 62 312 B of DenoiseState with the C62 patches (working set out of the
 * stack); 64 KiB leaves room for the alignment and for future state. */
static unsigned char ns_state[64 * 1024] __attribute__((aligned(8)));

static float ns_in[NS_FRAME];
static float ns_out[NS_FRAME];
#if NS_DUMP
static float ns_keep[NS_ITERS][NS_FRAME];
#endif
static volatile int32_t ns_sink;

/* Results, read out of guest memory by the runners that cannot print. */
static volatile uint32_t ns_res[8];

__attribute__((noinline, used)) void frame_marker(void);
void frame_marker(void) { __asm__ volatile("" ::: "memory"); }

static void ns_load(unsigned frame)
{
    const int16_t *p = ns_test_pcm[frame % NS_TEST_FRAMES];
    for (int i = 0; i < NS_FRAME; i++)
        ns_in[i] = (float)p[i];
}

int main(void)
{
#ifdef NS_RDIMON
    initialise_monitor_handles();
#endif
    DenoiseState *st = (DenoiseState *)ns_state;
    ns_res[0] = (uint32_t)rnnoise_get_size();
    ns_res[1] = (uint32_t)rnnoise_get_frame_size();
    if (rnnoise_init(st, NULL) != 0) {
        ns_res[2] = 0xdead;
        return 1;
    }
    ns_res[2] = 1;

    for (int k = 0; k < NS_WARM; k++) {
        ns_load((unsigned)k);
        ns_sink += (int32_t)rnnoise_process_frame(st, ns_out, ns_in);
    }

    /* The loop the cost measurement is taken on: one marker pair per 10 ms. */
    for (int k = 0; k < NS_ITERS; k++) {
        ns_load((unsigned)(NS_WARM + k));
        frame_marker();
        float vad = rnnoise_process_frame(st, ns_out, ns_in);
        frame_marker();
        ns_sink += (int32_t)vad;
#if NS_DUMP
        for (int i = 0; i < NS_FRAME; i++)
            ns_keep[k][i] = ns_out[i];
#else
        ns_sink += (int32_t)ns_out[(NS_WARM + k) & (NS_FRAME - 1)];
#endif
    }
    ns_res[3] = (uint32_t)ns_sink;
    ns_res[4] = (uint32_t)NS_ITERS;

#if NS_DUMP
    for (int k = 0; k < NS_ITERS; k++) {
        printf("F%02d", k);
        for (int i = 0; i < NS_FRAME; i++) {
            union { float f; uint32_t u; } v;
            v.f = ns_keep[k][i];
            printf(" %08x", v.u);
        }
        printf("\n");
    }
#endif
    return 0;
}
