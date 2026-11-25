/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Post-mortem recorder: see core/blackbox.h for the contract and
 * docs/BLACKBOX.md for the format, the flash layout and how to read it.
 *
 * This file is the portable half.  It knows nothing about Zephyr, pthreads or
 * flash: the supervisor clock is passed in, the thread table comes from a weak
 * hook the platform overrides, and the record is written through a byte
 * interface.  That is what lets the host tests drive the stall detector with a
 * fake clock and a fake flash, and it keeps the radio build to one small
 * translation unit with no dependencies of its own.
 */

#include "core/blackbox.h"
#include "core/crc.h"

#include <string.h>

/*
 * ---------------------------------------------------------------------------
 * Live state
 * ---------------------------------------------------------------------------
 *
 * All of it volatile and all of it lock-free by construction: the hot path
 * writes single 32-bit words and the reader tolerates a torn set.
 */

/** Beat counters, one per watched thread, incremented by the thread itself. */
static volatile uint32_t bbBeats[BB_BEAT_COUNT];

/** Thread that owns each beat, captured on its first pass. */
static const void *volatile bbThreadOf[BB_BEAT_COUNT];

/** Breadcrumb ring: (mark << 24) | (ms & 0xFFFFFF), newest at head - 1. */
static volatile uint32_t bbRing[BB_MARK_N];
static volatile uint32_t bbRingHead;
static volatile uint32_t bbMarks;

/** Coarse clock for the breadcrumbs, kept fresh by the beats. */
static volatile uint32_t bbNow;

/** Set while the supervisor is allowed to declare a hang. */
static volatile bool bbArmed;

/** Beats considered stuck at the last pass, bit = enum bbBeat. */
static volatile uint32_t bbStalled;

/*
 * Detector state, touched only by the supervisor: which beats it has seen
 * move, when, and with which value, so a thread that has not started yet is
 * not mistaken for one that has stopped.
 */
static uint32_t bbLastSeen[BB_BEAT_COUNT];
static uint32_t bbLastChange[BB_BEAT_COUNT];
static bool bbPrimed[BB_BEAT_COUNT];

/*
 * ---------------------------------------------------------------------------
 * Platform hooks: weak defaults, so every host test and every other target
 * links unchanged.  A platform that does not override them gets a recorder
 * that stamps time but never writes anything.
 * ---------------------------------------------------------------------------
 */

__attribute__((weak)) void bb_platformSnapshot(bbSnapshot_t *s)
{
    if (s != NULL)
        memset(s, 0, sizeof(*s));
}

__attribute__((weak)) int bb_platformPersist(const uint8_t *rec, size_t len)
{
    (void)rec;
    (void)len;
    return -1;
}

__attribute__((weak)) void bb_platformReboot(void)
{
}

__attribute__((weak)) const void *bb_platformCurrentThread(void)
{
    return NULL;
}

__attribute__((weak)) uint32_t bb_platformNowMs(void)
{
    return 0;
}

/**
 * Default reaction to a stall: write the record, then reset.  The reset comes
 * whatever the write did -- a radio that cannot say why it froze still has to
 * come back on its own, and that is the difference between a post-mortem and a
 * paperweight.
 */
__attribute__((weak)) void bb_platformHang(uint32_t mask, uint32_t nowMs)
{
    bb_capture(BB_REASON_STALL, mask, nowMs);
    bb_platformReboot();
}

/*
 * ---------------------------------------------------------------------------
 * Hot path
 * ---------------------------------------------------------------------------
 */

void bb_beat(uint8_t beat)
{
    if (beat >= BB_BEAT_COUNT)
        return;

    /*
     * The owning thread is learnt from the beat itself, on the first pass and
     * once per thread: the record then names the watched threads by role
     * without depending on how the platform names its threads, and the
     * comparison in the thread walk is one pointer per beat.
     */
    if (bbThreadOf[beat] == NULL)
        bbThreadOf[beat] = bb_platformCurrentThread();

    bbBeats[beat] = bbBeats[beat] + 1;

    /* Once per pass, not once per mark: see bb_mark() */
    bbNow = bb_platformNowMs();
}

