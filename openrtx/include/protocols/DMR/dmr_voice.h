/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_VOICE_H
#define DMR_VOICE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR voice, phase 2: AMBE+2 (3600x2450) decoding.
 *
 * Two backends, one compiled in: lib/dmr_vocoder by default (our decoder,
 * 113 k instructions per 20 ms frame) and mbelib with DMR_VOICE_ENABLED
 * (C62_DMR_VOICE=ON, 9.36 M per frame: it does not fit the M33 in real time,
 * but it is the only one that decodes D-STAR's 3600x2400 and P25's IMBE
 * 7200x4400). See docs/DMR_PKG.md, "The vocoders". Nothing is fetched for the
 * default build; the mbelib one needs scripts/c62/fetch_mbelib.sh. Without
 * either, everything below compiles to no-ops and DMR stays metadata-only
 * (dmr_voice_available() returns false).
 *
 * Each voice burst (A..F) carries three 72-bit AMBE+2 frames: dibits 0-35,
 * 36-53 + 78-95 (split around the centre sync/EMB field) and 96-131. Every
 * frame is de-interleaved with the DMR AMBE interleave schedule from DSD
 * (ISC licence, credited in dmr_voice.c) into mbelib's ambe_fr[4][24] planes,
 * then packed as the canonical A|B|C frame for lib/dmr_vocoder (or handed to
 * mbelib as it is).
 *
 * Only one slot is heard at a time: in AUTO the first slot to start talking
 * is followed until its call ends (terminator, or no voice for 500 ms); a
 * preference forces TS1 or TS2. Calls flagged as encrypted (privacy bit in
 * the LC service options) are not played, but for Motorola Basic Privacy
 * with a key set by hand (dmr_voice_set_bp_key()).
 */

#define DMR_VOICE_RATE 8000
#define DMR_VOICE_PREF_AUTO (-1)

/* Receives decoded audio: 8 kHz float, nominally within +-1.0. */
typedef void (*dmr_voice_sink_t)(const float *pcm, int n);

bool dmr_voice_available(void);
void dmr_voice_set_sink(dmr_voice_sink_t sink);
void dmr_voice_reset(void);

/* From dmr_proto: every voice burst of a slot (voice_idx 0 = A ... 5 = F). */
void dmr_voice_burst(int slot, const uint8_t burst[132], int voice_idx,
                     uint32_t t_ms);
void dmr_voice_call_end(int slot);
/* The call's privacy bit; basic: a Motorola LC (FID 0x10) and no PI header
 * this call, so if anything Basic Privacy (a PI header announces Enhanced
 * Privacy or AES, never descrambled) */
void dmr_voice_set_encrypted(int slot, bool enc, bool basic);

/*
 * Motorola Basic Privacy key, 0 none or 1..255, from the memory tuned (typed
 * there by hand). A call that is basic (above) is descrambled with this key
 * and played; nothing tries other keys or tells whether this one is right.
 * Lab use, with one's own transmitter.
 */
/* anytone: Anytone's 16-bit Basic Privacy (dmr_ep.h), else Motorola's */
void dmr_voice_set_bp_key(uint16_t key, bool anytone);
uint16_t dmr_voice_get_bp_key(void);
/* The key set is Anytone's BP */
bool dmr_voice_get_bp_anytone(void);
/* The key set is TYT's BP (on the coded frames, simplex) */
void dmr_voice_set_bp_tyt(bool tyt);
bool dmr_voice_get_bp_tyt(void);

void dmr_voice_set_bp_scan(bool enabled, uint32_t frequency);
bool dmr_voice_get_bp_scan(void);
uint8_t dmr_voice_get_bp_auto_key(int slot);
uint8_t dmr_voice_get_bp_last_found(void);

/*
 * A Motorola PI header on this slot (alg, key ID, 32-bit MI), or alg 0 at a
 * new call to forget the last one. With ARC4 (Enhanced Privacy) and that key
 * ID in the keyring typed by hand (dmr_ep.h), the call is deciphered and
 * played. @return true then.
 */
bool dmr_voice_set_pi(int slot, uint8_t alg, uint8_t key_id, uint32_t mi);
/* Hytera Enhanced (dmr_hytera.h): its PI header's algorithm 0x02, key ID
 * (the Enhanced Privacy keyring's) and 40-bit MI. True when it plays */
bool dmr_voice_set_pi_hytera(int slot, uint8_t alg, uint8_t key_id,
                             uint64_t mi);
/* Hytera Basic: the LC's feature set ID 0x68 with the privacy bit, no PI */
void dmr_voice_set_hytera_basic(int slot, bool on);

/*
 * Which calls are heard (C62: Ajustes > DMR > Escuchar). The filter is asked
 * at each call's IDs (voice LC header, embedded LC: dmr_voice_set_call()); a
 * call it refuses is not played, and leaves the other slot free to be. NULL
 * hears every call. dmr_voice_blocked(): the slot's call is being refused.
 */
typedef bool (*dmr_voice_filter_t)(bool group, uint32_t dst, uint32_t src);
void dmr_voice_set_filter(dmr_voice_filter_t fn);
void dmr_voice_set_call(int slot, bool group, uint32_t dst, uint32_t src);
bool dmr_voice_blocked(int slot);

