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
 * ## Where the evidence comes from: the UART, not the GPIO
 *
 * The caller does not pass a pin level.  While the pad is muxed to UART2 RXD
 * the GPIO block's DATAIN does **not** follow the line -- measured on hardware
 * with the port-mode build described in docs/ACCESSORY.md, which read it low
 * from boot while the same pad was carrying the PC's frames -- so a level read
 * there says nothing about the switch.  What does come from the line is the
 * receiver's own status for each character it frames:
 *
 *  - a **framing error** (LSR.FE) means the stop bit was sampled low, i.e. the
 *    line was low from the start bit through the stop bit: 87 us at 115200;
 *  - a **break** (LSR.BI) means it stayed low through the whole character.
 *
 * Both are produced only by a line held down across a character time.  A clean
 * character -- 0x00 included, it has a stop bit -- produces neither, and an idle
 * line (high) produces no character at all.  So @p heldLow is "the receiver
 * framed at least one character that was a break or had no stop bit", sampled
 * at whatever rate the caller runs, and the 30 ms rule above is unchanged.
 * This also closes the old hole where a flood of 0x00 bytes could imitate a
 * press: zeroes are clean characters and never count.
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
 * @param heldLow: true when the receiver framed at least one character since
 *                 the previous sample that was a break or had no stop bit,
 *                 i.e. the pad was held low through a whole character time.
 *                 A clean character (0x00 included) is *not* this.
 * @param nowMs:   monotonic milliseconds.
 * @return true when the *declared* pressed state changed on this sample, so
 *         that the caller can act on edges (abort the frame parser on press,
 *         report the counts on release) exactly once.
 */
bool port_ptt_update(portPttState_t *s, bool heldLow, uint32_t nowMs);

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