void bb_mark(uint8_t mark)
{
    /*
     * One store, one head update.  No lock on purpose: this has to work from a
     * thread, from an interrupt and from a fault handler, and a lock in any of
     * those is a deadlock waiting for the right moment.  Two threads marking in
     * the same instant lose a crumb (both write the same slot and the head
     * advances once); the surviving marks keep their order, which is what the
     * record is read for.
     */
    uint32_t head = bbRingHead;
    bbRing[head & (BB_MARK_N - 1)] = ((uint32_t)mark << 24)
                                   | (bbNow & 0x00FFFFFFU);
    bbRingHead = head + 1;
    bbMarks = bbMarks + 1;
}

void bb_tick(uint32_t nowMs)
{
    bbNow = nowMs;
}

void bb_clear(void)
{
    for (uint8_t i = 0; i < BB_BEAT_COUNT; i++) {
        bbBeats[i] = 0;
        bbThreadOf[i] = NULL;
        bbLastSeen[i] = 0;
        bbLastChange[i] = 0;
        bbPrimed[i] = false;
    }

    for (uint8_t i = 0; i < BB_MARK_N; i++)
        bbRing[i] = 0;

    bbRingHead = 0;
    bbMarks = 0;
    bbNow = 0;
    bbStalled = 0;
}

void bb_suspend(void)
{
    bbArmed = false;
    bbStalled = 0;
}

void bb_resume(void)
{
    /* Re-prime on the next pass: a thread that stopped while suspended is not
     * a hang, it is a thread we were not watching. */
    for (uint8_t i = 0; i < BB_BEAT_COUNT; i++)
        bbPrimed[i] = false;

    bbStalled = 0;
    bbArmed = true;
}

bool bb_supervise(uint32_t nowMs)
{
    if (!bbArmed) {
        bbStalled = 0;
        return false;
    }

    bbNow = nowMs;

    uint32_t mask = 0;

    for (uint8_t i = 0; i < BB_BEAT_COUNT; i++) {
        uint32_t seen = bbBeats[i];

        /* Never stamped: the thread is not in this build, or has not started.
         * Watching it would declare a hang at every boot. */
        if (seen == 0) {
            bbPrimed[i] = false;
            continue;
        }

        if (!bbPrimed[i]) {
            bbPrimed[i] = true;
            bbLastSeen[i] = seen;
            bbLastChange[i] = nowMs;
            continue;
        }

        if (seen != bbLastSeen[i]) {
            bbLastSeen[i] = seen;
            bbLastChange[i] = nowMs;
            continue;
        }

        if ((uint32_t)(nowMs - bbLastChange[i]) >= (uint32_t)BB_STALL_MS)
            mask |= 1U << i;
    }

    bbStalled = mask;

    if (mask == 0)
        return false;

    bb_platformHang(mask, nowMs);
    return true;
}

uint32_t bb_stalledMask(void)
{
    return bbStalled;
}

uint32_t bb_beatCount(uint8_t beat)
{
    return (beat < BB_BEAT_COUNT) ? bbBeats[beat] : 0;
}

uint32_t bb_markCount(void)
{
    return bbMarks;
}

uint16_t bb_threadRole(const void *thread)
{
    if (thread == NULL)
        return 0;

    for (uint8_t i = 0; i < BB_BEAT_COUNT; i++) {
        if (bbThreadOf[i] == thread)
            return (uint16_t)(i + 1);
    }

    return 0;
}

/*
 * ---------------------------------------------------------------------------
 * Record: byte-level helpers
 * ---------------------------------------------------------------------------
 *
 * Explicit little-endian loads and stores rather than a packed struct: the
 * layout is a documented ABI that a PC reads, so it does not get to depend on
 * the compiler's idea of padding.
 */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/*
 * ---------------------------------------------------------------------------
 * Record: build, validate, store
 * ---------------------------------------------------------------------------
 */

