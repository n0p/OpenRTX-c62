/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STACK_BUDGET_H
#define STACK_BUDGET_H

/**
 * @file stack_budget.h
 * @brief One place for every thread stack that is not in core/threads.h, and a
 *        compile-time floor under each one.
 *
 * Two of the failures that cost this project the most time were both stack
 * overruns on a thread nobody had measured: the DMR voice thread at 4 KiB died
 * with the PC inside dmr_rfft256, and the rtx thread took the TETRA branch (4
 * KiB) in a build that also had the DMR transmitter (about 7.1 KiB).  A radio
 * that overruns its stack does not misbehave slowly: the first write past the
 * guard is a CONFIG_HW_STACK_PROTECTION fatal error, Zephyr halts, no log comes
 * out (the thread that writes it is gone), the PA stays as it was, and the
 * watchdog resets the radio seconds later.  From the outside that is "it
 * rebooted when I pressed PTT".
 *
 * The rule this file enforces is: a stack size is a named constant, the
 * constant has a floor under it, and lowering the constant below the floor is a
 * build error here -- on the radio and on the host (tests/unit/thread_stacks.cpp)
 * -- instead of a dead radio on the bench.  Every source that creates one of
 * these threads takes the size from here; tests/unit/platform_wiring.cpp fails
 * if one of them goes back to a literal of its own, because a local literal is
 * exactly how the floor gets bypassed.
 *
 * Where the numbers come from, in order of what is worth trusting:
 *
 *  - measured here, on the ARM they run on, with -fstack-usage: D-STAR speech
 *    (lib/dstar_vocoder, Cortex-M33, -Os: 2 736 B);
 *  - measured by the project when the thread was written, documented at the
 *    source: the DMR voice thread (7.1 KiB chain in core/threads.h), the codec
 *    worker (about 0.9 KiB, -fcallgraph-info=su) and the supervisor (516 B
 *    record plus the flash path, about 2 KiB);
 *  - measured on the host, -Os, with -fstack-usage (an x86-64 frame is wider
 *    than the same function compiled for Cortex-M33, so it is an upper bound):
 *    the PC protocol thread's dispatch chain, about 1.1 KiB;
 *  - not measured at all: the keypad scanner and the TETRA speech thread
 *    (libopenacelp is not in this tree). Their floor is the value the radio
 *    ships with, so the build refuses to go lower; the high-water mark that
 *    closes the question is the thread walk of the black box
 *    (docs/BLACKBOX.md: `free` per thread, `tools/cps/blackbox.py threads`),
 *    and it can only be read on the radio.
 */

#include <stddef.h>

/*
 * _Static_assert is C11 and the firmware builds as C99 (it takes it as an
 * extension, as DmrSpeech.c already relies on); C++ tools get static_assert.
 */
#if defined(__cplusplus)
#define STACK_BUDGET_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define STACK_BUDGET_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/*
 * ---------------------------------------------------------------------------
 * Keypad scanner (platform/drivers/keyboard/keyboard_c62.c, "kbd_thread")
 * ---------------------------------------------------------------------------
 *
 * A GPIO matrix read and one log line per change, every 20 ms.  Small, but not
 * free: Zephyr's logging copies a formatted line, and the deferred-mode path is
 * not the 40 B the call looks like.  No high-water mark has been read from the
 * radio for this thread, so the floor is what ships: no change may lower it.
 */
#define STACK_BUDGET_KBD          1024
#define STACK_BUDGET_KBD_MIN      1024

