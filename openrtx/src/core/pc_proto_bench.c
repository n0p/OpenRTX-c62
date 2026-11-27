/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Bench namespace of the radio protocol: scripted measurements.
 *
 * The problem it solves is not reading a register, it is putting the radio and
 * an external receiver (an RTL-SDR, an Anritsu) on the same time line and on
 * the same frequency, so that "what the radio says it did" and "what the air
 * shows" can be compared by a program instead of by eye.
 *
 * The radio has no GPS and no trustworthy RTC, so the time line is built out
 * of three things and nothing else:
 *
 *  1. The radio tick: milliseconds since boot, monotonic, reported with every
 *     answer and with the TX/RX edge event.  It is the radio's only clock; it
 *     drifts against the PC's, and the drift is measured, not assumed.
 *  2. The negotiation: the PC stamps each answer on arrival and keeps the
 *     round trip.  A least-squares fit over the pairs of (radio tick, host
 *     time) gives the offset between the two clocks and their rate difference
 *     in ppm.  That fit lives on the PC side, tools/bench/correlate.py.
 *  3. The transmit edge: keying is the only event both instruments see at
 *     once.  The SDR sees the carrier rise, the radio reports the tick of the
 *     command and its own PA going up, and the difference is the key-up
 *     latency of the chain -- measured, not guessed.  docs/BENCH.md states the
 *     precision this gives in each direction.
 *
 * What the radio cannot do, and the protocol therefore does not pretend:
 *
 *  - There is no I/Q.  The only received signal it can hand over is the
 *    demodulated audio (the discriminator, 48 kHz s16 mono) plus RSSI, so no
 *    constellation, no EVM, no adjacent-channel power and no phase noise.
 *  - It cannot measure its own carrier: it has no reference to compare it
 *    with.  The frequency error of the carrier is measured by the SDR, and
 *    PC_TAG_BENCH_DELTA is 0 unless a platform finds a way to know better.
 *  - Its capture is receive-side only: while the PA is keyed that buffer holds
 *    nothing useful, so transmit measurements are made by the SDR.
 *
 * Gates: PC_CAP_BENCH, which only a build with CONFIG_C62_BENCH_PROTO
 * advertises, plus the session token for every message that moves the radio,
 * keys the PA or hands over audio.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core/pc_proto.h"
#include "core/settings.h"
#include "pc_proto_internal.h"

#ifdef CONFIG_C62_BENCH_PROTO

/* Longest capture, in milliseconds.  The platform clamps it to the buffer it
 * has (the C62 keeps 16000 samples in PSRAM, i.e. 333 ms at 48 kHz); ARM
 * reports the real ceiling so the PC sizes its windows without guessing. */
#define BENCH_CAP_MAX_MS 4000

/**
 * Samples per BENCH_CAPTURE read.  The status tags of the answer cost 71 bytes
 * (eleven TLVs) and the payload budget is 390, so 128 samples (256 bytes) leave
 * room to spare; 160 would not fit and the whole answer would be refused.
 */
#define BENCH_CAP_CHUNK 128

/** RSSI series and sweep size.  The point array is 96 * 6 = 576 bytes. */
#define BENCH_RSSI_MAX   32
#define BENCH_POINTS_MAX 96

/** Points per sweep answer. */
#define BENCH_POINTS_CHUNK 32

/** Size of the report buffer and of one chunk of it. */
#define BENCH_REPORT_LEN   768
#define BENCH_REPORT_CHUNK 300

/** The transport ticks at 10 ms; the collector samples at that cadence. */
#define BENCH_TICK_MS 10

/** Sweep phases.  RETUNE issues the next tuning word, WAIT lets the receiver
 *  settle for the dwell before the RSSI is taken: the chip's AGC and filters
 *  need a few tens of milliseconds after a retune, and an RSSI read before that
 *  describes the previous frequency. */
enum
{
    SW_IDLE = 0,
    SW_RETUNE,
    SW_WAIT
};

