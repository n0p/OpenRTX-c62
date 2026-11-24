/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 * SPDX-FileCopyrightText: 2009 Pavel Yazev
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_enc_basic.h
 * @brief The ETSI/AMR fixed-point primitives the AMBE+2 encoder is written in.
 *
 * These are the same operators as `encoder/basicop2.cc` of OpenDMR (which is
 * in turn the ETSI 3GPP TS 26.073 "basic operators" file, GPL): saturating
 * add/sub, `mult` with truncation, `L_mult` with the <<1 that makes it Q31,
 * shift with saturation and the rounding variants.
 *
 * The difference with the reference is that they are `static inline` in a
 * header instead of external functions in their own translation unit.  On the
 * Cortex-M33 that was the single most expensive thing in the encoder: 51 % of
 * the 4.19 M instructions per frame were calls to these, each with a stack
 * frame and its own `Overflow`/`Carry` stores.  Inlining them, and keeping the
 * whole analysis in one translation unit so that nothing else crosses a call
 * either, is what `-flto` was doing for the C++ build.
 *
 * The two sticky flags (`Overflow`, `Carry`) are part of the contract: the
 * ETSI operators set them and `L_sat()` reads them.  They are file scope here

 * single-threaded by construction (one frame at a time, from one task).
 */

#ifndef DMR_ENC_BASIC_H
#define DMR_ENC_BASIC_H

#include <stdint.h>

typedef int16_t Word16;
typedef int32_t Word32;
typedef uint16_t UWord16;
typedef uint32_t UWord32;
typedef int Flag;

#define MAX_32 ((Word32)0x7fffffffL)
#define MIN_32 ((Word32)(int32_t)0x80000000L)
#define MAX_16 ((Word16)0x7fff)
#define MIN_16 ((Word16)(int16_t)0x8000)

/* One pair per translation unit and only the analysis uses them; `static`
 * keeps the encoder from exporting two symbols named Overflow and Carry. */
static Flag Overflow __attribute__((unused));
static Flag Carry __attribute__((unused));

static inline Word16 extract_l(Word32 L_var1)
{
    return (Word16)L_var1;
}

static inline Word16 extract_h(Word32 L_var1)
{
    return (Word16)((UWord32)L_var1 >> 16);
}

static inline Word16 saturate(Word32 L_var1)
{
    if (L_var1 > (Word32)0x00007fffL) {
        Overflow = 1;
        return MAX_16;
    }
    if (L_var1 < (Word32)0xffff8000L) {
        Overflow = 1;
        return MIN_16;
    }
    return extract_l(L_var1);
}

static inline Word16 add(Word16 var1, Word16 var2)
{
    return saturate((Word32)var1 + var2);
}

static inline Word16 sub(Word16 var1, Word16 var2)
{
    return saturate((Word32)var1 - var2);
}

static inline Word16 abs_s(Word16 var1)
{
    if (var1 == MIN_16)
        return MAX_16;
    return (Word16)(var1 < 0 ? -var1 : var1);
}

static inline Word16 negate(Word16 var1)
{
    return (var1 == MIN_16) ? MAX_16 : (Word16)-var1;
}

/** Saturating left shift (the ETSI `shl`), also used by `shr` for negatives. */
static inline Word16 shl(Word16 var1, Word16 var2)
{
    if (var2 < 0) {
        if (var2 < -16)
            return (Word16)(var1 < 0 ? -1 : 0);
        return (Word16)(var1 >> (-var2));
    }
    if (var2 > 15) {
        if (var1 == 0)
            return 0;
        Overflow = 1;
        return (Word16)(var1 > 0 ? MAX_16 : MIN_16);
    }
    Word32 result = (Word32)var1 << var2;
    if (result != (Word32)(Word16)result) {
        Overflow = 1;
        return (Word16)(var1 > 0 ? MAX_16 : MIN_16);
    }
    return extract_l(result);
}

/* Arithmetic (sign-preserving) shift right, as the ETSI file defines it. */
static inline Word16 shr(Word16 var1, Word16 var2)
{
    if (var2 < 0) {
        if (var2 < -16)
            var2 = -16;
        return shl(var1, (Word16)-var2);
    }
    if (var2 >= 15)
        return (Word16)(var1 < 0 ? -1 : 0);
    return (Word16)(var1 >> var2); /* >> on int16 promotes to int: arithmetic */
}

/** Q15 x Q15 -> Q15, truncating (the ETSI `mult`). */
static inline Word16 mult(Word16 var1, Word16 var2)
{
    Word32 L = (Word32)var1 * (Word32)var2;
    L = (Word32)(((UWord32)L & 0xffff8000U) >> 15);
    if (L & 0x00010000L)
        L |= (Word32)0xffff0000L;
    return saturate(L);
}

