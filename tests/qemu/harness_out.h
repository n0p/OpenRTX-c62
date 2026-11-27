/* SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Console output for the QEMU cost harnesses, with no printf.
 *
 * newlib's printf goes through semihosting SYS_WRITE (0x05), which this QEMU
 * (11.1.3) drops often enough that the summary line of a run went missing -
 * and a missing hash line turns "the two builds agree" into a false alarm.
 * SYS_WRITE0 (0x04) is not dropped, and it costs a few instructions instead of
 * dragging newlib's formatter into the image.  On the host the same helpers
 * write to stdout, so one harness source serves both builds.
 */
#ifndef QEMU_HARNESS_OUT_H
#define QEMU_HARNESS_OUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __arm__

static inline void hout_str(const char *s)
{
    register int r0 __asm__("r0") = 0x04; /* SYS_WRITE0 */
    register const char *r1 __asm__("r1") = s;

    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
}

#else

#include <stdio.h>

static inline void hout_str(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

#endif /* __arm__ */

/** Append an unsigned decimal number, no padding. */
static inline char *hout_dec(char *p, uint32_t v)
{
    char tmp[11];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);

    while (n > 0)
        *p++ = tmp[--n];

    return p;
}

/** Append a signed 64-bit number. */
static inline char *hout_i64(char *p, int64_t v)
{
    uint64_t mag;

    if (v < 0) {
        *p++ = '-';
        mag = (uint64_t)(-(v + 1)) + 1u;
    } else {
        mag = (uint64_t)v;
    }

    char tmp[20];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (int)(mag % 10u));
        mag /= 10u;
    } while (mag != 0u);

    while (n > 0)
        *p++ = tmp[--n];

    return p;
}

/** Append 8 hex digits. */
static inline char *hout_hex32(char *p, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";

    for (int shift = 28; shift >= 0; shift -= 4)
        *p++ = digits[(v >> shift) & 0xfu];

    return p;
}

/** Append a literal. */
static inline char *hout_lit(char *p, const char *s)
{
    while (*s != '\0')
        *p++ = *s++;

    return p;
}

#endif /* QEMU_HARNESS_OUT_H */
