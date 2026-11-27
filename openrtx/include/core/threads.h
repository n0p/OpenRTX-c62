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
#if defined(PLATFORM_C62)
/*
 * The UI thread draws every screen and calls into the audio routes for the
 * mode prompts; on the C62 it measured 1 552 B of 2 048 in use after a mode
 * change, and a post-mortem from the black box caught K_ERR_STACK_CHK_FAIL
 * (detail 2) in DMR at the moment the speaker route opened -- 496 B is not a
 * margin when a screen and a route are set up in the same pass.  Double it:
 * the radio has RAM to spare and a stack overflow is a dead radio.
 */
#define UI_THREAD_STKSIZE     4096
#else
#define UI_THREAD_STKSIZE     2048
#endif
/* The radio thread's stack.  Every mode sets up, receives and transmits in it,
 * and the deepest chain of the build decides the size, not the mode that
 * happens to be selected: the branches below used to be exclusive, so a build
 * with TETRA and DMR_TX at once (the bench image) took the first one and gave
 * the DMR transmitter 4 KiB. */
#if defined(PLATFORM_C62) && defined(CONFIG_DMR_TX)
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
#elif defined(PLATFORM_C62) && defined(CONFIG_TETRA)
/* libtetra's decode chain lives in this thread too. */
#define RTX_THREAD_STKSIZE    4096
#elif defined(PLATFORM_C62)
#define RTX_THREAD_STKSIZE    1024
#else
#define RTX_THREAD_STKSIZE    1024
#endif
/*
 * Codec2's thread stack, allocated from the heap by audio_codec.c.  The review
 * proposed 8 KiB (reports/codecs.md section 3.3) on a static frame chain of
 * 7 016 B, but the chain is deeper than its largest frame: measured with
 * tests/qemu/codec2_stack.c (painted stack, explicit stack switch, the same
 * code and flags), the decoder uses 15 484 B and the encoder 10 716 B.  Built
 * at -O2 instead of -Os the decode figure moves by 72 B, and on an 8 KiB stack
 * the same harness never returns.  So the reserve stays at 16 KiB: 900 B of
 * headroom is thin, but it is what this radio has been running with and the
 * alternative takes the space from the heap codec2's 37 KiB of state also
 * comes from.
 */
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
