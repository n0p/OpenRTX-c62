/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PORT_PTT_H
#define PORT_PTT_H

/**
 * @file port_ptt.h
 * @brief The accessory jack's PTT switch and the PC serial port: one pad.
 *
 * On the C62 the accessory connector and the programming connector are the
 * same jack, and its PTT contact is GPIOA_15, which is also UART2 RXD.  A
 * speaker-microphone's PTT switch grounds that pad, and so does every zero bit
 * the PC sends: the two things are electrically indistinguishable except in
 * *how long* the line stays low.  A byte at 115200 baud holds it low for about
 * 78 us (a 0x00 byte), a short break for a couple of milliseconds; a finger on
 * a switch holds it for tens of milliseconds at least.
 *
 * This module is the rule that tells them apart, as a pure function of
 * (line evidence, time) so that it can be tested on a host without a radio:
 *
 *  - A low level is a press only when it lasts **more than**
 *    PORT_PTT_HOLD_MS.  A press is declared once, at that instant, and it is
 *    not re-declared while the level stays low.
 *  - A low level shorter than that is a glitch -- a break, a bouncing contact,
 *    a bit of garbage -- and it is counted, never keyed.
 *  - Releasing is immediate: the first sample that does not see the line held
 *    ends the press, because a transmitter must stop when the operator lets go,
 *    not thirty milliseconds later.
 *  - **A press needs a release first.**  No press can be declared until the
 *    line has been seen released at least once since the detector was reset or
 *    the pad changed owner.  A line that reads low from the very first sample
 *    is *unknown*, not pressed: the same rule the key gate applies to edges --
 *    no release, no grant -- and the reason a radio that boots with a grounded
 *    pad neither keys itself nor throws away what the client sends.
 *
 * ## Where the evidence comes from: a window on the pad, not the UART status
 *
 * While the pad is muxed to UART2 RXD the GPIO block's DATAIN does **not**
 * follow the line -- measured on hardware with the port-mode build described in
 * docs/ACCESSORY.md, which read it low from boot while the same pad was
 * carrying the PC's frames -- so a level read there, with the pad still on the
 * UART, says nothing about the switch.  What does work is the pad itself: mux
 * it back to GPIO, read DATAIN, and mux it to the UART again, all inside a
 * window of about a microsecond (`c62_portPadSample()`, in the C62 platform).
 * The window is short on purpose: one bit at 115200 baud is 8.7 us, so a window
 * of 1 us takes the line away from the receiver for at most an eighth of a bit,
 * and only while the receiver is quiet (see below).
 *
 * A second source used to be the receiver's framing status, and the sustained
 * low it reports is *not* usable:
 *
 *  - a **framing error** (LSR.FE) means the stop bit was sampled low, i.e. the
 *    line was low from the start bit through the stop bit: 87 us at 115200;
 *  - a **break** (LSR.BI) means it stayed low through the whole character.
 *
 * Both are produced only by a line held down across a character time, and both
 * are what an operator's press produces.  The catch, measured on hardware, is
 * that a line clamped at zero makes the receiver frame *one* character and then
 * hand nothing more over -- it waits for the line to return high -- so the
 * 30 ms rule never accumulates and the radio does not key (docs/ACCESSORY.md,
 * section "Lo que se sabe de Auto").  A window read every four milliseconds
 * does accumulate, because it looks at the pad rather than at what the receiver
 * managed to frame.
 *
 * The status register is still worth reading, for the opposite reason: a
 * *clean* character (framing and break bits clear) proves the line was high
 * through its stop bit, i.e. that the line is not clamped.  That is the release
 * witness used while a client is talking, and it is also why the sampler
 * refuses to take a window then: the pad does not need to be looked at while
 * the receiver is busy framing, and leaving it alone costs the stream nothing.
 * One pass of clean characters is a *weak* high (port_ptt_seen_high(): it arms
 * the detector and ends a run that was not declared, but does not unkey);
 * PORT_PTT_STREAM_PASSES of them are a release.  Without the weak high a client
 * whose polls happen to land on every due pass would arm nothing and the switch
 * would never key.
 *
 * A clean character -- 0x00 included, it has a stop bit -- is neither, and an
 * idle line (high) produces no character at all.  The two sources are not
 * exclusive, and a build without the PC protocol (nobody muxes the pad, it
 * stays a plain GPIO) reads the level directly, as "Never" does.
 *
 * The caller feeds it with a monotonic millisecond clock; feeding it twice in
 * the same millisecond is harmless.  Only one context may feed it (the
 * transport thread, which owns the UART FIFO); everybody else may read
 * port_ptt_pressed() at any time.
 *
 * docs/ACCESSORY.md has the wiring, the three port modes and what the protocol
 * loses while a press holds the pad.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * How long the pad has to stay low before it is read as a press, milliseconds.
 *
 * Strictly greater than this: 30 ms is 345 characters of break at 115200 baud,
 * so no frame of the PC protocol can ever hold the line that long, while the
 * shortest human press is an order of magnitude longer.
 */
