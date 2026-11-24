/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_PROTO_H
#define DMR_PROTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR (ETSI TS 102 361-1/-2) burst-level decoding, phase 1: metadata only.
 * Handles the burst types an amateur listener meets on repeaters (BS
 * sourced, with CACH), hotspots and simplex (MS sourced / direct mode):
 *   - voice LC header / terminator (BPTC + RS(12,9)): call start / end, IDs
 *   - embedded LC in voice bursts B..E (BPTC 8x16 + CRC5): IDs, talker alias
 *   - CSBK (BPTC + CRC-CCITT), idle: activity only
 *   - packet data (data header + rate 1/2, 3/4, 1 blocks, dmr_data.c): IP/UDP
 *     datagrams; Motorola LRRP location reports (UDP 4001, dmr_lrrp.c) feed
 *     the same position fields as the GPS Info LC
 * The AMBE+2 voice frames are decoded by dmr_voice.c (optional, mbelib).
 */

typedef enum {
    DMR_SYNC_NONE = 0,
    DMR_SYNC_BS_VOICE,
    DMR_SYNC_BS_DATA,
    DMR_SYNC_MS_VOICE,
    DMR_SYNC_MS_DATA,
    DMR_SYNC_TS1_VOICE, /* TDMA direct mode, timeslot 1 */
    DMR_SYNC_TS1_DATA,
    DMR_SYNC_TS2_VOICE,
    DMR_SYNC_TS2_DATA,
    DMR_SYNC_COUNT
} dmr_sync_t;

/* One burst as sliced by the demodulator (dibit values 0..3, first
 * transmitted first; dibit 01 = +3, 00 = +1, 10 = -1, 11 = -3). */
typedef struct {
    dmr_sync_t sync;    /* sync found in this burst's centre, or NONE */
    bool bs;            /* BS sourced stream (has a CACH before each burst) */
    uint8_t cach[12];   /* valid when bs */
    uint8_t burst[132]; /* 264 bits */
    uint32_t t_ms; /* stream time of the burst (demodulator sample clock) */
} dmr_frame_t;

/* ---- Per-slot state, for the UI ------------------------------------------- */
typedef enum {
    DMR_ACT_NONE = 0, /* nothing heard on this slot yet / timed out */
    DMR_ACT_IDLE,     /* repeater idle bursts */
    DMR_ACT_VOICE,
    DMR_ACT_DATA,
    DMR_ACT_CSBK,
} dmr_activity_t;

#define DMR_ALIAS_MAX 32

/* Where a position came from */
#define DMR_GPS_NONE 0
#define DMR_GPS_LC 1   /* GPS Info LC in the embedded LC of voice */
#define DMR_GPS_LRRP 2 /* LRRP location report over packet data */

typedef struct {
    dmr_activity_t activity;
    int cc;                    /* colour code, -1 unknown */
    bool ids_valid;
    bool group;                /* group call (talkgroup) vs private call */
    uint32_t dst;              /* talkgroup or destination radio ID */
    uint32_t src;              /* source radio ID */
    char alias[DMR_ALIAS_MAX]; /* talker alias, "" if none */
    bool encrypted; /* LC service options privacy bit: voice not played */
    /* Encrypted, Motorola (FID 0x10) and no PI header heard this call:
     * Basic Privacy, played with a key set by hand (dmr_voice.h) */
    bool basic;
    /* Enhanced Privacy with its key ID in the keyring (dmr_ep.h): played */
    bool ep;
    uint8_t ep_key_id;
    /* PI header of the call, if one was heard: what kind of encryption it
     * announces, even without its key (the screen says it) */
    bool pi_seen;
    uint8_t pi_alg, pi_fid;
    /* Feature set ID of the call's LC (0x10 Motorola, 0x68 Hytera) */
    uint8_t lc_fid;
    /* GPS Info LC (FLCO 0x08, ETSI TS 102 361-2), sent in the embedded LC of
     * voice by some radios */
    bool gps_valid;
    float gps_lat, gps_lon; /* degrees, + = N / E */
    int gps_err_m;          /* position error bound in metres, -1 unknown */
    char gps_grid[7];       /* Maidenhead locator of that position */
    uint8_t gps_kind;       /* DMR_GPS_* */
    int16_t gps_speed_kmh;  /* LRRP only, -1 unknown */
    int16_t gps_dir;        /* LRRP only, heading in degrees, -1 unknown */
    int16_t gps_alt_m;      /* LRRP only, INT16_MIN unknown */
    uint32_t last_ms;       /* stream time of the last burst on this slot */
    uint32_t call_start_ms;
} dmr_slot_info_t;

