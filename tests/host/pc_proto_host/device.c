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
#include <math.h>
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

/* A live capture is filled a sample at a time by benchFeed(). */
static uint32_t liveSample;

/* The radio's clock, defined with the transport below: milliseconds of
 * monotonic time, the same base the firmware gets from k_uptime_get(). */
static uint32_t nowMs(void);

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
    /*
     * The same rule the firmware's platform hook follows, and the same absence
     * of locks: what the radio would transmit with, from the channel when it
     * carries it and from the settings when it does not (docs/TX_SAFETY.md).
     */
    out->rxFreq = state.channel.rx_frequency != 0
                      ? state.channel.rx_frequency : 145500000;
    out->txFreq = state_txFrequency() != 0 ? state_txFrequency() : 145500000;
    out->mode = state.channel.mode != 0 ? state.channel.mode : 1;
    out->rssi = -73;
    out->channelIdx = 1;
    out->batteryMv = 7400;
    out->txState = 1;
    out->txPower = (uint16_t)state_txPowerMw();
    snprintf(out->txReason, sizeof(out->txReason), "%s",
             out->txPower == 0 ? "no tx power" : "none");
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
    out->heapKnown = 1;
    out->psramFree = 400000;
    out->psramTotal = 4194304;
    out->psramKnown = 1;
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
    out->txPower = (uint16_t)state_txPowerMw();
    snprintf(out->txReason, sizeof(out->txReason), "%s",
             out->txPower == 0 ? "no tx power" : "none");
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
            cap.t0Ms = nowMs();
            cap.blocks = 0;
            cap.blockLen = 0;
            /* A capture of a carrier: a ramp, so a truncated read is obvious
             * on the client side. */
            for (uint32_t i = 0; i < cap.capacity; i++)
                capBuf[i] = (int16_t)(i * 3);
            cap.written = cap.capacity;
            cap.blocks = cap.capacity / 480;
            cap.blockLen = 480;
            break;
        case PC_CAP_OP_START_LIVE:
            /* The bench flavour: empty, and filled 10 ms at a time by the
             * audio the simulated radio is "receiving" (benchFeed below). */
            cap.capacity = st->capacity > 16000 ? 16000 : st->capacity;
            cap.written = 0;
            cap.read = 0;
            cap.rate = 48000;
            cap.overflow = 0;
            cap.running = 1;
            cap.t0Ms = nowMs();
            cap.blocks = 0;
            cap.blockLen = 0;
            liveSample = 0;
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

/* --- the simulated radio of the bench namespace --------------------------- */

/*
 * A radio with a carrier where it says it is, a microphone, an RSSI that has a
 * peak at 145.500 MHz and a discriminator that carries a 1 kHz tone when it is
 * receiving one.  Enough for the PC tool to run its whole flow -- arm, tune,
 * key, capture, sweep, report -- against the real protocol code, and for the
 * correlation to have something to correlate.
 */
static struct
{
    uint32_t rxHz;
    uint32_t txHz;
    uint8_t mode;
    uint8_t bw;
    uint8_t ptt;
    uint16_t tone;      /* Hz the radio really generates               */
    uint8_t level;      /* deviation percentage in force               */
    uint16_t power;     /* mW the PA is driven with                    */
} radio = { 145500000, 145500000, 1, 2, 0, 0, 0, 0 };

/** The tone the simulated receiver puts in the discriminator, in Hz. */
#define RADIO_TONE_HZ 1000

/** The level the simulated receiver sees: a peak at 145.500 MHz. */
static int16_t simRssi(void)
{
    int32_t d = (int32_t)radio.rxHz - 145500000;
    if (d < 0)
        d = -d;
    return (int16_t)(-95 + (d < 50000 ? 40 - d / 5000 : 0));
}

int pc_proto_platformBench(uint8_t op, pcProtoBenchArgs_t *a)
{
    switch (op) {
        case PC_BENCH_OP_POLL:
            a->nowMs = nowMs();
            a->rxHz = radio.rxHz;
            a->txHz = radio.txHz;
            a->mode = radio.mode;
            a->bw = radio.bw;
            a->ptt = radio.ptt;
            a->txState = radio.ptt ? 2 : 1;
            a->toneHz = radio.tone;
            a->level = radio.level;
            a->power = radio.power;
            a->rssi = simRssi();
            a->rssiRaw = (int16_t)(a->rssi - 3);
            a->deltaHz = 0;
            return 0;

        case PC_BENCH_OP_TUNE:
            if (a->rxHz < 18000000u)
                return -1;
            radio.rxHz = a->rxHz;
            radio.txHz = a->txHz != 0 ? a->txHz : a->rxHz;
            if (a->mode != 0)
                radio.mode = a->mode;
            if (a->bw >= 1 && a->bw <= 3)
                radio.bw = a->bw;
            /* The power of the run, in the channel field the firmware keys
             * from; 0 keeps what is in force. */
            if (a->power != 0)
                radio.power = a->power;
            a->mode = radio.mode;
            a->bw = radio.bw;
            a->rxHz = radio.rxHz;
            a->txHz = radio.txHz;
            a->power = radio.power;
            return 0;

        case PC_BENCH_OP_PTT:
            if (a->ptt && radio.power == 0)
                return -1; /* nothing to transmit with: never keyed */
            radio.ptt = a->ptt;
            if (a->ptt) {
                /* The generator only does the standard CTCSS range. */
                radio.tone = (a->toneHz >= 60 && a->toneHz <= 260)
                                 ? a->toneHz : 0;
                radio.level = a->level;
            } else {
                radio.tone = 0;
                radio.level = 0;
            }
            a->toneHz = radio.tone;
            a->level = radio.level;
            a->power = radio.power;
            a->txState = radio.ptt ? 2 : 1;
            return 0;

        case PC_BENCH_OP_RSSI:
            a->nowMs = nowMs();
            a->rssi = simRssi();
            a->rssiRaw = a->rssi;
            return 0;

        case PC_BENCH_OP_RELEASE:
            radio.ptt = 0;
            radio.tone = 0;
            radio.level = 0;
            return 0;

        default:
            return -1;
    }
}

