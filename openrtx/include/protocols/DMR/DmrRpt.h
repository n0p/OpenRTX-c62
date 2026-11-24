/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef OPENRTX_DMR_RPT_H
#define OPENRTX_DMR_RPT_H

/**
 * @file DmrRpt.h
 * @brief Repeater operation of the DMR transmitter: BS activation, downlink
 * timing recovery and the colour-code / timeslot gate.
 *
 * Everything here is a pure function or a small state machine that takes the
 * time as an argument: no radio, no audio stream, no clock.  The mode calls it
 * from the rtx thread and the host tests walk every path, in the same spirit as
 * DmrTxFsm.h.
 *
 * Three pieces, from ETSI TS 102 361-1/-2:
 *
 *  - **BS activation** (TS 102 361-2 clause 5.1.1.1 and 7.1.2.1): a repeater
 *    that is not transmitting is hibernating and hears nothing of a call.  A
 *    mobile station that wants to use it sends one BS_Dwn_Act CSBK ("BS
 *    outbound activation") with an MS sourced sync, then waits for the
 *    outbound channel to come up and derives its timing from it (TS 102 361-1
 *    clause 4.4.1).  dmrRpt_csbkDownlinkActivity() builds the PDU and the
 *    wake machine below runs the retries.
 *
 *  - **Downlink timing** (TS 102 361-1 clauses 4.4.1 and 5.2.1.3): on a two
 *    frequency channel the base station is the timing master.  Every decoded
 *    outbound burst tells us where the start of its timeslot fell in our own
 *    sample clock, and the CACH says which of the two slots it was, so the
 *    start of TS1 of the 60 ms frame is measured once per burst.  The tracker
 *    filters those measurements (median, outlier rejection), fits the drift in
 *    ppm and hands out the sub-slot offset the transmitter has to delay its
 *    baseband by to land inside the repeater's slot.
 *
 *  - **Colour code and timeslot gate**: a repeater whose colour code is not
 *    the channel's is not our repeater, and transmitting at it is interference
 *    (TS 102 361-1 clause 5.2.1.6, "polite to own colour code").  Same for the
 *    timeslot when the channel fixes one.
 *
 * What this cannot do, and is documented in docs/DMR_REPEATER.md: the radio is
 * half duplex, so the frame phase can only be measured while receiving and is
 * extrapolated over the switch; the delay from the baseband stream to the air
 * is a constant of the audio path that has to be calibrated once on the bench
 * (DMR_RPT_TX_LATENCY) and is not observable from inside the firmware.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocols/DMR/DmrDefs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- geometry --- */

/** Samples of one 30 ms timeslot at 48 kHz. */
#define DMR_RPT_SLOT_SAMPLES 1440

/** Samples of one 60 ms TDMA frame (two timeslots) at 48 kHz. */
#define DMR_RPT_FRAME_SAMPLES 2880

/** Nominal samples per millisecond at 48 kHz: the slope a perfectly on
 *  frequency repeater presents to the phase tracker. */
#define DMR_RPT_SAMPLES_PER_MS 48

/**
 * The audio path delay, and why it is a setting and not a constant.
 *
 * Between the slot the modulator writes and the slot that leaves the antenna
 * there are two delays, and only one of them is the firmware's:
 *
 *  - **software**, deterministic and known here: the lead slots the plan asks
 *    for and the sample delay (`shift`) the plan computes, both counted in
 *    samples of the 48 kHz baseband stream.  Nothing in this file assumes
 *    anything else about them;
 *  - **hardware**, which has to be measured: the double buffered output
 *    stream, the codec FIFO, the DSP hop and its resampler.  On the C62 the
 *    DSP is a *replaced image* (the stock one runs at 16 kHz, the LSF one this
 *    firmware needs at 48 kHz), so the stock figure does not apply and the
 *    number has to come from a measurement on the radio at hand.
 *
 * The measured value lives in the settings (`audio_latency_us`) and reaches
 * this file as samples.  A wrong value moves every burst of a call by exactly
 * the error, so dmrRpt_fits() is what the caller uses to refuse to transmit
 * aligned when the uncertainty does not leave room.
 */
/** Hardware part of the path, default when nothing has been measured yet.
 *  It is a placeholder with an honest uncertainty, not a figure to trust: the
 *  first thing a radio should do before working a repeater is measure it
 *  (docs/DMR_REPEATER.md) or run the parrot calibration. */