typedef struct
{
    bool     armed;
    uint32_t epoch;
    uint32_t armTick;
    uint32_t nowMs;        /**< last tick the core saw                    */

    /* Radio configuration as the run programmed it. */
    bool     tuned;
    uint32_t rxHz, txHz;
    uint8_t  mode, bw;
    int32_t  deltaHz;

    /* Transmit state. */
    uint8_t  ptt;
    uint32_t pttOnTick, pttOffTick;
    uint16_t toneHz;
    uint8_t  level;

    /* Capture. */
    bool     capArmed;
    uint32_t capWant;      /**< samples asked for                         */
    uint32_t capT0;
    uint32_t capWritten;
    uint32_t capBlocks;
    uint16_t capBlockLen;
    uint16_t capRate;
    uint8_t  capOverflow;
    uint32_t capWindowMs;
    bool     capEverArmed;

    /* RSSI collector. */
    uint8_t  rssiWanted, rssiCount;
    uint32_t rssiNext;
    struct
    {
        uint32_t tick;
        int16_t  dbm;
    } rssi[BENCH_RSSI_MAX];

    /* Sweep. */
    uint8_t  swState;
    uint32_t swF0, swF1, swStep;
    uint16_t swDwell, swTotal, swIndex, swSent;
    uint32_t swNext;
    uint32_t swT0;
    struct
    {
        uint32_t hz;
        int16_t  dbm;
    } points[BENCH_POINTS_MAX];

    /* Report text, built on demand. */
    char     report[BENCH_REPORT_LEN];
    uint16_t reportLen;

    /**
     * Transmit power of the run, mW (PC_TAG_BENCH_POWER).
     *
     * A run keys the PA, and on a VFO there is no channel to inherit it from:
     * this used to be whatever state.channel.power held, which is 0 outside a
     * memory channel, and the radio keyed into nothing (docs/BENCH.md,
     * docs/TX_SAFETY.md).  TUNE sets it; ARM seeds it from what the radio
     * would use anyway, so a client that does not know the tag still keys with
     * the radio's own power instead of with zero.
     */
    uint16_t power;
} benchState_t;

static benchState_t bench;

/*
 * ----------------------------------------------------------------------------
 * Platform hook, weak so every host test and every other target links
 * unchanged.  A platform without it answers PC_ERR_NOT_SUPPORTED and the
 * report says the radio has no bench hooks, instead of the module pretending a
 * measurement happened.
 * ----------------------------------------------------------------------------
 */

__attribute__((weak)) int pc_proto_platformBench(uint8_t op, pcProtoBenchArgs_t *a)
{
    (void)op;
    if (a != NULL && op == PC_BENCH_OP_POLL)
        a->nowMs = bench.nowMs;
    return -1;
}

/*
 * ----------------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------------
 */

/** The radio's own clock and state, falling back to the core's tick when the
 *  platform has no bench hook (a host test that only checks the protocol). */
static void benchPoll(pcProtoBenchArgs_t *a)
{
    memset(a, 0, sizeof(*a));
    a->nowMs = bench.nowMs;
    (void)pc_proto_platformBench(PC_BENCH_OP_POLL, a);
    if (a->nowMs == 0)
        a->nowMs = bench.nowMs;
}

/** Unkey, restore the tone and the microphone gain, free the capture. */
static void benchRelease(void)
{
    pcProtoBenchArgs_t a;
    memset(&a, 0, sizeof(a));
    (void)pc_proto_platformBench(PC_BENCH_OP_RELEASE, &a);

    if (bench.capArmed || bench.capEverArmed) {
        pcProtoCapture_t st;
        memset(&st, 0, sizeof(st));
        (void)pc_proto_platformCapture(PC_CAP_OP_STOP, &st, NULL, 0, 0);
    }
}

/** Radio tick for a stamp: the platform's clock when it has one. */
static uint32_t benchNow(void)
{
    pcProtoBenchArgs_t a;
    benchPoll(&a);
    bench.nowMs = a.nowMs;
    return a.nowMs;
}

/** TX/RX edge notification: the PC timestamps its arrival, which is the whole
 *  point of the event existing.  TX_STATE here is 1 keyed, 0 unkeyed -- an
 *  edge, not an rtx status. */
static void benchEdge(struct pcReq *r, uint8_t state, uint32_t tick)
{
    uint8_t buf[16];
    size_t off = pc_tlv_u8(buf, sizeof(buf), 0, PC_TAG_TX_STATE, state);
    off = pc_tlv_u32(buf, sizeof(buf), off, PC_TAG_BENCH_TICK, tick);
    if (off != 0)
        pc_proto_sendEvent(r->p, PC_EVT_BENCH_EDGE, buf, off);
}

/** Token gate shared by every bench message that moves the radio. */
static bool benchTokenOk(struct pcReq *r)
{
    if (pc_proto_tokenOk(r->p, r->in, r->inLen))
        return true;
    r->err = PC_ERR_NOT_ALLOWED;
    return false;
}

/*
 * ----------------------------------------------------------------------------
 * ARM
 * ----------------------------------------------------------------------------
 */