int bb_recordBuild(uint8_t *rec, size_t cap, uint32_t nowMs, uint8_t reason,
                   uint32_t detail)
{
    if ((rec == NULL) || (cap < BB_RECORD_SIZE))
        return -1;

    memset(rec, 0, BB_RECORD_SIZE);

    bbSnapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    bb_platformSnapshot(&snap);

    put32(&rec[BB_OFF_MAGIC], BB_RECORD_MAGIC);
    put16(&rec[BB_OFF_VERSION], BB_RECORD_VERSION);
    put16(&rec[BB_OFF_HDRSIZE], BB_HDR_SIZE);
    put32(&rec[BB_OFF_VALID], BB_VALID_SIGNATURE);
    put32(&rec[BB_OFF_UPTIME], nowMs);
    rec[BB_OFF_REASON] = reason;
    put32(&rec[BB_OFF_DETAIL], detail);
    put32(&rec[BB_OFF_RESET], snap.resetCause);
    put32(&rec[BB_OFF_CRUMBSEQ], bbMarks);
    put16(&rec[BB_OFF_RTXMODE], snap.rtxMode);
    put16(&rec[BB_OFF_UIMODE], snap.uiMode);
    put32(&rec[BB_OFF_RXFREQ], snap.rxFreq);
    put32(&rec[BB_OFF_TXFREQ], snap.txFreq);
    put16(&rec[BB_OFF_RTXFLAGS], snap.rtxFlags);
    put16(&rec[BB_OFF_THREADSZ], BB_THREAD_REC_SIZE);

    uint16_t nThreads = snap.threadCount;
    if (nThreads > BB_MAX_THREADS)
        nThreads = BB_MAX_THREADS;

    put16(&rec[BB_OFF_THREADS], nThreads);

    for (uint16_t i = 0; i < nThreads; i++) {
        uint8_t *t = &rec[BB_OFF_THREADS_BASE + (size_t)i * BB_THREAD_REC_SIZE];

        memcpy(&t[BB_TR_NAME], snap.threads[i].name, 12);
        t[BB_TR_NAME + 11] = '\0';
        t[BB_TR_PRIO] = (uint8_t)snap.threads[i].prio;
        t[BB_TR_STATE] = snap.threads[i].state;
        put16(&t[BB_TR_FREE], snap.threads[i].stackFree);
        put16(&t[BB_TR_SIZE], snap.threads[i].stackSize);
        put16(&t[BB_TR_ROLE], snap.threads[i].role);
        put32(&t[BB_TR_PC], snap.threads[i].pc);
        put32(&t[BB_TR_LR], snap.threads[i].lr);
        put32(&t[BB_TR_SP], snap.threads[i].sp);
    }

    /*
     * Breadcrumbs, oldest first.  The ring is read through its head, taken
     * once: a mark that lands while this runs is simply not in the record,
     * and none of the ones that are can be out of order.
     */
    uint32_t head = bbRingHead;
    uint32_t seq = bbMarks;
    uint16_t nCrumbs = (seq < BB_MARK_N) ? (uint16_t)seq : (uint16_t)BB_MARK_N;

    put16(&rec[BB_OFF_CRUMBS], nCrumbs);

    for (uint16_t i = 0; i < nCrumbs; i++) {
        uint32_t slot = (head - nCrumbs + i) & (BB_MARK_N - 1);
        put32(&rec[BB_OFF_CRUMBS_BASE + (size_t)i * 4], bbRing[slot]);
    }

    put16(&rec[BB_OFF_CRC], crc_ccitt(rec, BB_OFF_CRC));

    return BB_RECORD_SIZE;
}

bool bb_recordValid(const uint8_t *rec, size_t len)
{
    if ((rec == NULL) || (len < BB_RECORD_SIZE))
        return false;
    if (get32(&rec[BB_OFF_MAGIC]) != BB_RECORD_MAGIC)
        return false;
    if (get16(&rec[BB_OFF_VERSION]) != BB_RECORD_VERSION)
        return false;
    if (get16(&rec[BB_OFF_HDRSIZE]) != BB_HDR_SIZE)
        return false;
    if (get32(&rec[BB_OFF_VALID]) != BB_VALID_SIGNATURE)
        return false;
    if (get16(&rec[BB_OFF_THREADS]) > BB_MAX_THREADS)
        return false;
    if (get16(&rec[BB_OFF_CRUMBS]) > BB_MARK_N)
        return false;
    if (get16(&rec[BB_OFF_CRC]) != crc_ccitt(rec, BB_OFF_CRC))
        return false;

    return true;
}

