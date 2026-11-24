/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file DmrRpt.c
 * @brief Repeater operation of the DMR transmitter, see DmrRpt.h.
 *
 * Three independent pieces, all of them time-in / time-out:
 *
 *  - the BS_Dwn_Act CSBK and its parse, straight from TS 102 361-2 table 7.5a
 *    and the CRC rules of TS 102 361-1 annex B;
 *  - the downlink phase tracker: a median filtered, drift fitted line through
 *    the measured positions of the repeater's TS1 boundary;
 *  - the wake machine and the colour code / timeslot gate.
 *
 * Everything is integer: the radio has no FPU headroom to spend on a control
 * loop that runs once per burst, and a ppm is an integer at the resolution the
 * standard cares about (TS 102 361-1 clause 10.1.4: +-2 ppm).
 */

#include "protocols/DMR/DmrRpt.h"

#include <stdio.h>
#include <string.h>

#include "protocols/DMR/DmrFec.h"

/** CRC mask of a CSBK, TS 102 361-1 annex B table B.21. */
#define CSBK_CRC_MASK 0xA5A5

/* --------------------------------------------------------------- helpers --- */

/**
 * Wrap a difference of sample positions into one 60 ms frame, in
 * (-FRAME/2, +FRAME/2]: the nearest explanation of "where this burst landed
 * with respect to where it should have".
 */
static int32_t wrapFrame(int32_t d)
{
    const int32_t half = DMR_RPT_FRAME_SAMPLES / 2;

    d %= DMR_RPT_FRAME_SAMPLES;
    if (d > half)
        d -= DMR_RPT_FRAME_SAMPLES;
    if (d <= -half)
        d += DMR_RPT_FRAME_SAMPLES;

    return d;
}

/** Modular reduction into [0, m). */
static int32_t modFrame(int32_t v)
{
    v %= DMR_RPT_FRAME_SAMPLES;
    if (v < 0)
        v += DMR_RPT_FRAME_SAMPLES;

    return v;
}

/** Median of n integers, insertion sort on a copy (n <= DMR_RPT_HIST). */
static int32_t median(const int32_t *v, unsigned n)
{
    int32_t s[DMR_RPT_HIST];

    for (unsigned i = 0; i < n; i++)
        s[i] = v[i];

    for (unsigned i = 1; i < n; i++) {
        int32_t x = s[i];
        unsigned j = i;

        while ((j > 0) && (s[j - 1] > x)) {
            s[j] = s[j - 1];
            j--;
        }
        s[j] = x;
    }

    return s[n / 2];
}

/* ---------------------------------------------------------- BS activation --- */

void dmrRpt_csbkDownlinkActivity(uint8_t info[DMR_CSBK_BITS],
                                 uint32_t bsAddress, uint32_t sourceId)
{
    uint8_t pdu[80];

    /*
     * Octet 0: LB = 1 (single block), PF = 0, CSBKO = 111000.  Octet 1: FID.
     * Then 16 reserved bits at zero, the base station address and ours.
     */
    pdu[0] = 1;                                  /* LB */
    pdu[1] = 0;                                  /* PF */
    for (int i = 0; i < 6; i++)                  /* CSBKO, MSB first */
        pdu[2 + i] = (uint8_t)((DMR_RPT_CSBKO_BS_DWN_ACT >> (5 - i)) & 1u);
    for (int i = 0; i < 8; i++)                  /* FID */
        pdu[8 + i] = (uint8_t)((DMR_RPT_FID_STANDARD >> (7 - i)) & 1u);
    for (int i = 0; i < 16; i++)                 /* reserved, zero */
        pdu[16 + i] = 0;
    for (int i = 0; i < 24; i++)                 /* BS address */
        pdu[32 + i] = (uint8_t)((bsAddress >> (23 - i)) & 1u);
    for (int i = 0; i < 24; i++)                 /* source address */
        pdu[56 + i] = (uint8_t)((sourceId >> (23 - i)) & 1u);

    for (int i = 0; i < 80; i++)
        info[i] = pdu[i];

    /* CRC over the 80 information bits, then the CSBK mask of annex B. */
    uint16_t crc = (uint16_t)(dmr_fec_crc16_append(pdu, 80) ^ CSBK_CRC_MASK);

    for (int i = 0; i < 16; i++)
        info[80 + i] = (uint8_t)((crc >> (15 - i)) & 1u);
}