int pc_bench_arm(struct pcReq *r)
{
    if (!benchTokenOk(r))
        return -1;

    uint32_t epoch = bench.epoch + 1;
    benchRelease();
    memset(&bench, 0, sizeof(bench));
    bench.epoch = epoch;
    bench.armed = true;
    bench.armTick = benchNow();

    /* Seed the power with what the radio would key with anyway: a client that
     * never sends PC_TAG_BENCH_POWER then runs at the radio's own power, and
     * one whose radio has none gets the refusal at PTT instead of a silent
     * 0 mW transmission. */
    pcProtoStatus_t s;
    if (pc_proto_status(&s) == 0)
        bench.power = s.txPower;

    /* What a capture can offer, so a client sizes its windows without
     * guessing.  rate 0 means this platform has no capture at all. */
    pcProtoCapture_t st;
    memset(&st, 0, sizeof(st));
    uint16_t rate = 0, maxMs = BENCH_CAP_MAX_MS;
    if (pc_proto_platformCapture(PC_CAP_OP_STATUS, &st, NULL, 0, 0) == 0 &&
        st.rate != 0 && st.capacity != 0) {
        rate = st.rate;
        uint32_t ms = (uint32_t)((uint64_t)st.capacity * 1000u / st.rate);
        maxMs = (uint16_t)(ms > 0xFFFFu ? 0xFFFFu : ms);
    }

    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_STATUS, PC_OK);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_EPOCH, bench.epoch);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_TICK, bench.armTick);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_UPTIME_MS, bench.armTick);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_RATE, rate);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_LENGTH, maxMs);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_BENCH_POWER, bench.power);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * TUNE
 * ----------------------------------------------------------------------------
 */

int pc_bench_tune(struct pcReq *r)
{
    uint32_t rx = 0, tx = 0;
    uint16_t power = 0;
    uint8_t mode = 0, bw = 0;

    if (!benchTokenOk(r))
        return -1;
    if (!pc_tlv_get_u32(r->in, r->inLen, PC_TAG_RX_FREQ, &rx) || rx == 0) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    if (!pc_tlv_get_u32(r->in, r->inLen, PC_TAG_TX_FREQ, &tx) || tx == 0)
        tx = rx;
    /* An absent mode or bandwidth keeps the current one. */
    if (pc_tlv_get_u8(r->in, r->inLen, PC_TAG_MODE, &mode))
        bench.mode = mode;
    if (pc_tlv_get_u8(r->in, r->inLen, PC_TAG_BENCH_BW, &bw))
        bench.bw = bw;
    mode = bench.mode;
    bw = bench.bw;

    /*
     * The transmit power, in mW (docs/BENCH.md).  This is what a bench run
     * needs and what a channel used to provide by accident: on a VFO there is
     * no channel, state.channel.power is 0, and the keying that followed put
     * the PA up with nothing behind it.  TUNE now carries it, so the number
     * the run measures is the number the run asked for.  Absent (or 0) keeps
     * whatever is in force.
     */
    if (pc_tlv_get_u16(r->in, r->inLen, PC_TAG_BENCH_POWER, &power)) {
        if (power > SETTINGS_TX_POWER_MAX) {
            r->err = PC_ERR_INVALID_VALUE;
            return -1;
        }
        bench.power = power;
    }
    power = bench.power;

    if (bench.capArmed || bench.swState != SW_IDLE || bench.ptt) {
        /* Retuning in the middle of a capture or a sweep would silently
         * invalidate what was collected; retuning with the PA up changes the
         * frequency of a live transmission, which is a sweep going out on the
         * air one point at a time. */
        r->err = PC_ERR_BUSY;
        return -1;
    }

    pcProtoBenchArgs_t a;
    memset(&a, 0, sizeof(a));
    a.rxHz = rx;
    a.txHz = tx;
    a.mode = mode;
    a.bw = bw;
    a.power = power;
    int rc = pc_proto_platformBench(PC_BENCH_OP_TUNE, &a);
    if (rc != 0) {
        /* A busy state lock is not an invalid frequency: say which it was so a
         * client knows whether retrying is worth anything. */
        r->err = (rc == -(int)PC_ERR_BUSY) ? PC_ERR_BUSY : PC_ERR_INVALID_VALUE;
        return -1;
    }

    bench.tuned = true;
    bench.power = a.power;
    bench.rxHz = a.rxHz != 0 ? a.rxHz : rx;
    bench.txHz = a.txHz != 0 ? a.txHz : tx;
    bench.mode = a.mode != 0 ? a.mode : mode;
    bench.bw = a.bw;
    bench.deltaHz = a.deltaHz;
    uint32_t tick = benchNow();

    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_STATUS, PC_OK);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_RX_FREQ, bench.rxHz);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_TX_FREQ, bench.txHz);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_MODE, bench.mode);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_BENCH_BW, bench.bw);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_DELTA,
                     (uint32_t)bench.deltaHz);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_TICK, tick);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_EPOCH, bench.epoch);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_BENCH_POWER, bench.power);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * PTT
 * ----------------------------------------------------------------------------
 */