int bb_recordLoad(const bbFlashIo_t *io, uint8_t *rec, size_t cap)
{
    if ((io == NULL) || (io->read == NULL) || (rec == NULL)
        || (cap < BB_RECORD_SIZE))
        return -1;

    if (io->read(io->ctx, 0, rec, BB_RECORD_SIZE) != 0)
        return -1;

    return bb_recordValid(rec, BB_RECORD_SIZE) ? 0 : -1;
}

int bb_recordInvalidate(const bbFlashIo_t *io)
{
    if ((io == NULL) || (io->write == NULL))
        return -1;

    /*
     * Zero over the validity word only.  Erasing the sector would work too and
     * would cost an erase and a rewrite of 4 KiB at every boot; a 1 -> 0 write
     * of the 4-byte write block is enough to make the record invalid and the
     * sector is ready for the next crash.
     */
    uint8_t zero[4] = { 0, 0, 0, 0 };

    return io->write(io->ctx, BB_OFF_VALID, zero, sizeof(zero));
}

bool bb_capture(uint8_t reason, uint32_t detail, uint32_t nowMs)
{
    uint8_t rec[BB_RECORD_SIZE];
    int len = bb_recordBuild(rec, sizeof(rec), nowMs, reason, detail);

    if (len < 0)
        return false;

    return bb_platformPersist(rec, (size_t)len) == 0;
}

/*
 * ---------------------------------------------------------------------------
 * Text rendering
 * ---------------------------------------------------------------------------
 *
 * A record that cannot be read is a record that will not be read.  Every line
 * is short enough for a 115200 baud console and for a 128-byte log ring, and
 * the numbers are printed the way an operator reads them: milliseconds, hertz,
 * and the mode's name.
 */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} bbLine_t;

static void lineStr(bbLine_t *l, const char *s)
{
    while ((*s != '\0') && (l->len + 1 < l->cap)) {
        l->buf[l->len++] = *s++;
    }
    l->buf[l->len] = '\0';
    if (*s != '\0')
        l->overflow = true;
}

static void lineU32(bbLine_t *l, uint32_t v, uint8_t base, uint8_t width,
                    char pad)
{
    char tmp[12];
    uint8_t n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while ((v > 0) && (n < sizeof(tmp))) {
            uint8_t d = (uint8_t)(v % base);
            tmp[n++] = (char)((d < 10) ? ('0' + d) : ('a' + d - 10));
            v /= base;
        }
    }

    while ((n < width) && (n < sizeof(tmp))) {
        tmp[n++] = pad;
    }

    while (n > 0) {
        char c = tmp[--n];
        if (l->len + 1 < l->cap) {
            l->buf[l->len++] = c;
            l->buf[l->len] = '\0';
        } else {
            l->overflow = true;
        }
    }
}

static void lineDec(bbLine_t *l, uint32_t v)
{
    lineU32(l, v, 10, 0, ' ');
}

static void lineHex(bbLine_t *l, uint32_t v, uint8_t digits)
{
    lineStr(l, "0x");
    lineU32(l, v, 16, digits, '0');
}

static void emitLine(bbLine_t *l, void (*emit)(void *, const char *), void *ctx)
{
    if (emit != NULL)
        emit(ctx, l->buf);

    l->len = 0;
    l->overflow = false;
    if (l->cap > 0)
        l->buf[0] = '\0';
}

const char *bb_beatName(uint8_t beat)
{
    switch (beat) {
        case BB_BEAT_UI:
            return "ui";
        case BB_BEAT_RTX:
            return "rtx";
        case BB_BEAT_PROTO:
            return "proto";
        case BB_BEAT_MAIN:
            return "main";
        default:
            return "?";
    }
}

