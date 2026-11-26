/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dmr_voice.h"
#include "dmr_port.h"
#include "dmr_ep.h"
#include "dmr_aes.h"
#include "dmr_hytera.h"
#include "dmr_bp_auto.h"

#include <stdio.h>
#include <string.h>

static volatile int s_pref = DMR_VOICE_PREF_AUTO;
static volatile int s_playing = -1;
static volatile uint16_t s_bp_key; /* Basic Privacy key, 0 none */
static volatile bool s_bp_anytone; /* Anytone's 16-bit BP, else Motorola */
static volatile bool s_bp_tyt;     /* TYT's 16-bit BP */
static volatile bool s_bp_scan;
static volatile uint32_t s_bp_scan_freq;
static volatile uint32_t s_bp_scan_epoch;
static volatile uint8_t s_bp_auto_found[2];
static volatile uint8_t s_bp_last_found;

void dmr_voice_set_bp_scan(bool enabled, uint32_t frequency)
{
    if (enabled != s_bp_scan || (enabled && frequency != s_bp_scan_freq)) {
        s_bp_auto_found[0] = s_bp_auto_found[1] = 0;
        s_bp_last_found = 0;
        ++s_bp_scan_epoch;
        printf("[DMR BP] VFO SCAN %s a %lu Hz\n",
               enabled ? "ON" : "OFF", (unsigned long)frequency);
    }
    s_bp_scan_freq = frequency;
    s_bp_scan = enabled;
}

bool dmr_voice_get_bp_scan(void)
{
    return s_bp_scan;
}

uint8_t dmr_voice_get_bp_auto_key(int slot)
{
    return s_bp_auto_found[slot & 1];
}

uint8_t dmr_voice_get_bp_last_found(void)
{
    return s_bp_last_found;
}

void dmr_voice_set_bp_key(uint16_t key, bool anytone)
{
    s_bp_anytone = anytone;
    s_bp_key = key;
}

void dmr_voice_set_bp_tyt(bool tyt)
{
    s_bp_tyt = tyt;
}

bool dmr_voice_get_bp_tyt(void)
{
    return s_bp_tyt;
}

bool dmr_voice_get_bp_anytone(void)
{
    return s_bp_anytone;
}

uint16_t dmr_voice_get_bp_key(void)
{
    return s_bp_key;
}

void dmr_voice_set_pref(int pref)
{
    s_pref = (pref == 0 || pref == 1) ? pref : DMR_VOICE_PREF_AUTO;
    if (s_pref != DMR_VOICE_PREF_AUTO && s_playing != s_pref) {
        s_playing = -1; /* re-choose on the next voice burst */
    }
}

int dmr_voice_get_pref(void)
{
    return s_pref;
}

int dmr_voice_get_playing(void)
{
    return s_playing;
}

/* The call filter (dmr_voice.h), and each slot's call refused by it */
static dmr_voice_filter_t volatile s_filter;
static volatile bool s_blocked[2];

/*
 * The filter re-armed for a call not identified yet. s_blocked held the
 * answer for the call just gone, and dmr_proto only calls set_call() once
 * the link control is decoded, so the voice bursts that arrive before it
 * went out with the PREVIOUS call's verdict: the start of a refused call
 * was heard, a fraction of a second of it, until its LC came in.
 *
 * Asking the filter about a group call with no identity gives what the
 * setting means on its own: Todos lets it through (its answer to anything
 * is true), while Mi TG and Lista hold it until the LC says whose it is.
 * So nothing is muted here that was not going to be muted anyway.
 */
static void rearm_filter(int slot)
{
    const dmr_voice_filter_t fn = s_filter;

    s_blocked[slot & 1] = fn != NULL && !fn(true, 0u, 0u);
}

void dmr_voice_set_filter(dmr_voice_filter_t fn)
{
    s_filter = fn;
    /* Set at enable() and on a change of setting: both slots start from it */
    rearm_filter(0);
    rearm_filter(1);
}

void dmr_voice_set_call(int slot, bool group, uint32_t dst, uint32_t src)
{
    const dmr_voice_filter_t fn = s_filter;
    const bool blocked = fn != NULL && !fn(group, dst, src);
    s_blocked[slot & 1] = blocked;
    /* Refused while being played: the other slot may take the speaker */
    if (blocked && s_playing == (slot & 1))
        s_playing = -1;
}

bool dmr_voice_blocked(int slot)
{
    return s_blocked[slot & 1];
}

#if !defined(DMR_VOICE_ENABLED) && !defined(DMR_VOICE_OWN)
/* ---- no vocoder in this build: metadata-only ------------------------------ */
void dmr_voice_get_timing(uint32_t *frames, uint32_t *avg_us)
{
    *frames = 0;
    *avg_us = 0;
}
uint32_t dmr_voice_get_concealed(void)
{
    return 0;
}
bool dmr_voice_available(void)
{
    return false;
}
void dmr_voice_set_sink(dmr_voice_sink_t sink)
{
    (void)sink;
}
void dmr_voice_reset(void)
{
    s_playing = -1;
}
void dmr_voice_burst(int slot, const uint8_t burst[132], int voice_idx,
                     uint32_t t_ms)
{
    (void)slot;
    (void)burst;
    (void)voice_idx;
    (void)t_ms;
}
/*
 * The stall rule with a clock of its own.  It used to live inside
 * dmr_voice_burst(), which meant a call that simply stops -- nobody sends a
 * terminator to say so -- kept the speaker for ever: with no burst there was
 * nothing to run the timeout.  Whoever knows the stream time calls this once
 * per block; the DMR status does, because it already runs there.
 */
void dmr_voice_tick(uint32_t t_ms)
{
    const int playing = s_playing; /* volatile: one read, one index */

    if (playing < 0 || playing > 1)
        return;

    const uint32_t hold = (s_pref == DMR_VOICE_PREF_AUTO) ? VOICE_AUTO_HOLD_MS
                                                        : VOICE_TIMEOUT_MS;

    if ((t_ms - s_call[playing].last_voice_ms) > hold)
        s_playing = -1;
}