#define DMR_RPT_HW_LATENCY_US_DEFAULT 5000

/** Uncertainty of that placeholder, microseconds: what makes the plan refuse
 *  to pretend it is aligned. */
#define DMR_RPT_HW_LATENCY_SPREAD_US_DEFAULT 2000

/** Range a stored measurement is clamped to: a settings record from another
 *  radio, or a corrupted one, must not turn into a delay of seconds. */
#define DMR_RPT_HW_LATENCY_US_MAX 100000

/**
 * Software part of the path, in samples of the 48 kHz baseband stream.
 *
 * The output stream is double buffered: it hands the writer one slot while the
 * other one plays, so a slot written now starts leaving the DSP one slot
 * later.  That is exact and it is the firmware's own doing -- the only other
 * component of the delay is the hardware, which is the measured setting.  The
 * caller adds the two: total = DMR_RPT_STREAM_LATENCY + us_to_samples(hw).
 */
#define DMR_RPT_STREAM_LATENCY 1440

/** Microseconds to samples at 48 kHz: 48 samples per millisecond. */
#define DMR_RPT_US_TO_SAMPLES(us) (((int32_t)(us) * 48) / 1000)

/** Samples back to microseconds. */
#define DMR_RPT_SAMPLES_TO_US(s) (((int32_t)(s) * 1000) / 48)

/**
 * Room a burst has inside the slot of the far end, samples.
 *
 * 48 samples are 1 ms: the allowance TS 102 361-1 clause 10.2.3.1.3 gives to
 * propagation and clock drift before slots start to interfere, and the budget
 * the delay uncertainty has to fit in together with the drift over the
 * transmission.
 */
#define DMR_RPT_SLOT_MARGIN 48

/* --------------------------------------------------------- BS activation --- */

/** CSBKO of BS_Dwn_Act, TS 102 361-2 table B.2: 111000. */
#define DMR_RPT_CSBKO_BS_DWN_ACT 0x38
/** CSBKO of the preamble CSBK, for reference: 111101. */
#define DMR_RPT_CSBKO_PREAMBLE 0x3D
/** Feature set ID of the standard feature set. */
#define DMR_RPT_FID_STANDARD 0x00
/** Address wildcard: a BS_Dwn_Act can be addressed to "any base station". */
#define DMR_RPT_ADDR_ANY 0xFFFFFF

/** A parsed control signalling block. */
struct dmrRptCsbk {
    uint8_t opcode;  ///< CSBKO, 6 bits
    uint8_t fid;     ///< Feature set ID
    uint32_t dst;    ///< Destination (BS) address, 24 bits
    uint32_t src;    ///< Source address, 24 bits
};

/**
 * Build the 96 information bits of a BS_Dwn_Act CSBK, ready for
 * dmrTx_dataSlot() with DMR_DT_CSBK.
 *
 * The PDU is 80 bits of content plus a 16 bit CRC (TS 102 361-2 table 7.5a):
 * LB = 1, PF = 0, CSBKO = 111000, FID = 0, 16 reserved bits at zero, the
 * 24 bit base station address and the 24 bit source address.  The CRC is
 * computed over the 96 bits and then XORed with the CSBK mask of
 * TS 102 361-1 annex B (A5A5), which is what the receiver undoes before it
 * checks the syndrome.
 *
 * @param info: destination, DMR_CSBK_BITS bits, one per element.
 * @param bsAddress: base station address, DMR_RPT_ADDR_ANY for "any".
 * @param sourceId: our radio ID, 24 bits.
 */
void dmrRpt_csbkDownlinkActivity(uint8_t info[DMR_CSBK_BITS],
                                 uint32_t bsAddress, uint32_t sourceId);

/**
 * Parse the 96 information bits of a received CSBK.
 *
 * @param info: 96 bits, one per element, as the data burst decoder returns
 *              them (burst->lc, MSB first).
 * @param out: destination.
 * @return false if the block is shorter than a CSBK header.
 */
bool dmrRpt_csbkParse(const uint8_t info[DMR_CSBK_BITS],
                      struct dmrRptCsbk *out);

/**
 * @return a short name for a CSBKO, for traces.
 */
const char *dmrRpt_csbkName(uint8_t opcode);