const char *bb_markName(uint8_t mark)
{
    switch (mark) {
        case BB_M_NONE:
            return "-";
        case BB_M_BOOT:
            return "BOOT";
        case BB_M_TERM:
            return "TERM";
        case BB_M_MODE_ENTER:
            return "MODE_ENTER";
        case BB_M_MODE_READY:
            return "MODE_READY";
        case BB_M_MODE_FAIL:
            return "MODE_FAIL";
        case BB_M_MODE_OFF:
            return "MODE_OFF";
        case BB_M_RTX_CONF:
            return "RTX_CONF";
        case BB_M_RADIO_CONF:
            return "RADIO_CONF";
        case BB_M_RX_ON:
            return "RX_ON";
        case BB_M_TX_ON:
            return "TX_ON";
        case BB_M_TX_OFF:
            return "TX_OFF";
        case BB_M_TX_ABORT:
            return "TX_ABORT";
        case BB_M_AUDIO_OPEN:
            return "AUDIO_OPEN";
        case BB_M_AUDIO_CLOSE:
            return "AUDIO_CLOSE";
        case BB_M_AIRBAND_LOAD:
            return "AIRBAND_LOAD";
        case BB_M_WFM_ENTER:
            return "WFM_ENTER";
        case BB_M_SETTINGS:
            return "SETTINGS";
        case BB_M_CHAN_LOAD:
            return "CHAN_LOAD";
        case BB_M_UI_MODE:
            return "UI_MODE";
        case BB_M_HANG:
            return "HANG";
        default:
            return "?";
    }
}

const char *bb_reasonName(uint8_t reason)
{
    switch (reason) {
        case BB_REASON_NONE:
            return "NONE";
        case BB_REASON_STALL:
            return "STALL";
        case BB_REASON_FATAL:
            return "FATAL";
        case BB_REASON_MANUAL:
            return "MANUAL";
        default:
            return "?";
    }
}

const char *bb_modeName(uint16_t mode)
{
    /* Mirrors enum opmode (rtx/rtx.h); a value this build does not know is
     * printed as its number, never guessed. */
    switch (mode) {
        case 0:
            return "NONE";
        case 1:
            return "FM";
        case 2:
            return "DMR";
        case 3:
            return "M17";
        case 4:
            return "TETRA";
        case 5:
            return "NXDN";
        case 6:
            return "dPMR";
        case 7:
            return "P25";
        case 8:
            return "YSF";
        case 9:
            return "D-STAR";
        case 10:
            return "AM";
        case 11:
            return "WFM";
        default:
            return "?";
    }
}

/** Zephyr's thread_state encoding, printed as a word rather than a number. */
static const char *threadStateName(uint8_t state)
{
    switch (state) {
        case 0:
            return "ready";
        case 1:
            return "pended";
        case 2:
            return "dead";
        case 4:
            return "dummy";
        case 8:
            return "prestart";
        case 16:
            return "suspended";
        default:
            return "?";
    }
}

