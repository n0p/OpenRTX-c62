/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A radio on the host: the real protocol code and the real stores, on a
 * character device (a pty) instead of the console UART.
 *
 *   pc_proto_device <path> [--debug] [--channels N]
 *
 * What it gives the pty test is the part that cannot be faked in Python: the
 * framing state machine, the dispatch table, the TLV codecs, the store
 * handlers with their read-back verification, the flash ping-pong under a cut,
 * the log ring with its rate limiter, and the capability gate of a production
 * build.  The platform hooks below are the bench: a register file, a memory
 * window, a capture buffer.
 *
 * Channels and contacts come from tests/host/pc_proto_host/ram_store.c, a
 * fake flash that can be cut in the middle of a write on request.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "core/pc_proto.h"
#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/zones.h"
#include "core/messaging.h"

/* From ram_store.c */
void ram_store_bind(void);
void ram_store_cut(int64_t bytes);
void ram_store_heal(void);

/* `state` and `state_mutex` come from state_stub.c: the settings handlers read
 * them and nothing else of core/state.c is needed here. */

static pcProto_t proto;
static int fd = -1;

/* Register files, as the two chips would answer. */
static uint16_t regs4819[128];
static uint16_t regs1080[0x22];

/* A window the memory read is allowed to look at, so the bound is testable. */
static uint8_t memWindow[256];

/* Capture buffer: 2 s of 48 kHz would be 192 kB, which is fine here. */
static int16_t capBuf[16000];
static pcProtoCapture_t cap;

/* A channel event and a status event, so the client has something to receive
 * without a transmitter. */
static void emitEvents(void)
{
    pc_proto_eventChannel(&proto, 1, "TEST1");
    pc_proto_eventPtt(&proto, 1);
    pc_proto_eventMessage(&proto, 1, 2143001, "hola desde la radio", 2);
}

int pc_proto_platformStatus(pcProtoStatus_t *out)
{
    memset(out, 0, sizeof(*out));
    out->rxFreq = 145500000;
    out->txFreq = 145500000;
    out->mode = 1;
    out->rssi = -73;
    out->channelIdx = 1;
    out->batteryMv = 7400;
    out->txState = 1;
    strcpy(out->channelName, "TEST1");
    return 0;
}

const char *pc_proto_platformModel(void) { return "c62-sim"; }
const char *pc_proto_platformFirmware(void) { return "host-test"; }
const char *pc_proto_platformHardware(void) { return "rev 0"; }

int pc_proto_platformDiag(pcProtoDiag_t *out)
{
    memset(out, 0, sizeof(*out));
    out->resetCause = 0x40;
    out->wdtResets = 1;
    out->heapFree = 12345;
    out->heapTotal = 40960;
    out->psramFree = 400000;
    out->psramTotal = 4194304;
    out->flashUsed = 480000;
    out->flashTotal = 1048576;
    out->ramUsed = 166000;
    out->ramTotal = 262144;
    out->agc = 3;
    out->taskCount = 2;
    strcpy(out->tasks[0].name, "main");
    out->tasks[0].unused = 1024;
    out->tasks[0].size = 2048;
    strcpy(out->tasks[1].name, "pcproto");
    out->tasks[1].unused = 3000;
    out->tasks[1].size = 4096;
    return 0;
}

int pc_proto_platformReg(int chip, uint8_t reg, uint16_t *value)
{
    if (chip == 0 && reg < 0x80) {
        *value = regs4819[reg];
        return 0;
    }
    if (chip == 1 && reg < 0x22) {
        *value = regs1080[reg];
        return 0;
    }
    return -1;
}

int pc_proto_platformRegWrite(int chip, uint8_t reg, uint16_t value)
{
    if (chip == 0 && reg < 0x80) {
        regs4819[reg] = value;
        return 0;
    }
    if (chip == 1 && reg < 0x22) {
        regs1080[reg] = value;
        return 0;
    }
    return -1;
}

/* The window sits at a fixed address so a test can ask for it without
 * guessing where the host put a static array. */
#define MEM_WINDOW_ADDR 0x40000000u

int pc_proto_platformMem(uint32_t addr, uint8_t *buf, uint16_t len)
{
    if (addr < MEM_WINDOW_ADDR ||
        addr + len > MEM_WINDOW_ADDR + sizeof(memWindow))
        return -1;
    memcpy(buf, memWindow + (addr - MEM_WINDOW_ADDR), len);
    return 0;
}

