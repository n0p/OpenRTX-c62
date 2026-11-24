/*
 * SPDX-License-Identifier: ISC
 *
 * Single DES, encryption only, for the radios' DES-OFB voice privacy
 * (des.c: dsd-neo's crypt-des.c, ISC licence, "Crypthings").
 */
#ifndef C62_DES_H
#define C62_DES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DES in output feedback mode: n bytes of keystream from the 8-byte key
 * and IV, the first `discard` blocks of 8 bytes dropped.
 */
void c62Des_ofb(const uint8_t key[8], const uint8_t iv[8], unsigned discard,
                uint8_t *out, unsigned n);

#ifdef __cplusplus
}
#endif

#endif
