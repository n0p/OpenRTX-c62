/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Debug namespace of the radio protocol: streaming log, diagnostics, register
 * and memory access, RX audio capture, baseband dump control and the transmit
 * trace.  It exists so the radio can be debugged with the cable that is
 * already plugged in, without opening it and without watching a LED.
 *
 * Three gates, and all of them have to open for anything to happen:
 *
 *  1. The build advertises PC_CAP_DEBUG, which happens only with
 *     CONFIG_C62_DEBUG_PROTO.  Without it the dispatcher answers
 *     PC_ERR_NOT_ALLOWED before this file is even reached, and the handlers
 *     below are compiled out into stubs.
 *  2. The session token: HELLO hands it out and every write has to echo it
 *     back.  It is a guard against a stale or accidental client, not a
 *     security boundary -- whoever holds the cable can read the token out of
 *     the HELLO response.  See docs/PC_PROTO.md, "What the debug namespace
 *     exposes".
 *  3. A whitelist per chip for the writes, and a hard ceiling on the memory
 *     reads and the captures.
 */

#include <stddef.h>
#include <string.h>

#include "core/pc_proto.h"
#include "pc_proto_internal.h"

/* Chips the register accessor knows. */
#define DBG_CHIP_BK4819 0
#define DBG_CHIP_BK1080 1

/** Longest block DBG_MEM_READ will hand over, whatever is asked for. */
#define DBG_MEM_MAX 128

/** Longest capture, in samples: 2 s at 8 kHz. */
#define DBG_CAP_MAX 16000

/** Samples per DBG_CAP_READ response. */
#define DBG_CAP_CHUNK 128

/** Longest record the post-mortem message will serve, and per-answer chunk. */
#define DBG_BLACKBOX_MAX   1024
#define DBG_BLACKBOX_CHUNK 200

/*
 * ----------------------------------------------------------------------------
 * Platform hooks, weak so every host test and every other target links
 * unchanged.  A platform that returns a negative value turns the message into
 * PC_ERR_NOT_SUPPORTED: the namespace answers, it just has nothing to read.
 * ----------------------------------------------------------------------------
 */

__attribute__((weak)) int pc_proto_platformDiag(pcProtoDiag_t *out)
{
    if (out != NULL)
        memset(out, 0, sizeof(*out));
    return -1;
}

__attribute__((weak)) int pc_proto_platformReg(int chip, uint8_t reg,
                                               uint16_t *value)
{
    (void)chip;
    (void)reg;
    (void)value;
    return -1;
}

__attribute__((weak)) int pc_proto_platformRegWrite(int chip, uint8_t reg,
                                                    uint16_t value)
{
    (void)chip;
    (void)reg;
    (void)value;
    return -1;
}

__attribute__((weak)) int pc_proto_platformMem(uint32_t addr, uint8_t *buf,
                                               uint16_t len)
{
    (void)addr;
    (void)buf;
    (void)len;
    return -1;
}

__attribute__((weak)) int pc_proto_platformCapture(int op, pcProtoCapture_t *st,
                                                   uint8_t *buf, uint32_t off,
                                                   uint32_t count)
{
    (void)op;
    (void)st;
    (void)buf;
    (void)off;
    (void)count;
    return -1;
}

__attribute__((weak)) int pc_proto_platformBbDump(int op, uint8_t *state)
{
    (void)op;
    if (state != NULL)
        *state = 0x80; /* no dump in this build */
    return 0;
}

__attribute__((weak)) int pc_proto_platformTxTrace(char *buf, size_t cap)
{
    (void)buf;
    (void)cap;
    return -1;
}

__attribute__((weak)) int pc_proto_platformTxWatch(uint16_t *seconds,
                                                   const char **reason)
{
    (void)seconds;
    (void)reason;
    return -1;
}

/**
 * The post-mortem the last crash left, as it sits on flash.
 *
 * @return bytes copied, or a negative value when there is no record, which the
 *         handler turns into PC_ERR_NOT_SUPPORTED (a radio that has not crashed
 *         is not a radio that cannot answer).
 */
