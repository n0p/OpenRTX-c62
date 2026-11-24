/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

#include "core/audio_latency.h"
#include "protocols/DMR/DmrFec.h"
#include "protocols/DMR/DmrRpt.h"

/*
 * Repeater operation of the DMR transmitter, on the host and without a radio.
 *
 * Three things are checked here: that the BS_Dwn_Act block a repeater receives
 * is the one TS 102 361-2 describes and closes its CRC, that the phase tracker
 * recovers a slot grid out of synthetic bursts (jitter, drift, losses), and
 * that the gate and the wake machine refuse and retry where they should.
 *
 * The synthetic bursts are what the *receiver* would measure: the position of
 * the repeater's TS1 boundary in our own 48 kHz sample clock, one value per
 * decoded outbound burst.  The generator below produces that from a true
 * grid, its ppm error, a jitter, a loss pattern and a scatter of outliers,
 * which is the whole input of the tracker.
 */

namespace
{

constexpr int32_t FRAME = DMR_RPT_FRAME_SAMPLES;
constexpr int32_t SLOT = DMR_RPT_SLOT_SAMPLES;
constexpr int32_t PER_MS = DMR_RPT_SAMPLES_PER_MS;

struct GridParams
{
    int32_t phase0 = 700;   ///< TS1 position at t = 0, samples
    int16_t ppm = 0;        ///< Repeater clock error
    int32_t jitter = 0;     ///< Uniform +-jitter, samples
    int32_t everyMs = 30;   ///< A burst every this many ms (one slot)
    int32_t skipEvery = 0;  ///< Drop one burst out of this many, 0 = none
    int32_t outlierEvery = 0; ///< Corrupt one in this many, 0 = none
    int32_t outlierBy = 0;  ///< ... by this many samples (a slot is 1440)
    uint32_t noise = 12345; ///< Deterministic pseudo-random state
};

/// The true TS1 position at t, in samples, unwrapped.
int32_t truth(const GridParams &g, int32_t tMs)
{
    /* the drift is applied to the rate, so the position is a whole sample */
    int64_t v = g.phase0
              + ((int64_t)PER_MS * tMs * (1000000 + (int64_t)g.ppm)) / 1000000;
    return (int32_t)v;
}

/// Feed a synthetic sequence; returns the measurements actually delivered.
size_t feedGrid(dmrRptPhase &p, const GridParams &g, int32_t durationMs)
{
    uint32_t s = g.noise;
    size_t delivered = 0;
    unsigned burst = 0;

    for (int32_t t = 0; t <= durationMs; t += g.everyMs, burst++) {
        if ((g.skipEvery != 0) && ((burst % (unsigned)g.skipEvery) == 0))
            continue;

        s = s * 1664525u + 1013904223u;
        int32_t j = 0;

        if (g.jitter != 0)
            j = (int32_t)(s >> 16) % (2 * g.jitter + 1) - g.jitter;

        int32_t v = truth(g, t) + j;

        if ((g.outlierEvery != 0) && (burst != 0)
            && ((burst % (unsigned)g.outlierEvery) == 0))
            v += g.outlierBy;

        dmrRpt_phaseFeed(&p, v, (uint32_t)t);
        delivered++;
    }

    return delivered;
}

/// The block dmrTx_dataSlot() would send, as the receiver's 96 bits.
void builtCsbk(uint32_t bsAddress, uint32_t sourceId, uint8_t lc[12])
{
    uint8_t info[DMR_CSBK_BITS];

    dmrRpt_csbkDownlinkActivity(info, bsAddress, sourceId);
    memset(lc, 0, 12);
    for (int i = 0; i < DMR_CSBK_BITS; i++)
        lc[i >> 3] |= (uint8_t)(info[i] << (7 - (i & 7)));
}

} // namespace

/* --------------------------------------------------------- BS activation --- */