/*
 * Fill a live capture with what the receiver would deliver: a 1 kHz tone at
 * 40 % of full scale, 10 ms per call, with a phase that carries across blocks
 * so the result is a continuous sine and not a click every 480 samples.
 */
static void benchFeed(void)
{
    if (!cap.running)
        return;
    /* The debug capture is pre-filled and not running by now; only a live one
     * gets here with room left. */
    if (cap.written >= cap.capacity)
        return;

    const uint32_t n = 480;
    for (uint32_t i = 0; i < n && cap.written < cap.capacity; i++) {
        /* A real sine, so the PC side can measure its frequency and its level
         * and cross-correlate it with the demodulated capture of the SDR. */
        double t = (double)liveSample++ / 48000.0;
        capBuf[cap.written++] = (int16_t)(12000.0 * sin(2.0 * M_PI *
                                                        RADIO_TONE_HZ * t));
    }
    if (cap.blockLen == 0)
        cap.blockLen = (uint16_t)n;
    cap.blocks++;
    if (cap.written >= cap.capacity) {
        cap.overflow = 1;
        cap.running = 0;
    }
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

/*
 * How long, and how, the fake radio's UI holds the state lock: the failure the
 * transport has to survive (docs/PC_PROTO.md).  The firmware's UI thread holds
 * state_mutex across ui_updateFSM() and the state task across the battery read;
 * a radio whose boot path blocks while holding it used to silence the whole
 * protocol, with no fault and no log line.  With --hold-lock-ms the device
 * reproduces exactly that from another thread, and the test checks that HELLO,
 * STATUS and the bench still answer.
 */
static void *holdStateLock(void *arg)
{
    long ms = (long)arg;
    pthread_mutex_lock(&state_mutex);
    usleep((useconds_t)ms * 1000);
    pthread_mutex_unlock(&state_mutex);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <char-device> [--vfo] [--zero-power] "
                "[--hold-lock-ms N] [--boot-channel]\n", argv[0]);
        return 2;
    }

    bool vfoBoot = false;
    bool zeroPower = false;
    bool bootChannel = false;
    long holdLockMs = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--vfo") == 0)
            vfoBoot = true;
        else if (strcmp(argv[i], "--zero-power") == 0)
            zeroPower = true;
        else if (strcmp(argv[i], "--boot-channel") == 0)
            bootChannel = true;
        else if (strcmp(argv[i], "--hold-lock-ms") == 0 && i + 1 < argc)
            holdLockMs = strtol(argv[++i], NULL, 10);
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

    /* What the capture platform answers before anything is armed: the rate it
     * delivers and the buffer it has, so ARM can advertise a ceiling. */
    cap.rate = 48000;
    cap.capacity = 16000;

    regs4819[0x30] = 0x1234;
    regs4819[0x67] = 0x00A5;
    for (size_t i = 0; i < sizeof(memWindow); i++)
        memWindow[i] = (uint8_t)i;

    /*
     * The two boot shapes that matter, both of them reported from hardware:
     * a radio that comes up on a stored channel (the one that used to silence
     * the protocol) and one that comes up on the VFO, with no channel power and
     * no transmit frequency of its own (the one that keyed nothing).
     */
    if (bootChannel) {
        /* Last used = 0, as PC_MSG_CHAN_LASTUSED leaves it, and the record
         * applied to state the way core/state.c does at boot. */
        chstore_setLastUsed(0);
        chstore_commit();
        chstore_toChannel(0, &state.channel);
        state.channel_index = 0;
        state.tuner_mode = CH;
    } else if (vfoBoot) {
        memset(&state.channel, 0, sizeof(state.channel));
        state.channel.mode = 1;               /* OPMODE_FM */
        state.channel.bandwidth = 1;
        state.channel.rx_frequency = 145500000;
        state.channel.tx_frequency = 0;       /* what the VFO record carries */
        state.channel.power = 0;              /* ...and what it does not    */
        strcpy(state.channel.name, "VFO");
        state.tuner_mode = VFO;
    }

    if (zeroPower)
        state.settings.tx_power = 0;
    else if (state.settings.tx_power == 0)
        state.settings.tx_power = default_settings.tx_power;

    if (holdLockMs > 0) {
        pthread_t holder;
        if (pthread_create(&holder, NULL, holdStateLock, (void *)holdLockMs) == 0)
            pthread_detach(holder);
    }

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
        benchFeed();

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