__attribute__((weak)) int pc_proto_platformBlackbox(uint32_t off, uint8_t *buf,
                                                    uint16_t cap,
                                                    uint16_t *total)
{
    (void)off;
    (void)buf;
    (void)cap;
    (void)total;
    return -1;
}

#ifdef CONFIG_C62_DEBUG_PROTO

/*
 * ----------------------------------------------------------------------------
 * Register whitelist
 * ----------------------------------------------------------------------------
 */

/**
 * Registers the debug accessor will write.
 *
 * Deliberately short and boring: registers the radio driver rewrites by itself
 * on the next mode or frequency change.  Poking one of them can make the radio
 * deaf or silent for a moment, but not for ever, which is exactly the property
 * a bench tool needs.  Anything outside this table is refused even with a
 * valid token, and the table is the one place to extend.
 */
static const struct
{
    uint8_t chip;
    uint8_t reg;
    const char *why;
}
dbgWriteWhitelist[] =
{
    { DBG_CHIP_BK4819, 0x30, "audio path"          }, /* REG_30 */
    { DBG_CHIP_BK4819, 0x40, "TX deviation"        }, /* REG_40 */
    { DBG_CHIP_BK4819, 0x47, "AF level"            }, /* REG_47 */
    { DBG_CHIP_BK4819, 0x48, "RX audio gains"      }, /* REG_48 */
    { DBG_CHIP_BK4819, 0x7D, "microphone gain"     }, /* REG_7D */
    { DBG_CHIP_BK1080, 0x02, "power and mute"      }, /* REG_02 */
};

#define DBG_WRITE_COUNT \
    (sizeof(dbgWriteWhitelist) / sizeof(dbgWriteWhitelist[0]))

static bool regReadable(uint8_t chip, uint8_t reg)
{
    /* The whole readable register file of each chip: the stock firmware dumps
     * it too, and a register the firmware does not use is exactly what a bench
     * session wants to look at. */
    if (chip == DBG_CHIP_BK4819)
        return reg <= 0x7F;
    if (chip == DBG_CHIP_BK1080)
        return reg <= 0x21;
    return false;
}

static bool regWritable(uint8_t chip, uint8_t reg)
{
    for (size_t i = 0; i < DBG_WRITE_COUNT; i++) {
        if (dbgWriteWhitelist[i].chip == chip &&
            dbgWriteWhitelist[i].reg == reg)
            return true;
    }
    return false;
}

/*
 * ----------------------------------------------------------------------------
 * Diagnostics
 * ----------------------------------------------------------------------------
 */

