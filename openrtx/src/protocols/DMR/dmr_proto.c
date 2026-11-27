/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dmr_proto.h"
#include "dmr_data.h"
#include "dmr_sms.h"
#ifdef CONFIG_KISS
#include "core/packet_service.h"
#endif
#include "dmr_fec.h"
#include "dmr_port.h"
#include "dmr_track.h"
#include "dmr_voice.h"
#include "dmr_hytera.h"
#ifdef CONFIG_DMR_TRUNK
#include "dmr_trunk.h"
#endif

#include <stdio.h>
#include <string.h>

/* Slot type data types (ETSI TS 102 361-1, 9.3.6) */
enum {
    DT_PI_HEADER = 0,
    DT_VOICE_LC_HEADER = 1,
    DT_TERMINATOR_LC = 2,
    DT_CSBK = 3,
    DT_MBC_HEADER = 4,
    DT_MBC_CONT = 5,
    DT_DATA_HEADER = 6,
    DT_RATE_12 = 7,
    DT_RATE_34 = 8,
    DT_IDLE = 9,
    DT_RATE_1 = 10,
};

/* Full LC opcodes (FLCO) we act on */
enum {
    FLCO_GROUP_VOICE = 0x00,
    FLCO_UNIT_VOICE = 0x03,
    FLCO_TA_HEADER = 0x04,
    FLCO_TA_BLOCK1 = 0x05,
    FLCO_TA_BLOCK3 = 0x07,
    FLCO_GPS_INFO = 0x08,
};

/* CACH -> TACT: the 7 TACT bits are CACH bits 0,4,8,12,14,18,22 (ETSI TS
 * 102 361-1 Figure 9.6 interleaving), then Hamming(7,4): AT, TC, LCSS(2). */
static const uint8_t k_tact_pos[7] = { 0, 4, 8, 12, 14, 18, 22 };

#define TA_BITS (49 + 3 * 56)

typedef struct {
    dmr_slot_info_t info; /* what the UI sees */
    int voice_idx;        /* -1 not in voice; 0 = burst A ... 5 = F */
    int emb_bad_run;      /* consecutive B..F bursts whose EMB failed */
    bool call_open;       /* an LC for the current call has been logged */
    /* Stream time of the last burst that belongs to the open call. NOT the
     * same as info.last_ms, which is the last burst of any kind on this
     * slot: a repeater fills a free slot with Idle bursts every 60 ms
     * (ETSI TS 102 361-1 clause 4.6.1), so info.last_ms never goes stale
     * while the repeater is on the air and a timeout measured against it
     * can never fire. */
    uint32_t call_ms;
    bool pi;              /* a PI header this call: not Basic Privacy */
    bool priv_logged;     /* the call's encryption is on the log */
    uint8_t emb[128];     /* embedded LC fragments B..E */
    int emb_nfrag;
    int emb_guess;        /* fragments taken by position, EMB not decoded */
    /* talker alias assembly */
    int ta_format, ta_len, ta_char_bits;
    uint8_t ta_bits[TA_BITS];
    uint8_t ta_have; /* bit0 header, bit1..3 blocks 1..3 */
} slot_state_t;

/*
 * The per-slot state machine, in PSRAM (EXT_RAM_BSS_ATTR): a burst writes
 * it, the next burst and the UI snapshot read it, nothing touches it inside
 * the sample or symbol loops.  Cleared at boot (platform_init) and by
 * dmr_proto_reset().
 */
static EXT_RAM_BSS_ATTR slot_state_t s_slot[2];

/* Defined further down, used by the terminator and the idle burst */
static void end_open_call(int slot_idx);
static int s_last_slot;
static bool s_last_frame_bs; /* The last frame came from a repeater */
static bool s_direct_family; /* last sync was a TDMA direct mode one */
static bool
    s_ms_mode; /* MS sourced stream (simplex / hotspot uplink): no timeslots */
/* No voice reaches the vocoder until a burst has passed a polarity-sensitive
 * FEC check: while the demodulator is still on the wrong polarity, bursts are
 * misclassified (an inverted idle burst reads as a voice sync) and would be
 * played as garbage. */
static bool s_proven;
/*
 * Proof of a real, correctly sliced stream. Golay(20,8) decoding up to 3
 * errors accepts a third of random words, QR(16,7) with 2 a quarter: on
 * their own they kept an inverted or false lock alive. A burst proves the
 * stream when a CRC checked content in it (RS LC, CSBK CRC, data header CRC,
 * packet CRC-32, embedded LC checksum), when its slot type or EMB decoded
 * without error, or with one error and the colour code already proven.
 */
static int s_cc_ref = -1;     /* colour code of the last proving burst */
static bool s_burst_checked;  /* a CRC checked content in this burst */
static int s_burst_errors;    /* bits the slot type / EMB code corrected */
static int s_burst_cc;        /* colour code it carries */
/*
 * Whether the colour code a burst carries may be published.  The codes that
 * protect it (Golay(20,8) for the slot type, QR(16,7) for the embedded LC)
 * always land on some word, so a field with an error in it decodes onto a
 * *different* colour code - and the screen showed the colour code dancing
 * from burst to burst while the audio was perfectly fine.  A correction is
 * taken only when the colour code it lands on is the one already heard; with
 * nothing heard yet there is nothing to compare against and it is taken.
 * tests/unit/dmr_rx_status.cpp holds this down. */
static bool cc_publishable(int cc)
{
    return (s_burst_errors == 0) || (s_cc_ref < 0) || (cc == s_cc_ref);
}
/* BS slot tracking: timeslots strictly alternate every 30 ms, so the slot is
 * predicted by alternation and the CACH TACT only re-phases the prediction
 * when two consecutive TACTs agree against it. A single TACT with two bit
 * errors "corrects" to the wrong word; trusting it alone sent voice bursts to
 * the other slot and let that slot's idle burst cut the superframe short. */
static int s_tact_disagree;
static bool
    s_slot_phase_known; /* false right after (re)lock: two TACTs set it */
/* The phase a single TACT proposed after a (re)lock, waiting for a second
 * one to back it. -1 while there is no candidate. See the slot phase block
 * below for why one TACT is not enough. */
static int s_tact_cand;
/* How many times the slot phase has been established or turned round since
 * the last report. A repeater reference built on the wrong phase is 30 ms
 * out, which puts every transmitted burst in the other slot, so this is
 * worth seeing in the log. */
static uint32_t s_phase_sets;
static EXT_RAM_BSS_ATTR dmr_log_entry_t s_log[DMR_LOG_LEN];
static int s_log_count;
static uint32_t s_log_seq;
static dmr_proto_stats_t s_stats;
static dmr_proto_print_fn s_print;

/* UI snapshots */
DMR_LOCK_DECLARE(s_lock);
/* UI snapshots: same reasoning as s_slot, PSRAM as well */
static EXT_RAM_BSS_ATTR dmr_slot_info_t s_pub_slot[2];
static EXT_RAM_BSS_ATTR dmr_log_entry_t s_pub_log[DMR_LOG_LEN];
static int s_pub_log_count;
/* s_log_seq of the copy the UI holds: 0 until the first publish */
static uint32_t s_pub_log_seq;
static dmr_proto_stats_t s_pub_stats;