/* -------------------------------------------------------- timing recovery --- */

/** Measurements kept for the filter: 8 bursts are 480 ms of a busy slot. */
#define DMR_RPT_HIST 8

/** An accepted measurement this far from the median is an outlier, samples.
 *  240 samples are 5 ms, four fifths of a timeslot of guard: no real timing
 *  step lands there by noise (the jitter of a decoded sync is +-2 samples). */
#define DMR_RPT_OUTLIER 240

/** Consecutive outliers that mean the grid moved rather than the burst being
 *  mis-decoded: the tracker restarts from scratch. */
#define DMR_RPT_RESTART 3

/** Measurements needed before the estimate is used to align a call. */
#define DMR_RPT_SETTLE_MIN 5

/** Spread of the accepted residuals a settled grid is allowed, samples.
 *  24 samples are 0.5 ms: a quarter of the 2.5 ms guard of the inbound
 *  channel, which is the budget the burst has to fit in. */
#define DMR_RPT_SETTLE_SPREAD 24

/** Drift past which the grid is not used: the MS clock tolerance of
 *  TS 102 361-1 clause 10.1.4 is +-2 ppm and the repeater's is the same order,
 *  so 25 ppm is a broken reference (a free running codec, a capture played
 *  back).  Aligning to it would be worse than not aligning. */
#define DMR_RPT_PPM_MAX 25

/** Baseline the drift is fitted over, ms.  A measurement is one sample of
 *  48 kHz, so two seconds of baseline resolve 1 ppm to about half a sample;
 *  the fit starts once the baseline is that long and reports 0 before. */
#define DMR_RPT_PPM_SPAN 2000

/** Baseline after which the drift fit starts again from the newest burst: the
 *  tracker follows a repeater that wanders instead of averaging its whole
 *  history. */
#define DMR_RPT_PPM_RENEW 8000

/** Outcome of one measurement fed to the tracker. */
enum dmrRptFeed {
    DMR_RPT_FEED_ACCEPTED = 0, ///< Used in the estimate
    DMR_RPT_FEED_OUTLIER,      ///< Rejected, the estimate is unchanged
    DMR_RPT_FEED_RESTART       ///< Too many in a row: the tracker started over
};

/**
 * Downlink frame phase and drift.
 *
 * The model is a straight line through the anchor: the start of TS1 of the
 * repeater's frame, in 48 kHz samples of our own receive stream, is
 *
 *     p(t) = p0 + 48 (t - t0) + intercept + slope (t - t0)
 *
 * @c offset is what the fast filter says the residual is right now (the
 * median-clipped mean of the last bursts), @c slope the drift fitted over the
 * long baseline and @c ppm that slope in parts per million.  The anchor is
 * never moved: the residuals are relative to it, which is what lets the drift
 * be fitted over seconds while the phase is filtered over tens of
 * milliseconds.  A tracker left running for minutes without a residual wrap
 * is the only thing that would need a reset, and a residual wrap is what the
 * outlier filter reports anyway.
 */
struct dmrRptPhase {
    int32_t t0;        ///< Anchor: tick, ms
    int32_t p0;        ///< Anchor: TS1 start, 48 kHz samples
    int32_t resid[DMR_RPT_HIST]; ///< Accepted residuals, samples
    int32_t dt[DMR_RPT_HIST];    ///< ... and their distance from t0, ms
    uint8_t count;     ///< Entries in the ring
    uint8_t head;      ///< Next slot of the ring
    uint8_t run;       ///< Consecutive outliers
    uint16_t seen;     ///< Measurements offered
    uint16_t kept;     ///< ... accepted
    int32_t refDt;     ///< Drift fit: baseline start, ms after t0
    int32_t refResid;  ///< ... and the filtered residual there
    bool refValid;     ///< The baseline exists
    int32_t slopeQ16;  ///< Fitted drift, Q16 samples per ms
    int16_t intercept; ///< Residual at t0 implied by the fit, samples
    int16_t ppm;       ///< Fitted drift, parts per million
    int16_t offset;    ///< Filtered sub-slot correction, samples
    uint8_t quality;   ///< Accepted / offered over the window, 0..100
    bool settled;      ///< Enough measurements and spread for alignment
};

/** Clear the tracker (new channel, lost sync). */
void dmrRpt_phaseReset(struct dmrRptPhase *p);