/*
 * ---------------------------------------------------------------------------
 * CSK6 codec worker (platform/drivers/audio/csk6_codec.c)
 * ---------------------------------------------------------------------------
 *
 * Holds the audio lock across the DSP's shared-memory calls for a route change
 * or a stream start.  Measured when it was written: about 0.9 KiB static worst
 * case with immediate-mode logging and an FPU exception frame
 * (-fcallgraph-info=su).  2 KiB is the value that ships; the floor keeps a
 * 0.6 KiB margin over the measurement, so a future "tidy-up" to 1 KiB -- which
 * would sit right on the worst case -- fails the build.
 *
 * CONFIG_C62_NOISE_SUPPRESS adds RNNoise to the same thread, inside
 * captureFrame(), and it is the deepest thing on it by an order of magnitude:
 * measured with -fstack-usage on the library as this tree builds it (Cortex-M33,
 * -O2 -fsingle-precision-constant), rnnoise_process_frame 2 832 B + the
 * compute_rnn frame it calls 3 632 B + compute_gru 1 592 B = 8 056 B, about
 * 8.9 KiB with the worker's own 0.9 KiB chain.  The DenoiseState itself is in
 * PSRAM (core/noise_suppress.c), so this is all that is left on the stack.
 * 12 KiB ships with the feature built in, which leaves ~3 KiB of margin for the
 * exception frame and the logging path; the floor at 10 KiB fails the build
 * rather than a radio that reboots the moment someone turns RX Noise on.  With
 * the option off the numbers are exactly the ones above: the image and the
 * stack are untouched.
 */
#ifdef CONFIG_C62_NOISE_SUPPRESS
#define STACK_BUDGET_CODEC_WORKER      12288
#define STACK_BUDGET_CODEC_WORKER_MIN  10240
#else
#define STACK_BUDGET_CODEC_WORKER      2048
#define STACK_BUDGET_CODEC_WORKER_MIN  1536
#endif

/*
 * ---------------------------------------------------------------------------
 * PC protocol transport (platform/targets/c62/pc_proto_c62.c, "pcproto")
 * ---------------------------------------------------------------------------
 *
 * One pass of the loop parses a frame and runs a handler: the TLV codecs, the
 * store handlers with their read-back verification, the bench capture copy and
 * the log ring.  Measured on the host with -Os and -fstack-usage, the deepest
 * chain (pc_proto_rx 160 + dispatch + the widest handler, pc_dbg_info 400, and
 * the store calls below it) is about 1.1 KiB; 4 KiB is what ships, so the floor
 * at 3 KiB still leaves nearly three times the measured chain.  A thread that
 * overruns here takes the cable with it, which is what makes a bench session
 * go silent in the middle of a measurement.
 */
#define STACK_BUDGET_PC_PROTO      4096
#define STACK_BUDGET_PC_PROTO_MIN  3072

/*
 * ---------------------------------------------------------------------------
 * Black box supervisor (platform/targets/c62/blackbox_c62.c, "bb_supervisor")
 * ---------------------------------------------------------------------------
 *
 * The only thread that cannot be waiting on anything the rest of the firmware
 * could be stuck behind, which is why it also feeds the watchdog.  It builds
 * the post-mortem record (516 B) and writes it to the flash sector; measured
 * worst case about 2 KiB, 3 KiB ships.  Sizing this one too small would remove
 * the last observer of a hang -- and the thread that writes the record is the
 * one whose death is silent.
 */
#define STACK_BUDGET_BB_SUPERVISOR      3072
#define STACK_BUDGET_BB_SUPERVISOR_MIN  2560

/*
 * ---------------------------------------------------------------------------
 * TETRA speech playback (openrtx/src/protocols/TETRA/TetraSpeech.c)
 * ---------------------------------------------------------------------------
 *
 * Decodes with libopenacelp (oa_dec_frame) into a static PCM buffer and pushes
 * it to the speaker.  The decoder is not in this tree, so the depth of
 * oa_dec_frame could not be measured here: the floor is the value the radio
 * ships with, and the high-water mark from the black box is what would raise
 * or (never) lower it.  The stack is malloc'd on Zephyr, so an overrun here is
 * a heap corruption, not a guard fault -- which is another reason the floor
 * exists: there is no CONFIG_HW_STACK_PROTECTION backstop on a malloc'd stack.
 */
#define STACK_BUDGET_TETRA_SPEECH      4096
#define STACK_BUDGET_TETRA_SPEECH_MIN  4096