static void printf_line(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#include <stdarg.h>
static void printf_line(const char *fmt, ...)
{
    /*
     * The only caller of this file's printers is the rtx thread, through
     * dmr_proto_frame(), so one buffer serves every line.  It used to be
     * 160 bytes of that thread's stack per formatted line -- every CSBK,
     * data header, LRRP and alias -- on a stack that lives in DTCM at
     * 95.9 %.
     */
    static EXT_RAM_BSS_ATTR char buf[160];
    va_list ap;
    if (s_print == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_print(buf);
}

void dmr_proto_set_printer(dmr_proto_print_fn fn)
{
    s_print = fn;
}

void dmr_proto_reset(void)
{
    memset(s_slot, 0, sizeof(s_slot));
    for (int i = 0; i < 2; i++) {
        s_slot[i].voice_idx = -1;
        s_slot[i].info.cc = -1;
        s_slot[i].info.gps_speed_kmh = -1;
        s_slot[i].info.gps_dir = -1;
        s_slot[i].info.gps_alt_m = INT16_MIN;
        s_slot[i].info.gps_err_m = -1;
    }
    s_last_slot = 0;
    s_direct_family = false;
    s_ms_mode = false;
    s_proven = false;
    s_cc_ref = -1;
    s_tact_disagree = 0;
    s_slot_phase_known = false;
    s_tact_cand = -1;
    s_phase_sets = 0;
    memset(s_log, 0, sizeof(s_log));
    s_log_count = 0;
    memset(&s_stats, 0, sizeof(s_stats));
    dmr_voice_reset();
    dmr_data_reset();
    dmr_track_clear();

    DMR_LOCK(s_lock);
    s_pub_slot[0] = s_slot[0].info;
    s_pub_slot[1] = s_slot[1].info;
    memset(s_pub_log, 0, sizeof(s_pub_log));
    s_pub_log_count = 0;
    s_pub_log_seq = ++s_log_seq;
    s_pub_stats = s_stats;
    DMR_UNLOCK(s_lock);
}

/* ------------------------------------------------------------------------- */

static void burst_bits(const dmr_frame_t *f, uint8_t bits[264])
{
    for (int i = 0; i < 132; i++) {
        bits[2 * i] = (uint8_t)((f->burst[i] >> 1) & 1u);
        bits[2 * i + 1] = (uint8_t)(f->burst[i] & 1u);
    }
}

static bool is_voice_sync(dmr_sync_t s)
{
    return s == DMR_SYNC_BS_VOICE || s == DMR_SYNC_MS_VOICE
        || s == DMR_SYNC_TS1_VOICE || s == DMR_SYNC_TS2_VOICE;
}

static bool is_data_sync(dmr_sync_t s)
{
    return s == DMR_SYNC_BS_DATA || s == DMR_SYNC_MS_DATA
        || s == DMR_SYNC_TS1_DATA || s == DMR_SYNC_TS2_DATA;
}

/* 1 / 2 for timeslotted streams (repeater, direct mode), 0 for MS simplex. */
static int slot_label(int slot_idx)
{
    return s_ms_mode ? 0 : slot_idx + 1;
}

/* ---- Call log -------------------------------------------------------------- */

static void log_new_call(int slot_idx, uint32_t t_ms)
{
    const dmr_slot_info_t *si = &s_slot[slot_idx].info;
    dmr_log_entry_t *e;

    if (s_log_count == DMR_LOG_LEN) {
        memmove(&s_log[0], &s_log[1],
                (DMR_LOG_LEN - 1) * sizeof(dmr_log_entry_t));
        s_log_count--;
    }
    e = &s_log[s_log_count++];
    e->t_ms = t_ms;
    e->slot = (uint8_t)slot_label(slot_idx);
    e->group = si->group;
    e->dst = si->dst;
    e->src = si->src;
    snprintf(e->alias, sizeof(e->alias), "%s", si->alias);
    e->gps_valid = si->gps_valid;
    e->gps_lat = si->gps_lat;
    e->gps_lon = si->gps_lon;
    memcpy(e->gps_grid, si->gps_grid, sizeof(e->gps_grid));
    e->gps_kind = si->gps_kind;
    e->gps_speed_kmh = si->gps_speed_kmh;
    e->gps_dir = si->gps_dir;
    e->data = false;
    e->seq = ++s_log_seq;
}

static void log_update_gps(int slot_idx)
{
    const dmr_slot_info_t *si = &s_slot[slot_idx].info;
    for (int i = s_log_count - 1; i >= 0; i--) {
        if (s_log[i].slot == slot_label(slot_idx) && s_log[i].src == si->src) {
            s_log[i].gps_valid = true;
            s_log[i].gps_lat = si->gps_lat;
            s_log[i].gps_lon = si->gps_lon;
            memcpy(s_log[i].gps_grid, si->gps_grid, sizeof(s_log[i].gps_grid));
            s_log[i].gps_kind = si->gps_kind;
            s_log[i].gps_speed_kmh = si->gps_speed_kmh;
            s_log[i].gps_dir = si->gps_dir;
            s_log[i].seq = ++s_log_seq;
            return;
        }
    }
}

void dmr_latlon_to_grid(float lat, float lon, char out[7])
{
    float x = lon + 180.0f, y = lat + 90.0f;
    if (x < 0.0f)
        x = 0.0f;
    if (x >= 360.0f)
        x = 359.9999f;
    if (y < 0.0f)
        y = 0.0f;
    if (y >= 180.0f)
        y = 179.9999f;
    out[0] = (char)('A' + (int)(x / 20.0f));
    out[1] = (char)('A' + (int)(y / 10.0f));
    out[2] = (char)('0' + (int)(x / 2.0f) % 10);
    out[3] = (char)('0' + (int)y % 10);
    out[4] = (char)('a' + (int)((x - 2.0f * (int)(x / 2.0f)) * 12.0f));
    out[5] = (char)('a' + (int)((y - (int)y) * 24.0f));
    out[6] = '\0';
}

/* Two's complement field of n bits (MSB first) as a signed value. */
static int32_t sfield(const uint8_t *bits, int n)
{
    uint32_t v = dmr_bits_to_u32(bits, n);
    if (v & (1u << (n - 1))) {
        return (int32_t)v - (int32_t)(1u << n);
    }
    return (int32_t)v;
}

/*
 * GPS Info LC (ETSI TS 102 361-2, FLCO 0x08), 72 bits:
 *   0-15 PF/R/FLCO/FID, 16-19 reserved, 20-22 position error (2*10^n m, 7 =
 *   unknown), 23-47 longitude (25-bit two's complement, 360/2^25 deg/LSB),
 *   48-71 latitude (24-bit two's complement, 180/2^24 deg/LSB) - about 1-2 m.
 * Checked against dsd-fme's dmr_embedded_gps(); unlike it, the sign is undone
 * as exact two's complement (its "0x800001 - value" is one LSB off).
 */
static void apply_gps_lc(int slot_idx, const uint8_t lc[72], uint32_t t_ms)
{
    slot_state_t *st = &s_slot[slot_idx];
    int err = (int)dmr_bits_to_u32(&lc[20], 3);
    float lon = (float)sfield(&lc[23], 25) * (360.0f / 33554432.0f);
    float lat = (float)sfield(&lc[48], 24) * (180.0f / 16777216.0f);
    (void)t_ms;

    if (lat <= -90.0f || lat >= 90.0f || lon <= -180.0f || lon >= 180.0f) {
        return;
    }
    st->info.gps_valid = true;
    st->info.gps_lat = lat;
    st->info.gps_lon = lon;
    st->info.gps_err_m =
        (err == 7) ?
            -1 :
            (err == 0 ?
                 2 :
                 (err == 1 ?
                      20 :
                      (err == 2 ?
                           200 :
                           (err == 3 ? 2000
                                     : (err == 4 ? 20000
                                                 : (err == 5 ? 200000 : -1))))));
    /* 110 is "more than 200 km" (ETSI TS 102 361-2 table 7.24), which is
     * not a bound at all: reported as unknown, like 111, rather than as
     * +-200 km. */
    st->info.gps_kind = DMR_GPS_LC;
    st->info.gps_speed_kmh = -1;
    st->info.gps_dir = -1;
    st->info.gps_alt_m = INT16_MIN;
    dmr_latlon_to_grid(lat, lon, st->info.gps_grid);
    s_stats.gps++;
    log_update_gps(slot_idx);
    {
        char errs[24];
        if (st->info.gps_err_m < 0) {
            snprintf(errs, sizeof(errs), "error unknown");
        } else {
            snprintf(errs, sizeof(errs), "+-%d m", st->info.gps_err_m);
        }
        printf_line("        TS%d GPS %.5f%c %.5f%c  %s  %s",
                    slot_label(slot_idx), lat < 0 ? -lat : lat,
                    lat < 0 ? 'S' : 'N', lon < 0 ? -lon : lon,
                    lon < 0 ? 'W' : 'E', st->info.gps_grid, errs);
    }
}

static void log_update_alias(int slot_idx)
{
    const dmr_slot_info_t *si = &s_slot[slot_idx].info;
    dmr_track_set_alias(si->src, si->alias); /* map station list */
    for (int i = s_log_count - 1; i >= 0; i--) {
        if (s_log[i].slot == slot_label(slot_idx) && s_log[i].src == si->src) {
            snprintf(s_log[i].alias, sizeof(s_log[i].alias), "%s", si->alias);
            s_log[i].seq = ++s_log_seq;
            break;
        }
    }
}

/* ---- Encryption ------------------------------------------------------------- */

bool dmr_proto_privacy_label(const dmr_slot_info_t *s, char *buf, size_t n)
{
    if (s->ep) {
        /* Deciphered with a key of the keyrings: ARC4 or AES-256 */
        snprintf(buf, n, "%s",
                 (s->pi_seen && s->pi_fid == 0x68u) ? "HYTERA EP"
                 : (s->pi_seen && (s->pi_alg & 0xDFu) == 0x05u) ? "AES"
                 : (s->pi_seen && (s->pi_alg & 0xDFu) == 0x04u) ? "AES128"
                 : (s->pi_seen && (s->pi_alg & 0xDFu) == 0x02u) ? "DES"
                                                                : "EP");
        return true;
    }
    /* Hytera Basic with its key set (dmr_hytera.h): played */
    if (s->encrypted && !s->pi_seen && s->lc_fid == 0x68u
        && dmrHyBp_present()) {
        snprintf(buf, n, "%s", "HYTERA BP");
        return true;
    }
    if (s->basic && dmr_voice_get_bp_key() != 0u) {
        /* The air does not tell Motorola's BP from Anytone's: the key
         * set (the memory's) does */
        snprintf(buf, n, "%s", dmr_voice_get_bp_tyt() ? "TYT BP"
                               : (dmr_voice_get_bp_anytone() ? "ANYTONE" : "BP"));
        return true;
    }
    const char *kind = "CIFRADA";
    if (s->pi_seen) {
        const unsigned a = s->pi_alg & 0x07u;
        if (s->pi_fid == 0x68u)
            kind = "HYTERA";
        else if (s->pi_alg < 0x28u && a == 1u)
            kind = "EP";
        else if (s->pi_alg < 0x28u && (a == 4u || a == 5u))
            kind = "AES";
        else if (s->pi_alg < 0x28u && a == 2u)
            kind = "DES";
    } else if (s->lc_fid == 0x68u) {
        kind = "HYTERA"; /* Hytera's own LC, encrypted: only told */
    } else if (s->basic) {
        kind = "BP?";
    }
    snprintf(buf, n, "%s", kind);
    return false;
}

/* Once per encrypted call, when its kind is known: at its first voice (its
 * PI header, if any, came before), or at its embedded LC (late entry) */
static void log_privacy(int slot_idx, uint32_t t_ms)
{
    slot_state_t *st = &s_slot[slot_idx];
    if (!st->info.encrypted || st->priv_logged)
        return;
    st->priv_logged = true;
    char label[16];
    const bool plays = dmr_proto_privacy_label(&st->info, label,
                                               sizeof(label));
    printf_line("%7.3f TS%d cifrada: %s, %s", t_ms / 1000.0,
                slot_label(slot_idx), label,
                plays ? "se descifra con la clave puesta a mano"
                      : "en silencio (sin clave para ella)");
}

/* ---- Link control ----------------------------------------------------------- */

static uint32_t lc_u24(const uint8_t *b)
{
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
}

/* A group/private voice LC (from a voice header, terminator or embedded).
 * A terminator only confirms IDs: it never opens (or logs) a new call. */
static const char *ta_recall(uint32_t src); /* below, by ta_render() */

static void apply_voice_lc(int slot_idx, const uint8_t bytes[9], uint32_t t_ms,
                           const char *origin)
{
    const bool terminator = (origin[0] == 'T');
    slot_state_t *st = &s_slot[slot_idx];
    uint8_t flco = bytes[0] & 0x3Fu;
    bool group = (flco == FLCO_GROUP_VOICE);
    uint32_t dst = lc_u24(&bytes[3]);
    uint32_t src = lc_u24(&bytes[6]);
    bool changed = !st->info.ids_valid || st->info.src != src
                || st->info.dst != dst || st->info.group != group;
    const bool new_call = !terminator && (changed || !st->call_open);

    /* A different station is talking, or nothing valid was known yet:
     * the alias and the position of the previous one must go. The
     * first half of the old condition was redundant and the pair left
     * out the case of no valid ids with the same src value. */
    if (!st->info.ids_valid || st->info.src != src) {
        /* new talker: forget the previous one's alias and position, and
         * show the alias heard from this one before, if any (ta_recall()) */
        st->info.alias[0] = '\0';
        st->ta_have = 0;
        st->info.gps_valid = false;
        const char *known = ta_recall(src);
        if (known != NULL)
            snprintf(st->info.alias, sizeof(st->info.alias), "%s", known);
    }
    /* A call we can actually tell apart from the one a PI header
     * belonged to: the ids were known and they are different now */
    const bool otherCall = st->info.ids_valid
                        && (st->info.src != src || st->info.dst != dst
                            || st->info.group != group);
    if (new_call) {
        st->priv_logged = false;
        /* Not the PI of this very call. A PI header carries no ids, so
         * the first LC that follows one always looks like a new call;
         * clearing here threw away the Enhanced Privacy key the PI had
         * just installed, and the call decoded for one superframe and
         * then went to noise for the rest of it. */
        if (!st->pi || otherCall) {
            st->pi = false; /* its PI header comes after this LC */
            st->info.ep = false;
            st->info.pi_seen = false;
            dmr_voice_set_pi(slot_idx, 0, 0, 0);
        }
    }
    st->info.encrypted = (bytes[2] & 0x40u) != 0; /* service options: privacy */
    /* Motorola's feature set ID; Enhanced Privacy and AES send a PI header
     * between the voice header and the voice, Basic Privacy none */
    st->info.lc_fid = bytes[1];
    st->info.basic = st->info.encrypted && bytes[1] == 0x10u && !st->pi;
    dmr_voice_set_encrypted(slot_idx, st->info.encrypted, st->info.basic);
    /* Hytera Basic: its feature set ID, the privacy bit, no PI header */
    dmr_voice_set_hytera_basic(slot_idx, st->info.encrypted
                                         && bytes[1] == 0x68u && !st->pi);
    st->info.ids_valid = true;
    st->info.group = group;
    st->info.dst = dst;
    st->info.src = src;
    /* Heard or not, by the call filter (dmr_voice.h) */
    dmr_voice_set_call(slot_idx, group, dst, src);

    st->call_ms = t_ms; /* this burst belongs to the call */
    if (new_call) {
        st->call_open = true;
        st->info.call_start_ms = t_ms;
        log_new_call(slot_idx, t_ms);
        printf_line("%7.3f TS%d CC%d %-3s %s %s%u  SRC %u", t_ms / 1000.0,
                    slot_label(slot_idx), st->info.cc, origin,
                    group ? "GROUP" : "PRIVATE", group ? "TG " : "ID ",
                    (unsigned)dst, (unsigned)src);
    }
    /* Joined late: no header, the embedded LC says it is encrypted */
    if (origin[0] == 'E')
        log_privacy(slot_idx, t_ms);
}

/*
 * Talker aliases heard, by radio ID, for its later calls: many stations
 * (or the network for them) send it only at the start of some calls. On
 * 2026-11-16 EA3V (2143077) sent "EA3V TONI JN11AJ" at 01:59:42 and nothing
 * in his calls after, so the screen showed his ID only, while a DM-1801
 * kept showing the alias it had heard. Newest last, the oldest goes.
 */
#define TA_MEM 16
static EXT_RAM_BSS_ATTR struct {
    uint32_t src;
    char alias[DMR_ALIAS_MAX];
} s_ta_mem[TA_MEM];
static bool s_ta_mem_ok; /* PSRAM is not cleared at start: once, here */

static void ta_mem_init(void)
{
    if (!s_ta_mem_ok) {
        memset(s_ta_mem, 0, sizeof(s_ta_mem));
        s_ta_mem_ok = true;
    }
}

static void ta_remember(uint32_t src, const char *alias)
{
    ta_mem_init();
    if (src == 0 || alias[0] == '\0')
        return;
    int at = TA_MEM - 1;
    for (int i = 0; i < TA_MEM; i++) {
        if (s_ta_mem[i].src == src) {
            at = i;
            break;
        }
    }
    /* to the end, the others moved down */
    for (int i = at; i < TA_MEM - 1; i++)
        s_ta_mem[i] = s_ta_mem[i + 1];
    s_ta_mem[TA_MEM - 1].src = src;
    snprintf(s_ta_mem[TA_MEM - 1].alias, DMR_ALIAS_MAX, "%s", alias);
}

static const char *ta_recall(uint32_t src)
{
    ta_mem_init();
    for (int i = TA_MEM - 1; i >= 0; i--) {
        if (s_ta_mem[i].src == src && src != 0)
            return s_ta_mem[i].alias;
    }
    return NULL;
}

static void ta_render(slot_state_t *st, int slot_idx)
{
    /* contiguous bits available: header, then blocks in order */
    int avail = (st->ta_char_bits == 7) ? 49 : 48;
    char out[DMR_ALIAS_MAX];
    int n = 0, maxc, pos = 0;

    if (!(st->ta_have & 1u)) {
        return;
    }
    for (int b = 1; b <= 3 && (st->ta_have & (1u << b)); b++) {
        avail += 56;
    }
    maxc = avail / st->ta_char_bits;
    if (st->ta_len > 0 && st->ta_len < maxc) {
        maxc = st->ta_len;
    }
    for (int c = 0; c < maxc && n < DMR_ALIAS_MAX - 1;
         c++, pos += st->ta_char_bits) {
        uint32_t v = dmr_bits_to_u32(&st->ta_bits[pos], st->ta_char_bits);
        if (st->ta_char_bits == 16 && v > 0x7E) {
            v = '?';
        }
        if (v >= 0x20 && v < 0x7F) {
            out[n++] = (char)v;
        } else if (v >= 0xA0 && st->ta_format == 1) {
            out[n++] =
                '?'; /* ISO 8859 accented letters: keep the position visible */
        }
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = '\0';
    /*
     * The alias is sent again and again during a call, each time from its
     * header, so it comes back a block at a time: "EA4CQH", "EA4CQH Jose
     * L", "EA4CQH Jose Luis", then "EA4CQH" again (log of 2026-11-16
     * 02:52), and the screen showed the whole name for a second out of
     * three. The beginning of what is already shown changes nothing.
     */
    {
        const size_t lo = strlen(out);
        if (lo < strlen(st->info.alias)
            && strncmp(out, st->info.alias, lo) == 0)
            return;
    }
    if (strcmp(out, st->info.alias) != 0) {
        snprintf(st->info.alias, sizeof(st->info.alias), "%s", out);
        ta_remember(st->info.src, out);
        log_update_alias(slot_idx);
        printf_line("        TS%d talker alias \"%s\"", slot_label(slot_idx),
                    out);
    }
}

static void apply_emb_lc(int slot_idx, const uint8_t lc[72], uint32_t t_ms)
{
    slot_state_t *st = &s_slot[slot_idx];
    uint8_t b[9];
    uint8_t flco, fid;

    for (int i = 0; i < 9; i++) {
        b[i] = (uint8_t)dmr_bits_to_u32(&lc[i * 8], 8);
    }
    flco = b[0] & 0x3Fu;
    fid = b[1];

    if (flco == FLCO_GROUP_VOICE || flco == FLCO_UNIT_VOICE)
        s_stats.elc_voice++;
    else if (fid == 0 && flco == FLCO_TA_HEADER)
        s_stats.elc_ta_hdr++;
    else if (fid == 0 && flco >= FLCO_TA_BLOCK1 && flco <= FLCO_TA_BLOCK3)
        s_stats.elc_ta_blk++;
    else
        s_stats.elc_other++;

    if (flco == FLCO_GROUP_VOICE || flco == FLCO_UNIT_VOICE) {
        apply_voice_lc(slot_idx, b, t_ms, "EMB");
    } else if ((fid == 0 || fid == 0x68)
               && flco == FLCO_GPS_INFO) /* standard, or Hytera's FID */
    {
        apply_gps_lc(slot_idx, lc, t_ms);
    } else if (fid == 0 && flco == FLCO_TA_HEADER) {
        int fmt = (int)dmr_bits_to_u32(&lc[16], 2);
        st->ta_format = fmt;
        st->ta_len = (int)dmr_bits_to_u32(&lc[18], 5);
        st->ta_char_bits = (fmt == 0) ? 7 : (fmt == 3 ? 16 : 8);
        if (st->ta_char_bits == 7) {
            memcpy(st->ta_bits, &lc[23], 49);
        } else {
            memcpy(st->ta_bits, &lc[24], 48);
        }
        st->ta_have = 1u; /* a new header invalidates older blocks */
        ta_render(st, slot_idx);
    } else if (fid == 0 && flco >= FLCO_TA_BLOCK1 && flco <= FLCO_TA_BLOCK3
               && (st->ta_have & 1u)) {
        int blk = flco - FLCO_TA_HEADER; /* 1..3 */
        int base = (st->ta_char_bits == 7) ? 49 : 48;
        memcpy(&st->ta_bits[base + (blk - 1) * 56], &lc[16], 56);
        st->ta_have |= (uint8_t)(1u << blk);
        ta_render(st, slot_idx);
    }
}

/* ---- Packet data (LRRP) ------------------------------------------------------- */

/* An LRRP position goes to the newest log entry of this slot when that is
 * the same radio's call or report of the last two minutes (radios usually
 * report on PTT release), otherwise to a new "data" entry. */
#define LRRP_JOIN_MS 120000u

typedef struct {
    float lat, lon;
    char grid[7];
    int16_t speed_kmh, dir;
} lrrp_fix_t;

static void log_lrrp(int slot_idx, const dmr_packet_t *pk, const lrrp_fix_t *fx,
                     uint32_t t_ms)
{
    const dmr_slot_info_t *si = &s_slot[slot_idx].info;
    dmr_log_entry_t *e = NULL;

    for (int i = s_log_count - 1; i >= 0; i--) {
        if (s_log[i].slot == slot_label(slot_idx)) {
            if (s_log[i].src == pk->hdr.src
                && (t_ms - s_log[i].t_ms) < LRRP_JOIN_MS) {
                e = &s_log[i];
            }
            break;
        }
    }
    if (e == NULL) {
        if (s_log_count == DMR_LOG_LEN) {
            memmove(&s_log[0], &s_log[1],
                    (DMR_LOG_LEN - 1) * sizeof(dmr_log_entry_t));
            s_log_count--;
        }
        e = &s_log[s_log_count++];
        memset(e, 0, sizeof(*e));
        e->t_ms = t_ms;
        e->slot = (uint8_t)slot_label(slot_idx);
        e->group = pk->hdr.group;
        e->dst = pk->hdr.dst;
        e->src = pk->hdr.src;
        e->data = true;
        if (si->src == pk->hdr.src) {
            snprintf(e->alias, sizeof(e->alias), "%s", si->alias);
        }
    }
    e->gps_valid = true;
    e->gps_lat = fx->lat;
    e->gps_lon = fx->lon;
    memcpy(e->gps_grid, fx->grid, sizeof(e->gps_grid));
    e->gps_kind = DMR_GPS_LRRP;
    e->gps_speed_kmh = fx->speed_kmh;
    e->gps_dir = fx->dir;
    e->seq = ++s_log_seq;
}

static void apply_lrrp_position(int slot_idx, const dmr_packet_t *pk,
                                uint32_t t_ms)
{
    slot_state_t *st = &s_slot[slot_idx];
    const dmr_lrrp_t *l = &pk->lrrp;
    lrrp_fix_t fx;

    fx.lat = (float)l->lat;
    fx.lon = (float)l->lon;
    if (fx.lat <= -90.0f || fx.lat >= 90.0f || fx.lon <= -180.0f
        || fx.lon >= 180.0f) {
        return;
    }
    dmr_latlon_to_grid(fx.lat, fx.lon, fx.grid);
    fx.speed_kmh = l->has_speed ? (int16_t)(l->speed_mps * 3.6f + 0.5f) : -1;
    fx.dir = l->has_dir ? (int16_t)l->dir_deg : -1;

    /* The stations heard with a position (dmr_track.h) */
    dmr_track_add(pk->hdr.src, fx.lat, fx.lon, fx.speed_kmh, fx.dir,
                  (st->info.ids_valid && st->info.src == pk->hdr.src) ?
                      st->info.alias :
                      NULL);

    /* the slot card follows the reporting radio unless someone else is
     * talking on this slot right now */
    if (st->info.activity == DMR_ACT_VOICE && st->call_open
        && st->info.src != pk->hdr.src) {
        log_lrrp(slot_idx, pk, &fx, t_ms);
        return;
    }
    if (!st->info.ids_valid || st->info.src != pk->hdr.src) {
        st->info.alias[0] = '\0';
        st->ta_have = 0;
        st->info.encrypted = false;
        st->info.basic = false;
        st->info.ep = false;
        st->info.pi_seen = false;
    }
    st->info.ids_valid = true;
    st->info.group = pk->hdr.group;
    st->info.dst = pk->hdr.dst;
    st->info.src = pk->hdr.src;
    st->info.gps_valid = true;
    st->info.gps_lat = fx.lat;
    st->info.gps_lon = fx.lon;
    st->info.gps_err_m = l->has_radius ? (int)(l->radius_m + 0.5f) : -1;
    st->info.gps_kind = DMR_GPS_LRRP;
    st->info.gps_speed_kmh = fx.speed_kmh;
    st->info.gps_dir = fx.dir;
    st->info.gps_alt_m = (l->has_alt && l->alt_m > -30000.0f
                          && l->alt_m < 30000.0f) ?
                             (int16_t)l->alt_m :
                             INT16_MIN;
    memcpy(st->info.gps_grid, fx.grid, sizeof(fx.grid));
    log_lrrp(slot_idx, pk, &fx, t_ms);
}

static void apply_packet(int slot_idx, const dmr_packet_t *pk, uint32_t t_ms)
{
#ifdef CONFIG_KISS
    /*
     * The packet modem's DMR IP mode: the user data of the call goes to the
     * host as one KISS frame, whether it is a UDP datagram or the raw octets.
     * See core/packet_service.h and docs/KISS.md.
     */
    if (pk->udp && (pk->data != NULL) && (pk->udp_len > 0))
        packet_service_dmrPayload(pk->data + pk->udp_off, (size_t)pk->udp_len);
#endif

    /* A text message (dmr_sms.c prints it) */
    if (dmrSms_rxPacket(pk, t_ms))
        return;
    const dmr_data_header_t *h = &pk->hdr;
    const int ts = slot_label(slot_idx);
    const int cc = s_slot[slot_idx].info.cc;

    if (!pk->lrrp_valid) {
        if (pk->udp) {
            printf_line(
                "%7.3f TS%d CC%d DATA SRC %u -> %s%u  UDP %u->%u (id %u->%u) %d bytes",
                t_ms / 1000.0, ts, cc, (unsigned)h->src,
                h->group ? "TG " : "ID ", (unsigned)h->dst, pk->src_port,
                pk->dst_port, pk->src_port_id, pk->dst_port_id, pk->udp_len);
        } else {
            printf_line("%7.3f TS%d CC%d DATA SRC %u -> %s%u  SAP %s, %d bytes",
                        t_ms / 1000.0, ts, cc, (unsigned)h->src,
                        h->group ? "TG " : "ID ", (unsigned)h->dst,
                        dmr_sap_name(h->sap), pk->len);
        }
        return;
    }

    {
        const dmr_lrrp_t *l = &pk->lrrp;
        printf_line("%7.3f TS%d CC%d LRRP %s  SRC %u -> %s%u%s", t_ms / 1000.0,
                    ts, cc, dmr_lrrp_doc_name(l->doc_id), (unsigned)h->src,
                    h->group ? "TG " : "ID ", (unsigned)h->dst,
                    h->dpf == DMR_DPF_CONFIRMED ? "  (confirmed)" : "");
        if (l->kind == DMR_LRRP_REPORT && l->has_pos) {
            char extra[80];
            int n = 0;
            apply_lrrp_position(slot_idx, pk, t_ms);
            extra[0] = '\0';
            if (l->has_radius && n < (int)sizeof(extra)) {
                n += snprintf(&extra[n], sizeof(extra) - (size_t)n,
                              "  +-%.0f m", l->radius_m);
            }
            if (l->has_alt && n < (int)sizeof(extra)) {
                n += snprintf(&extra[n], sizeof(extra) - (size_t)n,
                              "  alt %.0f m", l->alt_m);
            }
            if (l->has_speed && n < (int)sizeof(extra)) {
                n += snprintf(&extra[n], sizeof(extra) - (size_t)n,
                              "  %.1f km/h", l->speed_mps * 3.6f);
            }
            if (l->has_dir && n < (int)sizeof(extra)) {
                n += snprintf(&extra[n], sizeof(extra) - (size_t)n, "  %d deg",
                              l->dir_deg);
            }
            if (l->has_time && n < (int)sizeof(extra)) {
                snprintf(&extra[n], sizeof(extra) - (size_t)n,
                         "  %02u:%02u:%02uZ", l->hour, l->min, l->sec);
            }
            char grid[7];
            dmr_latlon_to_grid((float)l->lat, (float)l->lon, grid);
            printf_line("        TS%d LRRP %.5f%c %.5f%c  %s%s", ts,
                        l->lat < 0 ? -l->lat : l->lat, l->lat < 0 ? 'S' : 'N',
                        l->lon < 0 ? -l->lon : l->lon, l->lon < 0 ? 'W' : 'E',
                        grid, extra);
        } else if (l->kind == DMR_LRRP_REQUEST && l->interval_s >= 0) {
            printf_line("        TS%d LRRP periodic request, every %d s", ts,
                        l->interval_s);
        } else if (l->result > 0) {
            printf_line("        TS%d LRRP result code %d (no position)", ts,
                        l->result);
        }
    }
}

/* ---- Burst handlers ------------------------------------------------------------ */

static bool handle_data_burst(int slot_idx, const uint8_t bits[264],
                              uint32_t t_ms)
{
    slot_state_t *st = &s_slot[slot_idx];
    uint8_t stbits[20], info[196], data[96], bytes[12];
    uint32_t cw;
    int cc, dt;

    memcpy(stbits, &bits[98], 10);
    memcpy(&stbits[10], &bits[156], 10);
    cw = dmr_bits_to_u32(stbits, 20);
    s_burst_errors = dmr_golay_20_8_correct(&cw);
    if (s_burst_errors < 0) {
        return false;
    }
    cc = (int)((cw >> 16) & 0xFu);
    dt = (int)((cw >> 12) & 0xFu);
    s_burst_cc = cc;
    if (cc_publishable(cc))
        st->info.cc = cc;
    st->voice_idx = -1;

    memcpy(info, bits, 98);
    memcpy(&info[98], &bits[166], 98);

#ifdef CONFIG_DMR_TRUNK
    /* DMR Tier III (trunk/dmr_trunk.h): the control channel's blocks, a
     * call's header or terminator as one of its bursts, anything else as
     * activity on the slot. The engine checks every CRC itself. */
    if (dmrTrunk_running()) {
        if (dt == DT_CSBK || dt == DT_MBC_HEADER || dt == DT_MBC_CONT
            || dt == DT_DATA_HEADER || dt == DT_RATE_12) {
            uint8_t tbits[96], tbytes[12];
            (void)dmr_bptc196_decode(info, tbits);
            for (int i = 0; i < 12; i++)
                tbytes[i] = (uint8_t)dmr_bits_to_u32(&tbits[8 * i], 8);
            dmrTrunk_block(slot_idx, dt, cc, tbytes);
        } else if (dt == DT_VOICE_LC_HEADER || dt == DT_TERMINATOR_LC) {
            dmrTrunk_voice(slot_idx, cc);
        } else {
            dmrTrunk_activity(slot_idx);
        }
    }
#endif

    if (dt != DT_DATA_HEADER && dt != DT_RATE_12 && dt != DT_RATE_34
        && dt != DT_RATE_1) {
        dmr_data_interrupt(
            slot_idx); /* a packet's blocks follow its header back to back */
    }

    switch (dt) {
        case DT_VOICE_LC_HEADER:
        case DT_TERMINATOR_LC:
            /* BPTC leftovers are not fatal: the RS(12,9) behind it corrects one
         * more byte and then has to verify all three syndromes. */
            (void)dmr_bptc196_decode(info, data);
            {
                for (int i = 0; i < 12; i++) {
                    bytes[i] = (uint8_t)dmr_bits_to_u32(&data[i * 8], 8);
                }
                if (dmr_rs_12_9_decode(bytes, dt == DT_VOICE_LC_HEADER ?
                                                  DMR_RS_MASK_VOICE_HEADER :
                                                  DMR_RS_MASK_TERMINATOR)) {
                    uint8_t flco = bytes[0] & 0x3Fu;
                    s_burst_checked = true;
                    if (dt == DT_VOICE_LC_HEADER) {
                        s_stats
                            .lc_headers++; /* repeated headers of one call log it once */
                    } else {
                        s_stats.terminators++;
                    }
                    if (flco == FLCO_GROUP_VOICE || flco == FLCO_UNIT_VOICE) {
                        apply_voice_lc(slot_idx, bytes, t_ms,
                                       dt == DT_VOICE_LC_HEADER ? "VLC" :
                                                                  "TLC");
                    }
                }
            }
            if (dt == DT_TERMINATOR_LC) {
                if (st->call_open) {
                    printf_line("%7.3f TS%d end of call (%.1f s)",
                                t_ms / 1000.0, slot_label(slot_idx),
                                (t_ms - st->info.call_start_ms) / 1000.0);
                }
                end_open_call(slot_idx);
                st->info.activity = DMR_ACT_IDLE;
            } else {
                st->info.activity = DMR_ACT_VOICE;
            }
            break;

        case DT_CSBK: {
            const int bptc = dmr_bptc196_decode(info, data);
            uint16_t crc = (uint16_t)dmr_bits_to_u32(&data[80], 16);
            const bool crcOk =
                (dmr_crc_ccitt(data, 80) ^ DMR_CRC_MASK_CSBK) == crc;
            if (bptc == 0 && crcOk) {
                s_stats.csbk++;
                s_burst_checked = true;
            }
            /* Every CSBK, as bytes: to compare another radio's (a repeater
             * wake-up, BS_Dwn_Act) with ours */
            uint8_t b[12];
            for (int i = 0; i < 12; i++)
                b[i] = (uint8_t)dmr_bits_to_u32(&data[8 * i], 8);
            printf_line("%7.3f CSBK TS%d CC%d %02X %02X %02X %02X %02X %02X "
                        "%02X %02X %02X %02X %02X %02X (bptc %d, crc %s)",
                        t_ms / 1000.0, slot_label(slot_idx), cc,
                        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                        b[8], b[9], b[10], b[11], bptc, crcOk ? "ok" : "mal");
            }
            st->info.activity = DMR_ACT_CSBK;
            break;

        case DT_PI_HEADER: {
            /* Privacy indicator: algorithm, feature set ID, key ID and
             * message indicator (MI) of the call that follows. Motorola
             * ARC4 (Enhanced Privacy) is played when the keyring typed by
             * hand has that key ID (dmr_ep.h); anything else stays muted */
            const int bptc = dmr_bptc196_decode(info, data);
            const uint16_t crc = (uint16_t)dmr_bits_to_u32(&data[80], 16);
            /* Hytera's (feature set ID 0x68) has a checksum of its own in
             * byte 9 instead of the CRC (dmr_hytera.h) */
            uint8_t pib[10];
            for (int k = 0; k < 10; k++)
                pib[k] = (uint8_t)dmr_bits_to_u32(&data[8 * k], 8);
            const bool hyPi = pib[1] == 0x68u && dmrHyEp_piOk(pib);
            if (bptc == 0
                && ((dmr_crc_ccitt(data, 80) ^ DMR_CRC_MASK_PI) == crc
                    || hyPi)) {
                const uint8_t alg = (uint8_t)dmr_bits_to_u32(&data[0], 8);
                const uint8_t fid = (uint8_t)dmr_bits_to_u32(&data[8], 8);
                const uint8_t kid = (uint8_t)dmr_bits_to_u32(&data[16], 8);
                const uint32_t mi = dmr_bits_to_u32(&data[24], 32);
                s_burst_checked = true;
                /* The headers repeat: the first one of the call counts */
                if (!st->pi) {
                    const uint64_t mi40 = ((uint64_t)pib[3] << 32)
                                          | ((uint64_t)pib[4] << 24)
                                          | ((uint64_t)pib[5] << 16)
                                          | ((uint64_t)pib[6] << 8) | pib[7];
                    const bool ep =
                        (fid == 0x10u
                         && dmr_voice_set_pi(slot_idx, alg, kid, mi))
                        || (hyPi
                            && dmr_voice_set_pi_hytera(slot_idx, alg, kid,
                                                       mi40));
                    st->info.ep = ep;
                    st->info.ep_key_id = kid;
                    st->info.pi_seen = true;
                    st->info.pi_alg = alg;
                    st->info.pi_fid = fid;
                    printf_line("%7.3f TS%d CC%d PI header: algorithm %02X "
                                "FID %02X key %02X MI %08X: %s",
                                t_ms / 1000.0, slot_label(slot_idx), cc,
                                (unsigned)alg, (unsigned)fid, (unsigned)kid,
                                (unsigned)mi,
                                ep ? (hyPi ? "Hytera Enhanced, key in the keyring"
                                      : (alg & 0xDFu) == 0x05u
                                          ? "AES-256, key in the keyring"
                                      : (alg & 0xDFu) == 0x04u
                                          ? "AES-128, key in the keyring"
                                      : (alg & 0xDFu) == 0x02u
                                          ? "DES, key in the keyring"
                                          : "Enhanced Privacy, key in the keyring")
                                   : "muted (no key for it)");
                }
                st->pi = true;
                /* It opens the call as well, or nothing ever closes it:
                 * the idle burst and the timeout both test call_open, and
                 * a transmission that loses its terminator to a fade used
                 * to leave this slot marked encrypted, with its key, for
                 * good -- and the next call on it came out as noise. */
                if (!st->call_open) {
                    st->call_open = true;
                    st->info.call_start_ms = t_ms;
                }
                st->call_ms = t_ms;
                /* A PI header is itself the statement that the call is
                 * encrypted. Taking this only from the voice LC header
                 * meant that a call whose PI decoded and whose LC did not
                 * played its encrypted AMBE as noise. */
                st->info.encrypted = true;
                st->info.basic = false;
                dmr_voice_set_encrypted(slot_idx, st->info.encrypted, false);
                dmr_voice_set_hytera_basic(slot_idx, false);
            }
            st->info.activity = DMR_ACT_VOICE;
            break;
        }

        case DT_IDLE:
            s_stats.idle++;
            /*
             * REMOVED 2026-11-15: this used to close an open call, on the
             * grounds that the BS only sends Idle on a slot it has nothing
             * to send on (ETSI TS 102 361-1 clause 4.6.1). Two reasons it
             * is gone:
             *  - it is redundant. The timeout measures against call_ms,
             *    which only voice and LC bursts refresh, never an Idle, so
             *    a call that lost its terminator closes by itself even
             *    while the repeater keeps filling the other slot.
             *  - it was dangerous. One burst attributed to the wrong slot
             *    by a mis-corrected TACT closed a LIVE call, and the next
             *    header logged the same conversation again; with twelve
             *    entries the call log filled up with duplicates of one
             *    call, which is how it got reported: "the log does not
             *    work any more".
             */
            st->voice_idx = -1;
            st->info.activity = DMR_ACT_IDLE;
            break;

        case DT_DATA_HEADER: {
            dmr_data_header_t h;
            (void)dmr_bptc196_decode(info, data); /* the header CRC decides */
            if (dmr_data_header(slot_idx, data, t_ms, &h)) {
                s_burst_checked = true;
                if (h.dpf != DMR_DPF_RESPONSE) {
                    printf_line(
                        "%7.3f TS%d CC%d data header DPF %u SAP %s  SRC %u -> %s%u  blocks %d%s",
                        t_ms / 1000.0, slot_label(slot_idx), cc, h.dpf,
                        dmr_sap_name(h.sap), (unsigned)h.src,
                        h.group ? "TG " : "ID ", (unsigned)h.dst, h.btf,
                        h.dpf == DMR_DPF_CONFIRMED ?
                            (h.full ? " conf" : " conf retry") :
                            "");
                }
            }
            st->info.activity = DMR_ACT_DATA;
            break;
        }

        case DT_RATE_12:
        case DT_RATE_34:
        case DT_RATE_1: {
            dmr_packet_t pk;
            int r = dmr_data_block(slot_idx, dt, info, t_ms, &pk);
            if (r == DMR_DATA_PACKET) {
                s_burst_checked = true;
                apply_packet(slot_idx, &pk, t_ms);
            } else if (r == DMR_DATA_CRC_BAD) {
                printf_line("%7.3f TS%d data packet CRC-32 error",
                            t_ms / 1000.0, slot_label(slot_idx));
            }
            st->info.activity = DMR_ACT_DATA;
            break;
        }

        default:
            st->info.activity = DMR_ACT_DATA;
            break;
    }
    return true;
}

static bool handle_voice_nosync(int slot_idx, const uint8_t burst[132],
                                const uint8_t bits[264], uint32_t t_ms)
{
    slot_state_t *st = &s_slot[slot_idx];
    uint8_t emb[16];
    uint32_t cw;
    int lcss;

    /* B..F follow a detected burst A. (Tried: treating a sync-less burst
     * after F as the next A. It only helped at ~5 dB SNR and made the decoder
     * play non-voice bursts at the end of overs - reverted. The real cause of
     * the lost superframes was the slot assignment, see s_tact_disagree.) */
    st->voice_idx++;
    if (st->voice_idx > 5) {
        st->voice_idx = -1; /* no new burst A: the call paused or ended */
        return false;
    }
    /* the AMBE frames carry their own FEC: decode them even if the EMB fails */
    if (s_proven) {
        dmr_voice_burst(slot_idx, burst, st->voice_idx, t_ms);
    }

    memcpy(emb, &bits[108], 8);
    memcpy(&emb[8], &bits[148], 8);
    cw = dmr_bits_to_u32(emb, 16);
    s_burst_errors = dmr_qr_16_7_correct(&cw);
    s_stats.voice_bf++;
    const bool emb_ok = s_burst_errors >= 0;
    lcss = -1;
    if (!emb_ok) {
        s_stats.emb_bad++;
        if (++st->emb_bad_run >= 3) {
            st->voice_idx =
                -1; /* three undecodable EMBs in a row: the call has gone */
            st->emb_nfrag = 0;
            return false;
        }
    } else {
        st->emb_bad_run = 0;
        /* Not published when the word only decoded with a correction that
         * moved its colour code off the one being heard (cc_publishable) */
        if (cc_publishable((int)((cw >> 12) & 0xFu))) {
            st->info.cc = (int)((cw >> 12) & 0xFu);
            s_burst_cc = st->info.cc;
        }
        lcss = (int)((cw >> 9) & 0x3u);
        s_stats.lcss[lcss]++;
        st->info.activity = DMR_ACT_VOICE;
    }

    /* Embedded LC: bursts B, C, D and E carry its four fragments (LCSS 1,
     * 3, 3, 2; F has a single one). The burst position says which fragment
     * it is, so a burst whose EMB failed still gives its fragment: one per
     * LC at most, and the row Hamming codes and the checksum of the LC
     * reject it if it is bad. Losing the whole LC for one bad EMB made the
     * talker alias, sent over several LCs, almost never complete. */
    static const int8_t expect[5] = { -1, 1, 3, 3, 2 };
    const int p = st->voice_idx; /* 1..4 = B..E */
    if (p >= 1 && p <= 4) {
        if (p == 1) {
            st->emb_nfrag = 0;
            st->emb_guess = 0;
        }
        const bool fits = emb_ok ? (lcss == expect[p]) : (st->emb_guess == 0);
        if (fits && st->emb_nfrag == p - 1) {
            if (!emb_ok) {
                st->emb_guess++;
            }
            memcpy(&st->emb[32 * (p - 1)], &bits[116], 32);
            st->emb_nfrag++;
            if (st->emb_nfrag == 4) {
                uint8_t lc[72];
                st->emb_nfrag = 0;
                if (dmr_emb_lc_decode(st->emb, lc)) {
                    s_stats.emb_lc_ok++;
                    s_burst_checked = emb_ok && st->emb_guess == 0;
                    apply_emb_lc(slot_idx, lc, t_ms);
                } else {
                    s_stats.emb_lc_bad++;
                }
            }
        } else {
            st->emb_nfrag = 0; /* out of sequence, or not an LC */
        }
    }
    return emb_ok;
}

/* ---- Entry point ---------------------------------------------------------------- */

/* No burst on a slot for this long: whatever call was open on it ended
 * without a terminator (a voice call puts one burst every 60 ms on its own
 * slot, so two seconds is forty missed in a row). Without this the call
 * stayed open for ever: the same station calling the same talkgroup again
 * was never logged as a new call, and the "end of call" line measured from
 * the start of the previous one. */
#define CALL_OPEN_TIMEOUT_MS 2000u
/* And after this long with nothing at all, the slot has no activity to
 * show. dmr_proto.h documents DMR_ACT_NONE as "nothing heard on this slot
 * yet / timed out" and nothing ever timed it out. */
#define SLOT_IDLE_TIMEOUT_MS 60000u

/* Everything the terminator does, minus the log line: used as well when
 * the call is closed because the slot went idle or quiet, so that no state
 * of it colours the next one. The vocoder's own copy of the encryption has
 * to go too, or a later late entry in clear gets descrambled with the old
 * key and comes out as noise. */
static void end_open_call(int slot_idx)
{
    slot_state_t *a = &s_slot[slot_idx];
    a->call_open = false;
    a->pi = false;
    a->priv_logged = false;
    a->info.ep = false;
    a->info.pi_seen = false;
    /* These two are what the screen and log_privacy() read. Left set, a
     * finished call kept painting CIFRADA for the whole idle timeout and
     * the next call to come in late and in clear was announced as
     * encrypted while its audio played perfectly. */
    a->info.encrypted = false;
    a->info.basic = false;
    /* Nothing half assembled survives either, so the three closing paths
     * (terminator, idle burst and timeout) leave the same state */
    a->voice_idx = -1;
    a->emb_nfrag = 0;
    a->emb_guess = 0;
    a->emb_bad_run = 0;
    dmr_voice_call_end(slot_idx);
}

static void age_slots(uint32_t now_ms)
{
    for (int i = 0; i < 2; i++) {
        slot_state_t *a = &s_slot[i];
        if (a->info.activity == DMR_ACT_NONE && !a->call_open) {
            continue;
        }
        /* t_ms is the sample counter divided by 48, so it wraps back to
         * zero at 0x100000000/48 ms (about 24 h 51 min) and not at the top
         * of a uint32_t: an unsigned difference across that point gives
         * about 48 days. A stamp ahead of now is that wrap: pull the stamps
         * forward instead of freezing them, or the slot that is not
         * receiving never ages again. */
        if (now_ms < a->info.last_ms) {
            a->info.last_ms = now_ms;
        }
        if (now_ms < a->call_ms) {
            a->call_ms = now_ms;
        }
        if (now_ms < a->info.call_start_ms) {
            /* the third stamp of the same clock: without this the end of
             * call line prints about 48 days once every 24 h 51 min */
            a->info.call_start_ms = now_ms;
        }
        if (a->call_open && (now_ms - a->call_ms) > CALL_OPEN_TIMEOUT_MS) {
            end_open_call(i);
            a->info.activity = DMR_ACT_IDLE;
        }
        if ((now_ms - a->info.last_ms) > SLOT_IDLE_TIMEOUT_MS) {
            a->info.activity = DMR_ACT_NONE;
        }
    }
}

int dmr_proto_frame(const dmr_frame_t *f)
{
    uint8_t bits[264];
    int slot_idx;
    int grade = DMR_FRAME_BAD;
    slot_state_t *st;

    age_slots(f->t_ms);
    s_stats.frames++;
    s_last_frame_bs = false; /* Set below once its slot is known */
    burst_bits(f, bits);

    if (f->sync == DMR_SYNC_TS1_VOICE || f->sync == DMR_SYNC_TS1_DATA
        || f->sync == DMR_SYNC_TS2_VOICE || f->sync == DMR_SYNC_TS2_DATA) {
        s_direct_family = true;
    } else if (f->sync != DMR_SYNC_NONE) {
        s_direct_family = false;
    }
    if (f->bs || s_direct_family) {
        s_ms_mode = false;
    } else if (f->sync == DMR_SYNC_MS_VOICE || f->sync == DMR_SYNC_MS_DATA) {
        s_ms_mode = true;
    }

    if (f->bs) {
        uint8_t cbits[24], tact[7];
        uint32_t cw;
        for (int i = 0; i < 12; i++) {
            cbits[2 * i] = (uint8_t)((f->cach[i] >> 1) & 1u);
            cbits[2 * i + 1] = (uint8_t)(f->cach[i] & 1u);
        }
        for (int i = 0; i < 7; i++) {
            tact[i] = cbits[k_tact_pos[i]];
        }
        cw = dmr_bits_to_u32(tact, 7);
        slot_idx = 1 - s_last_slot; /* prediction: strict alternation */
        /* Hamming(7,4) is a perfect code: dmr_hamming_7_4_decode() never
         * reports a failure, it always lands on some word. So there is no
         * "unreadable TACT" to break the run on, and a word with two bit
         * errors comes back silently wrong. That is why the phase is only
         * re-set after two of them agree against the prediction. */
        if (dmr_hamming_7_4_decode(&cw)) {
            int tc =
                (int)((cw >> 5)
                      & 1u); /* TC bit (not proof of lock, see dmr_proto.h) */
#ifdef CONFIG_DMR_TRUNK
            /* The inbound slot this CACH announces (TC) and its AT bit */
            dmrTrunk_cach(tc, ((cw >> 6) & 1u) != 0);
#endif
            if (!s_slot_phase_known) {
                /*
                 * A fresh lock used to take the phase from this one word.
                 * By the reasoning three lines up that is the one thing it
                 * must not do: a TACT with two bit errors comes back
                 * silently wrong, and a wrong phase here is not a cosmetic
                 * mistake -- it puts the repeater's TS1 reference 30 ms out,
                 * so every burst we transmit lands in the other slot and the
                 * repeater drops the call. dmr_proto_new_lock() clears the
                 * phase, and a (re)lock happens after every transmission, so
                 * this coin was tossed again on every single PTT. The log
                 * showed it: the measured TS1 jumped by exactly +-30 ms
                 * between one report and the next.
                 *
                 * So hold the first word as a candidate and use it, but do
                 * not call the phase known. The next burst is the other slot
                 * by alternation; if its own TACT says so too, two
                 * independent words agree and the phase is settled. That is
                 * the same standard the re-phasing branch below already
                 * applies, and it costs one burst, 30 ms.
                 */
                slot_idx = tc;
                if (s_tact_cand >= 0 && tc == 1 - s_last_slot) {
                    s_slot_phase_known = true;
                    s_tact_cand = -1;
                    s_phase_sets++;
                } else {
                    s_tact_cand = tc; /* start, or start again, from here */
                }
                s_tact_disagree = 0;
            } else if (tc == slot_idx) {
                s_tact_disagree = 0;
            } else if (++s_tact_disagree >= 2) {
                slot_idx = tc; /* consistently out of phase: re-phase */
                s_tact_disagree = 0;
                s_phase_sets++;
            }
        }
    } else if (f->sync == DMR_SYNC_TS1_VOICE || f->sync == DMR_SYNC_TS1_DATA) {
        slot_idx = 0;
    } else if (f->sync == DMR_SYNC_TS2_VOICE || f->sync == DMR_SYNC_TS2_DATA) {
        slot_idx = 1;
    } else if (s_direct_family && f->sync == DMR_SYNC_NONE) {
        slot_idx = 1
                 - s_last_slot; /* direct mode: slots alternate every 30 ms */
    } else {
        slot_idx = 0; /* MS sourced / simplex: a single channel every 60 ms */
    }
#ifdef DMR_SLOT_TRACE
    printf("SLOT t=%u sync=%d slot=%d known=%d\n", (unsigned)f->t_ms,
           (int)f->sync, slot_idx, (int)s_slot_phase_known);
#endif
    s_last_slot = slot_idx;
    s_last_frame_bs = f->bs;
    st = &s_slot[slot_idx];
    /* A burst landed on this slot. age_slots() above has already used the
     * previous value, so stamping it here is what keeps the ageing honest:
     * the OK branch below is not reached by the paths that only set the
     * activity (a burst A with the voice sync grades NEUTRAL by design). */
    st->info.last_ms = f->t_ms;
    s_burst_checked = false;
    s_burst_errors = -1;
    s_burst_cc = -1;

    if (is_data_sync(f->sync)) {
        grade = handle_data_burst(slot_idx, bits, f->t_ms) ? DMR_FRAME_OK :
                                                             DMR_FRAME_BAD;
    } else if (is_voice_sync(f->sync)) {
        dmr_data_interrupt(slot_idx);
        st->voice_idx = 0;
        st->emb_nfrag = 0;
        st->emb_bad_run = 0;
        st->call_ms = f->t_ms; /* voice: the call is alive */
        /* A burst A with the voice sync is voice, whatever the EMBs of
         * B..F do afterwards: only those and the LC header used to set
         * this, so a late entry whose EMBs failed showed the slot idle
         * while its audio was coming out of the speaker */
        st->info.activity = DMR_ACT_VOICE;
#ifdef CONFIG_DMR_TRUNK
        dmrTrunk_voice(slot_idx, st->info.cc);
#endif
        grade = DMR_FRAME_NEUTRAL; /* proven (or not) by the EMBs of B..F */
        if (s_proven) {
            log_privacy(slot_idx, f->t_ms);
            dmr_voice_burst(slot_idx, f->burst, 0, f->t_ms);
        }
    } else if (st->voice_idx >= 0) {
        st->call_ms = f->t_ms; /* voice: the call is alive */
        grade = handle_voice_nosync(slot_idx, f->burst, bits, f->t_ms) ?
                    DMR_FRAME_OK :
                    DMR_FRAME_BAD;
#ifdef CONFIG_DMR_TRUNK
        dmrTrunk_voice(slot_idx, s_burst_cc >= 0 ? s_burst_cc : st->info.cc);
#endif
    }

    if (grade == DMR_FRAME_OK) {
        /* Decoded: does it prove the stream? (see s_cc_ref) */
        if (s_burst_checked || s_burst_errors == 0) {
            s_cc_ref = s_burst_cc;
        } else if (!(s_burst_errors == 1 && s_cc_ref >= 0
                     && s_burst_cc == s_cc_ref)) {
            grade = DMR_FRAME_NEUTRAL;
        }
    }
    if (grade == DMR_FRAME_OK) {
        s_proven = true;
        s_stats.frames_valid++;
    }
    {
        dmr_data_stats_t ds;
        dmr_data_get_stats(&ds);
        s_stats.data_headers = ds.headers;
        s_stats.data_packets = ds.packets;
        s_stats.data_crc_bad = ds.crc32_bad;
        s_stats.lrrp = ds.lrrp;
    }

    DMR_LOCK(s_lock);
    s_pub_slot[0] = s_slot[0].info;
    s_pub_slot[1] = s_slot[1].info;
    /*
     * The call log only moves when an entry is written or updated, and
     * that bumps s_log_seq: copying the twelve records (about 900 bytes
     * of PSRAM) under the lock that the UI takes, on every frame at
     * 16.7 frames/s, was paying for a snapshot that had not changed.
     */
    if (s_pub_log_seq != s_log_seq) {
        memcpy(s_pub_log, s_log, sizeof(s_log));
        s_pub_log_count = s_log_count;
        s_pub_log_seq = s_log_seq;
    }
    s_pub_stats = s_stats;
    DMR_UNLOCK(s_lock);

    return grade;
}

void dmr_proto_get_slot(int slot_idx, dmr_slot_info_t *out)
{
    DMR_LOCK(s_lock);
    *out = s_pub_slot[slot_idx & 1];
    DMR_UNLOCK(s_lock);
}

int dmr_proto_get_log(dmr_log_entry_t out[DMR_LOG_LEN])
{
    int n;
    DMR_LOCK(s_lock);
    n = s_pub_log_count;
    memcpy(out, s_pub_log, (size_t)n * sizeof(dmr_log_entry_t));
    DMR_UNLOCK(s_lock);
    return n;
}

void dmr_proto_get_stats(dmr_proto_stats_t *out)
{
    DMR_LOCK(s_lock);
    *out = s_pub_stats;
    DMR_UNLOCK(s_lock);
}

bool dmr_proto_is_ms_mode(void)
{
    return s_ms_mode;
}

uint32_t dmr_proto_phase_sets(void)
{
    const uint32_t n = s_phase_sets;
    s_phase_sets = 0;
    return n;
}

bool dmr_proto_bs_slot(int *slot)
{
    if (!s_last_frame_bs || !s_slot_phase_known)
        return false;
    *slot = s_last_slot;
    return true;
}

void dmr_proto_unprove(void)
{
    s_proven = false;
    s_cc_ref = -1;
    dmr_voice_reset();
}

void dmr_proto_new_lock(void)
{
    s_slot_phase_known =
        false; /* timing restarts: slot phase must be re-learned */
    /* And with it the candidate: a word proposed before the relock says
     * nothing about the stream after it, and leaving it behind would let
     * the very first TACT of the new lock "confirm" it. */
    s_tact_cand = -1;
    /* Nothing of what was being assembled survives a new lock: the stream
     * may be another one entirely. Only the slot of the first burst used to
     * be cleared, and the other one went on from where it was. */
    for (int i = 0; i < 2; i++) {
        s_slot[i].voice_idx = -1;
        s_slot[i].emb_nfrag = 0;
        s_slot[i].emb_guess = 0;
        s_slot[i].emb_bad_run = 0;
        s_slot[i].ta_have = 0;
        s_slot[i].ta_len = 0;
        s_slot[i].ta_char_bits = 0;
    }
    s_proven = false;
    dmr_data_interrupt(0);
    dmr_data_interrupt(1);
}