int pc_bench_ptt(struct pcReq *r)
{
    uint8_t on = 0;
    uint16_t tone = 0;
    uint8_t level = 0;

    if (!benchTokenOk(r))
        return -1;
    if (!pc_tlv_get_u8(r->in, r->inLen, PC_TAG_TX_STATE, &on) || on > 1) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    pc_tlv_get_u16(r->in, r->inLen, PC_TAG_BENCH_TONE, &tone);
    pc_tlv_get_u8(r->in, r->inLen, PC_TAG_BENCH_LEVEL, &level);

    /*
     * Only a keying with something to transmit with.  The power the run asked
     * TUNE for is the power of the answer; if it is 0 (a VFO with no setting,
     * a run that never tuned), keying would put the PA up at zero and the SDR
     * would measure silence with nothing on the radio to blame.  Refused, with
     * the error in the answer (docs/TX_SAFETY.md, docs/BENCH.md).
     */
    if (on && bench.power == 0) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }

    pcProtoBenchArgs_t a;
    memset(&a, 0, sizeof(a));
    a.ptt = on;
    a.toneHz = on ? tone : 0;
    a.level = level;
    a.power = bench.power;
    int rc = pc_proto_platformBench(PC_BENCH_OP_PTT, &a);
    if (rc != 0) {
        if (on) {
            /* Whatever the platform armed before it refused, take back: the
             * client is about to be told the order failed and nobody will send
             * the PTT-off for it. */
            pcProtoBenchArgs_t rel;
            memset(&rel, 0, sizeof(rel));
            (void)pc_proto_platformBench(PC_BENCH_OP_RELEASE, &rel);
        }
        if (rc == -(int)PC_ERR_BUSY)
            r->err = PC_ERR_BUSY;
        else if (on && a.power == 0)
            r->err = PC_ERR_INVALID_VALUE; /* nothing to transmit with */
        else
            r->err = PC_ERR_NOT_SUPPORTED;
        return -1;
    }
    /* The platform reports the power it really keyed with, which is what the
     * run has to be compared against. */
    if (a.power == 0 && on) {
        pcProtoBenchArgs_t rel;
        memset(&rel, 0, sizeof(rel));
        (void)pc_proto_platformBench(PC_BENCH_OP_RELEASE, &rel);
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }
    bench.power = a.power;

    uint32_t tick = benchNow();
    if (on) {
        bench.ptt = 1;
        bench.pttOnTick = tick;
        bench.toneHz = a.toneHz;
        bench.level = a.level;
    } else {
        bench.ptt = 0;
        bench.pttOffTick = tick;
        bench.toneHz = 0;
    }
    /* The answer says what the bench now holds, not what the PA is doing this
     * millisecond: the rtx thread takes up to one period (25 ms) to key, and
     * the moment the carrier appears is what the SDR measures, not what this
     * field could claim.  The edge event carries the same tick for that. */
    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_STATUS, PC_OK);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_TX_STATE, on ? 2 : 1);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_BENCH_TONE, bench.toneHz);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_BENCH_LEVEL, bench.level);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_BENCH_POWER, bench.power);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_TICK, tick);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;

    /* The answer goes out first and the edge event right behind it: a client
     * that is waiting for the answer must not have to sort the event out of
     * the same read, and the event is stamped with the same tick either way. */
    pc_proto_sendResponse(r->p, r->seq, r->id, 0, r->out, off);
    benchEdge(r, on, tick);
    return 1; /* already answered */
}

/*
 * ----------------------------------------------------------------------------
 * CAPTURE: arm a window, then read it back in chunks
 * ----------------------------------------------------------------------------
 */

/** Refresh the capture bookkeeping from the platform, if there is one. */
static void benchCaptureSync(void)
{
    pcProtoCapture_t st;
    memset(&st, 0, sizeof(st));
    if (pc_proto_platformCapture(PC_CAP_OP_STATUS, &st, NULL, 0, 0) != 0)
        return;
    bench.capWritten = st.written;
    bench.capBlocks = st.blocks;
    bench.capBlockLen = st.blockLen;
    bench.capOverflow = st.overflow;
    if (st.t0Ms != 0 && bench.capBlocks == 0)
        bench.capT0 = st.t0Ms;
}

/** Fill the capture tags of a response. */
static size_t benchCaptureTlv(struct pcReq *r, size_t off, uint32_t offset,
                              uint16_t count, const uint8_t *data)
{
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_STATUS, PC_OK);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_OFFSET, offset);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_LENGTH, count);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_T0, bench.capT0);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_RATE, bench.capRate);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_WRITTEN,
                     bench.capWritten);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_BENCH_WINDOW,
                     (uint16_t)bench.capWindowMs);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_BENCH_BLOCK,
                     bench.capBlockLen);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_BLOCKS,
                     bench.capBlocks);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_RUNNING,
                    bench.capArmed ? 1 : 0);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_BENCH_OVERFLOW,
                    bench.capOverflow);
    if (data != NULL && count != 0)
        off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_DATA, data,
                           (size_t)count * 2);
    return off;
}