TEST_CASE("BS_Dwn_Act closes its CRC and reads back", "[dmr][rpt]")
{
    uint8_t info[DMR_CSBK_BITS];
    const uint32_t bs = 0x123456;
    const uint32_t src = 0xABCDEF;

    dmrRpt_csbkDownlinkActivity(info, bs, src);

    /*
     * The receiver unmasks the last 16 bits with 0xA5A5 and requires a zero
     * syndrome (DmrPhy.c, DMR_DT_CSBK): that is the check a real repeater
     * makes, so it is the check here too.
     */
    REQUIRE(dmr_fec_crc16(info, DMR_CSBK_BITS) == 0xA5A5);

    /* Re-apply the mask to get the residue of the unmasked block */
    uint8_t unmasked[DMR_CSBK_BITS];

    memcpy(unmasked, info, sizeof(unmasked));
    for (int i = 0; i < 16; i++)
        unmasked[80 + i] ^= (uint8_t)((0xA5A5u >> (15 - i)) & 1u);
    REQUIRE(dmr_fec_crc16(unmasked, DMR_CSBK_BITS) == 0);

    /* Octet 0: LB = 1, PF = 0, CSBKO = 111000 -> 0xB8, then FID 0 */
    uint8_t lc[12];

    memset(lc, 0, sizeof(lc));
    for (int i = 0; i < DMR_CSBK_BITS; i++)
        lc[i >> 3] |= (uint8_t)(info[i] << (7 - (i & 7)));
    REQUIRE(lc[0] == 0xB8);
    REQUIRE(lc[1] == 0x00);
    REQUIRE(lc[2] == 0x00);
    REQUIRE(lc[3] == 0x00);
    /* reserved 16 bits at zero, then the BS address and the source */
    REQUIRE(lc[4] == 0x12);
    REQUIRE(lc[5] == 0x34);
    REQUIRE(lc[6] == 0x56);
    REQUIRE(lc[7] == 0xAB);
    REQUIRE(lc[8] == 0xCD);
    REQUIRE(lc[9] == 0xEF);

    dmrRptCsbk parsed;

    REQUIRE(dmrRpt_csbkParse(info, &parsed));
    REQUIRE(parsed.opcode == DMR_RPT_CSBKO_BS_DWN_ACT);
    REQUIRE(parsed.fid == 0);
    REQUIRE(parsed.dst == bs);
    REQUIRE(parsed.src == src);

    /* The wildcard address is what "any repeater" looks like on the air */
    uint8_t wildcard[12];

    builtCsbk(DMR_RPT_ADDR_ANY, src, wildcard);
    REQUIRE(wildcard[4] == 0xFF);
    REQUIRE(wildcard[5] == 0xFF);
    REQUIRE(wildcard[6] == 0xFF);
}

/* ------------------------------------------------------- timing recovery --- */

TEST_CASE("A quiet grid settles and predicts", "[dmr][rpt]")
{
    dmrRptPhase p;
    GridParams g;

    dmrRpt_phaseReset(&p);
    REQUIRE_FALSE(dmrRpt_phaseSettled(&p));
    REQUIRE(dmrRpt_phaseAt(&p, 0) == -1);

    feedGrid(p, g, 600);

    REQUIRE(p.count == DMR_RPT_HIST);
    REQUIRE(dmrRpt_phaseSettled(&p));
    REQUIRE(p.ppm == 0);
    REQUIRE(std::abs((int)p.offset) <= 1);

    /* The prediction is the truth, modulo the frame */
    for (int32_t t = 600; t < 1200; t += 30) {
        int32_t predicted = dmrRpt_phaseAt(&p, (uint32_t)t);
        int32_t reference = truth(g, t) % FRAME;

        if (reference < 0)
            reference += FRAME;
        REQUIRE(predicted == reference);
    }
}

TEST_CASE("Drift is measured in ppm", "[dmr][rpt]")
{
    const int16_t drifts[] = { -7, -2, 3, 11, 20 };

    for (int16_t ppm : drifts) {
        dmrRptPhase p;
        GridParams g;

        g.ppm = ppm;
        g.jitter = 0;
        dmrRpt_phaseReset(&p);
        feedGrid(p, g, 10000);

        /*
         * A measurement is one sample of 48 kHz, so ten seconds of baseline
         * resolve the drift to a couple of ppm: what is left over a minute of
         * transmission is 0.1 ms, two orders of magnitude inside the guard.
         */
        REQUIRE(dmrRpt_phaseSettled(&p));
        REQUIRE(std::abs((int)p.ppm - (int)ppm) <= 3);

        /*
         * And the prediction follows the drift: one second ahead, the error
         * against the true grid is a fraction of a millisecond.
         */
        int32_t predicted = dmrRpt_phaseAt(&p, 10000 + 1000);
        int32_t reference = truth(g, 10000 + 1000) % FRAME;

        if (reference < 0)
            reference += FRAME;

        int32_t err = predicted - reference;

        if (err > FRAME / 2)
            err -= FRAME;
        if (err < -FRAME / 2)
            err += FRAME;

        REQUIRE(std::abs(err) <= 6);
    }
}