void dmr_voice_call_end(int slot)
{
    (void)slot;
}
void dmr_voice_set_encrypted(int slot, bool enc, bool basic)
{
    (void)slot;
    (void)enc;
    (void)basic;
}
void dmr_voice_set_hytera_basic(int slot, bool on)
{
    (void)slot;
    (void)on;
}
bool dmr_voice_set_pi_hytera(int slot, uint8_t alg, uint8_t key_id,
                             uint64_t mi)
{
    (void)slot;
    (void)alg;
    (void)key_id;
    (void)mi;
    return false;
}
bool dmr_voice_set_pi(int slot, uint8_t alg, uint8_t key_id, uint32_t mi)
{
    (void)slot;
    (void)alg;
    (void)key_id;
    (void)mi;
    return false;
}
int dmr_voice_queued(void)
{
    return 0;
}
void dmr_voice_get_debug(uint32_t *max_us, uint32_t *queue_drops)
{
    *max_us = 0;
    *queue_drops = 0;
}
bool dmr_voice_decode_next(void)
{
    return false;
}
void dmr_voice_ambe49(const uint8_t bits[49], unsigned errs, uint32_t t_ms)
{
    (void)bits;
    (void)errs;
    (void)t_ms;
}
void dmr_voice_ambe49_flush(void)
{
}
void dmr_voice_ambe72(const uint8_t abc[9], uint32_t t_ms)
{
    (void)abc;
    (void)t_ms;
}
void dmr_voice_imbe144(const uint8_t bits[18], uint32_t t_ms)
{
    (void)bits;
    (void)t_ms;
}
void dmr_voice_dstar72(const uint8_t bits[9], uint32_t t_ms)
{
    (void)bits;
    (void)t_ms;
}
void dmr_voice_get_fec(uint32_t *bits, uint32_t *frames)
{
    *bits = 0;
    *frames = 0;
}
void dmr_voice_get_own(uint32_t *avg_us, uint32_t *max_us)
{
    *avg_us = 0;
    *max_us = 0;
}

#else
/* ---- voice: our vocoder (default) or mbelib ------------------------------- */
#ifdef DMR_VOICE_ENABLED
#include "mbelib.h"
#ifdef CONFIG_DSTAR
#include "protocols/DSTAR/dstar_ambe.h"
#endif
#else
/*
 * lib/dmr_vocoder through its per-instance header: the decoder state has to
 * sit in DTCM (DMR_FAST_BSS, dmr_port.h) as mbelib's did, and the
 * single-instance dmr_vocoder.h owns a plain .bss object.  Same sources,
 * same flags; see docs/DMR_PKG.md, "Voice".
 */
#include "dmr_decoder.h"
#endif
#include <math.h>

/*
 * DMR AMBE+2 interleave schedule, from DSD (Copyright (C) 2010 DSD Author,
 * ISC licence): dibit i of a 72-bit frame goes to ambe_fr[rW[i]][rX[i]] (its
 * high bit) and ambe_fr[rY[i]][rZ[i]] (low bit).
 */
static const uint8_t rW[36] = { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
                                0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 2,
                                0, 2, 0, 2, 0, 2, 0, 2, 0, 2, 0, 2 };
static const uint8_t rX[36] = { 23, 10, 22, 9, 21, 8, 20, 7, 19, 6, 18, 5,
                                17, 4,  16, 3, 15, 2, 14, 1, 13, 0, 12, 10,
                                11, 9,  10, 8, 9,  7, 8,  6, 7,  5, 6,  4 };
static const uint8_t rY[36] = { 0, 2, 0, 2, 0, 2, 0, 2, 0, 3, 0, 3,
                                1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3,
                                1, 3, 1, 3, 1, 3, 1, 3, 1, 3, 1, 3 };
static const uint8_t rZ[36] = { 5,  3,  4,  2,  3,  1, 2,  0, 1,  13, 0,  12,
                                22, 11, 21, 10, 20, 9, 19, 8, 18, 7,  17, 6,
                                16, 5,  15, 4,  14, 3, 13, 2, 12, 1,  11, 0 };

#define VOICE_TIMEOUT_MS 500 /* no voice burst for this long ends the call */

/*
 * In AUTO the call being followed keeps the speaker through the gaps that a
 * real conversation has -- the operator pauses, the repeater drops one burst --
 * and only lets go when the call is over: the terminator, or this long without
 * hearing it.  Five seconds is the hang time asked for in the quick menu's TS
 * row; a fixed slot keeps the short one, because there the operator chose who
 * to hear.
 */
#define VOICE_AUTO_HOLD_MS 5000
#ifndef DMR_VOICE_UVQUALITY
#define DMR_VOICE_UVQUALITY \
    3 /* mbelib unvoiced synthesis quality (DSD default) */
#endif
#ifdef DMR_VOICE_ENABLED
/* mbelib float synthesis output -> ~+-1 full scale */
#define OUT_SCALE \
    (1.0f / 4096.0f)        /* host-measured: speech peaks ~0.95, rms ~0.09 */
#endif

/*
 * Burst concealment. On weak or fading signals mbelib sometimes synthesises a
 * frame whose parameters were corrupted beyond what its FEC reports: it still
 * counts a few corrected errors (errs2 1-3), but the output is a loud 20 ms
 * squawk, sometimes at full scale. So: a frame WITH corrected errors whose
 * rms jumps above CONCEAL_RATIO x the recent clean level is scaled down to
 * that level. Error-free frames (and errored frames of normal energy, usually
 * decoded correctly) are left untouched; only error-free frames update the
 * level.
 */
/*
 * Only frames corrected near the codes' limit (Golay corrects 3 errors in
 * C0 and 3 in C1) and loud well beyond speech's own swings. With "any
 * corrected error" and twice the level, a clean call at 1 % BER (a third of
 * its frames with a corrected bit, and syllable onsets often over twice the
 * running level) lost 5-7 % of its frames to this: logs of 2026-11-15/16,
 * "concealed 247" of 3423 frames at BER 1.2-1.6 %, heard as level jumps and
 * dull syllables.
 */
#define CONCEAL_MIN_ERRS 3
#define CONCEAL_RATIO 4.0f
#define CONCEAL_FLOOR 0.02f /* never treat near-silence as a reference */
#define LEVEL_ALPHA 0.1f    /* per 20 ms frame */

/*
 * Threads. The receiver (dmr_voice_burst(), from dmr_proto on the rtx
 * thread) picks the slot to play and queues its voice bursts; the playback
 * thread runs the vocoder (dmr_voice_decode_next()). The vocoder can take a
 * good part of a burst's 60 ms, and running it on the receive side would
 * make the demodulator lose samples.
 *
 * Receiver side: the call bookkeeping (enc, last_voice_ms).
 * Playback side: the vocoder state (mbe parameters, concealment level).
 */