int pc_bench_capture(struct pcReq *r)
{
    uint16_t window = 0;
    uint32_t offset = 0;
    uint16_t count = BENCH_CAP_CHUNK;
    bool hasOffset = pc_tlv_get_u32(r->in, r->inLen, PC_TAG_OFFSET, &offset);

    if (!benchTokenOk(r))
        return -1;

    if (pc_tlv_get_u16(r->in, r->inLen, PC_TAG_BENCH_WINDOW, &window)) {
        if (window == 0 || window > BENCH_CAP_MAX_MS) {
            r->err = PC_ERR_INVALID_VALUE;
            return -1;
        }
        if (bench.swState != SW_IDLE) {
            r->err = PC_ERR_BUSY;
            return -1;
        }
        pcProtoCapture_t st;
        memset(&st, 0, sizeof(st));
        if (pc_proto_platformCapture(PC_CAP_OP_STATUS, &st, NULL, 0, 0) == 0 &&
            st.running) {
            /* One buffer, one user: the debug namespace may be holding it. */
            r->err = PC_ERR_BUSY;
            return -1;
        }
        uint16_t rate = st.rate != 0 ? st.rate : 48000;
        memset(&st, 0, sizeof(st));
        st.capacity = (uint32_t)window * rate / 1000u;
        /* LIVE: record what the audio path delivers, do not hand over a buffer
         * that was already full -- a bench window is watched while it fills. */
        if (pc_proto_platformCapture(PC_CAP_OP_START_LIVE, &st, NULL, 0, 0) != 0) {
            r->err = PC_ERR_NOT_SUPPORTED;
            return -1;
        }
        memset(&st, 0, sizeof(st));
        if (pc_proto_platformCapture(PC_CAP_OP_STATUS, &st, NULL, 0, 0) != 0) {
            st.capacity = (uint32_t)window * rate / 1000u;
            st.rate = rate;
            st.running = 1;
        }
        bench.capRate = st.rate != 0 ? st.rate : rate;
        bench.capWindowMs = window;
        bench.capWant = (uint32_t)window * bench.capRate / 1000u;
        if (st.capacity != 0 && bench.capWant > st.capacity)
            bench.capWant = st.capacity;
        bench.capT0 = st.t0Ms != 0 ? st.t0Ms : benchNow();
        bench.capWritten = 0;
        bench.capBlocks = 0;
        bench.capBlockLen = 0;
        bench.capOverflow = 0;
        bench.capArmed = true;
        bench.capEverArmed = true;
    }

    if (!hasOffset && window == 0) {
        /* Neither a window nor an offset: the status of the last one. */
        benchCaptureSync();
        size_t off = benchCaptureTlv(r, 0, 0, 0, NULL);
        if (off == 0) {
            r->err = PC_ERR_TOO_LARGE;
            return -1;
        }
        r->outLen = off;
        return 0;
    }

    if (!pc_tlv_get_u16(r->in, r->inLen, PC_TAG_LENGTH, &count))
        count = BENCH_CAP_CHUNK;
    if (count == 0)
        count = BENCH_CAP_CHUNK;
    if (count > BENCH_CAP_CHUNK)
        count = BENCH_CAP_CHUNK;

    uint8_t buf[BENCH_CAP_CHUNK * 2];
    pcProtoCapture_t st;
    uint16_t got = 0;
    benchCaptureSync();
    if (hasOffset && bench.capWritten != 0) {
        if (offset >= bench.capWritten) {
            count = 0;
        } else if (offset + count > bench.capWritten) {
            count = (uint16_t)(bench.capWritten - offset);
        }
        if (count != 0) {
            memset(&st, 0, sizeof(st));
            if (pc_proto_platformCapture(PC_CAP_OP_READ, &st, buf, offset,
                                         count) < (int)count) {
                r->err = PC_ERR_BAD_INDEX;
                return -1;
            }
            got = count;
        }
    } else {
        count = 0;
    }

    size_t off = benchCaptureTlv(r, 0, offset, got, got != 0 ? buf : NULL);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * RSSI
 * ----------------------------------------------------------------------------
 */

/** Append the collected RSSI series; returns the new offset. */
static size_t benchRssiTlv(struct pcReq *r, size_t off)
{
    uint8_t list[BENCH_RSSI_MAX * 6];
    size_t n = 0;
    for (uint8_t i = 0; i < bench.rssiCount; i++) {
        uint32_t t = bench.rssi[i].tick;
        uint16_t v = (uint16_t)bench.rssi[i].dbm;
        list[n++] = (uint8_t)(t & 0xFF);
        list[n++] = (uint8_t)((t >> 8) & 0xFF);
        list[n++] = (uint8_t)((t >> 16) & 0xFF);
        list[n++] = (uint8_t)((t >> 24) & 0xFF);
        list[n++] = (uint8_t)(v & 0xFF);
        list[n++] = (uint8_t)((v >> 8) & 0xFF);
    }
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_WRITTEN,
                     bench.rssiCount);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_COUNT, bench.rssiWanted);
    if (n != 0)
        off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_BENCH_RSSI_TS, list,
                           n);
    return off;
}