TEST_CASE("Jitter is filtered, bursts are lost, outliers are rejected",
          "[dmr][rpt]")
{
    dmrRptPhase p;
    GridParams g;

    g.jitter = 3;         /* +-3 samples of sync jitter */
    g.skipEvery = 5;      /* four bursts out of five arrive */
    g.outlierEvery = 4;   /* and one in four lands a slot away */
    g.outlierBy = SLOT;
    g.ppm = 4;

    dmrRpt_phaseReset(&p);

    size_t delivered = feedGrid(p, g, 10000);

    REQUIRE(delivered > 100);
    REQUIRE(dmrRpt_phaseSettled(&p));
    REQUIRE(std::abs((int)p.ppm - 4) <= 3);

    /*
     * Half of the measurements were corrupt, so the acceptance ratio says so
     * and the offset never moved by anything like the outlier step.
     */
    REQUIRE(p.quality < 90);
    REQUIRE(std::abs((int)p.offset - 3) <= 8);

    int32_t predicted = dmrRpt_phaseAt(&p, 10000 + 500);
    int32_t reference = truth(g, 10000 + 500) % FRAME;

    if (reference < 0)
        reference += FRAME;

    int32_t err = predicted - reference;

    if (err > FRAME / 2)
        err -= FRAME;
    if (err < -FRAME / 2)
        err += FRAME;

    REQUIRE(std::abs(err) <= 12);
}

TEST_CASE("Three outliers in a row move the grid", "[dmr][rpt]")
{
    dmrRptPhase p;
    GridParams g;

    dmrRpt_phaseReset(&p);
    feedGrid(p, g, 300);

    const int32_t before = dmrRpt_phaseAt(&p, 300);

    /*
     * A repeater that restarts its frame: every burst now lands half a frame
     * away.  The first two are rejected, the third restarts the tracker.
     */
    for (int i = 1; i <= 3; i++)
        dmrRpt_phaseFeed(&p, truth(g, 300 + 30 * i) + 600, 300 + 30 * i);

    int32_t after = dmrRpt_phaseAt(&p, 300 + 90);
    int32_t expect = (truth(g, 300 + 90) + 600) % FRAME;

    REQUIRE(p.count == 1);
    REQUIRE(after == expect);
    REQUIRE(after != before);
    REQUIRE_FALSE(dmrRpt_phaseSettled(&p));
}

TEST_CASE("A grid that never converges is not used", "[dmr][rpt]")
{
    dmrRptPhase p;
    GridParams g;

    /* A capture played back from a sound card: no stable reference at all */
    g.jitter = 200;
    g.ppm = 0;

    dmrRpt_phaseReset(&p);
    feedGrid(p, g, 3000);
    REQUIRE_FALSE(dmrRpt_phaseSettled(&p));

    /* And a drift past what the standard allows is a broken reference */
    dmrRptPhase q;
    GridParams h;

    h.ppm = 60;
    h.jitter = 0;
    dmrRpt_phaseReset(&q);
    feedGrid(q, h, 3000);
    REQUIRE_FALSE(dmrRpt_phaseSettled(&q));
}

/* ------------------------------------------------------------ start plan --- */

namespace
{

/// Settled tracker whose TS1 boundary sits at `phase` at tick `t`.
void settledAt(dmrRptPhase &p, int32_t phase, uint32_t t = 1000)
{
    GridParams g;

    g.phase0 = phase;
    dmrRpt_phaseReset(&p);
    for (uint32_t k = 0; k <= 10; k++)
        dmrRpt_phaseFeed(&p, phase + (int32_t)k * PER_MS * 30, t + 30 * k);
}

/// Air instant of the start of the burst slot the plan produces, mod a frame.
///
/// Waveform sample 0 of the call leaves the delay line at `shift` and reaches
/// the air `latency` later; the burst is in the first rendered slot or in the
/// second, and the frames of lead the PA settling asked for move nothing.
int32_t planSlotStart(const dmrRptPlan &plan, int32_t latency)
{
    int32_t v = (int32_t)plan.shift + latency
              + ((plan.burstFirst == 2) ? SLOT : 0);

    v %= FRAME;
    if (v < 0)
        v += FRAME;
    return v;
}

} // namespace