/**
 * Feed one measurement: the position of the start of TS1 of the 60 ms frame,
 * as the receiver saw it.
 *
 * @param p: tracker.
 * @param ts1Sample: 48 kHz sample index of the TS1 boundary, in the receive
 *                   stream's own numbering (absolute, any value: it is only
 *                   used as a difference).
 * @param tickMs: the tick at which that burst was decoded.
 * @return what happened to the measurement.
 */
enum dmrRptFeed dmrRpt_phaseFeed(struct dmrRptPhase *p, int32_t ts1Sample,
                                 uint32_t tickMs);

/**
 * @return the predicted position of the TS1 boundary at @p tickMs, in samples
 *         modulo one 60 ms frame (0..2879), or -1 if there is no estimate.
 */
int32_t dmrRpt_phaseAt(const struct dmrRptPhase *p, uint32_t tickMs);

/** @return true if the estimate can be used to align a call. */
bool dmrRpt_phaseSettled(const struct dmrRptPhase *p);

/* ------------------------------------------------------------ start plan --- */

/** What the transmitter has to do to land in the repeater's slot. */
struct dmrRptPlan {
    uint16_t shift;      ///< Extra delay of the baseband, samples (0..1439)
    uint8_t burstFirst;  ///< 1: first rendered slot carries the burst, 2: second
    uint8_t leadPairs;   ///< Extra 60 ms frames of lead (PA settling)
    bool aligned;        ///< false: free running, no estimate or not settled
};

/**
 * Plan the start of a call from the current phase estimate.
 *
 * The transmitter's slot clock counts from the moment the PA is keyed and the
 * first slot goes into the stream; the repeater's counts from its own frame.
 * Two knobs bring them together: the number of whole slots of lead (which
 * selects the slot parity, @c burstFirst) and a delay in samples applied to
 * the baseband (@c shift).  The remainder is exact to one sample as long as
 * the estimate is settled; otherwise the plan is the free running one (TS1 or
 * TS2 counted from keying) and @c aligned is false.
 *
 * @param p: phase tracker.
 * @param timeslot: 1 or 2, the slot the channel asks for.
 * @param nowTick: tick of the moment the plan is made (keying).
 * @param latencySamples: DMR_RPT_TX_LATENCY, or the calibrated value.
 * @param leadSlots: slots of lead the caller wants before the first burst for
 *                   the PA to settle; rounded up to a whole frame here, because
 *                   half of one would change the slot parity.
 * @param out: destination.
 */
void dmrRpt_plan(const struct dmrRptPhase *p, uint8_t timeslot,
                 uint32_t nowTick, int32_t latencySamples, uint8_t leadSlots,
                 struct dmrRptPlan *out);

/**
 * Does a burst placed with the given latency uncertainty still fit in the
 * slot of the far end?
 *
 * The budget is DMR_RPT_SLOT_MARGIN (1 ms) and what it has to hold is the
 * uncertainty of the measured latency -- the hardware path, from the audio
 * calibration -- plus the drift of our own clock against the repeater's over
 * the length of the transmission.  A caller that finds no room must say so and
 * transmit free running (or not at all) rather than key a burst that lands
 * outside the slot.
 *
 * @param latencySpreadSamples: half width of the latency measurement, samples
 * @param overMs: how long the transmission is going to last, ms
 * @param ppm: the drift estimate, parts per million
 * @param marginOut: optional, the samples of margin left (may be negative)
 * @return true if the burst fits.
 */
bool dmrRpt_fits(int32_t latencySpreadSamples, int32_t overMs, int16_t ppm,
                 int32_t *marginOut);

/* --------------------------------------------------------- baseband shift --- */

/**
 * Assemble one slot of the output stream, delaying the waveform by @p shift
 * samples.
 *
 * The modulator renders whole slots; a delay of part of a slot is built from
 * the previous rendered slot and the current one, which is why the caller has
 * to keep both.  @p shift = 0 is a plain copy.  The delay is constant for a
 * call (except for the one sample per slot of the drift slew), so the
 * waveform stays continuous.
 *
 * @param prev: the slot rendered before this one, DMR_TX_SLOT_SAMPLES.
 * @param cur: the slot rendered for this one.
 * @param shift: 0..1439 samples.
 * @param out: destination, DMR_TX_SLOT_SAMPLES samples.
 * @param n: samples per slot (DMR_TX_SLOT_SAMPLES on the radio).
 */