int pc_proto_platformCapture(int op, pcProtoCapture_t *st, uint8_t *buf,
                             uint32_t off, uint32_t count)
{
    switch (op) {
        case PC_CAP_OP_START:
            cap.capacity = st->capacity > 16000 ? 16000 : st->capacity;
            cap.written = 0;
            cap.read = 0;
            cap.rate = 48000;
            cap.overflow = 0;
            cap.running = 1;
            /* A capture of a carrier: a ramp, so a truncated read is obvious
             * on the client side. */
            for (uint32_t i = 0; i < cap.capacity; i++)
                capBuf[i] = (int16_t)(i * 3);
            cap.written = cap.capacity;
            break;
        case PC_CAP_OP_STOP:
            cap.running = 0;
            break;
        case PC_CAP_OP_STATUS:
            *st = cap;
            return 0;
        case PC_CAP_OP_READ:
            if (off >= cap.written || off + count > cap.written)
                return -1;
            memcpy(buf, &capBuf[off], (size_t)count * 2);
            cap.read = off + count;
            return (int)count;
        default:
            return -1;
    }
    *st = cap;
    return 0;
}

int pc_proto_platformBbDump(int op, uint8_t *state)
{
    static uint8_t bb;
    if (op == PC_BB_OP_ARM)
        bb = 1;
    else if (op == PC_BB_OP_DISARM)
        bb = 0;
    else if (op == PC_BB_OP_PAUSE)
        bb = 2;
    else if (op == PC_BB_OP_RESUME)
        bb = 1;
    *state = bb;
    return 0;
}

int pc_proto_platformTxTrace(char *buf, size_t cap_)
{
    snprintf(buf, cap_, "tx stage=idle slots=0 | last over ok, 0 retries");
    return 0;
}

/* --- transport ----------------------------------------------------------- */

static int portWrite(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return (int)len;
}

static const struct pcProtoPort port = { .write = portWrite, .ctx = NULL };

static uint32_t nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Outside the protocol, in this host build, the log would be the terminal. */
uint32_t logLines;

static void pushBootLog(void)
{
    pc_proto_logPush(&proto, PC_LOG_INFO, "net", "radio alive");
    pc_proto_logPush(&proto, PC_LOG_WARN, "audio", "buffer late by 3 ms");
    pc_proto_logPush(&proto, PC_LOG_INFO, "net", "channel 1 selected");
    pc_proto_logPush(&proto, PC_LOG_ERROR, "dmr", "sync lost");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <char-device> [--debug]\n", argv[0]);
        return 2;
    }

    /* Non-blocking: the loop has to keep ticking (events, log drain) even
     * when the client is quiet, exactly as the firmware's UART poll does. */
    fd = open(argv[1], O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(fd, TCSANOW, &tio);
    }

    /* The real stores, on the fake flash. */
    ram_store_bind();
    chstore_init();
    cstore_init();
    pthread_mutex_init(&state_mutex, NULL);
    state.settings = default_settings;
    messaging_init();

    chstore_channel_t ch;
    memset(&ch, 0, sizeof(ch));
    strcpy(ch.name, "CALL1");
    ch.rx_freq = 145500000;
    ch.tx_freq = 145500000;
    ch.power = 1000;
    ch.mode = 1;
    ch.bandwidth = 1;
    ch.flags = 0x02;
    chstore_add(&ch);

    memset(&ch, 0, sizeof(ch));
    strcpy(ch.name, "DMR1");
    ch.rx_freq = 433500000;
    ch.tx_freq = 438100000;
    ch.power = 5000;
    ch.mode = 2;
    ch.dmr_cc = 3;
    ch.dmr_ts = 1;
    chstore_add(&ch);

    cstore_contact_t ct;
    memset(&ct, 0, sizeof(ct));
    strcpy(ct.call, "EA5RCH");
    strcpy(ct.name, "Repeater club");
    ct.dmr_id = 2143001;
    ct.flags = 1;
    cstore_add(&ct);

    regs4819[0x30] = 0x1234;
    regs4819[0x67] = 0x00A5;
    for (size_t i = 0; i < sizeof(memWindow); i++)
        memWindow[i] = (uint8_t)i;

    pc_proto_init(&proto, &port);
    pushBootLog();

    bool debugBuilt = (pc_proto_caps() & PC_CAP_DEBUG) != 0;

    /* A quiet period: the client sends HELLO, we answer, and only then do the
     * events and the log go out. */
    bool greeted = false;
    uint32_t lastEvent = 0;
    uint32_t lastLog = 0;

    for (;;) {
        uint8_t chunk[256];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0)
            pc_proto_rx(&proto, chunk, n);

        pc_proto_tick(&proto, nowMs());

        if (!greeted && pc_proto_linked(&proto)) {
            greeted = true;
            emitEvents();
            lastEvent = nowMs();
        }
        /* A status event every 200 ms and a log line every 300 ms, so the
         * client has a stream to look at without a radio. */
        uint32_t t = nowMs();
        if (greeted && t - lastEvent > 200) {
            lastEvent = t;
            pc_proto_eventChannel(&proto, 1, "TEST1");
        }
        if (greeted && t - lastLog > 300) {
            lastLog = t;
            char text[48];
            snprintf(text, sizeof(text), "tick %u", logLines++);
            pc_proto_logPush(&proto, PC_LOG_INFO, "net", text);
        }

        if (n <= 0)
            usleep(2000);
    }

    (void)debugBuilt;
    close(fd);
    return 0;
}