TEST_CASE("The plan lands on the repeater's slot", "[dmr][rpt]")
{
    const int32_t phases[] = { 0, 1, 700, 1439, 1440, 1441, 2879 };
    const int32_t latencies[] = { 0, 240, DMR_RPT_US_TO_SAMPLES(DMR_RPT_HW_LATENCY_US_DEFAULT), 2879 };

    for (int32_t phase : phases) {
        for (int32_t latency : latencies) {
            for (uint8_t ts = 1; ts <= 2; ts++) {
                for (uint8_t lead = 0; lead <= 3; lead++) {
                    dmrRptPhase p;

                    settledAt(p, phase);

                    dmrRptPlan plan;

                    dmrRpt_plan(&p, ts, 1000, latency, lead, &plan);

                    REQUIRE(plan.aligned);
                    REQUIRE(plan.shift < SLOT);
                    REQUIRE(plan.burstFirst <= 2);
                    REQUIRE(plan.leadPairs == (lead + 1) / 2);

                    /*
                     * Two things have to hold: the burst lands on the target
                     * slot boundary (TS1 or TS2 of the repeater's frame), and
                     * the whole slot is in the stream, i.e. the delay never
                     * has to be larger than one slot.
                     */
                    int32_t target = (phase + ((ts == 2) ? SLOT : 0)) % FRAME;

                    if (target < 0)
                        target += FRAME;

                    REQUIRE(planSlotStart(plan, latency) == target);
                }
            }
        }
    }
}

TEST_CASE("Without an estimate the plan is free running", "[dmr][rpt]")
{
    dmrRptPhase p;
    dmrRptPlan plan;

    dmrRpt_phaseReset(&p);
    dmrRpt_plan(&p, 1, 0, DMR_RPT_US_TO_SAMPLES(DMR_RPT_HW_LATENCY_US_DEFAULT), 0, &plan);
    REQUIRE_FALSE(plan.aligned);
    REQUIRE(plan.shift == 0);
    REQUIRE(plan.burstFirst == 1);

    dmrRpt_plan(&p, 2, 0, DMR_RPT_US_TO_SAMPLES(DMR_RPT_HW_LATENCY_US_DEFAULT), 0, &plan);
    REQUIRE_FALSE(plan.aligned);
    REQUIRE(plan.burstFirst == 2);
}

TEST_CASE("The baseband delay line shifts by whole samples", "[dmr][rpt]")
{
    int16_t prev[DMR_RPT_SLOT_SAMPLES];
    int16_t cur[DMR_RPT_SLOT_SAMPLES];
    int16_t out[DMR_RPT_SLOT_SAMPLES];

    for (int i = 0; i < DMR_RPT_SLOT_SAMPLES; i++) {
        prev[i] = (int16_t)(-1 - i);
        cur[i] = (int16_t)(i);
    }

    /* No delay is a copy */
    dmrRpt_txShift(prev, cur, 0, out, DMR_RPT_SLOT_SAMPLES);
    REQUIRE(std::memcmp(out, cur, sizeof(cur)) == 0);

    /* A delay of 5 takes the last 5 samples of the previous slot first */
    dmrRpt_txShift(prev, cur, 5, out, DMR_RPT_SLOT_SAMPLES);
    REQUIRE(out[0] == prev[DMR_RPT_SLOT_SAMPLES - 5]);
    REQUIRE(out[4] == prev[DMR_RPT_SLOT_SAMPLES - 1]);
    REQUIRE(out[5] == cur[0]);
    REQUIRE(out[DMR_RPT_SLOT_SAMPLES - 1] == cur[DMR_RPT_SLOT_SAMPLES - 6]);
}

TEST_CASE("The drift slew moves one sample at a time", "[dmr][rpt]")
{
    REQUIRE(dmrRpt_txShiftStep(100, 110, 1) == 101);
    REQUIRE(dmrRpt_txShiftStep(100, 90, 1) == 99);
    REQUIRE(dmrRpt_txShiftStep(100, 100, 1) == 100);
    REQUIRE(dmrRpt_txShiftStep(100, 200, 8) == 108);
    REQUIRE(dmrRpt_txShiftStep(100, 50, 0) == 100);
}

/* ---------------------------------------------------------- the wake FSM --- */