bool dmrRpt_csbkParse(const uint8_t info[DMR_CSBK_BITS],
                      struct dmrRptCsbk *out)
{
    if ((info == NULL) || (out == NULL))
        return false;

    memset(out, 0, sizeof(*out));

    for (int i = 0; i < 6; i++)
        out->opcode = (uint8_t)((out->opcode << 1) | (info[2 + i] & 1u));
    for (int i = 0; i < 8; i++)
        out->fid = (uint8_t)((out->fid << 1) | (info[8 + i] & 1u));
    for (int i = 0; i < 24; i++)
        out->dst = (out->dst << 1) | (uint32_t)(info[32 + i] & 1u);
    for (int i = 0; i < 24; i++)
        out->src = (out->src << 1) | (uint32_t)(info[56 + i] & 1u);

    return true;
}

const char *dmrRpt_csbkName(uint8_t opcode)
{
    switch (opcode) {
        case DMR_RPT_CSBKO_BS_DWN_ACT:
            return "BS_Dwn_Act";
        case DMR_RPT_CSBKO_PREAMBLE:
            return "Preamble";
        default:
            return "CSBK";
    }
}

/* ------------------------------------------------------- timing recovery --- */

void dmrRpt_phaseReset(struct dmrRptPhase *p)
{
    if (p == NULL)
        return;

    memset(p, 0, sizeof(*p));
    p->ppm = 0;
    p->offset = 0;
    p->quality = 0;
    p->settled = false;
}

/**
 * Recompute the filtered phase, the drift fit and the settled flag.
 *
 * Two windows, because they answer two different questions:
 *
 *  - the fast one (the last DMR_RPT_HIST bursts, tens of milliseconds) says
 *    where the grid is *now*: median, outliers clipped, mean of what is left.
 *    That is the correction the transmitter applies.
 *  - the slow one is a baseline between the residual now and a reference kept
 *    for DMR_RPT_PPM_SPAN ms, which says how fast the grid runs.  One sample
 *    of 48 kHz over two seconds is 10 ppm, so a shorter baseline cannot say
 *    anything about a crystal.
 */
static void evaluate(struct dmrRptPhase *p)
{
    int32_t inl[DMR_RPT_HIST];
    unsigned n = 0;
    const int32_t med = median(p->resid, p->count);
    int64_t sum = 0;
    int64_t sumDt = 0;

    for (unsigned i = 0; i < p->count; i++) {
        if ((p->resid[i] - med <= DMR_RPT_OUTLIER)
            && (med - p->resid[i] <= DMR_RPT_OUTLIER)) {
            inl[n++] = p->resid[i];
            sum += p->resid[i];
            sumDt += p->dt[i];
        }
    }

    if (n == 0) {
        p->settled = false;
        return;
    }

    p->offset = (int16_t)(sum / (int32_t)n);

    const int32_t meanDt = (int32_t)(sumDt / (int32_t)n);

    /*
     * The newest measurement is the one the fast window is centred on: it is
     * the reference the baseline is measured from and the point where the
     * offset is true.
     */
    int32_t newestDt = p->dt[0];

    for (unsigned i = 1; i < p->count; i++) {
        if (p->dt[i] > newestDt)
            newestDt = p->dt[i];
    }

    if (!p->refValid) {
        p->refDt = newestDt;
        p->refResid = p->offset;
        p->refValid = true;
    } else if ((newestDt - p->refDt) >= DMR_RPT_PPM_SPAN) {
        const int32_t base = newestDt - p->refDt;

        p->slopeQ16 = (int32_t)((((int64_t)(p->offset - p->refResid)) << 16)
                                / base);
        /* samples per ms -> ppm: 1 ppm is 48 us/s, 4,8e-5 samples/ms */
        p->ppm = (int16_t)(((int64_t)p->slopeQ16 * 1000000)
                           / (DMR_RPT_SAMPLES_PER_MS << 16));

        if (base >= DMR_RPT_PPM_RENEW) {
            p->refDt = newestDt;
            p->refResid = p->offset;
        }
    }

    /* Intercept at the anchor: the offset is the residual at meanDt */
    p->intercept = (int16_t)(p->offset
                             - ((p->slopeQ16 * meanDt) >> 16));

    p->quality = (p->seen == 0) ? 0
                                : (uint8_t)((p->kept * 100u) / p->seen);

    /*
     * Settled: enough measurements, a tight spread, and a drift the standard
     * would recognise.  Anything else is a grid that has not converged, and
     * aligning to it would be worse than the free running start.
     */
    int32_t lo = inl[0];
    int32_t hi = inl[0];

    for (unsigned i = 1; i < n; i++) {
        if (inl[i] < lo)
            lo = inl[i];
        if (inl[i] > hi)
            hi = inl[i];
    }

    const int32_t spread = (hi - lo < 0) ? (lo - hi) : (hi - lo);
    const int32_t drift = (p->ppm < 0) ? -p->ppm : p->ppm;

    p->settled = (n >= DMR_RPT_SETTLE_MIN) && (spread <= DMR_RPT_SETTLE_SPREAD)
              && (drift <= DMR_RPT_PPM_MAX);
}