#define PORT_PTT_HOLD_MS 30

/**
 * How often the pad is taken away from the UART to be read as a GPIO, ms.
 *
 * The sampling period has three jobs and each one bounds it:
 *
 *  - it must be far shorter than PORT_PTT_HOLD_MS, or a 30 ms low only gets a
 *    handful of samples and the declared press lands late.  At 4 ms the low is
 *    sampled 0, 4, 8 ... 32 ms after its edge, so the press is declared between
 *    32 and 36 ms after the finger -- the threshold rounded up to a sample, plus
 *    up to one period of phase: never more than HOLD + 2 * PERIOD;
 *  - it is also the shortest low the detector can see at all, so it doubles as
 *    a break rejector: a 1-3 ms break (a host hanging up) can cover at most one
 *    sample 4 ms from its neighbour and can never be mistaken for a held line;
 *  - it multiplies the price of the window.  One window per 4 ms at 1 us is a
 *    duty cycle of 1/4000, and the sampler takes no window at all while the
 *    receiver is framing characters (see port_ptt_sample_step()).
 */
#define PORT_PTT_SAMPLE_PERIOD_MS 4

/**
 * The longest a window may keep the pad off the UART, microseconds.
 *
 * The design bound, not a measurement: the window is two register writes, a
 * couple of reads and a fixed handful of NOPs, and the C62 build measures what
 * it really costs (c62_portPadSample() reports the cycle count, and the boot
 * log prints it).  One bit at 115200 baud is 8.7 us, so this bound is a tenth
 * of a bit: the receiver can lose a bit it was sampling at most, never a
 * character's worth of framing.
 */
#define PORT_PTT_SAMPLE_WINDOW_MAX_US 1

/**
 * Passes of a live stream before its stop bits are believed, as the release.
 *
 * A clean character proves the line was high through its stop bit, so a stream
 * of them is a release witness.  One is not enough: a window taken while the
 * line is clamped can be the very thing that lets the receiver frame one
 * clean-looking character, and believing that single character would unkey a
 * transmitter the operator is still holding.  Two passes cannot be faked that
 * way -- the first clean character stops the sampler from taking the next
 * window, so a second one can only come from the wire.
 */
#define PORT_PTT_STREAM_PASSES 2

/** State of the detector.  Plain data: a caller may keep several. */
typedef struct
{
    uint32_t lowSince;    /**< tick of the first sample seen held, when held  */
    uint32_t pressSince;  /**< tick the current/last press really started    */
    uint32_t lastPressMs; /**< duration of the last completed press          */
    uint32_t lost;        /**< bytes/errors discarded while it was held      */
    uint16_t presses;     /**< presses declared                              */
    uint16_t glitches;    /**< short lows: breaks and bounces, not presses   */
    bool     low;         /**< the line was held through a whole character   */
    bool     pressed;     /**< the sustained-low rule says it is pressed     */
    bool     armed;       /**< the line has been seen released since reset   */
    bool     unknown;     /**< held, but never released: not a press yet     */
}
portPttState_t;

/** Where the pad is routed and who owns the PTT, for one port mode. */
typedef struct
{
    bool padIsUart;   /**< muxed to UART2 RXD (the protocol's RX)          */
    bool useProtocol; /**< the transport may read, dispatch and answer     */
    bool useDetector; /**< the sustained-low rule owns the PTT switch      */
    bool useButton;   /**< the raw level is the PTT: the pad is pure GPIO  */
}
portPttPlan_t;

/** Clear a detector: idle, no line seen yet, no counters, not armed. */
void port_ptt_reset(portPttState_t *s);

/**
 * Whether one framed character is evidence that the pad was held low.
 *
 * The whole classification of the line lives here, so that it can be tested:
 * a character counts only when the receiver flagged it as a break or sampled
 * its stop bit low.  The value of the byte never enters the decision -- a 0x00
 * is a start bit, eight zeroes and a stop bit, and it frames cleanly.
 *
 * @param noStopBit: the character was framed with a framing error (LSR.FE).
 * @param breakSeen: the character was a break (LSR.BI).
 */
static inline bool port_ptt_held_char(bool noStopBit, bool breakSeen)
{
    return noStopBit || breakSeen;
}

/**
 * Feed one sample of the line.
 *
 * @param s:       detector.
 * @param heldLow: true when the pad was seen low: a window read it grounded
 *                 ("Auto"), the switch level read directly (a build without the
 *                 transport), or the receiver framed a break or a character
 *                 without a stop bit.  A clean character (0x00 included) is
 *                 *not* this.
 * @param nowMs:   monotonic milliseconds.
 * @return true when the *declared* pressed state changed on this sample, so
 *         that the caller can act on edges (abort the frame parser on press,
 *         report the counts on release) exactly once.
 */
bool port_ptt_update(portPttState_t *s, bool heldLow, uint32_t nowMs);

