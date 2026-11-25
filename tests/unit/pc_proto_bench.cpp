/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests of the bench namespace (core/pc_proto_bench.c, docs/BENCH.md).
 *
 * The platform is a simulated radio: it keeps a frequency, a transmit state, a
 * register file and an audio capture that a test can fill, so what is exercised
 * is the real module -- the sequencing, the timestamps, the payload limits and
 * the report text -- not a mock of it.
 *
 * Built twice, as the debug tests are: with CONFIG_C62_BENCH_PROTO (the bench
 * build: everything is served) and without it (a production build: every
 * message answers NOT_ALLOWED and no radio data leaves).
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "core/pc_proto.h"
#include "core/channel_store.h"
#include "core/contact_store.h"
#include "core/messaging.h"
#include "core/settings.h"
#include "core/state.h"
}

/* ------------------------------------------------------------------------- */
/* The simulated radio                                                       */
/* ------------------------------------------------------------------------- */

namespace {

struct SimRadio
{
    uint32_t tickMs = 1000;
    uint32_t rxHz = 0;
    uint32_t txHz = 0;
    uint8_t mode = 0;
    uint8_t bw = 0;
    uint8_t ptt = 0;
    uint16_t tone = 0;
    uint8_t level = 0;
    int16_t rssi = -73;
    int16_t rssiRaw = -70;
    int tuned = 0;      /* how many TUNE calls landed                */
    int keys = 0;       /* how many PTT-on calls landed              */
    int releases = 0;
};

SimRadio sim;

/* Capture the platform reports: filled by the test through pushSamples(). */
struct SimCapture
{
    std::vector<int16_t> samples;
    pcProtoCapture_t st;
};

SimCapture cap;

void captureReset()
{
    cap.samples.clear();
    std::memset(&cap.st, 0, sizeof(cap.st));
    /* What a platform with a capture answers before one is armed: the rate it
     * delivers and the buffer it has, so ARM can advertise a ceiling. */
    cap.st.rate = 48000;
    cap.st.capacity = 16000;
}

} // namespace

extern "C" {

int pc_proto_platformBench(uint8_t op, pcProtoBenchArgs_t *a)
{
    switch (op) {
        case PC_BENCH_OP_POLL:
            a->nowMs = sim.tickMs;
            a->rxHz = sim.rxHz;
            a->txHz = sim.txHz;
            a->mode = sim.mode;
            a->bw = sim.bw;
            a->rssi = sim.rssi;
            a->rssiRaw = sim.rssiRaw;
            a->ptt = sim.ptt;
            a->toneHz = sim.tone;
            a->level = sim.level;
            a->txState = sim.ptt ? 2 : 1;
            return 0;

        case PC_BENCH_OP_TUNE:
            sim.rxHz = a->rxHz;
            sim.txHz = a->txHz;
            sim.mode = a->mode;
            sim.bw = a->bw;
            sim.tuned++;
            a->txState = sim.ptt ? 2 : 1;
            return sim.rxHz < 18000000u ? -1 : 0; /* below 18 MHz: no */

        case PC_BENCH_OP_PTT:
            sim.ptt = a->ptt;
            if (a->ptt) {
                sim.keys++;
                /* The platform refuses tones it cannot generate. */
                sim.tone = a->toneHz > 300 ? 0 : a->toneHz;
                sim.level = a->level;
                a->toneHz = sim.tone;
            } else {
                sim.releases++;
                sim.tone = 0;
                sim.level = 0;
                a->toneHz = 0;
            }
            a->level = sim.level;
            a->txState = sim.ptt ? 2 : 1;
            return 0;

        case PC_BENCH_OP_RSSI:
            a->nowMs = sim.tickMs;
            a->rssi = sim.rssi;
            a->rssiRaw = sim.rssiRaw;
            return 0;

        case PC_BENCH_OP_RELEASE:
            sim.ptt = 0;
            sim.tone = 0;
            sim.level = 0;
            sim.releases++;
            return 0;

        default:
            return -1;
    }
}

int pc_proto_platformCapture(int op, pcProtoCapture_t *st, uint8_t *buf,
                             uint32_t off, uint32_t count)
{
    switch (op) {
        case PC_CAP_OP_START:
        case PC_CAP_OP_START_LIVE:
            cap.samples.assign(st->capacity, 0);
            cap.st.capacity = st->capacity;
            cap.st.written = 0;
            cap.st.read = 0;
            cap.st.rate = 48000;
            cap.st.running = 1;
            cap.st.overflow = 0;
            cap.st.t0Ms = sim.tickMs;
            cap.st.blocks = 0;
            cap.st.blockLen = 0;
            break;
        case PC_CAP_OP_STOP:
            cap.st.running = 0;
            break;
        case PC_CAP_OP_STATUS:
            *st = cap.st;
            return 0;
        case PC_CAP_OP_READ:
            if (off + count > cap.st.written)
                return -1;
            std::memcpy(buf, &cap.samples[off], (size_t)count * 2);
            return (int)count;
        default:
            return -1;
    }
    return 0;
}

int pc_proto_platformStatus(pcProtoStatus_t *out)
{
    std::memset(out, 0, sizeof(*out));
    out->rxFreq = sim.rxHz;
    out->txFreq = sim.txHz;
    out->mode = sim.mode;
    out->rssi = sim.rssi;
    out->txState = sim.ptt ? 2 : 1;
    return 0;
}

const char *pc_proto_platformModel(void) { return "c62-sim"; }
const char *pc_proto_platformFirmware(void) { return "host-test"; }
const char *pc_proto_platformHardware(void) { return "rev 0"; }

} // extern "C"