int pc_bench_rssi(struct pcReq *r)
{
    uint8_t want = 1;
    /* COUNT starts a series, its absence reads the one in progress.  One
     * message cannot mean both, or a client polling a finished series would
     * restart it every time it asked for the result. */
    bool start = pc_tlv_get_u8(r->in, r->inLen, PC_TAG_COUNT, &want);

    if (!benchTokenOk(r))
        return -1;
    if (start) {
        if (want == 0)
            want = 1;
        if (want > BENCH_RSSI_MAX) {
            r->err = PC_ERR_INVALID_VALUE;
            return -1;
        }
        bench.rssiWanted = want;
        bench.rssiCount = 0;
    }
    if (bench.rssiWanted == 0)
        bench.rssiWanted = 1;

    if (bench.rssiCount == 0) {
        /* The first reading goes out in this answer: the common case, a single
         * sample, costs one message and no waiting. */
        pcProtoBenchArgs_t a;
        if (pc_proto_platformBench(PC_BENCH_OP_RSSI, &a) == 0) {
            bench.rssi[0].tick = a.nowMs != 0 ? a.nowMs : benchNow();
            bench.rssi[0].dbm = a.rssi;
            bench.rssiCount = 1;
            bench.rssiNext = bench.rssi[0].tick + BENCH_TICK_MS;
        }
    }

    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_STATUS, PC_OK);
    off = benchRssiTlv(r, off);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_TICK, bench.nowMs);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * SWEEP
 * ----------------------------------------------------------------------------
 */

/** Append the sweep points from bench.swSent onwards; advances the cursor so a
 *  client that keeps polling walks the whole series once. */
static size_t benchSweepTlv(struct pcReq *r, size_t off)
{
    uint8_t list[BENCH_POINTS_CHUNK * 6];
    size_t n = 0;
    uint16_t i = bench.swSent;
    while (i < bench.swIndex && n / 6 < BENCH_POINTS_CHUNK) {
        uint32_t hz = bench.points[i].hz;
        uint16_t v = (uint16_t)bench.points[i].dbm;
        list[n++] = (uint8_t)(hz & 0xFF);
        list[n++] = (uint8_t)((hz >> 8) & 0xFF);
        list[n++] = (uint8_t)((hz >> 16) & 0xFF);
        list[n++] = (uint8_t)((hz >> 24) & 0xFF);
        list[n++] = (uint8_t)(v & 0xFF);
        list[n++] = (uint8_t)((v >> 8) & 0xFF);
        i++;
    }
    bench.swSent = i;

    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_F0, bench.swF0);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_F1, bench.swF1);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_STEP, bench.swStep);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_BENCH_DWELL, bench.swDwell);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_BENCH_POINTS, bench.swTotal);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_WRITTEN, bench.swIndex);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_BENCH_T0, bench.swT0);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_RUNNING,
                    bench.swState == SW_IDLE ? 0 : 1);
    if (n != 0)
        off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_BENCH_POINTS_TS, list,
                           n);
    return off;
}

int pc_bench_sweep(struct pcReq *r)
{
    uint32_t f0 = 0, f1 = 0, step = 0;
    uint16_t dwell = 100;

    if (!benchTokenOk(r))
        return -1;

    if (pc_tlv_get_u32(r->in, r->inLen, PC_TAG_BENCH_F0, &f0) &&
        pc_tlv_get_u32(r->in, r->inLen, PC_TAG_BENCH_F1, &f1) &&
        pc_tlv_get_u32(r->in, r->inLen, PC_TAG_BENCH_STEP, &step)) {
        pc_tlv_get_u16(r->in, r->inLen, PC_TAG_BENCH_DWELL, &dwell);

        if (f1 <= f0 || step == 0 || step > (f1 - f0)) {
            r->err = PC_ERR_INVALID_VALUE;
            return -1;
        }
        if (bench.capArmed || bench.swState != SW_IDLE || bench.ptt) {
            /* A sweep retunes 96 times; with the PA up that is 96
             * transmissions nobody asked for (see pc_bench_tune). */
            r->err = PC_ERR_BUSY;
            return -1;
        }
        if (dwell < BENCH_TICK_MS)
            dwell = BENCH_TICK_MS;

        uint32_t total = (f1 - f0 + step - 1) / step;
        if (total == 0 || total > BENCH_POINTS_MAX) {
            r->err = PC_ERR_TOO_LARGE;
            return -1;
        }
        bench.swF0 = f0;
        bench.swF1 = f1;
        bench.swStep = step;
        bench.swDwell = dwell;
        bench.swTotal = (uint16_t)total;
        bench.swIndex = 0;
        bench.swSent = 0;
        bench.swT0 = benchNow();
        bench.swState = SW_RETUNE;
        bench.swNext = bench.swT0;
    }

    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_STATUS, PC_OK);
    off = benchSweepTlv(r, off);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * REPORT
 * ----------------------------------------------------------------------------
 */

