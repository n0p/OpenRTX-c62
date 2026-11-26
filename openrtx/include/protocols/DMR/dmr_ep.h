/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_EP_H
#define DMR_EP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Motorola Enhanced Privacy (ARC4, 40-bit keys) for DMR voice, with keys
 * typed by hand for laboratory tests of one's own transmitter: a keyring of
 * DMR_EP_KEYS keys, each with the key ID the PI header names (PC mode,
 * settings dmr.ep1 .. dmr.ep8; saved with the settings, record C62E, never
 * read back). Nothing here finds, tries or checks keys: a call whose key ID
 * is not in the keyring stays muted.
 *
 * The scheme, as DSD-FME decodes it (dsd_mbe.c, dmr_pi.c, dmr_le.c, ISC
 * licence): the PI header before the voice carries the algorithm (0x21,
 * ARC4), Motorola's feature set ID 0x10, the key ID and a 32-bit message
 * indicator (MI). ARC4 is keyed with the 5 key bytes followed by the 4 MI
 * bytes, its first 256 output bytes are dropped, and every voice frame of a
 * superframe (18 frames, bursts A..F) takes the next 7 bytes, XORed onto its
 * 49 data bits packed MSB first. Each new superframe starts again with the
 * next MI, the MI through 32 steps of the LFSR x^32 + x^4 + x^2 + 1.
 */

#define DMR_EP_KEYS 8
#define DMR_EP_KEY_BYTES 5
#define DMR_EP_FRAME_BYTES 7 /* keystream bytes per voice frame */
#define DMR_EP_DROP 256      /* ARC4 output dropped at each superframe */

/** One keyring entry as the settings keep it */
struct dmrEpKeyStore {
    uint8_t used; /* 1: key[] holds a key */
    uint8_t id;   /* the PI header's key ID */
    uint8_t key[DMR_EP_KEY_BYTES];
    uint8_t pad;
};

/** The keyring now, for saving */
void dmrEp_save(struct dmrEpKeyStore out[DMR_EP_KEYS]);

/** The keyring saved, at power on. @return false (and nothing loaded) if bad */
bool dmrEp_restore(const struct dmrEpKeyStore in[DMR_EP_KEYS]);

/**
 * Entry i (0 .. DMR_EP_KEYS-1) from the PC: "<id>:<10 hex digits>" (spaces,
 * ':' or '-' between the digits allowed), "<id>:cargada" (keeps the key,
 * changes its ID) or "" (clears it). @return false if malformed.
 */
bool dmrEp_set(unsigned i, const char *text);

/** Entry i for the PC: "" when empty, else "<id>:cargada" (never the key) */
int dmrEp_get(unsigned i, char *out, size_t n);

/** Counts the changes by hand (for saving them) */
uint32_t dmrEp_changes(void);

/** The key with that key ID, copied whole. @return false if none */
bool dmrEp_find(uint8_t id, uint8_t key[DMR_EP_KEY_BYTES]);

/** Keyring entry i (0 .. DMR_EP_KEYS-1), copied whole. @return false if empty */
bool dmrEp_entry(unsigned i, uint8_t *id, uint8_t key[DMR_EP_KEY_BYTES]);

/**
 * Motorola Basic Privacy (key 1..255 by hand, 0 none): the 48 keystream
 * bits, MSB first, XORed onto the first 48 of each frame's 49 data bits
 * (the same table both ways, receive and transmit).
 */
uint64_t dmrBp_keystream(uint8_t key);

/*
 * Anytone Basic Privacy (16-bit key by hand, 0..65535), as RadioFoundry's
 * dmrPrivacy.c (MIT) and DSD-FME have it: the key with nibbles 1 and 3
 * inverted and 2 and 4 plus 8, its 16 bits repeated over the 49 data bits
 * of the 18 frames of a superframe (frame f starts at bit 49 f of that
 * stream). The AMBE+2 silence frame and frames whose bits 24..43 are all
 * zero go in clear. The same both ways, receive and transmit.
 */
/** Deciphers (or enciphers) frame f (0..17) of a superframe */
void dmrBpAny_frame(char ambe_d[49], uint16_t key, unsigned f);
/** The same, the frame packed MSB first in 7 bytes (transmit) */
void dmrBpAny_packed(uint16_t key, unsigned f, uint8_t frame[7]);

/*
 * NXDN cipher type 1 and dPMR scrambled voice, the 15-bit scrambler (key
 * 1..32767 typed by hand), as dsd-neo has it (GPL-3.0-or-later,
 * nxdn_lfsr.c LFSRN): a 15-bit register loaded with the key at the start
 * of each superframe gives one bit a step (its lowest; the new top bit is
 * bit 1 ^ bit 0), 49 for each of the superframe's 16 voice positions in
 * turn (4 frames of 4 AMBE+2 frames; a FACCH1 or data frame uses its
 * positions all the same), XORed onto the 49 data bits after the FEC.
 */
/** Position slot's (0..15) 49 keystream bits, packed MSB first in 7 bytes */
void dmrScr15_bits(uint16_t key, unsigned slot, uint8_t out[7]);

/*
 * TYT Basic Privacy (16-bit key by hand), as dsd-neo has it (crypt-tyt.c,
 * ISC licence): on the 72 coded bits of each AMBE+2 frame as they go on air
 * (before the FEC, so a repeater that corrects them breaks it: simplex
 * only). The key's 16 bits round and round, read backwards: frames 1 and 3
 * of a burst from key bit 15, frame 2 from key bit 7. The same both ways.
 */
/** Applies it to the three frames of a voice burst (132 dibits on air) */
void dmrBpTyt_burst(uint16_t key, uint8_t burst[132]);

/** The MI of the next superframe */
uint32_t dmrEp_nextMi(uint32_t mi);

/**
 * n keystream bytes for the key and MI, starting skip bytes after the
 * DMR_EP_DROP dropped ones (frame f of a superframe: skip = 7 f).
 */
void dmrEp_keystream(const uint8_t key[DMR_EP_KEY_BYTES], uint32_t mi,
                     unsigned skip, uint8_t *out, unsigned n);

/**
 * Deciphers one voice frame's 49 data bits (one per char, as mbelib keeps
 * them) with its 7 keystream bytes. Frames left as sent, as DSD-FME does:
 * the AMBE+2 silence frame and frames whose bits 24..43 are all zero (sent
 * in clear), and frames with 3 or more Golay errors in C0 (left for the
 * vocoder to repeat the last one).
 */
void dmrEp_frame(char ambe_d[49], const uint8_t ks[DMR_EP_FRAME_BYTES],
                 int c0_errors);

/*
 * Transmit side: a frame's 49 data bits packed MSB first in 7 bytes (bit 48
 * in the top bit of the last one, its other bits zero), as the AMBE+2
 * encoder takes them for its FEC.
 */
/** Basic Privacy: the key's 48 keystream bits onto bits 0..47 */
void dmrBp_packed(uint8_t key, uint8_t frame[7]);
/** Enhanced Privacy: the frame's 7 keystream bytes onto its 49 bits */
void dmrEp_packed(uint8_t frame[7], const uint8_t ks[DMR_EP_FRAME_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* DMR_EP_H */