void dmrRpt_txShift(const int16_t *prev, const int16_t *cur, unsigned shift,
                    int16_t *out, unsigned n);

/**
 * Step a delay towards a target by at most @p maxStep samples.
 *
 * The estimated drift is paid back one sample at a time at slot boundaries: a
 * jump would be a phase discontinuity in the baseband, a sample per 30 ms is
 * a 21 µs slip nobody sees.
 */
int dmrRpt_txShiftStep(int current, int target, int maxStep);

/* ---------------------------------------------------------- the wake FSM --- */

/** Stages of the BS activation. */
enum dmrRptWakeStage {
    DMR_RPT_WAKE_IDLE = 0, ///< Nothing in progress
    DMR_RPT_WAKE_TX,       ///< The activation burst is being transmitted
    DMR_RPT_WAKE_LISTEN,   ///< Burst sent, waiting for the outbound channel
    DMR_RPT_WAKE_DONE      ///< Finished: the caller has its answer
};

/** What the caller has to do after dmrRptWake_next(). */
enum dmrRptWakeAction {
    DMR_RPT_WAKE_NONE = 0, ///< Nothing: keep receiving (still listening)
    DMR_RPT_WAKE_SEND,     ///< Key and transmit one BS_Dwn_Act burst
    DMR_RPT_WAKE_CALL,     ///< The repeater is up: start the call
    DMR_RPT_WAKE_FREERUN,  ///< No answer and the policy says so: call anyway
    DMR_RPT_WAKE_ABORT     ///< No answer: do not transmit at all
};

/** What to do when the repeater never answers. */
enum dmrRptNoAnswer {
    DMR_RPT_NOANSWER_FREERUN = 0, ///< Call with free running timing
    DMR_RPT_NOANSWER_ABORT   = 1  ///< Stay silent, tell the operator
};

/** How long to wait for the outbound channel after one activation burst, ms.
 *  A repeater that is up transmits within a few frames; 300 ms are five of
 *  them and still short next to the operator's patience. */
#define DMR_RPT_WAKE_WAIT_MS 300

/** How long the activation burst itself takes, ms: a slot of PA settle, the
 *  CSBK burst, a slot of tail. */
#define DMR_RPT_WAKE_TX_MS 150

/** Activation attempts before giving up: the first burst plus two retries. */
#define DMR_RPT_WAKE_RETRIES 2

struct dmrRptWake {
    enum dmrRptWakeStage stage; ///< Current stage
    uint8_t attempts;           ///< Bursts sent
    uint8_t retries;            ///< Retries allowed
    uint32_t deadline;          ///< Tick the current wait ends
    bool answered;              ///< The outbound channel came up
    bool pending;               ///< This attempt has not been handed over yet
};

/**
 * Start a wake sequence.
 *
 * @param w: machine.
 * @param retries: retries after the first burst (DMR_RPT_WAKE_RETRIES).
 * @param tickMs: now.
 */
void dmrRptWake_start(struct dmrRptWake *w, uint8_t retries, uint32_t tickMs);

/**
 * Skip the sequence: the repeater is already transmitting and the receiver is
 * locked to it (TS 102 361-1 clause 5.2.1.3: the activation is for a BS that
 * is not currently transmitting).
 */ 
void dmrRptWake_skip(struct dmrRptWake *w);

/** @return true while the machine is running (the mode is in the wake path). */
bool dmrRptWake_active(const struct dmrRptWake *w);

/** Account that the activation burst is on the air and start the wait. */
void dmrRptWake_sent(struct dmrRptWake *w, uint32_t tickMs);

/** Account that the outbound channel was seen. */
void dmrRptWake_answer(struct dmrRptWake *w);

/**
 * Advance the machine.
 *
 * @param w: machine.
 * @param tickMs: now.
 * @param policy: what to do if the repeater never answers.
 * @return what the caller has to do.
 */
enum dmrRptWakeAction dmrRptWake_next(struct dmrRptWake *w, uint32_t tickMs,
                                      enum dmrRptNoAnswer policy);

/* ------------------------------------------------------- parrot calibration --- */