int pc_dbg_info(struct pcReq *r)
{
    pcProtoDiag_t d;
    int haveDiag = pc_proto_platformDiag(&d);

    /* Protocol and firmware identity are always answered: they are what lets a
     * client tell "the radio is old" from "the radio is broken". */
    size_t off = pc_tlv_u16(r->out, r->outCap, 0, PC_TAG_VERSION,
                            PC_PROTO_VERSION);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_VERSION_MIN,
                     PC_PROTO_VERSION_MIN);
    off = pc_tlv_str(r->out, r->outCap, off, PC_TAG_FIRMWARE,
                     pc_proto_platformFirmware() != NULL
                         ? pc_proto_platformFirmware() : "");
    off = pc_tlv_str(r->out, r->outCap, off, PC_TAG_HARDWARE,
                     pc_proto_platformHardware() != NULL
                         ? pc_proto_platformHardware() : "");
    off = pc_tlv_str(r->out, r->outCap, off, PC_TAG_MODEL,
                     pc_proto_platformModel() != NULL
                         ? pc_proto_platformModel() : "");
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_UPTIME_MS, r->p->nowMs);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_CAPS, pc_proto_caps());

    if (haveDiag == 0) {
        off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_RESET_CAUSE,
                         d.resetCause);
        off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_WDT_RESETS,
                         d.wdtResets);
        /*
         * Memory, only where the platform measured it. A tag that is missing
         * says "this radio cannot tell", which is the truth: a zero would be
         * printed by a client as "0 of 40960 bytes free", and that is exactly
         * what the diagnostics used to say about a heap of 40 KiB that was
         * mostly free (docs/PC_PROTO.md).
         */
        if (d.heapKnown) {
            off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_HEAP_FREE,
                             d.heapFree);
            off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_HEAP_TOTAL,
                             d.heapTotal);
        }
        if (d.psramKnown) {
            off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_PSRAM_FREE,
                             d.psramFree);
            off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_PSRAM_TOTAL,
                             d.psramTotal);
        }
        if (d.flashTotal != 0) {
            off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_FLASH_USED,
                             d.flashUsed);
            off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_FLASH_TOTAL,
                             d.flashTotal);
        }
        if (d.ramTotal != 0) {
            off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_RAM_USED,
                             d.ramUsed);
            off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_RAM_TOTAL,
                             d.ramTotal);
        }
        /*
         * The regions travel as one array, the way the task stacks do: a tag
         * per region would collapse in any client that keeps a tag -> value
         * map, and the last one would be the only one shown.
         */
        if (d.regionCount > 0) {
            uint8_t count = (d.regionCount < PC_DIAG_REGIONS) ? d.regionCount
                                                             : PC_DIAG_REGIONS;
            off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_MEM_REGION,
                               &d.regions[0],
                               (size_t)count * sizeof(pcProtoRegion_t));
        }
        off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_AGC, d.agc);
        /* The power the radio would key with: in the diagnostics as well as in
         * the status, because "the PTT does nothing" is the first thing a bench
         * session asks about (docs/TX_SAFETY.md). */
        off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_TX_POWER, d.txPower);
        off = pc_tlv_str(r->out, r->outCap, off, PC_TAG_TX_REASON,
                         d.txReason[0] != '\0' ? d.txReason : "none");
        /*
         * The accessory port: which mode owns the pad, whether its switch is
         * down and what the transport has thrown away because of it.  With
         * "Auto" this is the pair a bench session reads first when a
         * speaker-microphone seems deaf or the CPS stops answering while the
         * operator keys (docs/ACCESSORY.md).
         */
        off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_PORT_MODE, d.portMode);
        off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_PORT_PTT, d.portPtt);
        off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_PORT_LOST, d.portLost);

        /*
         * The transmit clock: how much air the PA has put out since it was last
         * released and what it is doing. It goes out whether or not the radio
         * is transmitting, because the interesting case -- a transmission that
         * nobody can stop -- is one where the answer must not read as idle.
         */
        uint16_t txSeconds = 0;
        const char *txReason = NULL;
        if (pc_proto_platformTxWatch(&txSeconds, &txReason) == 0) {
            off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_TX_SECONDS,
                             txSeconds);
            off = pc_tlv_str(r->out, r->outCap, off, PC_TAG_TX_REASON,
                             (txReason != NULL) ? txReason : "");
        }
        if (d.taskCount > 8)
            d.taskCount = 8;
        off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_TASK_STACK,
                           d.tasks,
                           (size_t)d.taskCount * sizeof(pcProtoTaskStack_t));
        /*
         * The screen path.  A frozen panel with a live radio used to leave
         * nothing behind (docs/RX_AIRBAND.md §9): these four numbers and the
         * descriptor of the transfer that was lost are what turns the next
         * reproduction into a one-line answer.
         */
        off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_DISP_FRAMES,
                         d.dispFrames);
        off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_DISP_DROPS,
                         d.dispDrops);
        off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_DISP_MS, d.dispLastMs);
        off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_DISP_MAX_MS,
                         d.dispMaxMs);
        if (d.dispDrops > 0) {
            uint8_t fail[4];
            fail[0] = (uint8_t)(d.dispFailLen & 0xFF);
            fail[1] = (uint8_t)(d.dispFailLen >> 8);
            fail[2] = d.dispFailChunk;
            fail[3] = d.dispFailData;
            off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_DISP_FAIL, fail,
                               sizeof(fail));
        }
    }

    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_DROPPED,
                     pc_proto_logDropped(r->p));
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;

    /* The per-mode statistics go out on their own message: they are lines of
     * text and would push this frame past the ceiling. */
    return 0;
}