/** Q15 x Q15 -> Q31 with the ETSI doubling (so the product of 1.0*1.0 saturates). */
static inline Word32 L_mult(Word16 var1, Word16 var2)
{
    Word32 L = (Word32)var1 * (Word32)var2;
    if (L != (Word32)0x40000000L)
        return L * 2;
    Overflow = 1;
    return MAX_32;
}

/** Q15 x Q15 -> Q15, rounding. */
static inline Word16 mult_r(Word16 var1, Word16 var2)
{
    Word32 L = (Word32)var1 * (Word32)var2;
    L += (Word32)0x00004000L;
    L = (Word32)(((UWord32)L & 0xffff8000U) >> 15);
    if (L & 0x00010000L)
        L |= (Word32)0xffff0000L;
    return saturate(L);
}

static inline Word32 L_add(Word32 L_var1, Word32 L_var2)
{
    Word32 L = (Word32)((UWord32)L_var1 + (UWord32)L_var2);
    if (((L_var1 ^ L_var2) & MIN_32) == 0) {
        if ((L ^ L_var1) & MIN_32) {
            L = (L_var1 < 0) ? MIN_32 : MAX_32;
            Overflow = 1;
        }
    }
    return L;
}

static inline Word32 L_sub(Word32 L_var1, Word32 L_var2)
{
    Word32 L = (Word32)((UWord32)L_var1 - (UWord32)L_var2);
    if (((L_var1 ^ L_var2) & MIN_32) != 0) {
        if ((L ^ L_var1) & MIN_32) {
            L = (L_var1 < 0) ? MIN_32 : MAX_32;
            Overflow = 1;
        }
    }
    return L;
}

/** Add with carry, as the ETSI file: sets Carry for the next call. */
static inline Word32 L_add_c(Word32 L_var1, Word32 L_var2)
{
    Word32 L_test = (Word32)((UWord32)L_var1 + (UWord32)L_var2);
    Word32 L = (Word32)((UWord32)L_test + (UWord32)Carry);
    Flag carry_int = 0;

    if ((L_var1 > 0) && (L_var2 > 0) && (L_test < 0)) {
        Overflow = 1;
    } else if ((L_var1 < 0) && (L_var2 < 0)) {
        if (L_test >= 0) {
            Overflow = 1;
            carry_int = 1;
        } else {
            Overflow = 0;
            carry_int = 1;
        }
    } else if (((L_var1 ^ L_var2) < 0) && (L_test >= 0)) {
        Overflow = 0;
        carry_int = 1;
    } else {
        Overflow = 0;
    }

    if (Carry) {
        if (L_test == MAX_32) {
            Overflow = 1;
            Carry = carry_int;
        } else if (L_test == (Word32)0xFFFFFFFFL) {
            Carry = 1;
        } else {
            Carry = carry_int;
        }
    } else {
        Carry = carry_int;
    }
    return L;
}

static inline Word32 L_sub_c(Word32 L_var1, Word32 L_var2)
{
    Word32 L, L_test;
    Flag carry_int = 0;

    if (Carry) {
        Carry = 0;
        if (L_var2 != MIN_32) {
            return L_add_c(L_var1, -L_var2);
        }
        L = (Word32)((UWord32)L_var1 - (UWord32)L_var2);
        if (L_var1 > 0) {
            Overflow = 1;
            Carry = 0;
        }
        return L;
    }

    L = (Word32)((UWord32)L_var1 - (UWord32)L_var2 - 1U);
    L_test = (Word32)((UWord32)L_var1 - (UWord32)L_var2);
    if ((L_test < 0) && (L_var1 > 0) && (L_var2 < 0)) {
        Overflow = 1;
    } else if ((L_test > 0) && (L_var1 < 0) && (L_var2 > 0)) {
        Overflow = 1;
        carry_int = 1;
    } else if ((L_test > 0) && ((L_var1 ^ L_var2) > 0)) {
        Overflow = 0;
        carry_int = 1;
    }
    if (L_test == MIN_32) {
        Overflow = 1;
        Carry = carry_int;
    } else {
        Carry = carry_int;
    }
    return L;
}

static inline Word32 L_negate(Word32 L_var1)
{
    return (L_var1 == MIN_32) ? MAX_32 : -L_var1;
}

static inline Word32 L_deposit_h(Word16 var1)
{
    return (Word32)var1 << 16;
}

static inline Word32 L_deposit_l(Word16 var1)
{
    return (Word32)var1;
}

static inline Word32 L_shl(Word32 L_var1, Word16 var2);

static inline Word32 L_shr(Word32 L_var1, Word16 var2)
{
    if (var2 < 0)
        return L_shl(L_var1, (Word16)(var2 < -32 ? 32 : -var2));
    if (var2 >= 31)
        return (Word32)(L_var1 < 0 ? -1 : 0);
    return (Word32)(L_var1 >> var2);
}