/* ------------------------------------------------------------------------- */
/* Wire helpers                                                              */
/* ------------------------------------------------------------------------- */

namespace {

std::vector<uint8_t> wire;

struct Sink
{
    pcProtoPort port;
};

int sinkWrite(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    wire.insert(wire.end(), buf, buf + len);
    return (int)len;
}

const pcProtoPort sink = { sinkWrite, nullptr };

struct Frame
{
    uint8_t type = 0;
    uint8_t seq = 0;
    uint16_t id = 0;
    uint8_t flags = 0;
    std::vector<uint8_t> payload;
};

std::vector<Frame> takeFrames()
{
    std::vector<Frame> out;
    size_t start = 0;
    for (size_t i = 0; i < wire.size(); i++) {
        if (wire[i] != PC_PROTO_END)
            continue;
        if (i == start) {
            start = i + 1;
            continue;
        }
        std::vector<uint8_t> body;
        bool esc = false;
        for (size_t j = start; j < i; j++) {
            uint8_t b = wire[j];
            if (esc) {
                esc = false;
                body.push_back(b == PC_PROTO_ESC_END  ? PC_PROTO_END
                               : b == PC_PROTO_ESC_ESC ? PC_PROTO_ESC
                                                       : b);
            } else if (b == PC_PROTO_ESC) {
                esc = true;
            } else {
                body.push_back(b);
            }
        }
        start = i + 1;
        if (body.size() < 10)
            continue;
        Frame f;
        f.type = body[1];
        f.seq = body[2];
        f.id = (uint16_t)(body[3] | (body[4] << 8));
        f.flags = body[5];
        uint16_t plen = (uint16_t)(body[6] | (body[7] << 8));
        if (plen != body.size() - 10)
            continue;
        f.payload.assign(body.begin() + 8, body.end() - 2);
        out.push_back(f);
    }
    wire.clear();
    return out;
}

bool tlvFind(const std::vector<uint8_t> &p, uint16_t tag,
             const uint8_t **val, uint16_t *len)
{
    size_t off = 0;
    while (off + 4 <= p.size()) {
        uint16_t t = (uint16_t)(p[off] | (p[off + 1] << 8));
        uint16_t l = (uint16_t)(p[off + 2] | (p[off + 3] << 8));
        off += 4;
        if (off + l > p.size())
            return false;
        if (t == tag) {
            *val = &p[off];
            *len = l;
            return true;
        }
        off += l;
    }
    return false;
}

uint8_t u8(const std::vector<uint8_t> &p, uint16_t tag, uint8_t def = 0)
{
    const uint8_t *v;
    uint16_t l;
    return tlvFind(p, tag, &v, &l) && l == 1 ? v[0] : def;
}

uint16_t u16(const std::vector<uint8_t> &p, uint16_t tag, uint16_t def = 0)
{
    const uint8_t *v;
    uint16_t l;
    if (!tlvFind(p, tag, &v, &l) || l != 2)
        return def;
    return (uint16_t)(v[0] | (v[1] << 8));
}

uint32_t u32(const std::vector<uint8_t> &p, uint16_t tag, uint32_t def = 0)
{
    const uint8_t *v;
    uint16_t l;
    if (!tlvFind(p, tag, &v, &l) || l != 4)
        return def;
    return (uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16) |
           ((uint32_t)v[3] << 24);
}

int32_t i32(const std::vector<uint8_t> &p, uint16_t tag, int32_t def = 0)
{
    return (int32_t)u32(p, tag, (uint32_t)def);
}

std::vector<uint8_t> bytes(const std::vector<uint8_t> &p, uint16_t tag)
{
    const uint8_t *v;
    uint16_t l;
    if (!tlvFind(p, tag, &v, &l))
        return {};
    return std::vector<uint8_t>(v, v + l);
}

void putU8(std::vector<uint8_t> &p, uint16_t tag, uint8_t v)
{
    p.push_back((uint8_t)(tag & 0xFF));
    p.push_back((uint8_t)(tag >> 8));
    p.push_back(1);
    p.push_back(0);
    p.push_back(v);
}

void putU16(std::vector<uint8_t> &p, uint16_t tag, uint16_t v)
{
    p.push_back((uint8_t)(tag & 0xFF));
    p.push_back((uint8_t)(tag >> 8));
    p.push_back(2);
    p.push_back(0);
    p.push_back((uint8_t)(v & 0xFF));
    p.push_back((uint8_t)(v >> 8));
}

void putU32(std::vector<uint8_t> &p, uint16_t tag, uint32_t v)
{
    p.push_back((uint8_t)(tag & 0xFF));
    p.push_back((uint8_t)(tag >> 8));
    p.push_back(4);
    p.push_back(0);
    for (int i = 0; i < 4; i++)
        p.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}

struct Device
{
    pcProto_t proto;
    uint32_t token = 0;