enum dmrRptFeed dmrRpt_phaseFeed(struct dmrRptPhase *p, int32_t ts1Sample,
                                 uint32_t tickMs)
{
    if (p == NULL)
        return DMR_RPT_FEED_OUTLIER;

    p->seen++;

    if (p->count == 0) {
        p->t0 = (int32_t)tickMs;
        p->p0 = ts1Sample;
        p->count = 1;
        p->head = 1;
        p->resid[0] = 0;
        p->dt[0] = 0;
        p->kept++;
        p->run = 0;
        p->refValid = false;
        p->slopeQ16 = 0;
        p->ppm = 0;
        evaluate(p);
        return DMR_RPT_FEED_ACCEPTED;
    }

    const int32_t dt = (int32_t)(tickMs - (uint32_t)p->t0);
    const int32_t predicted = p->p0 + DMR_RPT_SAMPLES_PER_MS * dt;
    const int32_t err = wrapFrame(ts1Sample - predicted);

    /*
     * Outlier rejection against the median of what is already accepted: a
     * burst mis-decoded on the wrong slot (or a sync found in the other
     * timeslot) lands hundreds of samples away and must not move the grid.
     */
    if (p->count >= 3) {
        const int32_t med = median(p->resid, p->count);

        if ((err - med > DMR_RPT_OUTLIER) || (med - err > DMR_RPT_OUTLIER)) {
            if (++p->run >= DMR_RPT_RESTART) {
                /* not a glitch: the grid moved, start over from this burst */
                dmrRpt_phaseReset(p);
                return dmrRpt_phaseFeed(p, ts1Sample, tickMs);
            }
            evaluate(p);
            return DMR_RPT_FEED_OUTLIER;
        }
    }

    p->run = 0;
    p->resid[p->head] = err;
    p->dt[p->head] = dt;
    p->head = (uint8_t)((p->head + 1) % DMR_RPT_HIST);
    if (p->count < DMR_RPT_HIST)
        p->count++;
    p->kept++;

    evaluate(p);
    return DMR_RPT_FEED_ACCEPTED;
}

int32_t dmrRpt_phaseAt(const struct dmrRptPhase *p, uint32_t tickMs)
{
    if ((p == NULL) || (p->count == 0))
        return -1;

    const int64_t dt = (int64_t)(int32_t)(tickMs - (uint32_t)p->t0);

    int64_t v = (int64_t)p->p0 + (int64_t)DMR_RPT_SAMPLES_PER_MS * dt;
    v += p->intercept + ((int64_t)p->slopeQ16 * dt >> 16);

    return modFrame((int32_t)(v % (int64_t)DMR_RPT_FRAME_SAMPLES));
}

bool dmrRpt_phaseSettled(const struct dmrRptPhase *p)
{
    return (p != NULL) && p->settled;
}

/* ------------------------------------------------------------ start plan --- */

void dmrRpt_plan(const struct dmrRptPhase *p, uint8_t timeslot,
                 uint32_t nowTick, int32_t latencySamples, uint8_t leadSlots,
                 struct dmrRptPlan *out)
{
    if (out == NULL)
        return;