/* Call log: one entry per (slot, src, dst) call, newest last. */
#define DMR_LOG_LEN 12
typedef struct {
    uint32_t t_ms;
    uint8_t slot; /* 1, 2, or 0 for MS/simplex */
    bool group;
    uint32_t dst, src;
    char alias[DMR_ALIAS_MAX];
    bool gps_valid;
    float gps_lat, gps_lon;
    char gps_grid[7];
    uint8_t gps_kind;      /* DMR_GPS_* */
    int16_t gps_speed_kmh; /* -1 unknown */
    int16_t gps_dir;       /* -1 unknown */
    bool data;    /* packet data (e.g. an LRRP report), not a voice call */
    uint32_t seq; /* increments per new entry (UI change detection) */
} dmr_log_entry_t;

/* Decoding statistics */
typedef struct {
    uint32_t frames, frames_valid;
    uint32_t lc_headers, terminators, emb_lc_ok, emb_lc_bad, csbk, idle, gps;
    /* packet data (copied from dmr_data.c) */
    uint32_t data_headers, data_packets, data_crc_bad, lrrp;
    /* voice bursts B..F: seen, EMB undecodable, EMB LCSS 0..3 */
    uint32_t voice_bf, emb_bad, lcss[4];
    /* embedded LCs decoded, by kind: voice LC, talker alias header and
     * blocks, anything else (GPS, vendor) */
    uint32_t elc_voice, elc_ta_hdr, elc_ta_blk, elc_other;
} dmr_proto_stats_t;

void dmr_proto_reset(void);

/* Processes one burst and grades it for the demodulator's lock logic:
 *   DMR_FRAME_OK       a polarity-sensitive FEC field decoded (slot type
 *                      Golay, EMB QR) - proof of a real, correctly sliced stream
 *   DMR_FRAME_NEUTRAL  proves nothing either way (voice sync burst A: the
 *                      voice sync is the exact complement of the data sync,
 *                      so on its own it cannot tell voice from inverted data)
 *   DMR_FRAME_BAD      nothing decoded
 * The CACH TACT is deliberately NOT used as proof: its Hamming(7,4) code
 * contains the all-ones word, so a complemented (inverted) TACT still
 * decodes - exactly the failure it would have to catch. */
#define DMR_FRAME_OK 1
#define DMR_FRAME_NEUTRAL 0
#define DMR_FRAME_BAD (-1)
int dmr_proto_frame(const dmr_frame_t *f);

/* Snapshot accessors (thread-safe on the ESP32-P4, see dmr_port.h).
 * slot_idx 0 = TS1 (or the single channel of an MS/simplex stream), 1 = TS2. */
void dmr_proto_get_slot(int slot_idx, dmr_slot_info_t *out);

/*
 * What an encrypted call is, as the screen and the log say it, never with
 * the key (the user's wish): "BP" or "EP" while it is deciphered with a key
 * typed by hand (returns true), else "EP", "AES", "DES", "HYTERA" (from its
 * PI header), "BP?" (Motorola, privacy bit and no PI header: inferred) or
 * "CIFRADA" (returns false: muted). Only for a slot whose call is
 * encrypted.
 */
bool dmr_proto_privacy_label(const dmr_slot_info_t *s, char *buf, size_t n);
int dmr_proto_get_log(
    dmr_log_entry_t out[DMR_LOG_LEN]); /* returns count, oldest first */
void dmr_proto_get_stats(dmr_proto_stats_t *out);

/* True when the stream is MS sourced (simplex / hotspot uplink): no
 * timeslots, everything is reported on slot index 0 and labelled "MS". */
bool dmr_proto_is_ms_mode(void);

/* Slot (0 = TS1, 1 = TS2) of the last frame, if it came from a repeater and
 * its slot phase is known (from the CACH). */
bool dmr_proto_bs_slot(int *slot);

/**
 * How many times the repeater slot phase has been established or turned
 * round since the last call, and clears the count. A phase that keeps being
 * re-set means the TS1 reference cannot be trusted, and with it neither can
 * the slot every transmitted burst lands in.
 */
uint32_t dmr_proto_phase_sets(void);

/* The demodulator flipped polarity: forget that the stream was proven, so no
 * voice is decoded until a burst validates again. */
void dmr_proto_unprove(void);

/* The demodulator (re)acquired a stream: the BS slot phase must be re-learned
 * from the next good CACH TACT instead of predicted by alternation. */
void dmr_proto_new_lock(void);

/* Maidenhead locator (6 characters + NUL) of a position in degrees. */
void dmr_latlon_to_grid(float lat, float lon, char out[7]);

/* Optional line-oriented event printer (host test / serial log). */
typedef void (*dmr_proto_print_fn)(const char *line);
void dmr_proto_set_printer(dmr_proto_print_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* DMR_PROTO_H */