    Device()
    {
        wire.clear();
        captureReset();
        sim = SimRadio();
        pthread_mutex_init(&state_mutex, nullptr);
        state.settings = default_settings;
        pc_proto_init(&proto, &sink);
        pc_proto_tick(&proto, sim.tickMs);
    }

    /* Responses and events arrive in the same read; the response is what a
     * client waits for, so the events are set aside for a test that wants
     * them instead of making every request expect the exact frame count. */
    std::vector<Frame> lastEvents;

    Frame request(uint16_t id, uint8_t seq,
                  const std::vector<uint8_t> &payload = {})
    {
        uint8_t buf[600];
        size_t n = pc_proto_encode(PC_TYPE_REQUEST, seq, id, 0,
                                   payload.empty() ? nullptr : payload.data(),
                                   payload.size(), buf, sizeof(buf));
        REQUIRE(n > 0);
        pc_proto_rx(&proto, buf, n);
        auto frames = takeFrames();
        lastEvents.clear();
        Frame resp;
        bool got = false;
        for (auto &f : frames) {
            if (f.type == PC_TYPE_EVENT) {
                lastEvents.push_back(f);
            } else {
                REQUIRE(!got);
                resp = f;
                got = true;
            }
        }
        REQUIRE(got);
        return resp;
    }