    memset(out, 0, sizeof(*out));

    /* whole frames of lead before anything else: they keep the slot parity */
    out->leadPairs = (uint8_t)((leadSlots + 1) / 2);

    const bool aligned = dmrRpt_phaseSettled(p);
    out->aligned = aligned;

    if (!aligned) {
        /* Free running: TS1 is the first rendered slot, TS2 the second */
        out->burstFirst = (timeslot == 2) ? 2 : 1;
        out->shift = 0;
        return;
    }

    int32_t ts1 = dmrRpt_phaseAt(p, nowTick);
    if (ts1 < 0) {
        out->aligned = false;
        out->burstFirst = (timeslot == 2) ? 2 : 1;
        return;
    }

    /*
     * Where the slot we want starts, counted from the moment the stream runs.
     * The baseband reaches the air `latencySamples` after it is written, so the
     * waveform has to be written exactly that much earlier: at the air instant
     * the boundary we measured is `latencySamples` further on.
     */
    int32_t target = ts1 - latencySamples
                   + ((timeslot == 2) ? DMR_RPT_SLOT_SAMPLES : 0);

    /* extra whole frames of lead (PA settling) do not change the remainder */
    target -= (int32_t)out->leadPairs * 2 * DMR_RPT_SLOT_SAMPLES;

    const int32_t off = modFrame(target);

    out->shift = (uint16_t)(off % DMR_RPT_SLOT_SAMPLES);
    out->burstFirst = (off >= DMR_RPT_SLOT_SAMPLES) ? 2 : 1;
}

bool dmrRpt_fits(int32_t latencySpreadSamples, int32_t overMs, int16_t ppm,
                 int32_t *marginOut)
{
    int32_t drift = (ppm < 0) ? -ppm : ppm;
    int32_t margin = DMR_RPT_SLOT_MARGIN - latencySpreadSamples;

    /* the drift of our clock against the repeater's, over the transmission */
    margin -= (int32_t)(((int64_t)drift * (int64_t)overMs)
                        * DMR_RPT_SAMPLES_PER_MS / 1000000);

    if (marginOut != NULL)
        *marginOut = margin;

    return margin > 0;
}

/* --------------------------------------------------------- baseband shift --- */

void dmrRpt_txShift(const int16_t *prev, const int16_t *cur, unsigned shift,
                    int16_t *out, unsigned n)
{
    if ((out == NULL) || (n == 0))
        return;

    if (shift == 0) {
        memcpy(out, cur, n * sizeof(int16_t));
        return;
    }

    if (shift >= n) {
        memcpy(out, prev, n * sizeof(int16_t));
        return;
    }

    memcpy(out, prev + (n - shift), shift * sizeof(int16_t));
    memcpy(out + shift, cur, (n - shift) * sizeof(int16_t));
}

int dmrRpt_txShiftStep(int current, int target, int maxStep)
{
    if (maxStep <= 0)
        return current;

    if (target > current + maxStep)
        return current + maxStep;
    if (target < current - maxStep)
        return current - maxStep;

    return target;
}

/* ---------------------------------------------------------- the wake FSM --- */

void dmrRptWake_start(struct dmrRptWake *w, uint8_t retries, uint32_t tickMs)
{
    if (w == NULL)
        return;

    w->stage = DMR_RPT_WAKE_TX;
    w->attempts = 0;
    w->retries = retries;
    w->deadline = tickMs + DMR_RPT_WAKE_TX_MS;
    w->answered = false;
    w->pending = true;
}

void dmrRptWake_skip(struct dmrRptWake *w)
{
    if (w == NULL)
        return;

    w->stage = DMR_RPT_WAKE_DONE;
    w->answered = true;
}

bool dmrRptWake_active(const struct dmrRptWake *w)
{
    return (w != NULL) && (w->stage != DMR_RPT_WAKE_IDLE)
        && (w->stage != DMR_RPT_WAKE_DONE);
}

void dmrRptWake_sent(struct dmrRptWake *w, uint32_t tickMs)
{
    if (w == NULL)
        return;

    w->attempts++;
    w->stage = DMR_RPT_WAKE_LISTEN;
    w->deadline = tickMs + DMR_RPT_WAKE_WAIT_MS;
    w->pending = false;
}