TEST_CASE("The wake machine retries and then decides", "[dmr][rpt]")
{
    for (int policy = 0; policy <= 1; policy++) {
        dmrRptWake w;

        dmrRptWake_start(&w, DMR_RPT_WAKE_RETRIES, 0);
        REQUIRE(dmrRptWake_active(&w));

        /* First burst is handed to the caller at once */
        REQUIRE(dmrRptWake_next(&w, 0, DMR_RPT_NOANSWER_FREERUN)
                == DMR_RPT_WAKE_SEND);
        dmrRptWake_sent(&w, 20);

        uint32_t t = 20;
        unsigned sends = 1;
        bool decided = false;

        for (unsigned i = 0; i < 100; i++) {
            t += 10;
            dmrRptWakeAction a = dmrRptWake_next(&w, t,
                                                 (dmrRptNoAnswer)policy);

            if (a == DMR_RPT_WAKE_SEND) {
                sends++;
                dmrRptWake_sent(&w, t);
            } else if (a != DMR_RPT_WAKE_NONE) {
                decided = true;
                REQUIRE(a == ((policy == DMR_RPT_NOANSWER_ABORT)
                                  ? DMR_RPT_WAKE_ABORT : DMR_RPT_WAKE_FREERUN));
                break;
            }
        }

        REQUIRE(decided);
        /* The first burst and the retries */
        REQUIRE(sends == 1 + DMR_RPT_WAKE_RETRIES);
        REQUIRE_FALSE(dmrRptWake_active(&w));
    }
}

TEST_CASE("An answered wake goes straight to the call", "[dmr][rpt]")
{
    dmrRptWake w;

    dmrRptWake_start(&w, DMR_RPT_WAKE_RETRIES, 0);
    REQUIRE(dmrRptWake_next(&w, 0, DMR_RPT_NOANSWER_ABORT)
            == DMR_RPT_WAKE_SEND);
    dmrRptWake_sent(&w, 10);
    REQUIRE(dmrRptWake_active(&w));

    dmrRptWake_answer(&w);
    REQUIRE(dmrRptWake_next(&w, 20, DMR_RPT_NOANSWER_ABORT)
            == DMR_RPT_WAKE_CALL);
    REQUIRE_FALSE(dmrRptWake_active(&w));
}

TEST_CASE("A repeater that is already up is not woken", "[dmr][rpt]")
{
    dmrRptWake w;

    dmrRptWake_start(&w, DMR_RPT_WAKE_RETRIES, 0);
    dmrRptWake_skip(&w);
    REQUIRE_FALSE(dmrRptWake_active(&w));
    REQUIRE(dmrRptWake_next(&w, 100, DMR_RPT_NOANSWER_ABORT)
            == DMR_RPT_WAKE_NONE);
    REQUIRE(w.stage == DMR_RPT_WAKE_DONE);
}

/* ---------------------------------------------------------------- the gate --- */

TEST_CASE("The gate refuses the wrong channel", "[dmr][rpt]")
{
    dmrRptChannel ch;
    dmrRptSeen seen;

    ch.colourCode = 3;
    ch.timeslot = 1;
    ch.polite = true;

    seen.colourCode = 3;
    seen.timeslot = 1;
    seen.busy = false;

    REQUIRE(dmrRpt_gate(&ch, &seen) == DMR_RPT_GATE_OK);

    /* Nothing heard yet: the colour code is not a reason to refuse */
    seen.colourCode = DMR_RPT_CC_UNKNOWN;
    seen.timeslot = 0;
    REQUIRE(dmrRpt_gate(&ch, &seen) == DMR_RPT_GATE_OK);

    /* Another colour code is another repeater */
    seen.colourCode = 4;
    REQUIRE(dmrRpt_gate(&ch, &seen) == DMR_RPT_GATE_CC);

    /* Our slot busy */
    seen.colourCode = 3;
    seen.busy = true;
    REQUIRE(dmrRpt_gate(&ch, &seen) == DMR_RPT_GATE_BUSY);

    /* The repeater is on the other timeslot and the channel fixes ours */
    seen.busy = false;
    seen.timeslot = 2;
    REQUIRE(dmrRpt_gate(&ch, &seen) == DMR_RPT_GATE_TS);

    /* ... unless the channel follows the repeater */
    ch.timeslot = 0;
    REQUIRE(dmrRpt_gate(&ch, &seen) == DMR_RPT_GATE_OK);

    /* And politeness is optional */
    ch.timeslot = 1;
    ch.polite = false;
    seen.timeslot = 1;
    seen.busy = true;
    REQUIRE(dmrRpt_gate(&ch, &seen) == DMR_RPT_GATE_OK);
}

/* -------------------------------------------------------- audio latency --- */

namespace
{

constexpr uint32_t RATE = 48000;

/// A capture of `pre` samples of silence, the marker delayed by `delay`, noise
int makeCapture(int16_t *cap, size_t n, size_t pre, int32_t delay, int16_t noise,
                uint32_t seed = 424242)
{
    int16_t marker[DMR_LAT_MARKER_SAMPLES];

    audioLatency_marker(marker, RATE);
    std::memset(cap, 0, n * sizeof(int16_t));

    uint32_t s = seed;

    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        cap[i] = (int16_t)((int32_t)(s >> 20) % (2 * noise + 1) - noise);
    }