    Frame hello()
    {
        std::vector<uint8_t> p;
        putU16(p, PC_TAG_VERSION, PC_PROTO_VERSION);
        putU32(p, PC_TAG_CAPS, PC_CAP_EVENTS);
        Frame f = request(PC_MSG_HELLO, 1, p);
        token = u32(f.payload, PC_TAG_TOKEN);
        return f;
    }

    std::vector<uint8_t> tok() const
    {
        std::vector<uint8_t> p;
        putU32(p, PC_TAG_TOKEN, token);
        return p;
    }

    void tick(uint32_t ms)
    {
        sim.tickMs = ms;
        pc_proto_tick(&proto, ms);
    }
};

/* Push `count` samples into the simulated capture, as the codec worker would. */
void pushCapture(size_t count)
{
    if (!cap.st.running || cap.st.written + count > cap.st.capacity)
        return;
    for (size_t i = 0; i < count; i++)
        cap.samples[cap.st.written + i] = (int16_t)(cap.st.written + i);
    cap.st.written += (uint32_t)count;
    if (cap.st.blocks == 0)
        cap.st.blockLen = (uint16_t)count;
    else if (cap.st.blockLen != (uint16_t)count)
        cap.st.blockLen = 0;
    cap.st.blocks++;
    if (cap.st.written >= cap.st.capacity) {
        cap.st.overflow = 1;
        cap.st.running = 0;
    }
}

} // namespace

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */
/* ------------------------------------------------------------------------- */

#ifdef CONFIG_C62_BENCH_PROTO

TEST_CASE("Bench: capability and ARM epoch", "[bench]")
{
    Device d;
    Frame h = d.hello();
    REQUIRE((u32(h.payload, PC_TAG_CAPS) & PC_CAP_BENCH) != 0);

    Frame a = d.request(PC_MSG_BENCH_ARM, 2, d.tok());
    REQUIRE(a.id == PC_MSG_BENCH_ARM);
    REQUIRE(u32(a.payload, PC_TAG_BENCH_TICK) == sim.tickMs);
    REQUIRE(u32(a.payload, PC_TAG_BENCH_EPOCH) == 1);
    /* A bench build with a capture advertises its rate and its ceiling. */
    REQUIRE(u16(a.payload, PC_TAG_RATE) == 48000);
    REQUIRE(u16(a.payload, PC_TAG_LENGTH) > 0);

    Frame b = d.request(PC_MSG_BENCH_ARM, 3, d.tok());
    REQUIRE(u32(b.payload, PC_TAG_BENCH_EPOCH) == 2);
}

TEST_CASE("Bench: ARM needs the session token", "[bench]")
{
    Device d;
    d.hello();
    Frame a = d.request(PC_MSG_BENCH_ARM, 2);
    REQUIRE(a.id == PC_MSG_ERROR);
    REQUIRE(u8(a.payload, PC_TAG_ERROR) == PC_ERR_NOT_ALLOWED);
}

TEST_CASE("Bench: TUNE programs the radio and reports what it took", "[bench]")
{
    Device d;
    d.hello();
    d.request(PC_MSG_BENCH_ARM, 2, d.tok());

    std::vector<uint8_t> p;
    putU32(p, PC_TAG_RX_FREQ, 145500000);
    putU8(p, PC_TAG_MODE, 1);
    putU8(p, PC_TAG_BENCH_BW, 2); /* 25 kHz */
    Frame t = d.request(PC_MSG_BENCH_TUNE, 3, [&] {
        auto q = p;
        auto tok = d.tok();
        q.insert(q.end(), tok.begin(), tok.end());
        return q;
    }());
    REQUIRE(t.id == PC_MSG_BENCH_TUNE);
    REQUIRE(u32(t.payload, PC_TAG_RX_FREQ) == 145500000);
    /* tx follows rx when it is not given */
    REQUIRE(u32(t.payload, PC_TAG_TX_FREQ) == 145500000);
    REQUIRE(u8(t.payload, PC_TAG_MODE) == 1);
    REQUIRE(u8(t.payload, PC_TAG_BENCH_BW) == 2);
    /* The radio cannot measure its own carrier: the tag is there and zero. */
    REQUIRE(i32(t.payload, PC_TAG_BENCH_DELTA) == 0);
    REQUIRE(sim.tuned == 1);
    REQUIRE(sim.rxHz == 145500000);
}

