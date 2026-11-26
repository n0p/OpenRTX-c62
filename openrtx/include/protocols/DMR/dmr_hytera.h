/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_HYTERA_H
#define DMR_HYTERA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hytera voice privacy, receive only, with keys typed by hand. Nothing
 * here finds, tries or checks keys: without the key the call stays muted.
 * The schemes as dsd-neo has them (GPL-3.0-or-later, crypt-etc.c,
 * crypt-rc4.c, dmr_pi.c):
 *
 * Basic (the LC's feature set ID 0x68 with the privacy bit, no PI header):
 * a key of 40, 128 or 256 bits (10, 32 or 64 hex digits, PC setting
 * dmr.hbp, record C62H) repeated bit by bit over the 882 data bits of a
 * superframe; frame f (0..17) takes bits 49 f .. 49 f + 48. The AMBE+2
 * silence frame goes in clear.
 *
 * Enhanced (a PI header with feature set ID 0x68, algorithm 0x02, the key
 * ID and a 40-bit MI, its byte 9 minus the sum of bytes 0..8): the 40-bit
 * key from the Enhanced Privacy keyring (dmr_ep.h, by its key ID); RC4
 * keyed with the 5 key bytes, nothing dropped, 135 bytes, each XORed with
 * key[i % 5] ^ MI[i % 5]; frame f takes bits 49 f .. 49 f + 48. Each
 * superframe's MI: the bytes of the last one through a byte LFSR (taps
 * 0x12 0x24 0x48 0x22 0x14).
 */

/* Basic: the key as the settings keep it */
struct dmrHyBpStore {
    uint8_t len;  /* key bytes: 0 none, 5, 16 or 32 */
    uint8_t pad[3];
    uint8_t key[32];
};
void dmrHyBp_save(struct dmrHyBpStore *out);
bool dmrHyBp_restore(const struct dmrHyBpStore *in);
/** From the PC: 10, 32 or 64 hex digits, "cargada" (kept) or "" (none) */
bool dmrHyBp_set(const char *text);
/** For the PC: "" none, else "cargada" (never the key) */
int dmrHyBp_get(char *out, size_t n);
uint32_t dmrHyBp_changes(void);
bool dmrHyBp_present(void);
/** Deciphers frame f (0..17) of a superframe with the key set */
void dmrHyBp_frame(char ambe_d[49], unsigned f);

/* Enhanced: frame f's 49 keystream bits, packed MSB first in 7 bytes (as
 * dmrEp_frame() takes them) */
void dmrHyEp_bits(const uint8_t key[5], uint64_t mi, unsigned f,
                  uint8_t out[7]);
uint64_t dmrHyEp_nextMi(uint64_t mi);
/** The PI header's own check: byte 9 is minus the sum of bytes 0..8 */
bool dmrHyEp_piOk(const uint8_t pi[10]);

#ifdef __cplusplus
}
#endif

#endif /* DMR_HYTERA_H */