typedef struct {
    bool enc;
    bool basic; /* if encrypted, Basic Privacy (dmr_voice_set_encrypted()) */
    uint32_t last_voice_ms;
    /* Enhanced Privacy (dmr_voice_set_pi()): the key, the PI header's MI,
     * and the superframe count since the first burst A after it */
    bool ep;
    bool ep_aes;     /* AES-256, AES-128 or DES (dmr_aes.h), else ARC4 */
    uint8_t aes_len; /* its key's length, the algorithm */
    bool ep_hy;      /* Hytera Enhanced (dmr_hytera.h) */
    bool hbasic;     /* Hytera Basic */
    uint8_t ep_key[DMR_EP_KEY_BYTES];
    uint8_t aes_key[DMR_AES_KEY_BYTES];
    uint64_t ep_mi0;
    bool ep_started;
    uint32_t ep_t0;  /* stream time of the first superframe's burst A */
    uint32_t ep_sf;  /* superframe of ep_mi */
    uint64_t ep_mi;
} vcall_t;

typedef struct {
#ifdef DMR_VOICE_ENABLED
    mbe_parms cur, prev, prev_enh;
#else
    dmr_vocoder_t voc; /* lib/dmr_vocoder, 8 540 B */
#endif
    float level; /* running rms of recent error-free frames (concealment reference) */
} vdec_t;

/*
 * One queued voice burst: its three AMBE frames, 36 dibits each. A burst
 * of raw frames holds instead, per frame, either the 49 data bits packed
 * MSB first in bytes 0-6 and the corrected bit count in byte 7 (System
 * Fusion, RAW_AMBE49), or the 72 channel bits a, b, c packed MSB first in
 * bytes 0-8 (NXDN, RAW_AMBE72); it may hold fewer than three frames (the
 * end of a transmission).
 */
enum { RAW_NONE = 0, RAW_AMBE49 = 1, RAW_AMBE72 = 2, RAW_IMBE144 = 3,
       RAW_DSTAR72 = 4 };

typedef struct {
    uint8_t frames[3][36];
    uint8_t slot;
    bool new_call;  /* reset the slot's vocoder state first */
    uint8_t raw;    /* RAW_NONE: DMR dibits, else the packed format */
    uint8_t count;  /* frames held, raw bursts only */
    uint8_t kinds[3]; /* per frame format of a raw burst */
    uint8_t rks;      /* raw IMBE frame i enciphered: bit i, its 11
                       * keystream bytes at frames[i][18] */
    uint16_t bp_key;  /* DMR: Basic Privacy key to descramble with, 0 none */
    bool bp_any;      /* Anytone's (frames numbered by ep_burst) */
    bool bp_auto;
    uint32_t bp_epoch;
    /* DMR Enhanced Privacy: key and MI of the burst's superframe, and its
     * place there (0 = A .. 5 = F) */
    bool ep;
    bool ep_aes;
    uint8_t aes_len;
    bool ep_hy;
    bool bp_hy;       /* Hytera Basic (the key set, dmr_hytera.h) */
    uint8_t ep_burst;
    uint8_t ep_key[DMR_EP_KEY_BYTES];
    uint8_t aes_key[DMR_AES_KEY_BYTES];
    uint64_t ep_mi;
} vburst_t;

#define QUEUE_BURSTS 12     /* 720 ms */

/*
 * The package's state that is not touched per sample lives in PSRAM on the
 * C62 (EXT_RAM_BSS_ATTR, dmr_port.h): the call bookkeeping, the Basic
 * Privacy key votes and the voice burst queue below.  They are written once
 * per burst (60 ms, or 30 ms per slot) and read once per burst or per 20 ms
 * frame, so the slower memory costs nothing measurable, and the section is
 * cleared at boot (platform_init) and by dmr_voice_reset().
 */
static EXT_RAM_BSS_ATTR vcall_t s_call[2]; /* receiver side */
static EXT_RAM_BSS_ATTR uint8_t s_bp_votes[2][256];
static uint32_t s_bp_decoder_epoch;
/*
 * Playback side, one slot's worth. Not in PSRAM: the vocoder reads and writes
 * it all through every frame, and on the C62 PSRAM made it barely real time.
 * On the C62 in DTCM (dmr_port.h).
 *
 * ONE state, not one per slot: DTCM is 16 KB and the speech thread's stack
 * (8 KB) is in it too, so two vdec_t (2 x 7.6 KB) do not fit and the link
 * fails.  Only one slot is decoded at a time (AUTO contrast: the slot being
 * heard), so the state belongs to whichever slot was decoded last; switching
 * slot resets it, which costs one corrupt frame at the switch.  Measured:
 * the TX build's .dtcm_bss was 23 848 B with two, 16 020 B with one.
 */
static DMR_FAST_BSS vdec_t s_dec[1];
static int s_dec_slot = -1;

static void dec_init(int slot);

static vdec_t *dec_of(int slot)
{
    if (s_dec_slot != slot)
        dec_init(slot);
    return &s_dec[0];
}
static EXT_RAM_BSS_ATTR vburst_t s_queue[QUEUE_BURSTS];
static int s_q_head, s_q_count;
static volatile bool s_dec_reset; /* reset all vocoder state on next pop */
DMR_LOCK_DECLARE(s_q_lock);

static volatile uint32_t s_frames;
static volatile uint64_t s_frame_us_total;
static volatile uint32_t s_frame_us_max;
static volatile uint32_t s_queue_drops;
static volatile uint32_t s_concealed;
static volatile uint64_t s_own_us_total; /* vocoder's own run time */
static volatile uint32_t s_own_us_max;
static volatile uint32_t s_fec_bits;   /* bits corrected by the frame FEC */
static volatile uint32_t s_fec_frames; /* frames that went through it */
static dmr_voice_sink_t s_sink;

/* Soft limiter: linear to 0.7, then a smooth knee toward 1.0 (no hard clip). */
static inline float soft_limit(float v)
{
    const float a = v < 0.0f ? -v : v;
    if (a <= 0.7f) {
        return v;
    }
    {
        const float over = (a - 0.7f) / 0.3f;
        const float y =
            0.7f + 0.3f * (over / (1.0f + over)); /* -> 1.0 asymptotically */
        return v < 0.0f ? -y : y;
    }
}

bool dmr_voice_available(void)
{
    return true;
}

uint32_t dmr_voice_get_concealed(void)
{
    return s_concealed;
}

void dmr_voice_get_timing(uint32_t *frames, uint32_t *avg_us)
{
    uint32_t n = s_frames;
    *frames = n;
    *avg_us = n ? (uint32_t)(s_frame_us_total / n) : 0;
}

void dmr_voice_set_sink(dmr_voice_sink_t sink)
{
    s_sink = sink;
}

static void raw_reset(void);

#ifdef DMR_VOICE_ENABLED
#ifdef MBE_COS_TABLE
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
/* mbelib's table cosine (mbe_fastmath.h): one turn, plus the first point
 * again for the interpolation at the end */