TEST_CASE("Bench: a frequency the platform refuses is an error", "[bench]")
{
    Device d;
    d.hello();
    d.request(PC_MSG_BENCH_ARM, 2, d.tok());
    std::vector<uint8_t> p;
    putU32(p, PC_TAG_RX_FREQ, 1000000); /* below the simulator's floor */
    auto tok = d.tok();
    p.insert(p.end(), tok.begin(), tok.end());
    Frame t = d.request(PC_MSG_BENCH_TUNE, 3, p);
    REQUIRE(t.id == PC_MSG_ERROR);
    REQUIRE(u8(t.payload, PC_TAG_ERROR) == PC_ERR_INVALID_VALUE);
}

TEST_CASE("Bench: PTT keys, stamps the edge and emits the event", "[bench]")
{
    Device d;
    d.hello();
    d.request(PC_MSG_BENCH_ARM, 2, d.tok());

    std::vector<uint8_t> p;
    putU8(p, PC_TAG_TX_STATE, 1);
    putU16(p, PC_TAG_BENCH_TONE, 88);
    putU8(p, PC_TAG_BENCH_LEVEL, 50);
    auto tok = d.tok();
    p.insert(p.end(), tok.begin(), tok.end());

    sim.tickMs = 2000;
    Frame on = d.request(PC_MSG_BENCH_PTT, 4, p);
    REQUIRE(on.id == PC_MSG_BENCH_PTT);
    REQUIRE(u8(on.payload, PC_TAG_TX_STATE) == 2);
    REQUIRE(u16(on.payload, PC_TAG_BENCH_TONE) == 88);
    REQUIRE(u32(on.payload, PC_TAG_BENCH_TICK) == 2000);
    /* The edge event follows the answer, on the same tick. */
    REQUIRE(d.lastEvents.size() == 1);
    REQUIRE(d.lastEvents[0].id == PC_EVT_BENCH_EDGE);
    REQUIRE(u8(d.lastEvents[0].payload, PC_TAG_TX_STATE) == 1);
    REQUIRE(u32(d.lastEvents[0].payload, PC_TAG_BENCH_TICK) == 2000);
    REQUIRE(sim.keys == 1);

    /* Unkey: the tick is kept separately, and the release is reported. */
    std::vector<uint8_t> q;
    putU8(q, PC_TAG_TX_STATE, 0);
    auto t2 = d.tok();
    q.insert(q.end(), t2.begin(), t2.end());
    sim.tickMs = 2600;
    Frame off = d.request(PC_MSG_BENCH_PTT, 5, q);
    REQUIRE(u8(off.payload, PC_TAG_TX_STATE) == 1);
    REQUIRE(sim.releases >= 1);
    REQUIRE(sim.ptt == 0);

    /* The report carries both edges. */
    std::vector<uint8_t> r;
    Frame rep = d.request(PC_MSG_BENCH_REPORT, 6, r);
    std::string text;
    uint16_t total = u16(rep.payload, PC_TAG_LENGTH);
    auto head = bytes(rep.payload, PC_TAG_TEXT);
    text.append((const char *)head.data(), head.size());
    while (text.size() < total) {
        std::vector<uint8_t> rq;
        putU32(rq, PC_TAG_OFFSET, (uint32_t)text.size());
        Frame c = d.request(PC_MSG_BENCH_REPORT, 7, rq);
        auto chunk = bytes(c.payload, PC_TAG_TEXT);
        REQUIRE(!chunk.empty());
        text.append((const char *)chunk.data(), chunk.size());
    }
    REQUIRE(text.find("on 2000 ms") != std::string::npos);
    REQUIRE(text.find("off 2600 ms") != std::string::npos);
}

