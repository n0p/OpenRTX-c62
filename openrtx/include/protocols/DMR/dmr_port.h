/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_PORT_H
#define DMR_PORT_H

/*
 * Platform shim of the DMR receiver, so the same demodulator and decoder
 * build in OpenRTX and in a plain host test (gcc, no OS). All DSP and
 * protocol work runs in one thread (the rtx thread in OpenRTX); only the
 * snapshots the UI reads cross thread boundaries, guarded by
 * DMR_LOCK()/DMR_UNLOCK().
 */

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#ifdef __ZEPHYR__
#include <pthread.h>

#define DMR_LOCK_DECLARE(name) \
    static pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define DMR_LOCK(name) pthread_mutex_lock(&(name))
#define DMR_UNLOCK(name) pthread_mutex_unlock(&(name))
#else
#define DMR_LOCK_DECLARE(name) static int name
#define DMR_LOCK(name) ((void)(name))
#define DMR_UNLOCK(name) ((void)(name))
#endif

/*
 * Large, rarely touched state (call log, vocoder state). On the C62 it goes to
 * PSRAM, in a section of its own (platform/targets/c62/psram_dmr.ld). It is
 * NOLOAD -- the loader neither copies nor zeroes it -- and platform_init
 * zeroes it once at boot, so it reads as zeros before any reset function has
 * run; every module still rearms its own state in its reset.
 *
 * Not .psram_section: that one also holds the CSK6 system heap, so it cannot
 * be zeroed.
 */
#ifdef PLATFORM_C62
#define EXT_RAM_BSS_ATTR __attribute__((section(".psram_dmr")))
#else
#define EXT_RAM_BSS_ATTR
#endif

/*
 * The vocoder's working memory (its state, its thread's stack), read and
 * written all through every frame: on the C62 in DTCM, the core's zero wait
 * state data memory, otherwise unused (16 KB). Zeroed at boot.
 */
#ifdef PLATFORM_C62
#define DMR_FAST_BSS __attribute__((section(".dtcm_bss")))
#else
#define DMR_FAST_BSS
#endif

/*
 * The part of the vocoder's working set that is filled and consumed inside one
 * frame decode -- the FFT/WOLA scratch -- and never inside a sample loop.  On
 * the C62 it goes to PSRAM, the same NOLOAD `.psram_section` the library's own
 * single instance uses (platform/targets/c62/CMakeLists.txt), so that DTCM
 * keeps only the state that has to be fast: 4 056 B out of a 16 KB region that
 * was at 96 %.  Nothing zeroes it, and nothing needs to: every field is
 * written before it is read.
 */
#ifdef PLATFORM_C62
#define DMR_SLOW_BSS __attribute__((section(".psram_section")))
#else
#define DMR_SLOW_BSS
#endif

/* Large buffers (the demodulator's sample ring) */
static inline void *dmr_port_calloc_large(size_t n, size_t sz)
{
    return calloc(n, sz);
}

/*
 * Short intervals, for the vocoder timing figures: a free running counter
 * (wraps, so only differences of nearby readings mean anything) and the
 * conversion of a difference to microseconds.
 */
#ifdef __ZEPHYR__
#include <zephyr/kernel.h>

static inline uint32_t dmr_port_cycles(void)
{
    return k_cycle_get_32();
}

static inline uint32_t dmr_port_cycles_to_us(uint32_t cycles)
{
    return k_cyc_to_us_floor32(cycles);
}

/* Cycles the calling thread has run itself, preemptions excluded; 0 when
 * the kernel does not count them */
static inline uint32_t dmr_port_thread_cycles(void)
{
#ifdef CONFIG_THREAD_RUNTIME_STATS
    k_thread_runtime_stats_t stats;
    if (k_thread_runtime_stats_get(k_current_get(), &stats) == 0)
        return (uint32_t)stats.execution_cycles;
#endif
    return 0;
}
#else
static inline uint32_t dmr_port_thread_cycles(void)
{
    return 0;
}

#ifdef DMR_PORT_NO_CLOCK
/* A bare-metal test with no wall clock (tests/host/dmr_pkg_qemu): the cost is
 * counted in instructions by the emulator, and the per-frame microsecond
 * figures of the log are not used. */
static inline uint32_t dmr_port_cycles(void)
{
    return 0;
}
#else
static inline uint32_t dmr_port_cycles(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000);
}
#endif

static inline uint32_t dmr_port_cycles_to_us(uint32_t cycles)
{
    return cycles;
}
#endif

#endif /* DMR_PORT_H */