float mbe_cos_table[1024 + 1];

static void mbe_cos_init(void)
{
    static bool done;
    if (done)
        return;
    for (int i = 0; i <= 1024; i++)
        mbe_cos_table[i] = cosf((float)(2.0 * M_PI * i / 1024.0));
    done = true;
}
#else
static void mbe_cos_init(void)
{
}
#endif
#else /* our vocoder: every table is const, in flash */
static void mbe_cos_init(void)
{
}
#endif

static void dec_init(int slot)
{
    memset(&s_dec[0], 0, sizeof(s_dec[0]));
#ifdef DMR_VOICE_ENABLED
    mbe_initMbeParms(&s_dec[0].cur, &s_dec[0].prev, &s_dec[0].prev_enh);
#else
    dmr_vocoder_init(&s_dec[0].voc);
#endif
    s_dec_slot = slot;
}

void dmr_voice_reset(void)
{
    rearm_filter(0);
    rearm_filter(1);
    memset(s_call, 0, sizeof(s_call));
    s_bp_auto_found[0] = s_bp_auto_found[1] = 0;
    ++s_bp_scan_epoch;
    s_playing = -1;
    DMR_LOCK(s_q_lock);
    memset(s_queue, 0, sizeof(s_queue)); /* keys do not outlive the mode */
    s_q_head = s_q_count = 0;
    s_dec_reset = true;
    DMR_UNLOCK(s_q_lock);
    raw_reset();
}

void dmr_voice_set_encrypted(int slot, bool enc, bool basic)
{
    s_call[slot & 1].enc = enc;
    s_call[slot & 1].basic = basic;
}

void dmr_voice_set_hytera_basic(int slot, bool on)
{
    s_call[slot & 1].hbasic = on;
}

bool dmr_voice_set_pi_hytera(int slot, uint8_t alg, uint8_t key_id,
                             uint64_t mi)
{
    vcall_t *c = &s_call[slot & 1];
    c->ep = false;
    c->ep_aes = false;
    c->ep_hy = false;
    c->ep_started = false;
    memset(c->ep_key, 0, sizeof(c->ep_key));
    if (alg != 0x02u || !dmrEp_find(key_id, c->ep_key))
        return false;
    c->ep_mi0 = mi;
    c->ep_hy = true;
    c->ep = true;
    return true;
}

/*
 * The stall rule with a clock of its own.  It used to live inside
 * dmr_voice_burst(), which meant a call that simply stops -- nobody sends a
 * terminator to say so -- kept the speaker for ever: with no burst there was
 * nothing to run the timeout.  Whoever knows the stream time calls this once
 * per block; the DMR status does, because it already runs there.
 */
void dmr_voice_tick(uint32_t t_ms)
{
    const int playing = s_playing; /* volatile: one read, one index */

    if (playing < 0 || playing > 1)
        return;

    const uint32_t hold = (s_pref == DMR_VOICE_PREF_AUTO) ? VOICE_AUTO_HOLD_MS
                                                        : VOICE_TIMEOUT_MS;

    if ((t_ms - s_call[playing].last_voice_ms) > hold)
        s_playing = -1;
}

void dmr_voice_call_end(int slot)
{
    if (s_playing == slot) {
        s_playing = -1;
    }
    /* The next call on this slot starts muted unless the setting says
     * otherwise, until its link control identifies it */
    rearm_filter(slot);
    s_call[slot & 1].enc = false;
    s_call[slot & 1].basic = false;
    s_call[slot & 1].ep = false;
}

bool dmr_voice_set_pi(int slot, uint8_t alg, uint8_t key_id, uint32_t mi)
{
    vcall_t *c = &s_call[slot & 1];
    c->ep = false;
    c->ep_aes = false;
    c->ep_hy = false;
    c->ep_started = false;
    memset(c->ep_key, 0, sizeof(c->ep_key));
    dmrAes_wipe(c->aes_key, sizeof(c->aes_key));
    /* AES-256 0x25, AES-128 0x24, DES 0x22, or 0x05, 0x04, 0x02 from other
     * makes (dmr_aes.h): the key of that ID, of the algorithm's length */
    const uint8_t want = dmrAes_dmrLen(alg);
    if (want != 0u && alg < 0x28u) {
        uint8_t len = 0;
        if (key_id == 0u || !dmrAes_find(key_id, c->aes_key, &len)
            || len != want) {
            dmrAes_wipe(c->aes_key, sizeof(c->aes_key));
            return false;
        }
        c->aes_len = len;
        c->ep_mi0 = mi;
        c->ep_aes = true;
        c->ep = true;
        return true;
    }
    /* ARC4: 0x21, or 0x01 from some firmware (DSD-FME's dmr_pi.c) */
    if (alg >= 0x28u || (alg & 0x07u) != 0x01u)
        return false;
    if (!dmrEp_find(key_id, c->ep_key))
        return false;
    c->ep_mi0 = mi;
    c->ep = true;
    return true;
}

/* Motorola Basic Privacy (dmr_ep.h): the key's keystream off the first 48
 * of the 49 data bits, after the frame FEC and before the vocoder */
static void bp_descramble(uint8_t key, char ambe_d[49])
{
    const uint64_t ks = dmrBp_keystream(key);
    for (int j = 0; j < 48; j++)
        ambe_d[j] ^= (char)((ks >> (47 - j)) & 1u);
}

int dmr_voice_queued(void)
{
    int n;
    DMR_LOCK(s_q_lock);
    n = s_q_count;
    DMR_UNLOCK(s_q_lock);
    return n;
}

/* mbelib's de-interleaved planes (C0 24 bits, C1 23, then the 25 unprotected
 * bits) as the canonical 9-byte frame lib/dmr_vocoder decodes: bit 0 is the
 * first bit of C0, which is how OpenDMR's encoder packs it. */
#ifndef DMR_VOICE_ENABLED
static void planes_to_frame(const char fr[4][24], uint8_t frame[9])
{
    memset(frame, 0, 9);
    for (int j = 0; j < 24; j++)
        if (fr[0][j] != 0) {
            const int t = 23 - j; /* C0, MSB first */
            frame[t >> 3] |= (uint8_t)(0x80u >> (t & 7));
        }
    for (int j = 0; j < 23; j++)
        if (fr[1][j] != 0) {
            const int t = 46 - j;
            frame[t >> 3] |= (uint8_t)(0x80u >> (t & 7));
        }
    for (int j = 0; j < 11; j++)
        if (fr[2][j] != 0) {
            const int t = 57 - j;
            frame[t >> 3] |= (uint8_t)(0x80u >> (t & 7));
        }
    for (int j = 0; j < 14; j++)
        if (fr[3][j] != 0) {
            const int t = 71 - j;
            frame[t >> 3] |= (uint8_t)(0x80u >> (t & 7));
        }
}
#endif

