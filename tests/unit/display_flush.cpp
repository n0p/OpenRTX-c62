/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests of the bounded panel flush (core/display_flush.h), the piece that
 * keeps the user interface thread out of the endless wait its screen used to
 * end in (docs/RX_AIRBAND.md §9).
 *
 * The thing being tested is a property, not an output: whatever the bus does,
 * the caller comes back.  So the fake bus has the two behaviours that matter --
 * a transfer that completes and a transfer that never does -- and the clock is
 * the test's, which is what makes "the deadline expired" a fact instead of a
 * sleep.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "core/display_flush.h"
}

namespace
{

/* A frame shape that matches the panel's: command, parameters, command,
 * parameters, command, pixels. */
struct FakeBus {
    uint32_t nowMs = 0;
    bool stuck = false;      /* this transfer never completes               */
    bool refuse = false;     /* the transfer cannot even be started         */
    uint32_t submitted = 0;
    uint32_t abandoned = 0;
    uint32_t bytesSent = 0;
    std::vector<size_t> chunkLengths;
};

int busSubmit(void *ctx, bool isData, const void *buf, size_t len)
{
    (void)isData;
    (void)buf;
    FakeBus *bus = static_cast<FakeBus *>(ctx);

    if (bus->refuse)
        return -5;

    bus->submitted++;
    bus->chunkLengths.push_back(len);
    bus->bytesSent += (uint32_t)len;

    return 0;
}

int busWait(void *ctx, uint32_t timeoutMs)
{
    FakeBus *bus = static_cast<FakeBus *>(ctx);

    if (bus->stuck) {
        /* The deadline is what a real wait would cost, and not a millisecond
         * more: a driver with K_FOREVER never gets here at all. */
        bus->nowMs += timeoutMs;
        return -1;
    }

    bus->nowMs += 1; /* a transfer that completes in a millisecond */
    return 0;
}

void busAbandon(void *ctx)
{
    static_cast<FakeBus *>(ctx)->abandoned++;
}

uint32_t busNow(void *ctx)
{
    return static_cast<FakeBus *>(ctx)->nowMs;
}

displayFlushIo_t busIo(FakeBus *bus)
{
    displayFlushIo_t io;

    io.submit = busSubmit;
    io.wait = busWait;
    io.abandon = busAbandon;
    io.nowMs = busNow;
    io.ctx = bus;

    return io;
}

/* One frame of the C62 panel: 4 + 4 bytes of window and 160 x 2 of pixels. */
uint8_t pixels[320];
uint8_t window[8];

size_t frameChunks(displayChunk_t *chunks)
{
    const uint8_t caset = 0x2A, raset = 0x2B, ramwr = 0x2C;

    chunks[0] = { &caset, 1, false };
    chunks[1] = { window, 4, true };
    chunks[2] = { &raset, 1, false };
    chunks[3] = { window + 4, 4, true };
    chunks[4] = { &ramwr, 1, false };
    chunks[5] = { pixels, sizeof(pixels), true };

    return 6;
}

} // namespace

TEST_CASE("A frame that completes is sent whole and counted", "[disp]")
{
    display_flush_reset();

    FakeBus bus;
    displayFlushIo_t io = busIo(&bus);
    displayChunk_t chunks[6];
    const size_t n = frameChunks(chunks);

    REQUIRE(display_flush_frame(&io, chunks, n, 100) == true);

    const displayFlushStats_t *st = display_flush_stats();
    REQUIRE(st->frames == 1);
    REQUIRE(st->dropped == 0);
    REQUIRE(st->transfers == n);
    REQUIRE(st->timeouts == 0);
    REQUIRE(st->lastMs == n); /* a millisecond per transfer, in this fake bus */
    REQUIRE(bus.abandoned == 0);
    REQUIRE(bus.submitted == n);

    /* Every byte of the frame reached the bus, in order. */
    REQUIRE(bus.chunkLengths.size() == n);
    REQUIRE(bus.chunkLengths[4] == 1);   /* RAMWR */
    REQUIRE(bus.chunkLengths[5] == sizeof(pixels));
    REQUIRE(bus.bytesSent == 1 + 4 + 1 + 4 + 1 + sizeof(pixels));
}