TEST_CASE("Bench: a capture is armed, filled and read back with its t0",
          "[bench]")
{
    Device d;
    d.hello();
    d.request(PC_MSG_BENCH_ARM, 2, d.tok());

    std::vector<uint8_t> p;
    putU16(p, PC_TAG_BENCH_WINDOW, 20); /* 20 ms = 960 samples */
    auto tok = d.tok();
    p.insert(p.end(), tok.begin(), tok.end());
    sim.tickMs = 5000;
    Frame a = d.request(PC_MSG_BENCH_CAPTURE, 3, p);
    REQUIRE(a.id == PC_MSG_BENCH_CAPTURE);
    REQUIRE(u16(a.payload, PC_TAG_RATE) == 48000);
    REQUIRE(u32(a.payload, PC_TAG_BENCH_T0) == 5000);
    REQUIRE(u8(a.payload, PC_TAG_RUNNING) == 1);

    /* The codec worker delivers 10 ms blocks: two of them fill the window. */
    pushCapture(480);
    d.tick(5010);
    REQUIRE(u8(d.request(PC_MSG_BENCH_CAPTURE, 4,
                         [&] { auto q = d.tok(); return q; }()).payload,
               PC_TAG_RUNNING) == 1);
    pushCapture(480);
    d.tick(5020);

    std::vector<uint8_t> q;
    putU32(q, PC_TAG_OFFSET, 0);
    putU16(q, PC_TAG_LENGTH, 160);
    auto t2 = d.tok();
    q.insert(q.end(), t2.begin(), t2.end());
    Frame r = d.request(PC_MSG_BENCH_CAPTURE, 5, q);
    REQUIRE(r.id == PC_MSG_BENCH_CAPTURE);
    REQUIRE(u32(r.payload, PC_TAG_BENCH_WRITTEN) == 960);
    REQUIRE(u32(r.payload, PC_TAG_BENCH_BLOCKS) == 2);
    REQUIRE(u16(r.payload, PC_TAG_BENCH_BLOCK) == 480);
    REQUIRE(u32(r.payload, PC_TAG_BENCH_T0) == 5000);
    auto data = bytes(r.payload, PC_TAG_DATA);
    /* 128 samples per answer: what fits a 390-byte payload with its tags. */
    REQUIRE(data.size() == 256);
    /* Sample i was filled with i, so the first one proves the offset. */
    int16_t first = (int16_t)(data[0] | (data[1] << 8));
    REQUIRE(first == 0);

    /* A capture past the end is not an error, it is an empty chunk. */
    std::vector<uint8_t> z;
    putU32(z, PC_TAG_OFFSET, 900);
    putU16(z, PC_TAG_LENGTH, 160);
    auto t3 = d.tok();
    z.insert(z.end(), t3.begin(), t3.end());
    Frame e = d.request(PC_MSG_BENCH_CAPTURE, 6, z);
    REQUIRE(e.id == PC_MSG_BENCH_CAPTURE);
    REQUIRE(bytes(e.payload, PC_TAG_DATA).size() == 120); /* 60 samples left */
}

TEST_CASE("Bench: RSSI series is stamped and collected over the ticks",
          "[bench]")
{
    Device d;
    d.hello();
    d.request(PC_MSG_BENCH_ARM, 2, d.tok());

    std::vector<uint8_t> p;
    putU8(p, PC_TAG_COUNT, 4);
    auto tok = d.tok();
    p.insert(p.end(), tok.begin(), tok.end());
    sim.tickMs = 7000;
    sim.rssi = -80;
    Frame a = d.request(PC_MSG_BENCH_RSSI, 3, p);
    REQUIRE(u32(a.payload, PC_TAG_BENCH_WRITTEN) == 1);

    for (int i = 0; i < 6; i++) {
        sim.rssi = (int16_t)(-80 + i);
        d.tick((uint32_t)(7000 + 10 * (i + 1)));
    }
    /* A poll carries no COUNT: it reads the series, it does not restart it. */
    std::vector<uint8_t> poll;
    auto tok2 = d.tok();
    poll.insert(poll.end(), tok2.begin(), tok2.end());
    Frame b = d.request(PC_MSG_BENCH_RSSI, 4, poll);
    REQUIRE(u32(b.payload, PC_TAG_BENCH_WRITTEN) == 4);
    auto list = bytes(b.payload, PC_TAG_BENCH_RSSI_TS);
    REQUIRE(list.size() == 4 * 6);
    /* First entry: tick and value, little endian, as the firmware packs it. */
    uint32_t t0 = (uint32_t)list[0] | ((uint32_t)list[1] << 8) |
                  ((uint32_t)list[2] << 16) | ((uint32_t)list[3] << 24);
    int16_t v0 = (int16_t)(list[4] | (list[5] << 8));
    REQUIRE(t0 == 7000);
    REQUIRE(v0 == -80);
}