static void decode_frame(int slot, const uint8_t *dib, uint8_t kind,
                         uint16_t bp_key, bool bp_auto, const uint8_t *ep_ks,
                         bool bp_any, unsigned frame, bool bp_hy)
{
#ifdef DMR_VOICE_ENABLED
    char ambe_fr[4][24], ambe_d[49];
    char err_str[64];
    char imbe_fr[8][23], imbe_d[88];
#else
    char ambe_fr[4][24];
#endif
    float pcm[160];
    int errs = 0, errs2 = 0;
    vdec_t *v = dec_of(slot);
    const bool raw = kind == RAW_AMBE49;
    const bool imbe = kind == RAW_IMBE144;
    const bool dstar = kind == RAW_DSTAR72;

    if (kind == RAW_AMBE49) {
#ifdef DMR_VOICE_ENABLED
        for (int i = 0; i < 49; i++)
            ambe_d[i] = (char)((dib[i / 8] >> (7 - i % 8)) & 1u);
#endif
        /* Bounded: mbelib writes one character per error into err_str */
        errs2 = dib[7] > 8 ? 8 : dib[7];
    } else if (kind == RAW_AMBE72) {
        /* a: C0 bits 23..0, b: C1 bits 22..0, c: C2 bits 10..0 then C3
         * bits 13..0, as mbelib's ECC and demodulation read them */
        unsigned n = 0;
#define ABC_BIT() ((char)((dib[n / 8] >> (7 - n % 8)) & 1u)); n++
        for (int j = 23; j >= 0; j--) {
            ambe_fr[0][j] = ABC_BIT();
        }
        for (int j = 22; j >= 0; j--) {
            ambe_fr[1][j] = ABC_BIT();
        }
        ambe_fr[1][23] = 0;
        for (int j = 10; j >= 0; j--) {
            ambe_fr[2][j] = ABC_BIT();
        }
        for (int j = 13; j >= 0; j--) {
            ambe_fr[3][j] = ABC_BIT();
        }
#undef ABC_BIT
        for (int j = 11; j < 24; j++)
            ambe_fr[2][j] = 0;
        for (int j = 14; j < 24; j++)
            ambe_fr[3][j] = 0;
    } else if (dstar) {
#ifdef DMR_VOICE_ENABLED
#ifdef CONFIG_DSTAR
        dstar_ambe_toFrame(dib, ambe_fr);
#else
        memset(ambe_fr, 0, sizeof(ambe_fr));
#endif
#else
        memset(ambe_fr, 0, sizeof(ambe_fr));
#endif
    } else if (imbe) {
#ifdef DMR_VOICE_ENABLED
        /* Each word first bit (MSB) at its top index, as mbelib's Golay,
         * Hamming and descrambling read them */
        static const uint8_t len[8] = { 23, 23, 23, 23, 15, 15, 15, 7 };
        unsigned n = 0;
        memset(imbe_fr, 0, sizeof(imbe_fr));
        for (int i = 0; i < 8; i++)
            for (int j = len[i] - 1; j >= 0; j--, n++)
                imbe_fr[i][j] = (char)((dib[n / 8] >> (7 - n % 8)) & 1u);
#else
        memset(ambe_fr, 0, sizeof(ambe_fr));
#endif
    } else {
        memset(ambe_fr, 0, sizeof(ambe_fr));
        for (int i = 0; i < 36; i++) {
            ambe_fr[rW[i]][rX[i]] = (char)((dib[i] >> 1) & 1u);
            ambe_fr[rY[i]][rZ[i]] = (char)(dib[i] & 1u);
        }
    }
#ifdef DMR_VOICE_ENABLED
    err_str[0] = '\0';
#endif
    {
        uint32_t t0 = dmr_port_cycles();
        uint32_t own0 = dmr_port_thread_cycles();
        uint32_t us;
#ifdef DMR_VOICE_ENABLED
        if (dstar)
#ifdef CONFIG_DSTAR
            mbe_processAmbe3600x2400Framef(pcm, &errs, &errs2, err_str,
                                           ambe_fr, ambe_d, &v->cur,
                                           &v->prev, &v->prev_enh,
                                           DMR_VOICE_UVQUALITY);
#else
            memset(pcm, 0, sizeof(pcm));
#endif
        else if (imbe && ep_ks != NULL) {
            /* mbe_processImbe7200x4400Framef() in its steps, the AES
             * keystream taken off the corrected u0..u7 (dmr_aes.h) */
            errs = mbe_eccImbe7200x4400C0(imbe_fr);
            mbe_demodulateImbe7200x4400Data(imbe_fr);
            errs2 = errs + mbe_eccImbe7200x4400Data(imbe_fr, imbe_d);
            for (int k = 0; k < 88; k++)
                imbe_d[k] ^= (char)((ep_ks[k / 8] >> (7 - k % 8)) & 1u);
            mbe_processImbe4400Dataf(pcm, &errs, &errs2, err_str, imbe_d,
                                     &v->cur, &v->prev, &v->prev_enh,
                                     DMR_VOICE_UVQUALITY);
        } else if (imbe)
            mbe_processImbe7200x4400Framef(pcm, &errs, &errs2, err_str,
                                           imbe_fr, imbe_d, &v->cur,
                                           &v->prev, &v->prev_enh,
                                           DMR_VOICE_UVQUALITY);
        else if (raw)
            mbe_processAmbe2450Dataf(pcm, &errs, &errs2, err_str, ambe_d,
                                     &v->cur, &v->prev, &v->prev_enh,
                                     DMR_VOICE_UVQUALITY);
        else if (bp_key != 0u || bp_auto || ep_ks != NULL) {
            /* mbe_processAmbe3600x2450Framef() in its steps, the Basic
             * or Enhanced Privacy keystream taken off the corrected data
             * bits */
            errs = mbe_eccAmbe3600x2450C0(ambe_fr);
            mbe_demodulateAmbe3600x2450Data(ambe_fr);
            errs2 = errs + mbe_eccAmbe3600x2450Data(ambe_fr, ambe_d);
            if (bp_auto) {
                bp_key = s_bp_auto_found[slot];
                if (bp_key == 0u && errs2 <= 2) {
                    bp_key = dmr_bp_auto_feed(ambe_d, s_bp_votes[slot]);
                    if (bp_key != 0u) {
                        s_bp_auto_found[slot] = bp_key;
                        s_bp_last_found = bp_key;
                        printf("[DMR BP] TS%d clave probable %u "
                               "(3 silencios AMBE); verificar en CPS\n",
                               slot + 1, (unsigned)bp_key);
                    }
                }
                if (bp_key == 0u)
                    return;
            }
            if (ep_ks != NULL && kind == RAW_AMBE72) {
                /* NXDN / dPMR scrambler: every frame, as it was sent */
                for (int k = 0; k < 49; k++)
                    ambe_d[k] ^= (char)((ep_ks[k / 8] >> (7 - k % 8)) & 1u);
            } else if (ep_ks != NULL)
                dmrEp_frame(ambe_d, ep_ks, errs);
            else if (bp_hy)
                dmrHyBp_frame(ambe_d, frame);
            else if (bp_any)
                dmrBpAny_frame(ambe_d, bp_key, frame);
            else
                bp_descramble((uint8_t)bp_key, ambe_d);
            mbe_processAmbe2450Dataf(pcm, &errs, &errs2, err_str, ambe_d,
                                     &v->cur, &v->prev, &v->prev_enh,
                                     DMR_VOICE_UVQUALITY);
        } else
            mbe_processAmbe3600x2450Framef(pcm, &errs, &errs2, err_str,
                                           ambe_fr, ambe_d, &v->cur, &v->prev,
                                           &v->prev_enh, DMR_VOICE_UVQUALITY);
#else  /* lib/dmr_vocoder */
        uint8_t cframe[9];
        int16_t pcm16[160];
        int rc;

        if (dstar || imbe) {
            /* AMBE 3600x2400 and IMBE 7200x4400 are not in this decoder
             * (3600x2450 only): silence.  The mbelib build decodes them. */
            rc = -1;
        } else if (raw) {
            /* 49 data bits, FEC already removed by the caller (YSF) */
            uint8_t d[49];
            errs2 = dib[7] > 8 ? 8 : dib[7];
            for (int i = 0; i < 49; i++)
                d[i] = (uint8_t)((dib[i / 8] >> (7 - i % 8)) & 1u);
            rc = dmr_vocoder_decode_data(&v->voc, d, pcm16);
        } else {
            /* DMR (RAW_NONE) or NXDN (RAW_AMBE72) */
            if (kind == RAW_AMBE72)
                memcpy(cframe, dib, 9); /* already canonical A|B|C */
            else
                planes_to_frame(ambe_fr, cframe);
            if (bp_key != 0u || bp_auto || ep_ks != NULL) {
                /* FEC, then the keystream off the data bits, then the
                 * synthesis: mbelib's mbe_processAmbe3600x2450Framef()
                 * split in three */
                uint8_t d[49];
                errs2 = dmr_vocoder_unpack(cframe, d);
                errs = errs2; /* dmrEp_frame()'s c0_errors: total here */
                if (bp_auto) {
                    bp_key = s_bp_auto_found[slot];
                    if (bp_key == 0u && errs2 <= 2) {
                        bp_key = dmr_bp_auto_feed((char *)d, s_bp_votes[slot]);
                        if (bp_key != 0u) {
                            s_bp_auto_found[slot] = bp_key;
                            s_bp_last_found = bp_key;
                            printf("[DMR BP] TS%d clave probable %u "
                                   "(3 silencios AMBE); verificar en CPS\n",
                                   slot + 1, (unsigned)bp_key);
                        }
                    }
                    if (bp_key == 0u)
                        return;
                }
                if (ep_ks != NULL && kind == RAW_AMBE72) {
                    for (int k = 0; k < 49; k++)
                        d[k] ^= (uint8_t)((ep_ks[k / 8] >> (7 - k % 8)) & 1u);
                } else if (ep_ks != NULL)
                    dmrEp_frame((char *)d, ep_ks, errs);
                else if (bp_hy)
                    dmrHyBp_frame((char *)d, frame);
                else if (bp_any)
                    dmrBpAny_frame((char *)d, bp_key, frame);
                else
                    bp_descramble((uint8_t)bp_key, (char *)d);
                rc = dmr_vocoder_decode_data(&v->voc, d, pcm16);
            } else {
                int e = 0;
                rc = dmr_vocoder_decode_frame(&v->voc, cframe, pcm16, &e);
                errs2 = e;
            }
        }
        if (rc != 0)
            memset(pcm16, 0, sizeof(pcm16));
        for (int k = 0; k < 160; k++)
            pcm[k] = (float)pcm16[k] * (1.0f / 32768.0f);
#endif /* DMR_VOICE_ENABLED */
        us = dmr_port_cycles_to_us(dmr_port_cycles() - t0);
        {
            /* The vocoder's own time, without the threads that preempted
             * it: their difference is time spent waiting */
            const uint32_t own =
                dmr_port_cycles_to_us(dmr_port_thread_cycles() - own0);
            s_own_us_total += own;
            if (own > s_own_us_max)
                s_own_us_max = own;
        }
        if (kind == RAW_NONE || kind == RAW_AMBE72) {
            /* errs2: corrections in C0 and C1 together. Only the frames
             * with the 72 bit DMR coding count, as dmr_voice.h says: DMR
             * (RAW_NONE) and NXDN (RAW_AMBE72). The test used to be the
             * local bool raw, which is just "is this RAW_AMBE49", so IMBE
             * and D-STAR went into the same average and the BER printed
             * for P25 and D-STAR meant nothing. */
            s_fec_bits += (uint32_t)(errs2 > 0 ? errs2 : 0);
            s_fec_frames++;
        }
        s_frame_us_total += us;
        if (us > s_frame_us_max) {
            s_frame_us_max = us;
        }
        s_frames++;
    }

    {
        float e = 0.0f, rms;
        for (int k = 0; k < 160; k++) {
#ifdef DMR_VOICE_ENABLED
            pcm[k] *= OUT_SCALE; /* mbelib's scale; ours is already +-1 */
#endif
            e += pcm[k] * pcm[k];
        }
        rms = sqrtf(e / 160.0f);
        if (errs2 >= CONCEAL_MIN_ERRS && v->level > 0.0f
            && rms > CONCEAL_RATIO * v->level + CONCEAL_FLOOR) {
            const float g = (v->level + CONCEAL_FLOOR) / rms;
            for (int k = 0; k < 160; k++) {
                pcm[k] *= g;
            }
            s_concealed++;
        } else if (errs2 == 0) {
            v->level += LEVEL_ALPHA * (rms - v->level);
        }
        for (int k = 0; k < 160; k++) {
            pcm[k] = soft_limit(pcm[k]);
        }
    }
    if (s_sink != NULL) {
        s_sink(pcm, 160);
    }
}