int pc_dbg_rxCounters(struct pcReq *r)
{
    /* One implementation, two entry points: the CPS message and the debug one
     * answer with the same lines, so a client that knows one knows both. */
    return pc_ops_statsGet(r);
}

/*
 * ----------------------------------------------------------------------------
 * Log streaming
 * ----------------------------------------------------------------------------
 */

int pc_dbg_logSub(struct pcReq *r)
{
    uint8_t level = PC_LOG_INFO;
    uint16_t rate;
    const uint8_t *mod;
    uint16_t modLen;

    pc_tlv_get_u8(r->in, r->inLen, PC_TAG_LEVEL, &level);
    if (level > PC_LOG_DEBUG) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }
    if (!pc_tlv_get_u16(r->in, r->inLen, PC_TAG_RATE, &rate))
        rate = 20; /* a readable default: 20 lines a second on a 11.5 kB cable */

    /* A client cannot ask for more than the cable can carry: 200 lines a
     * second of 40 bytes each is already 8 kB/s of the 11.5 available. */
    if (rate > 200)
        rate = 200;

    r->p->logLevel = level;
    if (pc_tlv_find(r->in, r->inLen, PC_TAG_MODULE, &mod, &modLen)) {
        if (modLen >= sizeof(r->p->logModule)) {
            r->err = PC_ERR_TOO_LARGE;
            return -1;
        }
        memset(r->p->logModule, 0, sizeof(r->p->logModule));
        memcpy(r->p->logModule, mod, modLen);
    } else {
        r->p->logModule[0] = '\0';
    }

    r->p->logRate = rate;
    r->p->logLastMs = r->p->nowMs;
    /* A subscription starts a new stream: lines buffered before it belong to
     * whoever was listening then (nobody), and keeping them would send lines
     * that do not match the filter the client just set. */
    r->p->logCount = 0;
    r->p->logHead = 0;
    /* No free line at the start: the first one goes out when a line's worth of
     * time has passed, so "N lines a second" holds from the first second. */
    r->p->logTokens = 0;

    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_LEVEL,
                           r->p->logLevel);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_RATE, r->p->logRate);
    off = pc_tlv_str(r->out, r->outCap, off, PC_TAG_MODULE, r->p->logModule);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_DROPPED,
                     pc_proto_logDropped(r->p));
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_dbg_logDrain(struct pcReq *r)
{
    /* The transport drains on its own tick; this only reports, so that a
     * client can see whether lines are waiting or the ring is empty. */
    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_COUNT,
                           r->p->logCount);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_DROPPED,
                     pc_proto_logDropped(r->p));
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_LOG_SEQ, r->p->logSeq);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * Hardware access
 * ----------------------------------------------------------------------------
 */