    for (size_t i = 0; i < DMR_LAT_MARKER_SAMPLES; i++) {
        const size_t at = pre + (size_t)delay + i;

        if (at >= n)
            break;
        int32_t v = cap[at] + marker[i];

        if (v > 32767)
            v = 32767;
        if (v < -32768)
            v = -32768;
        cap[at] = (int16_t)v;
    }

    return 0;
}

} // namespace

TEST_CASE("The marker is generated and correlates to itself", "[dmr][lat]")
{
    int16_t marker[DMR_LAT_MARKER_SAMPLES];
    int32_t peak = 0;

    audioLatency_marker(marker, RATE);

    for (int i = 0; i < DMR_LAT_MARKER_SAMPLES; i++)
        peak = (std::abs((int)marker[i]) > peak) ? std::abs((int)marker[i])
                                                 : peak;

    /* Hann windowed: it starts and ends at zero and it is loud in the middle */
    REQUIRE(marker[0] == 0);
    REQUIRE(std::abs((int)marker[DMR_LAT_MARKER_SAMPLES - 1]) < 200);
    REQUIRE(peak > 4000);
    REQUIRE(peak <= DMR_LAT_AMPLITUDE);
}

TEST_CASE("The audio path delay is measured to a sample", "[dmr][lat]")
{
    const int32_t delays[] = { 0, 1, 12, 120, 480, 1000 };
    int16_t cap[2048];

    for (int32_t delay : delays) {
        int32_t us = -1;
        int32_t spread = -1;

        makeCapture(cap, sizeof(cap) / sizeof(cap[0]), 100, delay, 200);

        REQUIRE(audioLatency_measure(cap, sizeof(cap) / sizeof(cap[0]), 100,
                                     RATE, &us, &spread));

        const int32_t expected = (delay * 1000000) / (int32_t)RATE;

        REQUIRE(std::abs(us - expected) <= 1000000 / (int32_t)RATE);
        REQUIRE(spread > 0);
        /* a 3.6 kHz chirp: the peak is about a quarter of a millisecond wide */
        REQUIRE(spread < 1000);
    }
}

TEST_CASE("A capture with no marker is not a measurement", "[dmr][lat]")
{
    int16_t cap[1024];
    int32_t us = 0;
    int32_t spread = 0;

    std::memset(cap, 0, sizeof(cap));
    REQUIRE_FALSE(audioLatency_measure(cap, 1024, 0, RATE, &us, &spread));

    /* the marker does not fit in the window it was told to search */
    REQUIRE_FALSE(audioLatency_measure(cap, 100, 0, RATE, &us, &spread));
}

/* ------------------------------------------------------ latency in the plan --- */

TEST_CASE("The start lands on the grid whatever the audio latency", "[dmr][rpt]")
{
    dmrRptPhase p;

    settledAt(p, 900);

    for (int32_t latencyMs = 0; latencyMs <= 20; latencyMs += 5) {
        for (uint8_t ts = 1; ts <= 2; ts++) {
            const int32_t latency = DMR_RPT_US_TO_SAMPLES(latencyMs * 1000);

            REQUIRE(latency == latencyMs * 48);

            dmrRptPlan plan;

            dmrRpt_plan(&p, ts, 1000, latency, 0, &plan);

            int32_t target = (900 + ((ts == 2) ? SLOT : 0)) % FRAME;

            REQUIRE(plan.aligned);
            REQUIRE(planSlotStart(plan, latency) == target);

            /*
             * And the error of a wrong latency is exactly the error of the
             * number: this is what the margin of dmrRpt_fits() is spent on.
             */
            const int32_t wrong = latency + 24; /* 0.5 ms too long */
            dmrRptPlan bad;

            dmrRpt_plan(&p, ts, 1000, wrong, 0, &bad);
            REQUIRE(planSlotStart(bad, latency) == (target - 24 + FRAME) % FRAME);
        }
    }
}