void dmr_voice_get_debug(uint32_t *max_us, uint32_t *queue_drops)
{
    *max_us = s_frame_us_max;
    *queue_drops = s_queue_drops;
}

void dmr_voice_get_fec(uint32_t *bits, uint32_t *frames)
{
    *bits = s_fec_bits;
    *frames = s_fec_frames;
}

void dmr_voice_get_own(uint32_t *avg_us, uint32_t *max_us)
{
    const uint32_t n = s_frames;
    *avg_us = n ? (uint32_t)(s_own_us_total / n) : 0;
    *max_us = s_own_us_max;
}

bool dmr_voice_decode_next(void)
{
    vburst_t b;
    bool reset;

    mbe_cos_init(); /* before the first frame; once */
    DMR_LOCK(s_q_lock);
    reset = s_dec_reset;
    s_dec_reset = false;
    if (s_q_count == 0) {
        DMR_UNLOCK(s_q_lock);
        if (reset) {
            dec_init(0);
            dec_init(1);
        }
        return false;
    }
    b = s_queue[s_q_head];
    /* Taken, so nothing of it is left in the ring: the key of an encrypted
     * burst used to stay there until that slot was written over */
    memset(&s_queue[s_q_head], 0, sizeof(s_queue[s_q_head]));
    s_q_head = (s_q_head + 1) % QUEUE_BURSTS;
    s_q_count--;
    DMR_UNLOCK(s_q_lock);

    if (reset) {
        dec_init(0);
        dec_init(1);
    }
    if (s_bp_decoder_epoch != s_bp_scan_epoch) {
        memset(s_bp_votes, 0, sizeof(s_bp_votes));
        s_bp_auto_found[0] = s_bp_auto_found[1] = 0;
        s_bp_decoder_epoch = s_bp_scan_epoch;
    }
    if (b.bp_auto && (!s_bp_scan || b.bp_epoch != s_bp_scan_epoch))
        return true;
    if (b.new_call) {
        dec_init(b.slot); /* new call on this slot: fresh vocoder state */
    }
    
    /* Enhanced Privacy: the burst's 3 frames of keystream, 7 bytes each */
    uint8_t ks[3 * DMR_EP_FRAME_BYTES];
    const bool ep = !b.raw && b.ep;
    if (ep && b.ep_aes)
        dmrAes_keystream(b.aes_key, b.aes_len, (uint32_t)b.ep_mi,
                         (unsigned)b.ep_burst * sizeof(ks), ks, sizeof(ks));
    else if (ep && b.ep_hy)
        for (unsigned k = 0; k < 3u; k++)
            dmrHyEp_bits(b.ep_key, b.ep_mi, (unsigned)b.ep_burst * 3u + k,
                         &ks[k * DMR_EP_FRAME_BYTES]);
    else if (ep)
        dmrEp_keystream(b.ep_key, (uint32_t)b.ep_mi,
                        (unsigned)b.ep_burst * sizeof(ks), ks, sizeof(ks));
    for (int i = 0; i < (b.raw ? b.count : 3); i++) {
        /* A raw IMBE frame's own keystream (P25 AES), after its bits */
        const uint8_t *fks = ep ? &ks[i * DMR_EP_FRAME_BYTES]
                           : ((b.raw && ((b.rks >> i) & 1u))
                              ? &b.frames[i][18] : NULL);
        decode_frame(b.slot, b.frames[i], b.raw ? b.kinds[i] : RAW_NONE,
                     b.raw ? 0u : b.bp_key, b.bp_auto, fks,
                     !b.raw && b.bp_any, (unsigned)b.ep_burst * 3u + (unsigned)i,
                     !b.raw && b.bp_hy);
    }
    if (ep) {
        memset(ks, 0, sizeof(ks));
        memset(b.ep_key, 0, sizeof(b.ep_key));
        dmrAes_wipe(b.aes_key, sizeof(b.aes_key));
    }
    return true;
}