/* Slot preference: 0 = TS1, 1 = TS2, DMR_VOICE_PREF_AUTO. */
void dmr_voice_set_pref(int pref);
int dmr_voice_get_pref(void);
/* Slot currently being played, or -1. */
int dmr_voice_get_playing(void);

/* Vocoder frames decoded since boot and their average decode time (us) -
 * a 20 ms frame must take well under 20 ms, shared with everything else. */
void dmr_voice_get_timing(uint32_t *frames, uint32_t *avg_us);

/* Frames attenuated by the burst concealment (corrupted-frame squawks). */
uint32_t dmr_voice_get_concealed(void);

/* Playback side. dmr_voice_burst() only queues the bursts of the slot being
 * heard; the vocoder runs in the caller of dmr_voice_decode_next(), which
 * decodes the oldest queued burst into three 20 ms frames handed to the sink.
 * Returns false if nothing was queued. */
bool dmr_voice_decode_next(void);
/* Voice bursts waiting for the vocoder (60 ms each). */
int dmr_voice_queued(void);
/* Longest vocoder frame so far (us), and bursts dropped because the queue
 * was full (the vocoder fell behind). */
void dmr_voice_get_debug(uint32_t *max_us, uint32_t *queue_drops);

/**
 * Bits corrected by the AMBE+2 frame FEC (Golay codes of C0 and C1, 47
 * protected bits per frame) and the frames that went through it, since
 * power on: their ratio is an estimate of the channel bit error rate. Only
 * frames with their 72-bit coding count (DMR, NXDN).
 *
 * @param bits: corrected bits.
 * @param frames: frames decoded with the FEC.
 */
void dmr_voice_get_fec(uint32_t *bits, uint32_t *frames);

/**
 * The vocoder's own run time per frame, without the time other threads
 * preempted it (0 when the kernel does not count thread time).
 *
 * @param avg_us: average since power on.
 * @param max_us: largest.
 */
void dmr_voice_get_own(uint32_t *avg_us, uint32_t *max_us);

/**
 * Queue one 20 ms AMBE+2 frame given as its 49 data bits, FEC already
 * removed (System Fusion V/D mode 2). Played by the same speech thread as
 * DMR; frames are grouped three per queued burst.
 *
 * @param bits: 49 bits, one per byte, in mbelib's ambe_d order.
 * @param errs: bits corrected by the caller's FEC, for the vocoder's
 * repeat-or-mute decision.
 * @param t_ms: arrival time, ms; a gap of more than 500 ms starts a new
 * call with a fresh vocoder state.
 */
void dmr_voice_ambe49(const uint8_t bits[49], unsigned errs, uint32_t t_ms);

/**
 * Queue one 20 ms AMBE+2 frame in its 72-bit channel coding (NXDN): the
 * Golay (24,12) word a, the scrambled Golay (23,12) word b and the 25
 * unprotected bits c, packed MSB first in that order into 9 bytes. The
 * vocoder corrects and descrambles it as a DMR frame. Grouped three per
 * burst with the frames of dmr_voice_ambe49().
 *
 * @param abc: the 72 bits.
 * @param t_ms: arrival time, ms, as in dmr_voice_ambe49().
 */
void dmr_voice_ambe72(const uint8_t abc[9], uint32_t t_ms);
/* The same, scrambled: its 7 keystream bytes (49 bits, dmrScr15_bits())
 * taken off its 49 data bits after the FEC, every frame */
void dmr_voice_ambe72_ks(const uint8_t abc[9], const uint8_t ks[7],
                         uint32_t t_ms);

/**
 * Queue one 20 ms IMBE frame (System Fusion VW, full rate): its 144 channel
 * bits de-interleaved, c0 to c7 in order (four Golay (23,12) words, three
 * Hamming (15,11) words, seven bits), packed MSB first into 18 bytes. The
 * vocoder corrects, descrambles and decodes it (mbelib IMBE 7200x4400).
 * Grouped three per burst with the other raw frames.
 *
 * @param bits: the 144 bits.
 * @param t_ms: arrival time, ms, as in dmr_voice_ambe49().
 */
void dmr_voice_imbe144(const uint8_t bits[18], uint32_t t_ms);
/* The same, enciphered: its 11 keystream bytes (dmr_aes.h, P25 AES) taken
 * off its 88 data bits after the FEC */
void dmr_voice_imbe144_ks(const uint8_t bits[18], const uint8_t ks[11],
                          uint32_t t_ms);

/**
 * Queue one 20 ms D-STAR voice frame (AMBE 3600x2400): its 72 bits in air
 * order, first received bit in the MSB of bits[0]. The vocoder reorders
 * them for mbelib (dstar_ambe_toFrame()), corrects and decodes them.
 * Grouped three per burst with the other raw frames.
 */
void dmr_voice_dstar72(const uint8_t bits[9], uint32_t t_ms);

/**
 * Queue the frames held back for a partial burst: end of a transmission.
 */
void dmr_voice_ambe49_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* DMR_VOICE_H */