void bb_textDump(const uint8_t *rec, size_t len,
                 void (*emit)(void *ctx, const char *line), void *ctx)
{
    if ((rec == NULL) || (len < BB_RECORD_SIZE) || (emit == NULL))
        return;

    char lineBuf[128];
    bbLine_t l = { lineBuf, sizeof(lineBuf), 0, false };
    lineBuf[0] = '\0';

    const uint8_t reason = rec[BB_OFF_REASON];
    const uint32_t detail = get32(&rec[BB_OFF_DETAIL]);

    lineStr(&l, "C62 post-mortem: record v");
    lineDec(&l, get16(&rec[BB_OFF_VERSION]));
    lineStr(&l, ", ");
    lineDec(&l, (uint32_t)BB_RECORD_SIZE);
    lineStr(&l, " B, ");
    lineStr(&l, bb_recordValid(rec, len) ? "crc ok" : "CRC BAD");
    emitLine(&l, emit, ctx);

    lineStr(&l, "reason: ");
    lineStr(&l, bb_reasonName(reason));
    lineStr(&l, " detail ");
    lineHex(&l, detail, 8);

    if (reason == BB_REASON_STALL) {
        lineStr(&l, " [");
        bool first = true;
        for (uint8_t i = 0; i < BB_BEAT_COUNT; i++) {
            if ((detail & (1U << i)) == 0)
                continue;
            if (!first)
                lineStr(&l, " ");
            lineStr(&l, bb_beatName(i));
            first = false;
        }
        lineStr(&l, "]");
    }
    emitLine(&l, emit, ctx);

    uint32_t uptime = get32(&rec[BB_OFF_UPTIME]);
    lineStr(&l, "uptime: ");
    lineDec(&l, uptime);
    lineStr(&l, " ms (");
    lineDec(&l, uptime / 60000U);
    lineStr(&l, " min ");
    lineDec(&l, (uptime / 1000U) % 60U);
    lineStr(&l, " s), reset cause ");
    lineHex(&l, get32(&rec[BB_OFF_RESET]), 8);
    emitLine(&l, emit, ctx);

    lineStr(&l, "radio: rtx=");
    lineStr(&l, bb_modeName(get16(&rec[BB_OFF_RTXMODE])));
    lineStr(&l, "(");
    lineDec(&l, get16(&rec[BB_OFF_RTXMODE]));
    lineStr(&l, ") ui=");
    lineStr(&l, bb_modeName(get16(&rec[BB_OFF_UIMODE])));
    lineStr(&l, "(");
    lineDec(&l, get16(&rec[BB_OFF_UIMODE]));
    lineStr(&l, ") flags ");
    lineHex(&l, get16(&rec[BB_OFF_RTXFLAGS]), 2);
    emitLine(&l, emit, ctx);

    lineStr(&l, "  rx ");
    lineDec(&l, get32(&rec[BB_OFF_RXFREQ]));
    lineStr(&l, " Hz, tx ");
    lineDec(&l, get32(&rec[BB_OFF_TXFREQ]));
    lineStr(&l, " Hz");
    emitLine(&l, emit, ctx);

    const uint16_t nThreads = get16(&rec[BB_OFF_THREADS]);
    lineStr(&l, "threads: ");
    lineDec(&l, nThreads);
    emitLine(&l, emit, ctx);

    for (uint16_t i = 0; i < nThreads; i++) {
        const uint8_t *t =
            &rec[BB_OFF_THREADS_BASE + (size_t)i * BB_THREAD_REC_SIZE];

        char name[13];
        memcpy(name, &t[BB_TR_NAME], 12);
        name[12] = '\0';

        lineStr(&l, "  ");
        lineStr(&l, (name[0] != '\0') ? name : "?");
        for (uint8_t k = (uint8_t)strlen(name); k < 10; k++)
            lineStr(&l, " ");

        lineStr(&l, " pr ");
        int8_t prio = (int8_t)t[BB_TR_PRIO];
        if (prio < 0) {
            lineStr(&l, "-");
            lineDec(&l, (uint32_t)(-(int32_t)prio));
        } else {
            lineDec(&l, (uint32_t)prio);
        }
        lineStr(&l, " st ");
        lineStr(&l, threadStateName(t[BB_TR_STATE]));
        lineStr(&l, " free ");
        lineDec(&l, get16(&t[BB_TR_FREE]));
        lineStr(&l, "/");
        lineDec(&l, get16(&t[BB_TR_SIZE]));
        emitLine(&l, emit, ctx);

        lineStr(&l, "      role ");
        uint16_t role = get16(&t[BB_TR_ROLE]);
        if (role == 0) {
            lineStr(&l, "-");
        } else {
            lineStr(&l, bb_beatName((uint8_t)(role - 1)));
        }
        lineStr(&l, " pc ");
        lineHex(&l, get32(&t[BB_TR_PC]), 8);
        lineStr(&l, " lr ");
        lineHex(&l, get32(&t[BB_TR_LR]), 8);
        lineStr(&l, " sp ");
        lineHex(&l, get32(&t[BB_TR_SP]), 8);
        emitLine(&l, emit, ctx);
    }

    const uint16_t nCrumbs = get16(&rec[BB_OFF_CRUMBS]);
    lineStr(&l, "breadcrumbs: ");
    lineDec(&l, nCrumbs);
    lineStr(&l, " shown of ");
    lineDec(&l, get32(&rec[BB_OFF_CRUMBSEQ]));
    lineStr(&l, " marks, oldest first");
    emitLine(&l, emit, ctx);

    for (uint16_t i = 0; i < nCrumbs; i++) {
        uint32_t w = get32(&rec[BB_OFF_CRUMBS_BASE + (size_t)i * 4]);
        uint8_t mark = (uint8_t)(w >> 24);

        lineStr(&l, "  +");
        lineDec(&l, w & 0x00FFFFFFU);
        lineStr(&l, " ms  ");
        lineStr(&l, bb_markName(mark));
        if (strcmp(bb_markName(mark), "?") == 0) {
            lineStr(&l, "(");
            lineDec(&l, mark);
            lineStr(&l, ")");
        }
        emitLine(&l, emit, ctx);
    }
}