/*
 * ---------------------------------------------------------------------------
 * D-STAR speech playback (openrtx/src/protocols/DSTAR/DStarSpeech.c)
 * ---------------------------------------------------------------------------
 *
 * Measured here, on Cortex-M33 with -Os:
 *
 *   dstarVocoder_decode 88 + dstar_decoder_frame 160 +
 *   parms_repeat_model 2456                                   = 2 704 B
 *
 * before the playback thread's own frames (the other branch under the same
 * frame is dmr_rfft256's 1 048 B through synthesize_unvoiced, so 2 704 is the
 * peak).  The shipped 4 KiB is about 1.5 times that; the floor at 3.5 KiB still
 * fails a build that goes back to 3 KiB, which would leave under 300 B over the
 * measured peak -- the same margin that killed the DMR voice thread at 4 KiB.
 */
#define STACK_BUDGET_DSTAR_SPEECH      4096
#define STACK_BUDGET_DSTAR_SPEECH_MIN  3584

/*
 * ---------------------------------------------------------------------------
 * DMR speech playback (openrtx/src/protocols/DMR/DmrSpeech.c)
 * ---------------------------------------------------------------------------
 *
 * Two builds, two numbers, both already documented at the source:
 *  - lib/dmr_vocoder (DMR_VOICE_OWN): 4 096 B overflowed with the PC inside
 *    dmr_rfft256, so the thread gets 7 KiB;
 *  - mbelib (DMR_VOICE_ENABLED): its synthesis keeps a few KB of arrays on the
 *    stack, so it gets 8 KiB.
 * The floors are the measured values: this is the thread that proved the rule,
 * and it must never be allowed to creep back down.
 */
#define STACK_BUDGET_DMR_SPEECH_OWN      7168
#define STACK_BUDGET_DMR_SPEECH_OWN_MIN  7168
#define STACK_BUDGET_DMR_SPEECH_MBELIB      8192
#define STACK_BUDGET_DMR_SPEECH_MBELIB_MIN  8192

/*
 * ---------------------------------------------------------------------------
 * P25 and YSF speech playback (P25Speech.c, YsfSpeech.c)
 * ---------------------------------------------------------------------------
 *
 * Both decode IMBE with lib/imbe_vocoder, measured here on Cortex-M33 -Os:
 *
 *   imbe_decoder_decode_ex 3 792 + render_spec_frame 1 008 +
 *   imbe_irfft256 1 072                                  = 5 872 B
 *
 * of the 8 KiB that ship (72 %).  The deepest frame is the decoder's own
 * fbuf[170]; a 7 KiB floor is the measurement plus a fifth and refuses the
 * "tidy it to 4 KiB" that reads harmless in a diff.
 */
#define STACK_BUDGET_IMBE_SPEECH      8192
#define STACK_BUDGET_IMBE_SPEECH_MIN  7168

/*
 * ---------------------------------------------------------------------------
 * NXDN and dPMR speech playback (NxdnSpeech.c, DpmrSpeech.c)
 * ---------------------------------------------------------------------------
 *
 * AMBE with lib/dmr_vocoder: dmr_vocoder_decode_frame 80 + decode_params
 * 1 536 + dmr_rfft256 1 048 = 2 664 B measured the same way.  8 KiB ships, so
 * the floor stays at half of it: these two are the threads with the most air
 * under them, not the ones to shave.
 */
#define STACK_BUDGET_AMBE_SPEECH      8192
#define STACK_BUDGET_AMBE_SPEECH_MIN  4096

/*
 * ---------------------------------------------------------------------------
 * Packet modem (openrtx/src/core/packet_service.c, "pkt")
 * ---------------------------------------------------------------------------
 *
 * The KISS TNC: a Zephyr thread with the frame assembler and the modem calls
 * on its stack.  Not measured off the radio, so the floor is what ships.
 */
#define STACK_BUDGET_PACKET      3072
#define STACK_BUDGET_PACKET_MIN  3072