/**
 * Feed one *weak* high: the line was up, but only for one pass.
 *
 * A clean character carries its stop bit, so the line was high through it --
 * but a window taken over a clamped line can be the very thing that lets the
 * receiver frame one clean-looking character, and unkeying on that would drop a
 * press the operator is still holding.  So this arms the detector and ends a
 * low run that has not been declared yet (the line is demonstrably not clamped
 * now), and leaves a declared press alone: only a second consecutive pass of
 * clean characters, or a window that reads the pad high, ends a press
 * (port_ptt_sample_step()'s PORT_PTT_STEP_RELEASE).
 *
 * Without this, a client whose polls happen to land on every due pass would
 * starve the detector of release evidence: the sampler never takes a window, the
 * detector is never armed, and the switch would never key -- which is what the
 * host test found.
 */
void port_ptt_seen_high(portPttState_t *s);

/**
 * Tell the detector that the line was *not* observed between two samples.
 *
 * Two lows seen on either side of a gap are not one long low: a window that is
 * due but not taken (the receiver is busy framing, the pad read was unstable)
 * leaves the line unobserved, and without this the 30 ms rule would count the
 * gap as held time and declare a press out of two unrelated samples.
 *
 * A press that was already declared is left alone: the only thing that ends it
 * is seeing the line released.  Dropping a not-yet-declared run is not a
 * glitch, so it is not counted as one.
 */
void port_ptt_unobserved(portPttState_t *s);

/** What the sampler decided for one pass of the transport loop. */
typedef enum
{
    PORT_PTT_STEP_IDLE = 0, /**< no window due: keep the run, do nothing      */
    PORT_PTT_STEP_TAKE,     /**< due and the line is quiet: take the window    */
    PORT_PTT_STEP_WAIT,     /**< due but a clean character arrived in this
                             *   pass: the first pass of a stream, feed it as a
                             *   weak high (port_ptt_seen_high)              */
    PORT_PTT_STEP_RELEASE   /**< the stream has run PORT_PTT_STREAM_PASSES
                             *   passes: feed the line as released           */
} portPttStep_t;

/**
 * The pacing of the windows and the gate that decides whether to take one.
 *
 * Kept out of the transport, and out of the hardware, so that the arithmetic
 * that makes "Auto" cheap can be tested on a host: how often a window is due,
 * what a stalled loop does (one window, not a burst), and when a live stream
 * suppresses it.
 */
typedef struct
{
    uint32_t nextMs;   /**< tick the next window is due at                    */
    uint16_t samples;  /**< windows taken                                     */
    uint16_t skipped;  /**< windows not taken because a stream was live       */
    uint16_t unstable; /**< windows whose two reads disagreed                 */
    uint8_t  stream;   /**< consecutive passes that framed a clean character  */
    bool     started;  /**< the schedule has been set                         */
} portPttSampler_t;

/** Restart the schedule: the next pass is due, no stream seen yet. */
void port_ptt_sampler_reset(portPttSampler_t *s, uint32_t nowMs);

/**
 * Decide what this pass does with the pad.
 *
 * @param s:         sampler.
 * @param cleanChar: true when the receiver framed at least one *clean*
 *                   character since the previous pass -- a character with its
 *                   stop bit, which proves the line was not clamped.
 * @param nowMs:     monotonic milliseconds (the same clock the detector gets).
 * @return PORT_PTT_STEP_TAKE at most once per PORT_PTT_SAMPLE_PERIOD_MS, and
 *         never on a pass that framed a clean character.
 *
 * A window is taken on the first pass, then every PORT_PTT_SAMPLE_PERIOD_MS.
 * A loop that stalls for longer does not catch up with a burst of windows: the
 * next one is due a period after the pass that took it, because a burst would
 * multiply the price without telling the 30 ms rule anything new.
 */
portPttStep_t port_ptt_sample_step(portPttSampler_t *s, bool cleanChar,
                                   uint32_t nowMs);


/** True while the sustained-low rule says the switch is down. */
static inline bool port_ptt_pressed(const portPttState_t *s)
{
    return s->pressed;
}

/**
 * True once the line has been seen released since the last reset.
 *
 * While it is false the line is low and the detector refuses to call that a
 * press: that level was there before anybody could have pressed anything.
 */
static inline bool port_ptt_armed(const portPttState_t *s)
{
    return s->armed;
}

/** Count bytes (or UART errors) discarded because the switch held the pad. */
static inline void port_ptt_noteLost(portPttState_t *s, uint32_t n)
{
    s->lost += n;
}

/**
 * What a port mode means for the pad, the protocol and the PTT.
 *
 * Pure, so a host test can walk the three modes and the menu, the platform and
 * the transport all read the same table instead of each keeping its own idea
 * of what "Always" means.
 */
portPttPlan_t port_ptt_plan(uint8_t mode);

/**
 * The PTT level the transmitter should see.
 *
 * @param mode:   enum portMode.
 * @param detected: port_ptt_pressed() result (AUTO only).
 * @param raw:    the pad read as a GPIO, true when grounded (NEVER only).
 */
bool port_ptt_keyed(uint8_t mode, bool detected, bool raw);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PORT_PTT_H */