TEST_CASE("A latency that does not fit is refused, not transmitted", "[dmr][rpt]")
{
    int32_t margin = 0;

    /* 1 ms of margin, a 0.25 ms measurement and a short over: it fits */
    REQUIRE(dmrRpt_fits(12, 1000, 2, &margin));
    REQUIRE(margin == DMR_RPT_SLOT_MARGIN - 12 - 0);

    /* 30 s of over at 20 ppm eats 29 samples of the 48 */
    REQUIRE(dmrRpt_fits(12, 30000, 20, &margin));
    REQUIRE(margin > 0);

    /* A 5 ms uncertainty cannot fit in a 1 ms budget */
    REQUIRE_FALSE(dmrRpt_fits(DMR_RPT_US_TO_SAMPLES(5000), 30000, 2, &margin));
    REQUIRE(margin < 0);

    /* And a drift past the allowance over a long over does not either */
    REQUIRE_FALSE(dmrRpt_fits(12, 300000, 25, &margin));
}

/* ----------------------------------------------------- parrot calibration --- */

namespace
{

/// A simulated repeater: it hears a burst only if it lands inside its window.
struct Parrot
{
    int16_t centre = 30;   ///< True error of the radio against the window
    int16_t window = 30;   ///< Half width of the window it accepts, samples
    int16_t crcWindow = 12;///< ... and of the one where the CRC still passes
    int32_t jitter = 0;    ///< Uniform +-jitter of the echo delay
    bool deaf = false;     ///< No echo at all
    uint32_t seed = 7;
};

/// Run a whole pass against the simulated repeater; returns the actions seen.
std::vector<int> runCalibration(dmrRptCal &cal, const Parrot &parrot,
                                uint32_t &tick, bool cancelAt = false,
                                int cancelAfter = -1)
{
    std::vector<int> actions;

    dmrRptCal_start(&cal, tick);

    for (unsigned guard = 0; guard < 500; guard++) {
        if (cancelAt && (cancelAfter >= 0) && (guard == (unsigned)cancelAfter))
            dmrRptCal_cancel(&cal);

        const int action = (int)dmrRptCal_next(&cal, tick);

        actions.push_back(action);
        tick += 10;

        if (action == DMR_RPT_CAL_SEND) {
            const int16_t offset = dmrRptCal_offset(&cal);
            const int16_t err = (int16_t)(offset - parrot.centre);

            if (!parrot.deaf && (std::abs((int)err) <= parrot.window)) {
                /* the echo comes back on the repeater's own grid */
                const uint32_t expected = 100000 + (uint32_t)offset;

                dmrRptCal_sent(&cal, tick, expected);

                uint32_t s = parrot.seed + (uint32_t)offset;

                for (int b = 0; b < 3; b++) {
                    s = s * 1664525u + 1013904223u;

                    const int32_t j = (parrot.jitter == 0)
                                    ? 0
                                    : (int32_t)(s >> 16) % (2 * parrot.jitter + 1)
                                      - parrot.jitter;

                    dmrRptCal_heard(&cal,
                                    expected + (uint32_t)(1440 * b)
                                    + (uint32_t)j,
                                    std::abs((int)err) <= parrot.crcWindow);
                }
            } else {
                /* nothing comes back: the attempt is over when it is over */
                tick += DMR_RPT_WAKE_TX_MS + DMR_RPT_CAL_LISTEN_MS;
            }
        }

        if ((action == DMR_RPT_CAL_FINISH) || (action == DMR_RPT_CAL_ABORT))
            break;
    }

    return actions;
}

} // namespace

TEST_CASE("The calibration finds the window and keeps the median", "[dmr][rpt]")
{
    dmrRptCal cal;
    Parrot parrot;
    uint32_t tick = 1000;

    /* the radio is 18 samples (0.375 ms) out, the window is +-30 */
    parrot.centre = 18;
    parrot.crcWindow = 20;

    runCalibration(cal, parrot, tick);

    dmrRptCalResult res;

    REQUIRE(dmrRptCal_result(&cal, &res));
    REQUIRE(res.store);
    /* the offsets that come back with quality: 0 and 24 -> median 12 */
    REQUIRE(res.valid >= 2);
    REQUIRE(std::abs((int)res.offset - 18) <= 24);
    REQUIRE(res.spread >= 0);
}

TEST_CASE("A centered radio keeps its calibration", "[dmr][rpt]")
{
    dmrRptCal cal;
    Parrot parrot;
    uint32_t tick = 1000;

    parrot.centre = 0;
    parrot.window = 30;
    /* the window where the CRC still passes is about the slot margin */
    parrot.crcWindow = 26;

    runCalibration(cal, parrot, tick);

    dmrRptCalResult res;

    REQUIRE(dmrRptCal_result(&cal, &res));
    REQUIRE(res.store);
    REQUIRE(res.offset == 0);
}