/*
 * ---------------------------------------------------------------------------
 * Remote-control transport (platform/drivers/audio/c62_remote.c)
 * ---------------------------------------------------------------------------
 *
 * The CSK6 remote service: a Zephyr thread parsing frames from the other core.
 * Not measured off the radio, so the floor is what ships.
 */
#define STACK_BUDGET_REMOTE      2048
#define STACK_BUDGET_REMOTE_MIN  2048

/*
 * ---------------------------------------------------------------------------
 * Baseband dump sender (platform/drivers/audio/baseband_dump_c62.c)
 * ---------------------------------------------------------------------------
 *
 * Streams captured I/Q out of the UART.  A bring-up tool, but its thread has a
 * stack like any other: dropping it to a size that overflows turns a capture
 * session into a reset, and the capture is exactly what one is doing when the
 * radio goes away.
 */
#define STACK_BUDGET_DUMP_SENDER      1024
#define STACK_BUDGET_DUMP_SENDER_MIN  1024

/* ------------------------------------------------------------------------- */

STACK_BUDGET_ASSERT(STACK_BUDGET_KBD >= STACK_BUDGET_KBD_MIN,
                    "kbd_thread: the keypad scanner stack may not go down");
STACK_BUDGET_ASSERT(STACK_BUDGET_CODEC_WORKER >= STACK_BUDGET_CODEC_WORKER_MIN,
                    "csk6 codec worker: 0.9 KiB measured, keep the margin");
STACK_BUDGET_ASSERT(STACK_BUDGET_PC_PROTO >= STACK_BUDGET_PC_PROTO_MIN,
                    "pcproto: the dispatch chain needs about 1.1 KiB");
STACK_BUDGET_ASSERT(STACK_BUDGET_BB_SUPERVISOR >= STACK_BUDGET_BB_SUPERVISOR_MIN,
                    "bb_supervisor: record 516 B + flash path, about 2 KiB");
STACK_BUDGET_ASSERT(STACK_BUDGET_TETRA_SPEECH >= STACK_BUDGET_TETRA_SPEECH_MIN,
                    "TETRA speech: malloc'd stack, no guard to save it");
STACK_BUDGET_ASSERT(STACK_BUDGET_DSTAR_SPEECH >= STACK_BUDGET_DSTAR_SPEECH_MIN,
                    "D-STAR speech: 2 736 B measured in parms_repeat_model");
STACK_BUDGET_ASSERT(STACK_BUDGET_DMR_SPEECH_OWN >= STACK_BUDGET_DMR_SPEECH_OWN_MIN,
                    "DMR speech (own vocoder): 4 KiB overflowed in dmr_rfft256");
STACK_BUDGET_ASSERT(STACK_BUDGET_DMR_SPEECH_MBELIB >=
                        STACK_BUDGET_DMR_SPEECH_MBELIB_MIN,
                    "DMR speech (mbelib): its synthesis is not flat");
STACK_BUDGET_ASSERT(STACK_BUDGET_IMBE_SPEECH >= STACK_BUDGET_IMBE_SPEECH_MIN,
                    "P25/YSF speech: 5 872 B measured through imbe_irfft256");
STACK_BUDGET_ASSERT(STACK_BUDGET_AMBE_SPEECH >= STACK_BUDGET_AMBE_SPEECH_MIN,
                    "NXDN/dPMR speech: 2 664 B measured through dmr_rfft256");
STACK_BUDGET_ASSERT(STACK_BUDGET_PACKET >= STACK_BUDGET_PACKET_MIN,
                    "packet modem: the KISS thread may not go down");
STACK_BUDGET_ASSERT(STACK_BUDGET_REMOTE >= STACK_BUDGET_REMOTE_MIN,
                    "the remote transport thread may not go down");
STACK_BUDGET_ASSERT(STACK_BUDGET_DUMP_SENDER >= STACK_BUDGET_DUMP_SENDER_MIN,
                    "the baseband dump sender may not go down");

#endif /* STACK_BUDGET_H */