void dmrRptWake_answer(struct dmrRptWake *w)
{
    if (w == NULL)
        return;

    w->answered = true;
}

enum dmrRptWakeAction dmrRptWake_next(struct dmrRptWake *w, uint32_t tickMs,
                                      enum dmrRptNoAnswer policy)
{
    if ((w == NULL) || (w->stage == DMR_RPT_WAKE_IDLE)
        || (w->stage == DMR_RPT_WAKE_DONE))
        return DMR_RPT_WAKE_NONE;

    if (w->stage == DMR_RPT_WAKE_TX) {
        /*
         * The burst is due: hand it over before waiting on it.  The deadline
         * of this stage is only a guard for a caller that never reports it on
         * the air -- a transmitter that failed to key would otherwise park the
         * machine here for ever.
         */
        if (w->pending) {
            w->pending = false;
            return DMR_RPT_WAKE_SEND;
        }

        if ((int32_t)(tickMs - w->deadline) > 0) {
            w->stage = DMR_RPT_WAKE_LISTEN;
            w->attempts++;
            w->deadline = tickMs + DMR_RPT_WAKE_WAIT_MS;
        }
        return DMR_RPT_WAKE_NONE;
    }

    /* LISTEN */
    if (w->answered) {
        w->stage = DMR_RPT_WAKE_DONE;
        return DMR_RPT_WAKE_CALL;
    }

    if ((int32_t)(tickMs - w->deadline) <= 0)
        return DMR_RPT_WAKE_NONE;

    if (w->attempts <= w->retries) {
        w->stage = DMR_RPT_WAKE_TX;
        w->deadline = tickMs + DMR_RPT_WAKE_TX_MS;
        w->pending = true;
        return DMR_RPT_WAKE_SEND;
    }

    w->stage = DMR_RPT_WAKE_DONE;
    return (policy == DMR_RPT_NOANSWER_ABORT) ? DMR_RPT_WAKE_ABORT
                                              : DMR_RPT_WAKE_FREERUN;
}

/* ---------------------------------------------------------------- the gate --- */

enum dmrRptGate dmrRpt_gate(const struct dmrRptChannel *ch,
                            const struct dmrRptSeen *seen)
{
    if ((ch == NULL) || (seen == NULL))
        return DMR_RPT_GATE_OK;

    /*
     * Colour code first: a channel that speaks another colour code is another
     * repeater (or the same one on another site) and transmitting at it is
     * interference.  TS 102 361-1 clause 5.2.1.6, "polite to own colour code".
     */
    if ((seen->colourCode != DMR_RPT_CC_UNKNOWN)
        && (seen->colourCode != ch->colourCode))
        return DMR_RPT_GATE_CC;

    /* The timeslot, when the channel fixes one */
    if ((ch->timeslot != 0) && (seen->timeslot != 0)
        && (seen->timeslot != ch->timeslot))
        return DMR_RPT_GATE_TS;

    /*
     * Politeness: the CACH access type of the last burst in our slot.  Only
     * when the receiver actually decoded a CACH -- an unknown access type is
     * not a busy channel.
     */
    if (ch->polite && seen->busy)
        return DMR_RPT_GATE_BUSY;

    return DMR_RPT_GATE_OK;
}

const char *dmrRpt_gateName(enum dmrRptGate gate)
{
    switch (gate) {
        case DMR_RPT_GATE_OK:
            return "ok";
        case DMR_RPT_GATE_CC:
            return "cc";
        case DMR_RPT_GATE_TS:
            return "ts";
        case DMR_RPT_GATE_BUSY:
            return "busy";
    }

    return "?";
}

/* ------------------------------------------------------- parrot calibration --- */

/*
 * One pass, five attempts, one result.  The machine only sequences and
 * aggregates: the caller transmits and the caller stores, and a cancel wins
 * over everything, which is what keeps a calibration from leaving the channel
 * occupied when the operator changes his mind.
 */

/* The five attempts, staggered around zero by half a millisecond (24 samples) */
static const int16_t calOffsets[DMR_RPT_CAL_TRIES] = { 0, -24, 24, -48, 48 };