TEST_CASE("Bench: the sweep walks the band and stops by itself", "[bench]")
{
    Device d;
    d.hello();
    d.request(PC_MSG_BENCH_ARM, 2, d.tok());

    std::vector<uint8_t> p;
    putU32(p, PC_TAG_BENCH_F0, 145400000);
    putU32(p, PC_TAG_BENCH_F1, 145500000);
    putU32(p, PC_TAG_BENCH_STEP, 25000);
    putU16(p, PC_TAG_BENCH_DWELL, 20);
    auto tok = d.tok();
    p.insert(p.end(), tok.begin(), tok.end());
    Frame a = d.request(PC_MSG_BENCH_SWEEP, 3, p);
    REQUIRE(u16(a.payload, PC_TAG_BENCH_POINTS) == 4);
    REQUIRE(u8(a.payload, PC_TAG_RUNNING) == 1);

    /* Four points, 20 ms of dwell each, at the 10 ms tick: give it 300 ms. */
    for (uint32_t t = 8010; t < 8500; t += 10) {
        sim.rssi = (int16_t)(-90 + (int16_t)((t / 10) % 30));
        d.tick(t);
    }
    std::vector<uint8_t> q;
    auto t2 = d.tok();
    q.insert(q.end(), t2.begin(), t2.end());
    Frame s = d.request(PC_MSG_BENCH_SWEEP, 4, q);
    REQUIRE(u32(s.payload, PC_TAG_BENCH_WRITTEN) == 4);
    REQUIRE(u8(s.payload, PC_TAG_RUNNING) == 0);
    auto pts = bytes(s.payload, PC_TAG_BENCH_POINTS_TS);
    REQUIRE(pts.size() == 4 * 6);
    uint32_t f0 = (uint32_t)pts[0] | ((uint32_t)pts[1] << 8) |
                  ((uint32_t)pts[2] << 16) | ((uint32_t)pts[3] << 24);
    uint32_t f1 = (uint32_t)pts[6] | ((uint32_t)pts[7] << 8) |
                  ((uint32_t)pts[8] << 16) | ((uint32_t)pts[9] << 24);
    REQUIRE(f0 == 145400000);
    REQUIRE(f1 == 145425000);
    REQUIRE(sim.rxHz == 145475000); /* last point visited */
}

TEST_CASE("Bench: a sweep too long for the point buffer is refused",
          "[bench]")
{
    Device d;
    d.hello();
    d.request(PC_MSG_BENCH_ARM, 2, d.tok());
    std::vector<uint8_t> p;
    putU32(p, PC_TAG_BENCH_F0, 145000000);
    putU32(p, PC_TAG_BENCH_F1, 146000000);
    putU32(p, PC_TAG_BENCH_STEP, 1000); /* 1000 points */
    auto tok = d.tok();
    p.insert(p.end(), tok.begin(), tok.end());
    Frame a = d.request(PC_MSG_BENCH_SWEEP, 3, p);
    REQUIRE(a.id == PC_MSG_ERROR);
    REQUIRE(u8(a.payload, PC_TAG_ERROR) == PC_ERR_TOO_LARGE);
}