/*
 * What a laboratory cannot do for every radio in the field, a repeater that
 * repeats can: the audio path delay of *this* unit -- DSP image included -- is
 * the number that makes its bursts fall inside the window of the far end, and
 * the only place where that is observable is the echo.
 *
 * The calibration is **one pass of five short transmissions**.  Each one goes
 * out at a different offset around the estimate and is followed by a pause in
 * which the echo is measured; the attempts that do not come back, or come back
 * badly, are discarded, and the median of the offsets that came back well is
 * the correction.  The median and not the last one, because a single good echo
 * is a coincidence and a repeater does not repeat identically twice.
 *
 * Why the offset changes between the transmissions: a repeater that re-times
 * its outbound (a parrot always does) puts the echo on its own slot grid
 * whatever we did, so the *arrival time of the echo* cannot measure our
 * alignment -- only its quality can.  What the echo proves is that our burst
 * fell inside the window of the far end, and a search over offsets with
 * quality as the criterion is what turns that proof into a number.  The echo
 * delay is still measured and reported: on a repeater that relays with a fixed
 * turnaround it is the alignment error itself, and on a parrot it is the
 * "nothing to see" answer that tells the operator which kind of box he has.
 *
 * The result goes into the same single persistent setting as the laboratory
 * measurement (`audio_latency_us`), so there is one number to trust and one
 * place to correct it.
 *
 * Safety, in the machine and not in the caller: five attempts, a hard limit of
 * DMR_RPT_CAL_MAX_MS of channel time, a cancel that wins over everything, and
 * no listening left running after the last attempt.
 */

/** Attempts of the calibration pass. */
#define DMR_RPT_CAL_TRIES 5

/** How long to listen for the echo after one attempt, ms. */
#define DMR_RPT_CAL_LISTEN_MS 400

/** Wall clock the whole pass may take before it gives up, ms: five attempts
 *  of a burst and a pause, with a wide margin. */
#define DMR_RPT_CAL_MAX_MS 12000

/** Valid measurements needed before anything is stored: with one echo there
 *  is no median and no reason to overwrite a calibrated value. */
#define DMR_RPT_CAL_MIN_VALID 2

/** Steps of the calibration. */
enum dmrRptCalStage {
    DMR_RPT_CAL_IDLE = 0, ///< Not running
    DMR_RPT_CAL_TX,       ///< Transmitting the short call of this attempt
    DMR_RPT_CAL_LISTEN,   ///< Waiting for its echo
    DMR_RPT_CAL_DONE      ///< Finished, the result is available
};

/** What the caller has to do after dmrRptCal_next(). */
enum dmrRptCalAction {
    DMR_RPT_CAL_NONE = 0, ///< Keep listening, or nothing to do
    DMR_RPT_CAL_SEND,     ///< Transmit the test call at calOffset()
    DMR_RPT_CAL_FINISH,   ///< Over: read dmrRptCal_result() and store it
    DMR_RPT_CAL_ABORT     ///< Cancelled or out of time: transmit nothing more
};

/** One attempt of the pass. */
struct dmrRptCalStep {
    int16_t offset;    ///< Extra delay this attempt applied, samples
    int16_t echoDelay; ///< Echo arrival minus the expected one, samples
    uint8_t bursts;    ///< Bursts of the echo that decoded
    uint8_t crcOk;     ///< ... of which data bursts with a good CRC
    bool heard;        ///< Anything came back at all
};

/** What a finished pass found. */
struct dmrRptCalResult {
    uint8_t valid;     ///< Attempts that came back well
    int16_t offset;    ///< Median of their offsets, samples
    int16_t spread;    ///< Highest minus lowest of them, samples
    bool store;        ///< True if the caller may store it
};

struct dmrRptCal {
    enum dmrRptCalStage stage;
    uint8_t step;       ///< Attempt in progress
    uint32_t started;   ///< Tick the pass started
    uint32_t deadline;  ///< Tick the current stage ends
    uint32_t expected;  ///< 48 kHz sample the echo should start at
    bool pending;       ///< This attempt has not been handed to the caller yet
    bool cancelled;     ///< The operator cancelled
    struct dmrRptCalStep steps[DMR_RPT_CAL_TRIES];
};

/**
 * @return the extra delay the attempt in progress uses, samples: the five
 *         attempts are staggered around zero, 0.5 ms apart (-1, -0.5, 0,
 *         +0.5, +1 ms).
 */