static inline Word32 L_shl(Word32 L_var1, Word16 var2)
{
    if (var2 <= 0)
        return L_shr(L_var1, (Word16)-var2);

    Word32 L = 0;
    for (; var2 > 0; var2--) {
        if (L_var1 > (Word32)0x3fffffffL) {
            Overflow = 1;
            return MAX_32;
        }
        if (L_var1 < (Word32)0xc0000000L) {
            Overflow = 1;
            return MIN_32;
        }
        L_var1 = (Word32)((UWord32)L_var1 << 1);
        L = L_var1;
    }
    return L;
}

static inline Word16 shr_r(Word16 var1, Word16 var2)
{
    if (var2 > 15)
        return 0;
    Word16 out = shr(var1, var2);
    if (var2 > 0 && (var1 & ((Word16)1 << (var2 - 1))) != 0)
        out++;
    return out;
}

static inline Word32 L_shr_r(Word32 L_var1, Word16 var2)
{
    if (var2 > 31)
        return 0;
    Word32 out = L_shr(L_var1, var2);
    if (var2 > 0 && (L_var1 & ((Word32)1 << (var2 - 1))) != 0)
        out++;
    return out;
}

static inline Word16 round_h(Word32 L_var1)
{
    return extract_h(L_add(L_var1, (Word32)0x00008000L));
}

static inline Word32 L_abs(Word32 L_var1)
{
    if (L_var1 == MIN_32)
        return MAX_32;
    return (L_var1 < 0) ? -L_var1 : L_var1;
}

static inline Word32 L_sat(Word32 L_var1)
{
    Word32 out = L_var1;
    if (Overflow) {
        out = Carry ? MIN_32 : MAX_32;
        Carry = 0;
        Overflow = 0;
    }
    return out;
}

static inline Word32 L_mac(Word32 L_var3, Word16 var1, Word16 var2)
{
    return L_add(L_var3, L_mult(var1, var2));
}

static inline Word32 L_msu(Word32 L_var3, Word16 var1, Word16 var2)
{
    return L_sub(L_var3, L_mult(var1, var2));
}

static inline Word32 L_macNs(Word32 L_var3, Word16 var1, Word16 var2)
{
    return L_add_c(L_var3, L_mult(var1, var2));
}

static inline Word32 L_msuNs(Word32 L_var3, Word16 var1, Word16 var2)
{
    return L_sub_c(L_var3, L_mult(var1, var2));
}

static inline Word16 mac_r(Word32 L_var3, Word16 var1, Word16 var2)
{
    return extract_h(L_add(L_mac(L_var3, var1, var2), (Word32)0x00008000L));
}

static inline Word16 msu_r(Word32 L_var3, Word16 var1, Word16 var2)
{
    return extract_h(L_add(L_msu(L_var3, var1, var2), (Word32)0x00008000L));
}

static inline Word16 norm_s(Word16 var1)
{
    if (var1 == 0)
        return 0;
    if (var1 == (Word16)0xffff)
        return 15;
    Word16 v = var1;
    if (v < 0)
        v = (Word16)~v;
    Word16 out;
    for (out = 0; v < 0x4000; out++)
        v = (Word16)((UWord32)v << 1);
    return out;
}

static inline Word16 norm_l(Word32 L_var1)
{
    if (L_var1 == 0)
        return 0;
    if (L_var1 == (Word32)0xffffffffL)
        return 31;
    Word32 v = L_var1;
    if (v < 0)
        v = ~v;
    Word16 out;
    for (out = 0; v < (Word32)0x40000000L; out++)
        v = (Word32)((UWord32)v << 1);
    return out;
}

/**
 * 15-step restoring division, Q15 result.  The ETSI version aborts the process
 * on a bad argument; here the precondition (0 <= var1 <= var2, var2 > 0) is
 * documented and a violation returns MAX_16 instead of taking the radio down.
 */
static inline Word16 div_s(Word16 var1, Word16 var2)
{
    if (var2 == 0 || var1 < 0 || var2 < 0 || var1 > var2)
        return MAX_16;
    if (var1 == 0)
        return 0;
    if (var1 == var2)
        return MAX_16;

    Word16 out = 0;
    Word32 L_num = var1;
    Word32 L_den = var2;
    for (Word16 it = 0; it < 15; it++) {
        out = (Word16)((UWord32)out << 1);
        L_num = (Word32)((UWord32)L_num << 1);
        if (L_num >= L_den) {
            L_num = L_sub(L_num, L_den);
            out = add(out, 1);
        }
    }
    return out;
}

#endif /* DMR_ENC_BASIC_H */