int pc_dbg_regRead(struct pcReq *r)
{
    uint8_t chip, reg;
    uint16_t value = 0;

    if (!pc_tlv_get_u8(r->in, r->inLen, PC_TAG_CHIP, &chip) ||
        !pc_tlv_get_u8(r->in, r->inLen, PC_TAG_REG, &reg)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    if (!regReadable(chip, reg)) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }
    if (pc_proto_platformReg(chip, reg, &value) != 0) {
        r->err = PC_ERR_NOT_SUPPORTED;
        return -1;
    }

    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_CHIP, chip);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_REG, reg);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_VALUE, value);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_dbg_regWrite(struct pcReq *r)
{
    uint8_t chip, reg, confirm = 0;
    uint16_t value;

    /* The token first: a client without it learns nothing about the whitelist
     * or the chip, it only learns that it is not allowed. */
    if (!pc_proto_tokenOk(r->p, r->in, r->inLen)) {
        r->err = PC_ERR_NOT_ALLOWED;
        return -1;
    }
    if (!pc_tlv_get_u8(r->in, r->inLen, PC_TAG_CHIP, &chip) ||
        !pc_tlv_get_u8(r->in, r->inLen, PC_TAG_REG, &reg) ||
        !pc_tlv_get_u16(r->in, r->inLen, PC_TAG_VALUE, &value) ||
        !pc_tlv_get_u8(r->in, r->inLen, PC_TAG_CONFIRM, &confirm) ||
        confirm != 1) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    if (!regWritable(chip, reg) || !regReadable(chip, reg)) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }
    if (pc_proto_platformRegWrite(chip, reg, value) != 0) {
        r->err = PC_ERR_NOT_SUPPORTED;
        return -1;
    }

    /* Read back: a register write that did not take is the sort of bug this
     * whole namespace exists to find. */
    uint16_t back = 0;
    int rc = pc_proto_platformReg(chip, reg, &back);
    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_CHIP, chip);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_REG, reg);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_VALUE, back);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_VERIFY,
                    (uint8_t)(rc == 0 && back == value));
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_dbg_agc(struct pcReq *r)
{
    pcProtoStatus_t s;
    pc_proto_status(&s);

    size_t off = pc_tlv_i16(r->out, r->outCap, 0, PC_TAG_RSSI, s.rssi);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_AGC, 0xFF); /* unknown */
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_RX_FREQ, s.rxFreq);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_SQUELCH, s.squelchOpen);

    /* The register that carries the RSSI is readable on this radio, so the
     * answer is the filtered value and the raw one, when the platform has it. */
    uint16_t raw = 0;
    if (pc_proto_platformReg(DBG_CHIP_BK4819, 0x67, &raw) == 0)
        off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_VALUE, raw);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_dbg_memRead(struct pcReq *r)
{
    uint32_t addr;
    uint16_t len = DBG_MEM_MAX;

    if (!pc_proto_tokenOk(r->p, r->in, r->inLen)) {
        r->err = PC_ERR_NOT_ALLOWED;
        return -1;
    }
    if (!pc_tlv_get_u32(r->in, r->inLen, PC_TAG_ADDR, &addr)) {
        r->err = PC_ERR_BAD_REQUEST;
        return -1;
    }
    pc_tlv_get_u16(r->in, r->inLen, PC_TAG_LENGTH, &len);
    if (len == 0) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }
    if (len > DBG_MEM_MAX)
        len = DBG_MEM_MAX;

    uint8_t buf[DBG_MEM_MAX];
    if (pc_proto_platformMem(addr, buf, len) != 0) {
        /* Out of the ranges the platform hands over: no partial answer, no
         * hint about what is there. */
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }

    size_t off = pc_tlv_u32(r->out, r->outCap, 0, PC_TAG_ADDR, addr);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_LENGTH, len);
    off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_DATA, buf, len);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * RX audio capture
 * ----------------------------------------------------------------------------
 */

static size_t captureStatus(struct pcReq *r, const pcProtoCapture_t *st)
{
    size_t off = pc_tlv_u32(r->out, r->outCap, 0, PC_TAG_LENGTH,
                            st->capacity);
    off = pc_tlv_u32(r->out, r->outCap, off, PC_TAG_OFFSET, st->written);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_COUNT,
                     (uint16_t)st->read);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_RATE, st->rate);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_RUNNING, st->running);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_DROPPED, st->overflow);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return 0;
    }
    r->outLen = off;
    return off;
}