/* Queue a burst, dropping the oldest when the vocoder is not keeping up */
static void queue_push(const vburst_t *src)
{
    DMR_LOCK(s_q_lock);
    if (s_q_count == QUEUE_BURSTS) {
        /* Dropped, not taken: wipe it as take_burst() does, or an
         * encrypted burst leaves its key behind in the ring */
        memset(&s_queue[s_q_head], 0, sizeof(s_queue[s_q_head]));
        s_q_head = (s_q_head + 1) % QUEUE_BURSTS;
        s_q_count--;
        s_queue_drops++;
    }
    s_queue[(s_q_head + s_q_count) % QUEUE_BURSTS] = *src;
    s_q_count++;
    DMR_UNLOCK(s_q_lock);
}

/* System Fusion: raw frames gathered three per burst (receiver side) */
static EXT_RAM_BSS_ATTR vburst_t s_raw;
static uint32_t s_raw_last_ms;
static bool s_raw_started;

static void raw_reset(void)
{
    memset(&s_raw, 0, sizeof(s_raw));
    s_raw_started = false;
}

void dmr_voice_ambe49_flush(void)
{
    if (s_raw.count == 0)
        return;
    queue_push(&s_raw);
    s_raw.count = 0;
    s_raw.new_call = false;
}

/* Room for the next raw frame; a gap starts a new call */
static uint8_t *raw_next(uint8_t kind, uint32_t t_ms)
{
    if (!s_raw_started || (t_ms - s_raw_last_ms) > VOICE_TIMEOUT_MS) {
        dmr_voice_ambe49_flush();
        s_raw.new_call = true;
    }
    s_raw_started = true;
    s_raw_last_ms = t_ms;
    s_raw.slot = 0;
    s_raw.raw = kind;
    s_raw.kinds[s_raw.count] = kind;
    s_raw.rks &= (uint8_t)~(1u << s_raw.count);
    uint8_t *f = s_raw.frames[s_raw.count];
    memset(f, 0, 9);
    return f;
}

static void raw_done(void)
{
    if (++s_raw.count == 3)
        dmr_voice_ambe49_flush();
}

void dmr_voice_ambe49(const uint8_t bits[49], unsigned errs, uint32_t t_ms)
{
    uint8_t *f = raw_next(RAW_AMBE49, t_ms);
    for (int i = 0; i < 49; i++)
        if (bits[i])
            f[i / 8] |= (uint8_t)(0x80u >> (i % 8));
    f[7] = (uint8_t)(errs > 255 ? 255 : errs);
    raw_done();
}

