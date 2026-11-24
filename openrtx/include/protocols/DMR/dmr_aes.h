/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_AES_H
#define DMR_AES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AES-256, AES-128 and DES voice privacy with keys typed by hand (PC mode,
 * settings dmr.aes1 .. dmr.aes4; saved with the settings, record C62Z,
 * never read back): a keyring of DMR_AES_KEYS keys, each with the key ID
 * the call's header names; the key's length is the algorithm (8 bytes DES,
 * 16 AES-128, 32 AES-256). Nothing here finds, tries or checks keys: a
 * call whose key ID is not in the keyring, or whose key is of another
 * length than its algorithm's, stays muted. The same keyring serves DMR
 * (key IDs 1..255) and P25 (1..65535).
 *
 * DMR, as the DMR Association's scheme is read by DSD-FME, dsd-neo and
 * dmrgo, and by RadioFoundry's voice_aes.c (MIT, which this follows): the
 * PI header carries algorithm 0x25 AES-256, 0x24 AES-128 or 0x22 DES (0x05,
 * 0x04, 0x02 from other makes), Motorola's feature set ID 0x10, the key ID
 * and a 32-bit MI. The IV is the MI's 4 bytes then more bits of the LFSR
 * x^32 + x^22 + x^2 + x + 1 run on from it (16 bytes for AES, 8 for DES);
 * the cipher in OFB mode, its first block dropped, gives the superframe's
 * 126 bytes: frame f of the 18 takes bytes 7 f .. 7 f + 6, XORed onto its
 * 49 data bits packed MSB first. The next superframe's MI: 32 steps of
 * that LFSR.
 */

#define DMR_AES_KEYS 4
#define DMR_AES_KEY_BYTES 32
#define DMR_AES_SF_BYTES 126 /* keystream of a superframe, 7 per frame */
/* Key lengths, the algorithm */
#define DMR_KEY_DES 8u
#define DMR_KEY_AES128 16u
#define DMR_KEY_AES256 32u

/** One keyring entry as the settings keep it */
struct dmrAesKeyStore {
    uint8_t used; /* 1: key[] holds a key */
    uint8_t len;  /* its length: 8, 16, 32; 0 (older settings): 32 */
    uint16_t id;  /* the header's key ID */
    uint8_t key[DMR_AES_KEY_BYTES];
};

void dmrAes_save(struct dmrAesKeyStore out[DMR_AES_KEYS]);
bool dmrAes_restore(const struct dmrAesKeyStore in[DMR_AES_KEYS]);

/**
 * Entry i (0 .. DMR_AES_KEYS-1) from the PC: "<id>:<hex digits>", 16 for
 * DES, 32 for AES-128, 64 for AES-256 (spaces, ':' or '-' between the
 * digits allowed), "<id>:cargada" (keeps the key, changes its ID) or ""
 * (clears it). @return false if malformed.
 */
bool dmrAes_set(unsigned i, const char *text);
/** Entry i for the PC: "" when empty, else "<id>:cargada" (never the key) */
int dmrAes_get(unsigned i, char *out, size_t n);
/** Counts the changes by hand (for saving them) */
uint32_t dmrAes_changes(void);

/** The key with that key ID, copied whole, and its length. @return false
 * if none */
bool dmrAes_find(uint16_t id, uint8_t key[DMR_AES_KEY_BYTES], uint8_t *len);
/** Keyring entry i, copied whole, with its length. @return false if empty */
bool dmrAes_entry(unsigned i, uint16_t *id, uint8_t key[DMR_AES_KEY_BYTES],
                  uint8_t *len);
/** Entry i's key length only (0: empty), for the screen */
uint8_t dmrAes_lenOf(unsigned i);

/** The algorithm of a key length, and the length an algorithm wants (0:
 * none of these): DMR's PI header, P25's ALGID */
uint8_t dmrAes_dmrAlg(uint8_t len);
uint8_t dmrAes_dmrLen(uint8_t alg);
uint8_t dmrAes_p25Alg(uint8_t len);
uint8_t dmrAes_p25Len(uint8_t alg);

/** DMR: the MI of the next superframe */
uint32_t dmrAes_nextMi(uint32_t mi);
/**
 * DMR: n keystream bytes of the superframe with that MI, from byte skip
 * (frame f of the superframe: skip = 7 f), with a key of length len.
 */
void dmrAes_keystream(const uint8_t key[DMR_AES_KEY_BYTES], uint8_t len,
                      uint32_t mi, unsigned skip, uint8_t *out, unsigned n);

/*
 * P25 phase 1 (TIA-102.AAAD-A: ALGID 0x84 AES-256, 0x89 AES-128, 0x81
 * DES-OFB), as RadioFoundry's voice_aes.c, OP25 and dsd-neo do it: the IV
 * is the superframe's 64-bit MI (the first 8 bytes of the 72-bit MI field),
 * and for AES then the MI 64 steps on through the LFSR x^64 + x^62 + x^46
 * + x^38 + x^27 + x^15 + 1; OFB, the first block dropped, gives
 * DMR_AES_P25_BYTES. IMBE frame f (0..17: LDU1's 9, then LDU2's) takes 11
 * bytes from 11 + 11 f (+2 from frame 8, +2 more from frame 17), XORed bit
 * by bit onto its 88 data bits u0..u7 (12, 12, 12, 12, 11, 11, 11, 7 bits,
 * most significant first: mbelib's imbe_d order). The MI of each
 * superframe comes in the LDU2 before it.
 */
#define DMR_AES_P25_BYTES 213
#define DMR_AES_P25_FRAME_BYTES 11
void dmrAes_p25Keystream(const uint8_t key[DMR_AES_KEY_BYTES], uint8_t len,
                         uint64_t mi, uint8_t out[DMR_AES_P25_BYTES]);
/** P25: the MI of the next superframe (64 steps of the LFSR) */
uint64_t dmrAes_p25NextMi(uint64_t mi);
/** The 11 keystream bytes of IMBE frame f (0..17) of that superframe */
void dmrAes_p25Frame(const uint8_t ks[DMR_AES_P25_BYTES], unsigned f,
                     uint8_t out[DMR_AES_P25_FRAME_BYTES]);

/** Erase key material (a volatile write the compiler keeps) */
void dmrAes_wipe(void *p, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DMR_AES_H */