int pc_dbg_capStart(struct pcReq *r)
{
    uint16_t samples = DBG_CAP_MAX / 2;

    pc_tlv_get_u16(r->in, r->inLen, PC_TAG_LENGTH, &samples);
    if (samples == 0 || samples > DBG_CAP_MAX) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }

    pcProtoCapture_t st;
    memset(&st, 0, sizeof(st));
    st.capacity = samples;
    if (pc_proto_platformCapture(PC_CAP_OP_START, &st, NULL, 0, 0) != 0) {
        r->err = PC_ERR_NOT_SUPPORTED;
        return -1;
    }
    if (pc_proto_platformCapture(PC_CAP_OP_STATUS, &st, NULL, 0, 0) != 0) {
        st.capacity = samples;
        st.running = 1;
    }
    if (captureStatus(r, &st) == 0)
        return -1;
    return 0;
}

int pc_dbg_capStop(struct pcReq *r)
{
    pcProtoCapture_t st;
    memset(&st, 0, sizeof(st));
    if (pc_proto_platformCapture(PC_CAP_OP_STOP, &st, NULL, 0, 0) != 0) {
        r->err = PC_ERR_NOT_SUPPORTED;
        return -1;
    }
    if (pc_proto_platformCapture(PC_CAP_OP_STATUS, &st, NULL, 0, 0) != 0) {
        r->err = PC_ERR_NOT_SUPPORTED;
        return -1;
    }
    if (captureStatus(r, &st) == 0)
        return -1;
    return 0;
}

int pc_dbg_capRead(struct pcReq *r)
{
    uint32_t offset = 0;
    uint16_t count = DBG_CAP_CHUNK;

    if (!pc_proto_tokenOk(r->p, r->in, r->inLen)) {
        r->err = PC_ERR_NOT_ALLOWED;
        return -1;
    }
    pc_tlv_get_u32(r->in, r->inLen, PC_TAG_OFFSET, &offset);
    pc_tlv_get_u16(r->in, r->inLen, PC_TAG_LENGTH, &count);
    if (count == 0) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }
    if (count > DBG_CAP_CHUNK)
        count = DBG_CAP_CHUNK;

    /* Samples are s16 on the wire, two bytes each. */
    uint8_t buf[DBG_CAP_CHUNK * 2];
    pcProtoCapture_t st;
    memset(&st, 0, sizeof(st));
    if (pc_proto_platformCapture(PC_CAP_OP_READ, &st, buf, offset,
                                 count) < (int)count) {
        r->err = PC_ERR_BAD_INDEX;
        return -1;
    }

    size_t off = pc_tlv_u32(r->out, r->outCap, 0, PC_TAG_OFFSET, offset);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_LENGTH, count);
    off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_DATA, buf,
                       (size_t)count * 2);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * Baseband dump and transmit trace
 * ----------------------------------------------------------------------------
 */

int pc_dbg_bb(struct pcReq *r)
{
    uint8_t op = PC_BB_OP_STATUS;
    uint8_t state = 0;

    pc_tlv_get_u8(r->in, r->inLen, PC_TAG_BB_STATE, &op);
    if (op > PC_BB_OP_RESUME) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }
    /* Arming and disarming change what the radio does with its PSRAM and its
     * UART, so they need the token; asking for the state does not. */
    if (op != PC_BB_OP_STATUS &&
        !pc_proto_tokenOk(r->p, r->in, r->inLen)) {
        r->err = PC_ERR_NOT_ALLOWED;
        return -1;
    }
    if (pc_proto_platformBbDump(op, &state) != 0) {
        r->err = PC_ERR_NOT_SUPPORTED;
        return -1;
    }

    size_t off = pc_tlv_u8(r->out, r->outCap, 0, PC_TAG_BB_STATE, state);
    off = pc_tlv_u8(r->out, r->outCap, off, PC_TAG_RUNNING,
                    (uint8_t)((state & 0x03) == 1));
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