/** Build the whole report text.  Deterministic: same state, same text. */
static uint16_t benchReportBuild(void)
{
    char *b = bench.report;
    size_t cap = sizeof(bench.report);
    size_t n = 0;

    n += (size_t)snprintf(b + n, cap - n, "bench run %lu\n",
                          (unsigned long)bench.epoch);
    if (!bench.armed) {
        n += (size_t)snprintf(b + n, cap - n,
                              "not armed: run bench.arm first\n");
        bench.reportLen = (uint16_t)(n < cap ? n : cap - 1);
        return bench.reportLen;
    }
    n += (size_t)snprintf(b + n, cap - n, "armed  %lu ms\n",
                          (unsigned long)bench.armTick);

    if (bench.tuned) {
        n += (size_t)snprintf(b + n, cap - n,
                              "tune   rx %lu Hz tx %lu Hz mode %u bw %u "
                              "delta %ld Hz\n",
                              (unsigned long)bench.rxHz,
                              (unsigned long)bench.txHz, bench.mode, bench.bw,
                              (long)bench.deltaHz);
    } else {
        n += (size_t)snprintf(b + n, cap - n, "tune   none\n");
    }

    n += (size_t)snprintf(b + n, cap - n,
                          "ptt    %s on %lu ms off %lu ms tone %u Hz level %u\n",
                          bench.ptt ? "keyed" : "off",
                          (unsigned long)bench.pttOnTick,
                          (unsigned long)bench.pttOffTick, bench.toneHz,
                          bench.level);
    n += (size_t)snprintf(b + n, cap - n, "power  %u mW\n", bench.power);

    if (bench.capRate != 0) {
        n += (size_t)snprintf(b + n, cap - n,
                              "capture %lu ms at %u Hz t0 %lu samples %lu "
                              "blocks %lu x %u overflow %u\n",
                              (unsigned long)bench.capWindowMs, bench.capRate,
                              (unsigned long)bench.capT0,
                              (unsigned long)bench.capWritten,
                              (unsigned long)bench.capBlocks,
                              bench.capBlockLen, bench.capOverflow);
    } else {
        n += (size_t)snprintf(b + n, cap - n, "capture none\n");
    }

    if (bench.rssiCount != 0) {
        int16_t lo = bench.rssi[0].dbm, hi = lo;
        int32_t sum = 0;
        for (uint8_t i = 0; i < bench.rssiCount; i++) {
            if (bench.rssi[i].dbm < lo)
                lo = bench.rssi[i].dbm;
            if (bench.rssi[i].dbm > hi)
                hi = bench.rssi[i].dbm;
            sum += bench.rssi[i].dbm;
        }
        /* Mean in tenths, printed by hand: newlib-nano has no %f. */
        int32_t mean10 = sum * 10 / bench.rssiCount;
        int32_t frac = mean10 < 0 ? -mean10 : mean10;
        n += (size_t)snprintf(b + n, cap - n,
                              "rssi   %u samples min %d mean %ld.%ld max %d dBm"
                              " span %lu..%lu ms\n",
                              bench.rssiCount, lo, (long)(mean10 / 10),
                              (long)(frac % 10), hi,
                              (unsigned long)bench.rssi[0].tick,
                              (unsigned long)bench.rssi[bench.rssiCount - 1]
                                  .tick);
    } else {
        n += (size_t)snprintf(b + n, cap - n, "rssi   none\n");
    }

    if (bench.swTotal != 0) {
        int16_t lo = 0, hi = 0;
        if (bench.swIndex != 0) {
            lo = bench.points[0].dbm;
            hi = lo;
            for (uint16_t i = 1; i < bench.swIndex; i++) {
                if (bench.points[i].dbm < lo)
                    lo = bench.points[i].dbm;
                if (bench.points[i].dbm > hi)
                    hi = bench.points[i].dbm;
            }
        }
        n += (size_t)snprintf(b + n, cap - n,
                              "sweep  %lu..%lu Hz step %lu Hz %u/%u points "
                              "min %d max %d dBm\n",
                              (unsigned long)bench.swF0,
                              (unsigned long)bench.swF1,
                              (unsigned long)bench.swStep, bench.swIndex,
                              bench.swTotal, lo, hi);
    } else {
        n += (size_t)snprintf(b + n, cap - n, "sweep  none\n");
    }

    n += (size_t)snprintf(b + n, cap - n,
                          "note   no carrier reference: frequency error is "
                          "measured by the SDR, not by the radio\n");

    bench.reportLen = (uint16_t)(n < cap ? n : cap - 1);
    return bench.reportLen;
}