void dmr_voice_ambe72(const uint8_t abc[9], uint32_t t_ms)
{
    uint8_t *f = raw_next(RAW_AMBE72, t_ms);
    memcpy(f, abc, 9);
    raw_done();
}

void dmr_voice_dstar72(const uint8_t bits[9], uint32_t t_ms)
{
    uint8_t *f = raw_next(RAW_DSTAR72, t_ms);
    memcpy(f, bits, 9);
    raw_done();
}

void dmr_voice_imbe144(const uint8_t bits[18], uint32_t t_ms)
{
    uint8_t *f = raw_next(RAW_IMBE144, t_ms);
    memcpy(f, bits, 18);
    raw_done();
}

void dmr_voice_ambe72_ks(const uint8_t abc[9], const uint8_t ks[7],
                         uint32_t t_ms)
{
    uint8_t *f = raw_next(RAW_AMBE72, t_ms);
    memcpy(f, abc, 9);
    memcpy(f + 18, ks, 7);
    s_raw.rks |= (uint8_t)(1u << s_raw.count);
    raw_done();
}

void dmr_voice_imbe144_ks(const uint8_t bits[18], const uint8_t ks[11],
                          uint32_t t_ms)
{
    uint8_t *f = raw_next(RAW_IMBE144, t_ms);
    memcpy(f, bits, 18);
    memcpy(f + 18, ks, 11);
    s_raw.rks |= (uint8_t)(1u << s_raw.count);
    raw_done();
}

void dmr_voice_burst(int slot, const uint8_t burst[132], int voice_idx,
                     uint32_t t_ms)
{
    vcall_t *c = &s_call[slot & 1];
    bool new_call = false;
    vburst_t *b;
    (void)voice_idx;

    slot &= 1;
    /* drop a stalled playing slot (AUTO holds it through the call's gaps) */
    dmr_voice_tick(t_ms);
    if (c->last_voice_ms == 0 || (t_ms - c->last_voice_ms) > VOICE_TIMEOUT_MS) {
        new_call = true;
    }
    c->last_voice_ms = t_ms;
    /* A call the filter refuses: neither played nor holding the speaker */
    if (s_blocked[slot]) {
        if (s_playing == slot)
            s_playing = -1;
        return;
    }

    if (s_playing < 0) {
        if (s_pref == DMR_VOICE_PREF_AUTO || s_pref == slot) {
            s_playing = slot;
        }
    }
    /* Encrypted: played only as Basic Privacy with a key set, or as
     * Enhanced Privacy with its key in the keyring */
    /* TYT's BP: off the coded frames here, then played as clear */
    uint8_t tytBurst[132];
    const bool tyt = c->enc && c->basic && !c->ep && s_bp_tyt
                     && s_bp_key != 0u;
    if (tyt) {
        memcpy(tytBurst, burst, sizeof(tytBurst));
        dmrBpTyt_burst(s_bp_key, tytBurst);
        burst = tytBurst;
    }
    /* Hytera Basic with its key set: like a BP key, by the frame's place */
    const bool hyb = c->enc && c->hbasic && !c->ep && dmrHyBp_present();
    const uint16_t key = tyt ? 0u
                       : (hyb ? 1u : ((c->enc && c->basic) ? s_bp_key : 0u));
    /* Anytone's keystream runs over the superframe: its place is needed */
    const bool any = !hyb && key != 0u && s_bp_anytone;
    if ((any || hyb) && (voice_idx < 0 || voice_idx > 5))
        return;
    const bool ep = c->enc && c->ep && voice_idx >= 0 && voice_idx <= 5;
    const bool auto_bp = s_bp_scan && c->enc && c->basic
                         && key == 0u && !ep;
    if (s_playing != slot || (c->enc && key == 0u && !ep && !auto_bp && !tyt)) {
        return; /* only the slot being listened to costs CPU */
    }
    if (ep) {
        /* This burst's superframe, by time: 6 bursts of 60 ms each, so a
         * superframe lost in a fade still moves the MI on */
        const uint32_t tA = t_ms - 60u * (uint32_t)voice_idx;
        if (!c->ep_started) {
            c->ep_started = true;
            c->ep_t0 = tA;
            c->ep_sf = 0;
            c->ep_mi = c->ep_mi0;
        }
        const uint32_t sf = (uint32_t)(tA - c->ep_t0 + 180u) / 360u;
        if (sf < c->ep_sf) {
            c->ep_sf = 0;
            c->ep_mi = c->ep_mi0;
        }
        if (sf - c->ep_sf > 1000u)
            return; /* lost track of it (6 minutes): stays muted */
        while (c->ep_sf < sf) {
            c->ep_mi = c->ep_hy ? dmrHyEp_nextMi(c->ep_mi)
                     : (c->ep_aes ? dmrAes_nextMi((uint32_t)c->ep_mi)
                                  : dmrEp_nextMi((uint32_t)c->ep_mi));
            c->ep_sf++;
        }
    }

    DMR_LOCK(s_q_lock);
    if (s_q_count == QUEUE_BURSTS) {
        /* The vocoder is not keeping up: drop the oldest burst, wiped as
         * take_burst() does so no key is left in the ring */
        memset(&s_queue[s_q_head], 0, sizeof(s_queue[s_q_head]));
        s_q_head = (s_q_head + 1) % QUEUE_BURSTS;
        s_q_count--;
        s_queue_drops++;
    }
    b = &s_queue[(s_q_head + s_q_count) % QUEUE_BURSTS];
    memcpy(b->frames[0], &burst[0], 36);
    memcpy(b->frames[1], &burst[36], 18);
    memcpy(&b->frames[1][18], &burst[78], 18);
    memcpy(b->frames[2], &burst[96], 36);
    b->slot = (uint8_t)slot;
    b->new_call = new_call;
    b->raw = RAW_NONE;
    b->count = 3;
    b->bp_key = key;
    b->bp_any = any;
    b->bp_hy = hyb;
    b->ep_burst = (uint8_t)(voice_idx >= 0 ? voice_idx : 0);
    b->bp_auto = auto_bp;
    b->bp_epoch = s_bp_scan_epoch;
    b->ep = ep;
    b->ep_aes = ep && c->ep_aes;
    b->ep_hy = ep && c->ep_hy;
    if (ep) {
        b->ep_burst = (uint8_t)voice_idx;
        memcpy(b->ep_key, c->ep_key, sizeof(b->ep_key));
        if (c->ep_aes) {
            memcpy(b->aes_key, c->aes_key, sizeof(b->aes_key));
            b->aes_len = c->aes_len;
        }
        b->ep_mi = c->ep_mi;
    }
    s_q_count++;
    DMR_UNLOCK(s_q_lock);
}
#endif
