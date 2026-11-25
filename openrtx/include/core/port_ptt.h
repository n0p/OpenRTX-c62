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
 * 87 us (a 0x00 byte), a short break for a couple of milliseconds; a finger on
 * a switch holds it for tens of milliseconds at least.
 *
 * This module is the rule that tells them apart, as a pure function of
 * (level, time) so that it can be tested on a host without a radio:
 *
 *  - A low level is a press only when it lasts **more than**
 *    PORT_PTT_HOLD_MS.  A press is declared once, at that instant, and it is
 *    not re-declared while the level stays low.
 *  - A low level shorter than that is a glitch -- a byte, a break, a bouncing
 *    contact -- and it is counted, never keyed.
 *  - Releasing is immediate: the first sample that sees the line high ends the
 *    press, because a transmitter must stop when the operator lets go, not
 *    thirty milliseconds later.
 *
 * The caller feeds it with a monotonic millisecond clock; feeding it twice in
 * the same millisecond is harmless, which is what lets the protocol transport
 * (every 1 ms) and the key scan (every 30 ms) share one instance.
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
 * Strictly greater than this: 30 ms is 345 bytes of 0x00 at 115200 baud, so no
 * frame of the PC protocol can ever hold the line that long, while the shortest
 * human press is an order of magnitude longer.
 */
#define PORT_PTT_HOLD_MS 30

/** State of the detector.  Plain data: a caller may keep several. */
typedef struct
{
    uint32_t lowSince;    /**< tick of the first sample seen low, when low   */
    uint32_t pressSince;  /**< tick the current/last press really started    */
    uint32_t lastPressMs; /**< duration of the last completed press         */
    uint32_t lost;        /**< bytes/errors discarded while it was held     */
    uint16_t presses;     /**< presses declared                             */
    uint16_t glitches;    /**< short lows: bytes and breaks, not presses    */
    bool     low;         /**< the pad is low right now                     */
    bool     pressed;     /**< the sustained-low rule says it is pressed    */
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

/** Clear a detector: idle, line high, no counters. */
void port_ptt_reset(portPttState_t *s);

/**
 * Feed one sample of the pad.
 *
 * @param s:    detector.
 * @param low:  true when the pad reads low (the switch grounds it).
 * @param nowMs: monotonic milliseconds.
 * @return true when the *declared* pressed state changed on this sample, so
 *         that the caller can act on edges (abort the frame parser on press,
 *         report the counts on release) exactly once.
 */
bool port_ptt_update(portPttState_t *s, bool low, uint32_t nowMs);

/** True while the sustained-low rule says the switch is down. */
static inline bool port_ptt_pressed(const portPttState_t *s)
{
    return s->pressed;
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
 * @param raw:    gpio_pin_get_dt(&button_ptt), the logical level (NEVER only).
 */
bool port_ptt_keyed(uint8_t mode, bool detected, bool raw);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PORT_PTT_H */
