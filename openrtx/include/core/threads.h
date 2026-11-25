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
/* The radio thread's stack.  Every mode sets up, receives and transmits in it,
 * and the deepest chain of the build decides the size, not the mode that
 * happens to be selected: the branches below used to be exclusive, so a build
 * with TETRA and DMR_TX at once (the bench image) took the first one and gave
 * the DMR transmitter 4 KiB. */
#if defined(PLATFORM_C62) && (defined(CONFIG_TETRA) || defined(CONFIG_DMR_TX))
/*
 * The DMR transmitter encodes in the rtx thread, and the AMBE+2 encoder is
 * deep.  Measured on the C62 build itself (-Os, Cortex-M33, -fstack-usage,
 * tests/host/dmr_tx_host/stack.sh):
 *
 *   update() 32 + txSlot() 64 + fillMic() 32 + dmrEnc_encode() 32 +
 *   dmr_encoder_encode() 16 + dmr_enc_frame() 112 + enc_ambe() 2280 +
 *   e_p() 1688 + pitch_est() 2112           = 6.4 KiB of frames
 *
 * on top of dmr_tx_voice()'s 520 B and the 816 B a modulator slot keeps
 * around them: about 7.1 KiB.  TETRA's decode chain is 1.5-2.5 KiB and fits in
 * the same block.
 *
 * Sizing this too small is not a slow transmitter, it is a dead radio: the
 * first frame written past the guard is a CONFIG_HW_STACK_PROTECTION fatal
 * error, and Zephyr halts.  No log comes out (the thread that writes it is
 * gone with the rest), the PA is left as it was, and the application watchdog
 * resets the radio a couple of seconds later -- which is exactly what a keyed
 * DMR transmission looked like from the outside.
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