int16_t dmrRptCal_offset(const struct dmrRptCal *c);

/** Start a pass (the first attempt is due immediately). */
void dmrRptCal_start(struct dmrRptCal *c, uint32_t tickMs);

/** Cancel a pass: no more transmissions, the result will not be stored. */
void dmrRptCal_cancel(struct dmrRptCal *c);

/** @return true while the pass is running. */
bool dmrRptCal_active(const struct dmrRptCal *c);

/**
 * The test call is on the air: start listening for its echo.
 *
 * @param c: pass.
 * @param tickMs: now.
 * @param expectedSample: the 48 kHz sample of our receive stream at which the
 *        echo of the burst just sent should start, or 0 when the repeater's
 *        grid is not known and only quality can be measured.
 */
void dmrRptCal_sent(struct dmrRptCal *c, uint32_t tickMs,
                    uint32_t expectedSample);

/**
 * A burst of the echo arrived.
 *
 * @param c: pass.
 * @param slotSample: where the burst's timeslot started, receive stream.
 * @param crcOk: the burst was a data burst with a good CRC.
 */
void dmrRptCal_heard(struct dmrRptCal *c, uint32_t slotSample, bool crcOk);

/**
 * Advance the pass.
 *
 * @param c: pass.
 * @param tickMs: now.
 * @return what the caller has to do.
 */
enum dmrRptCalAction dmrRptCal_next(struct dmrRptCal *c, uint32_t tickMs);

/**
 * Result of a finished pass.
 *
 * @param c: pass.
 * @param out: destination.
 * @return true if the pass is over and there is a result (which is not the
 *         same as "store it": see result->store).
 */
bool dmrRptCal_result(const struct dmrRptCal *c, struct dmrRptCalResult *out);

/**
 * Format the pass for the monitor, one attempt per line as
 * "i:offset/echo/bursts/crc", space separated.
 *
 * @param c: pass.
 * @param line: destination.
 * @param len: size of line.
 * @return the number of characters written, as snprintf.
 */
int dmrRptCal_format(const struct dmrRptCal *c, char *line, size_t len);

/* ---------------------------------------------------------------- the gate --- */

/** Why a transmission is allowed or refused. */
enum dmrRptGate {
    DMR_RPT_GATE_OK = 0,   ///< Transmit
    DMR_RPT_GATE_CC,       ///< Colour code of the repeater is not the channel's
    DMR_RPT_GATE_TS,       ///< The repeater is on the other timeslot
    DMR_RPT_GATE_BUSY      ///< Our inbound slot is busy (CACH access type)
};

/** The channel, as far as the gate is concerned. */
struct dmrRptChannel {
    uint8_t colourCode; ///< Colour code of the channel, 0..15
    uint8_t timeslot;   ///< 1 or 2: fixed by the channel. 0: follow the repeater
    bool polite;        ///< Respect the CACH access type of our slot
};

/** What was last heard from the repeater. */
struct dmrRptSeen {
    uint8_t colourCode; ///< 0..15, or DMR_RPT_CC_UNKNOWN
    uint8_t timeslot;   ///< 1 or 2, or 0 for unknown
    bool busy;          ///< CACH access type of the last burst in our slot
};

/** "No colour code decoded yet". */
#define DMR_RPT_CC_UNKNOWN 0xFF

/**
 * Decide whether a transmission may start.
 *
 * @param ch: the channel.
 * @param seen: what the receiver last heard.
 * @return the verdict, mapped to a message by dmrRpt_gateName().
 */
enum dmrRptGate dmrRpt_gate(const struct dmrRptChannel *ch,
                            const struct dmrRptSeen *seen);

/** @return a short name for a verdict ("ok", "cc", "ts", "busy"). */
const char *dmrRpt_gateName(enum dmrRptGate gate);

/**
 * Format the state of the timing recovery for a trace or the quick menu, with
 * no floating point: "ppm=2 off=12 sh=180 q=87" or "none" when there is no
 * estimate.
 *
 * @param p: tracker, may be NULL.
 * @param plan: the plan in use, may be NULL.
 * @param buf: destination.
 * @param len: size of buf.
 * @return the number of characters written (as snprintf).
 */
int dmrRpt_format(const struct dmrRptPhase *p, const struct dmrRptPlan *plan,
                  char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* OPENRTX_DMR_RPT_H */