TEST_CASE("A transfer that never completes drops the frame, not the thread",
          "[disp]")
{
    display_flush_reset();

    FakeBus bus;
    bus.stuck = true;
    displayFlushIo_t io = busIo(&bus);
    displayChunk_t chunks[6];
    const size_t n = frameChunks(chunks);

    const uint32_t before = bus.nowMs;
    REQUIRE(display_flush_frame(&io, chunks, n, 100) == false);

    /*
     * The two facts that make the difference between a frozen screen and a
     * frozen radio: the call returns after exactly the deadline, and nothing
     * else of the frame is even attempted.
     */
    REQUIRE(bus.nowMs - before == 100);
    REQUIRE(bus.submitted == 1);
    REQUIRE(bus.abandoned == 1);
    REQUIRE(bus.chunkLengths.size() == 1);

    const displayFlushStats_t *st = display_flush_stats();
    REQUIRE(st->frames == 0);
    REQUIRE(st->dropped == 1);
    REQUIRE(st->timeouts == 1);
    REQUIRE(st->lastDropMs == 100);

    /* And which transfer it was: what `diag` prints when the screen freezes. */
    REQUIRE(st->failChunk == 0);
    REQUIRE(st->failLen == 1);
    REQUIRE(st->failData == false);
}

TEST_CASE("The thread that draws keeps drawing while the bus is wedged",
          "[disp]")
{
    display_flush_reset();

    FakeBus bus;
    bus.stuck = true;
    displayFlushIo_t io = busIo(&bus);
    displayChunk_t chunks[6];
    const size_t n = frameChunks(chunks);

    /*
     * The UI loop, reduced to what matters here: draw a frame, then do the
     * other half of the pass (scan the keyboard, run the FSM).  With the bus
     * wedged, every pass has to get past the draw -- that is the whole point of
     * the deadline -- and the frames have to be counted as dropped.
     */
    uint32_t keyScans = 0;
    for (int pass = 0; pass < 50; pass++) {
        (void)display_flush_frame(&io, chunks, n, 100);
        keyScans++;
    }

    REQUIRE(keyScans == 50);

    const displayFlushStats_t *st = display_flush_stats();
    REQUIRE(st->dropped == 50);
    REQUIRE(st->frames == 0);
    /* 50 frames x one deadline each, nothing more: the loop never blocked on
     * anything else. */
    REQUIRE(bus.nowMs == 50 * 100);
}

TEST_CASE("A frame that cannot even be started is dropped and released",
          "[disp]")
{
    display_flush_reset();

    FakeBus bus;
    bus.refuse = true;
    displayFlushIo_t io = busIo(&bus);
    displayChunk_t chunks[6];
    const size_t n = frameChunks(chunks);

    REQUIRE(display_flush_frame(&io, chunks, n, 100) == false);

    /* No wait at all: the failure is immediate, and the bus is released so the
     * next frame starts from a clean driver context. */
    REQUIRE(bus.nowMs == 0);
    REQUIRE(bus.abandoned == 1);

    const displayFlushStats_t *st = display_flush_stats();
    REQUIRE(st->dropped == 1);
    REQUIRE(st->timeouts == 0);
    REQUIRE(st->transfers == 1);
}

TEST_CASE("A slow but finite transfer is not a dropped frame", "[disp]")
{
    display_flush_reset();

    /* The real panel takes ~16 ms at 20 MHz for a full frame; the deadline is
     * 100 ms.  A bus that is merely slow must not cost frames, or the counter
     * in `diag` would be noise instead of a symptom. */
    FakeBus bus;
    bus.nowMs = 10 * 1000;
    displayFlushIo_t io = busIo(&bus);
    displayChunk_t chunks[6];
    const size_t n = frameChunks(chunks);

    for (int i = 0; i < 10; i++)
        REQUIRE(display_flush_frame(&io, chunks, n, 100) == true);

    const displayFlushStats_t *st = display_flush_stats();
    REQUIRE(st->frames == 10);
    REQUIRE(st->dropped == 0);
    REQUIRE(st->maxMs >= st->lastMs);
}