int16_t dmrRptCal_offset(const struct dmrRptCal *c)
{
    if (c == NULL)
        return 0;

    const unsigned step = (c->step < DMR_RPT_CAL_TRIES) ? c->step
                                                        : DMR_RPT_CAL_TRIES - 1;

    return calOffsets[step];
}

void dmrRptCal_start(struct dmrRptCal *c, uint32_t tickMs)
{
    if (c == NULL)
        return;

    memset(c, 0, sizeof(*c));
    c->stage = DMR_RPT_CAL_TX;
    c->step = 0;
    c->started = tickMs;
    c->deadline = tickMs + DMR_RPT_WAKE_TX_MS;
    c->pending = true;
}

void dmrRptCal_cancel(struct dmrRptCal *c)
{
    if (c == NULL)
        return;

    /*
     * A request, not a jump: the machine reports the abort on the next step so
     * the caller stops transmitting on the same path it always does.
     */
    c->cancelled = true;
}

bool dmrRptCal_active(const struct dmrRptCal *c)
{
    return (c != NULL) && ((c->stage == DMR_RPT_CAL_TX)
                           || (c->stage == DMR_RPT_CAL_LISTEN));
}

void dmrRptCal_sent(struct dmrRptCal *c, uint32_t tickMs,
                    uint32_t expectedSample)
{
    if (c == NULL)
        return;

    c->stage = DMR_RPT_CAL_LISTEN;
    c->deadline = tickMs + DMR_RPT_CAL_LISTEN_MS;
    c->expected = expectedSample;
    c->pending = false;
}

void dmrRptCal_heard(struct dmrRptCal *c, uint32_t slotSample, bool crcOk)
{
    if ((c == NULL) || (c->stage != DMR_RPT_CAL_LISTEN)
        || (c->step >= DMR_RPT_CAL_TRIES))
        return;

    struct dmrRptCalStep *s = &c->steps[c->step];

    s->heard = true;
    if (s->bursts < 255)
        s->bursts++;
    if (crcOk && (s->crcOk < 255))
        s->crcOk++;

    /*
     * The echo delay is what the attempt measures when the repeater relays
     * with a fixed turnaround; on a parrot it comes out as the slot structure
     * of the box and is reported as such, not used.
     */
    if (c->expected != 0) {
        /* sub-slot residual: how far the echo sits from the grid of the box */
        const int32_t delay = wrapFrame((int32_t)(slotSample - c->expected));

        s->echoDelay = (int16_t)((delay > 32767) ? 32767
                                                 : ((delay < -32768) ? -32768
                                                                     : delay));
    }
}

enum dmrRptCalAction dmrRptCal_next(struct dmrRptCal *c, uint32_t tickMs)
{
    if ((c == NULL) || (c->stage == DMR_RPT_CAL_IDLE)
        || (c->stage == DMR_RPT_CAL_DONE))
        return DMR_RPT_CAL_NONE;

    if (c->cancelled) {
        c->stage = DMR_RPT_CAL_DONE;
        c->pending = false;
        return DMR_RPT_CAL_ABORT;
    }

    /* The attempt is due: hand it to the caller before waiting on it */
    if ((c->stage == DMR_RPT_CAL_TX) && c->pending) {
        c->pending = false;
        c->steps[c->step].offset = calOffsets[c->step];
        return DMR_RPT_CAL_SEND;
    }

    /* Hard limit: the pass may not hold the channel for ever */
    if ((int32_t)(tickMs - c->started) > DMR_RPT_CAL_MAX_MS) {
        c->stage = DMR_RPT_CAL_DONE;
        return DMR_RPT_CAL_ABORT;
    }

    if ((int32_t)(tickMs - c->deadline) <= 0)
        return DMR_RPT_CAL_NONE;

    /* This attempt is over, whatever it heard: move to the next one */
    c->step++;

    if (c->step >= DMR_RPT_CAL_TRIES) {
        c->stage = DMR_RPT_CAL_DONE;
        return DMR_RPT_CAL_FINISH;
    }

    c->stage = DMR_RPT_CAL_TX;
    c->deadline = tickMs + DMR_RPT_WAKE_TX_MS;
    c->pending = true;
    return DMR_RPT_CAL_NONE;
}