TEST_CASE("Bench: a capture and a retune cannot share the radio", "[bench]")
{
    Device d;
    d.hello();
    d.request(PC_MSG_BENCH_ARM, 2, d.tok());

    std::vector<uint8_t> w;
    putU16(w, PC_TAG_BENCH_WINDOW, 200);
    auto tok = d.tok();
    w.insert(w.end(), tok.begin(), tok.end());
    d.request(PC_MSG_BENCH_CAPTURE, 3, w);

    std::vector<uint8_t> t;
    putU32(t, PC_TAG_RX_FREQ, 145500000);
    auto t2 = d.tok();
    t.insert(t.end(), t2.begin(), t2.end());
    Frame r = d.request(PC_MSG_BENCH_TUNE, 4, t);
    REQUIRE(r.id == PC_MSG_ERROR);
    REQUIRE(u8(r.payload, PC_TAG_ERROR) == PC_ERR_BUSY);
}

TEST_CASE("Bench: the report says what it does not know", "[bench]")
{
    Device d;
    d.hello();
    Frame r = d.request(PC_MSG_BENCH_REPORT, 2);
    auto plain = bytes(r.payload, PC_TAG_TEXT);
    std::string text((const char *)plain.data(), plain.size());
    REQUIRE(text.find("not armed") != std::string::npos);

    d.request(PC_MSG_BENCH_ARM, 3, d.tok());
    std::vector<uint8_t> p;
    putU32(p, PC_TAG_RX_FREQ, 145500000);
    putU8(p, PC_TAG_MODE, 1);
    auto tok = d.tok();
    p.insert(p.end(), tok.begin(), tok.end());
    d.request(PC_MSG_BENCH_TUNE, 4, p);
    Frame rep = d.request(PC_MSG_BENCH_REPORT, 5);
    auto head = bytes(rep.payload, PC_TAG_TEXT);
    std::string all((const char *)head.data(), head.size());
    uint16_t total = u16(rep.payload, PC_TAG_LENGTH);
    while (all.size() < total) {
        std::vector<uint8_t> rq;
        putU32(rq, PC_TAG_OFFSET, (uint32_t)all.size());
        Frame c = d.request(PC_MSG_BENCH_REPORT, 6, rq);
        auto chunk = bytes(c.payload, PC_TAG_TEXT);
        REQUIRE(!chunk.empty());
        all.append((const char *)chunk.data(), chunk.size());
    }
    REQUIRE(all.size() == total);
    REQUIRE(all.find("145500000") != std::string::npos);
    REQUIRE(all.find("no carrier reference") != std::string::npos);
}

#else /* !CONFIG_C62_BENCH_PROTO */

TEST_CASE("Bench: a production build refuses every bench message", "[bench]")
{
    Device d;
    Frame h = d.hello();
    REQUIRE((u32(h.payload, PC_TAG_CAPS) & PC_CAP_BENCH) == 0);
    /* No token either: a build with neither namespace hands out none. */
    REQUIRE(u32(h.payload, PC_TAG_TOKEN) == 0);

    const uint16_t ids[] = { PC_MSG_BENCH_ARM,    PC_MSG_BENCH_TUNE,
                             PC_MSG_BENCH_PTT,    PC_MSG_BENCH_CAPTURE,
                             PC_MSG_BENCH_RSSI,   PC_MSG_BENCH_SWEEP,
                             PC_MSG_BENCH_REPORT };
    for (uint16_t id : ids) {
        Frame f = d.request(id, 3);
        REQUIRE(f.id == PC_MSG_ERROR);
        REQUIRE(u8(f.payload, PC_TAG_ERROR) == PC_ERR_NOT_ALLOWED);
        REQUIRE(u16(f.payload, PC_TAG_MSG_ID) == id);
        REQUIRE(f.payload.size() == 11); /* the error and the id, nothing */
    }
    REQUIRE(sim.tuned == 0);
    REQUIRE(sim.keys == 0);
}

#endif