int pc_bench_report(struct pcReq *r)
{
    uint32_t offset = 0;

    pc_tlv_get_u32(r->in, r->inLen, PC_TAG_OFFSET, &offset);
    uint16_t total = benchReportBuild();
    if (offset > total)
        offset = total;
    uint16_t n = (uint16_t)(total - offset);
    if (n > BENCH_REPORT_CHUNK)
        n = BENCH_REPORT_CHUNK;

    /* The text goes out as bytes, not as a NUL-terminated string: a chunk can
     * start anywhere, including inside a line. */
    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_STATUS, PC_OK);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_OFFSET, offset);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_LENGTH, total);
    off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_TEXT, &bench.report[offset],
                       n);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * Tick and reset
 * ----------------------------------------------------------------------------
 */

void pc_proto_benchTick(uint32_t nowMs)
{
    bench.nowMs = nowMs;

    if (bench.capArmed) {
        benchCaptureSync();
        pcProtoCapture_t st;
        memset(&st, 0, sizeof(st));
        if (pc_proto_platformCapture(PC_CAP_OP_STATUS, &st, NULL, 0, 0) == 0) {
            if (bench.capWritten >= bench.capWant || !st.running) {
                memset(&st, 0, sizeof(st));
                (void)pc_proto_platformCapture(PC_CAP_OP_STOP, &st, NULL, 0, 0);
                bench.capArmed = false;
            }
        }
    }

    if (bench.rssiWanted != 0 && bench.rssiCount < bench.rssiWanted &&
        (int32_t)(nowMs - bench.rssiNext) >= 0) {
        pcProtoBenchArgs_t a;
        benchPoll(&a);
        bench.rssi[bench.rssiCount].tick = a.nowMs;
        bench.rssi[bench.rssiCount].dbm = a.rssi;
        bench.rssiCount++;
        bench.rssiNext = bench.rssi[bench.rssiCount - 1].tick + BENCH_TICK_MS;
    }

    if (bench.swState == SW_RETUNE) {
        if (bench.swIndex >= bench.swTotal) {
            bench.swState = SW_IDLE;
            return;
        }
        uint32_t target = bench.swF0 + (uint32_t)bench.swIndex * bench.swStep;
        pcProtoBenchArgs_t a;
        memset(&a, 0, sizeof(a));
        a.rxHz = target;
        a.txHz = target;
        a.mode = bench.mode;
        a.bw = bench.bw;
        if (pc_proto_platformBench(PC_BENCH_OP_TUNE, &a) != 0) {
            /* Out of reach: the point is recorded as 0 dBm and the sweep goes
             * on, so one hole does not end the run. */
            bench.points[bench.swIndex].hz = target;
            bench.points[bench.swIndex].dbm = 0;
            bench.swIndex++;
            return;
        }
        bench.swNext = nowMs + bench.swDwell;
        bench.swState = SW_WAIT;
        return;
    }

    if (bench.swState == SW_WAIT && (int32_t)(nowMs - bench.swNext) >= 0) {
        uint32_t target = bench.swF0 + (uint32_t)bench.swIndex * bench.swStep;
        pcProtoBenchArgs_t a;
        benchPoll(&a);
        /* The dwell has passed; if the receiver has not reached the frequency
         * yet, wait one more tick rather than label the old one as this. */
        if (a.rxHz != 0 && a.rxHz != target) {
            bench.swNext = nowMs + BENCH_TICK_MS;
            return;
        }
        bench.points[bench.swIndex].hz = target;
        bench.points[bench.swIndex].dbm = a.rssi;
        bench.swIndex++;
        bench.swState = SW_RETUNE;
    }
}

void pc_proto_benchReset(void)
{
    benchRelease();
    memset(&bench, 0, sizeof(bench));
}

#else /* !CONFIG_C62_BENCH_PROTO */

/* A production build: the messages exist in the table, the dispatcher refuses
 * them on the missing capability, and these stubs make sure that even a call
 * that got past it gets nothing. */

static int benchDenied(struct pcReq *r)
{
    r->err = PC_ERR_NOT_ALLOWED;
    return -1;
}

int pc_bench_arm(struct pcReq *r)     { return benchDenied(r); }
int pc_bench_tune(struct pcReq *r)    { return benchDenied(r); }
int pc_bench_ptt(struct pcReq *r)     { return benchDenied(r); }
int pc_bench_capture(struct pcReq *r) { return benchDenied(r); }
int pc_bench_rssi(struct pcReq *r)    { return benchDenied(r); }
int pc_bench_sweep(struct pcReq *r)   { return benchDenied(r); }
int pc_bench_report(struct pcReq *r)  { return benchDenied(r); }

void pc_proto_benchTick(uint32_t nowMs) { (void)nowMs; }
void pc_proto_benchReset(void) { }

#endif /* CONFIG_C62_BENCH_PROTO */