bool dmrRptCal_result(const struct dmrRptCal *c, struct dmrRptCalResult *out)
{
    if ((c == NULL) || (out == NULL) || (c->stage != DMR_RPT_CAL_DONE))
        return false;

    memset(out, 0, sizeof(*out));

    /*
     * Quality decides which attempts count: the best CRC-ok count of the pass
     * is the yardstick and an attempt that does not reach it is what "came
     * back badly" means.  Without any CRC (a repeater that returns voice
     * bursts of its own) the number of bursts that decoded is the yardstick.
     */
    uint8_t bestCrc = 0;
    uint8_t bestBursts = 0;

    for (unsigned i = 0; i < DMR_RPT_CAL_TRIES; i++) {
        if (!c->steps[i].heard)
            continue;
        if (c->steps[i].crcOk > bestCrc)
            bestCrc = c->steps[i].crcOk;
        if (c->steps[i].bursts > bestBursts)
            bestBursts = c->steps[i].bursts;
    }

    int16_t valid[DMR_RPT_CAL_TRIES];
    unsigned n = 0;

    for (unsigned i = 0; i < DMR_RPT_CAL_TRIES; i++) {
        const struct dmrRptCalStep *s = &c->steps[i];

        if (!s->heard)
            continue;
        /*
         * "Came back well" is at least half of the best quality of the pass:
         * requiring equality would throw away every attempt but one as soon as
         * a single offset sits in the sweet spot, which is exactly the attempt
         * a median needs company for.
         */
        if ((bestCrc > 0) && ((unsigned)s->crcOk * 2u < bestCrc))
            continue;
        if ((bestCrc == 0) && ((unsigned)s->bursts * 2u < bestBursts))
            continue;

        valid[n++] = s->offset;
    }

    out->valid = (uint8_t)n;

    if (n < DMR_RPT_CAL_MIN_VALID)
        return true; /* no median to speak of: result->store stays false */

    /* insertion sort: five samples, and the median has to be the real one */
    for (unsigned i = 1; i < n; i++) {
        const int16_t x = valid[i];
        unsigned j = i;

        while ((j > 0) && (valid[j - 1] > x)) {
            valid[j] = valid[j - 1];
            j--;
        }
        valid[j] = x;
    }

    /* median: the middle one, or the mean of the two middle ones */
    out->offset = (n & 1) ? valid[n / 2]
                          : (int16_t)(((int32_t)valid[n / 2 - 1]
                                       + (int32_t)valid[n / 2]) / 2);
    out->spread = (int16_t)(valid[n - 1] - valid[0]);
    out->store = true;

    return true;
}

int dmrRptCal_format(const struct dmrRptCal *c, char *line, size_t len)
{
    if ((c == NULL) || (line == NULL) || (len == 0))
        return 0;

    int n = 0;

    for (unsigned i = 0; (i <= c->step) && (i < DMR_RPT_CAL_TRIES); i++) {
        const struct dmrRptCalStep *s = &c->steps[i];
        const int w = snprintf(line + n, (n < (int)len) ? len - (size_t)n : 0,
                               "%u:%d/%d/%u/%u%s ", i, (int)s->offset,
                               (int)s->echoDelay, (unsigned)s->bursts,
                               (unsigned)s->crcOk, s->heard ? "" : "-");

        if (w < 0)
            break;
        n += w;
        if ((size_t)n >= len)
            break;
    }

    return n;
}

/* ---------------------------------------------------------------- trace --- */

int dmrRpt_format(const struct dmrRptPhase *p, const struct dmrRptPlan *plan,
                  char *buf, size_t len)
{
    if ((buf == NULL) || (len == 0))
        return 0;

    if ((p == NULL) || (p->count == 0))
        return snprintf(buf, len, "none");

    int n = snprintf(buf, len, "ppm=%d off=%d q=%u n=%u%s", (int)p->ppm,
                     (int)p->offset, (unsigned)p->quality,
                     (unsigned)p->count, p->settled ? " ok" : " raw");

    if ((plan != NULL) && (plan->aligned) && (n > 0) && ((size_t)n < len)) {
        n += snprintf(buf + n, len - (size_t)n, " sh=%u%s", (unsigned)plan->shift,
                      (plan->burstFirst == 2) ? " s2" : " s1");
    }

    return n;
}