TEST_CASE("A noisy echo still finds the middle", "[dmr][rpt]")
{
    dmrRptCal cal;
    Parrot parrot;
    uint32_t tick = 1000;

    parrot.centre = 0;
    parrot.window = 60;
    parrot.crcWindow = 26;
    parrot.jitter = 6;

    runCalibration(cal, parrot, tick);

    dmrRptCalResult res;

    REQUIRE(dmrRptCal_result(&cal, &res));
    REQUIRE(res.store);
    REQUIRE(std::abs((int)res.offset) <= 24);
}

TEST_CASE("No echo stores nothing and says so", "[dmr][rpt]")
{
    dmrRptCal cal;
    Parrot parrot;
    uint32_t tick = 1000;

    parrot.deaf = true;

    runCalibration(cal, parrot, tick);

    dmrRptCalResult res;

    REQUIRE(dmrRptCal_result(&cal, &res));
    REQUIRE(res.valid == 0);
    REQUIRE_FALSE(res.store);

    /* A single echo is not a median either */
    dmrRptCal one;
    Parrot p2;
    uint32_t t2 = 1000;

    p2.centre = 0;
    p2.window = 1;   /* only the 0 offset attempt gets through */

    runCalibration(one, p2, t2);
    REQUIRE(dmrRptCal_result(&one, &res));
    REQUIRE(res.valid == 1);
    REQUIRE_FALSE(res.store);
}

TEST_CASE("Cancelling the calibration transmits nothing more", "[dmr][rpt]")
{
    dmrRptCal cal;
    Parrot parrot;
    uint32_t tick = 1000;
    std::vector<int> actions;

    parrot.centre = 0;
    actions = runCalibration(cal, parrot, tick, true, 30);

    REQUIRE_FALSE(actions.empty());
    REQUIRE(actions.back() == DMR_RPT_CAL_ABORT);
    REQUIRE_FALSE(dmrRptCal_active(&cal));
    REQUIRE(cal.stage == DMR_RPT_CAL_DONE);

    dmrRptCalResult res;

    REQUIRE(dmrRptCal_result(&cal, &res));
    REQUIRE_FALSE(res.store);
}

TEST_CASE("The calibration pass is bounded", "[dmr][rpt]")
{
    dmrRptCal cal;

    dmrRptCal_start(&cal, 0);

    /* A caller that never reports the call on the air cannot hang the pass */
    uint32_t t = 0;
    int last = 0;

    for (unsigned i = 0; i < 10000; i++) {
        last = (int)dmrRptCal_next(&cal, t);
        t += 10;
        if ((last == DMR_RPT_CAL_FINISH) || (last == DMR_RPT_CAL_ABORT))
            break;
    }

    /* the pass ends, by itself, well inside the hard limit */
    REQUIRE(((last == DMR_RPT_CAL_ABORT) || (last == DMR_RPT_CAL_FINISH)));
    REQUIRE(t < (uint32_t)DMR_RPT_CAL_MAX_MS + 2000u);
    REQUIRE_FALSE(dmrRptCal_active(&cal));
}

TEST_CASE("The calibration table fits in a monitor line", "[dmr][rpt]")
{
    dmrRptCal cal;
    Parrot parrot;
    uint32_t tick = 1000;
    char buf[200];

    parrot.centre = 24;
    runCalibration(cal, parrot, tick);

    REQUIRE(dmrRptCal_format(&cal, buf, sizeof(buf)) > 0);
    REQUIRE(std::strlen(buf) < sizeof(buf));
    /* every attempt of the pass is in there, offset echoed */
    REQUIRE(std::strstr(buf, "0:0/") != nullptr);
    REQUIRE(std::strstr(buf, "4:48/") != nullptr);
}

TEST_CASE("The trace fits and says nothing when it has nothing", "[dmr][rpt]")
{
    char buf[80];
    dmrRptPhase p;
    dmrRptPlan plan;

    REQUIRE(dmrRpt_format(NULL, NULL, buf, sizeof(buf)) > 0);
    REQUIRE(std::strcmp(buf, "none") == 0);

    settledAt(p, 700);
    dmrRpt_plan(&p, 1, 1000, DMR_RPT_US_TO_SAMPLES(DMR_RPT_HW_LATENCY_US_DEFAULT), 0, &plan);
    REQUIRE(dmrRpt_format(&p, &plan, buf, sizeof(buf)) > 0);
    REQUIRE(std::strstr(buf, "ok") != nullptr);
    REQUIRE(std::strstr(buf, "sh=") != nullptr);
}
