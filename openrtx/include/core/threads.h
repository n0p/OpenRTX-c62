/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef THREADS_H
#define THREADS_H

#include <stddef.h>

/**
 * Threads' stack sizes
 */
#define UI_THREAD_STKSIZE     2048
#if defined(PLATFORM_C62) && defined(CONFIG_TETRA)
/*
 * The TETRA mode decodes in the rtx thread: libtetra needs up to about 1.5
 * KB (Viterbi, MAC, SDS), plus the event callback and newlib's snprintf.
 */
#define RTX_THREAD_STKSIZE    4096
#elif defined(PLATFORM_C62) && defined(CONFIG_DMR_TX)
/*
 * The DMR transmitter encodes in the rtx thread, and the AMBE+2 encoder is
 * deep: enc_ambe() 2288 B, e_p() 1696 B, pitch_est() 2112 B and
 * dmr_enc_analysis() 232 B are nested on top of the 816 B of one modulator
 * slot, 6.4 KB of frames measured with -fstack-usage.  Without this the first
 * encoded frame smashes the stack guard (CONFIG_HW_STACK_PROTECTION) and the
 * radio reboots as soon as the PTT is pressed.
 */
#define RTX_THREAD_STKSIZE    9216
#elif defined(PLATFORM_C62)
#define RTX_THREAD_STKSIZE    1024
#else
#define RTX_THREAD_STKSIZE    1024
#endif
#define CODEC2_THREAD_STKSIZE 16384
#define AUDIO_THREAD_STKSIZE  512

/**
 * Thread priority levels, UNIX-like: lower level, higher thread priority
 */
#ifdef _MIOSIX
#define THREAD_PRIO_RT      0
#define THREAD_PRIO_HIGH    1
#define THREAD_PRIO_NORMAL  2
#define THREAD_PRIO_LOW     3
#endif

/**
 * Spawn all the threads for the various functionalities.
 */
void create_threads();

#endif /* THREADS_H */