int pc_dbg_txTrace(struct pcReq *r)
{
    char text[160];

    if (pc_proto_platformTxTrace(text, sizeof(text)) < 0) {
        r->err = PC_ERR_NOT_SUPPORTED;
        return -1;
    }
    size_t off = pc_tlv_str(r->out, r->outCap, 0, PC_TAG_TEXT, text);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

/*
 * ----------------------------------------------------------------------------
 * Post-mortem (docs/BLACKBOX.md)
 * ----------------------------------------------------------------------------
 */

/**
 * One chunk of the record the last crash left.
 *
 * The record is 516 bytes and a frame payload is 390 at most, so it travels in
 * chunks: the client asks for an offset, gets PC_TAG_BLACKBOX_LEN (the total)
 * and up to DBG_BLACKBOX_CHUNK bytes from there, and repeats until the offset
 * reaches the length.  The bytes are the flash image, not a re-rendered copy,
 * so a reader that knows the layout can parse it and one that does not can
 * still keep it.
 */
int pc_dbg_blackbox(struct pcReq *r)
{
    uint32_t offset = 0;
    uint16_t total = 0;
    uint8_t buf[DBG_BLACKBOX_CHUNK];

    pc_tlv_get_u32(r->in, r->inLen, PC_TAG_OFFSET, &offset);
    if (offset >= DBG_BLACKBOX_MAX) {
        r->err = PC_ERR_INVALID_VALUE;
        return -1;
    }

    int n = pc_proto_platformBlackbox(offset, buf, sizeof(buf), &total);
    if (n < 0) {
        /* No crash since the last time the record was read: nothing to serve,
         * and saying NOT_SUPPORTED is honest -- this radio has no post-mortem
         * to show, not a broken one. */
        r->err = PC_ERR_NOT_SUPPORTED;
        return -1;
    }

    size_t off = pc_tlv_u32(r->out, r->outCap, 0, PC_TAG_OFFSET, offset);
    off = pc_tlv_u16(r->out, r->outCap, off, PC_TAG_BLACKBOX_LEN, total);
    off = pc_tlv_bytes(r->out, r->outCap, off, PC_TAG_BLACKBOX, buf,
                       (size_t)n);
    if (off == 0) {
        r->err = PC_ERR_TOO_LARGE;
        return -1;
    }
    r->outLen = off;
    return 0;
}

#else /* !CONFIG_C62_DEBUG_PROTO */

/*
 * Production build: every debug message answers NOT_ALLOWED and none of the
 * handlers above is linked.  The dispatcher refuses them on the capability
 * before reaching here; these stubs are the second gate, so no future change
 * to the table can turn a refusal into a leak.
 */
static int dbgDenied(struct pcReq *r)
{
    r->err = PC_ERR_NOT_ALLOWED;
    return -1;
}

int pc_dbg_info(struct pcReq *r)        { return dbgDenied(r); }
int pc_dbg_logSub(struct pcReq *r)      { return dbgDenied(r); }
int pc_dbg_logDrain(struct pcReq *r)    { return dbgDenied(r); }
int pc_dbg_regRead(struct pcReq *r)     { return dbgDenied(r); }
int pc_dbg_regWrite(struct pcReq *r)    { return dbgDenied(r); }
int pc_dbg_agc(struct pcReq *r)         { return dbgDenied(r); }
int pc_dbg_memRead(struct pcReq *r)     { return dbgDenied(r); }
int pc_dbg_capStart(struct pcReq *r)    { return dbgDenied(r); }
int pc_dbg_capStop(struct pcReq *r)     { return dbgDenied(r); }
int pc_dbg_capRead(struct pcReq *r)     { return dbgDenied(r); }
int pc_dbg_bb(struct pcReq *r)          { return dbgDenied(r); }
int pc_dbg_txTrace(struct pcReq *r)     { return dbgDenied(r); }
int pc_dbg_rxCounters(struct pcReq *r)  { return dbgDenied(r); }
int pc_dbg_blackbox(struct pcReq *r)    { return dbgDenied(r); }

#endif /* CONFIG_C62_DEBUG_PROTO */
