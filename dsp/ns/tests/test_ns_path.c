/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Two properties of the noise-reduction stage that the frame harness does not
 * cover: that a stream of audio keeps its sample count and stays inside the
 * latency budget, and that the AP's audio survives a DSP that never answers.
 *
 * This is a *model* of the AP path, not the AP path: the stage is not wired
 * into the image yet (no LSF service, no DSP audio drivers), so what is
 * exercised here is the client side of the contract with a ring of the same
 * depth the service is designed with (4 frames of 480 samples each way) and a
 * deadline per frame.  docs/TEST_DSP.md §4 and §5 say exactly what the real
 * measurement needs and what this one cannot prove.
 *
 * Mutations, for checking that the tests can fail:
 *
 *   -DNS_DROP_FRAME=1   the stage loses one output frame       (breaks §4)
 *   -DNS_NO_FALLBACK=1  a missing DSP returns silence instead
 *                       of the input, as a naive client would  (breaks §5)
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "ns_dsp.h"

#define FRAME       NS_DSP_FRAME          /* 480 samples, 10 ms at 48 kHz */
#define RING_FRAMES 4                     /* the depth docs/NOISE.md designs */
#define SECONDS     60
#define NFRAMES     (SECONDS * 100)
#define NSAMPLES    (NFRAMES * FRAME)
#define TONE_HZ     997.0   /* not a submultiple of 48 samples: otherwise every
                              * lag that is a multiple of the period correlates
                              * equally and the delay cannot be measured */
#define TONE_AMP    8000.0

/* BB_STALL_MS: what the blackbox calls a stall (openrtx/include/core/blackbox.h).
 * The fallback has to be two orders of magnitude faster than that. */
#define SUPERVISOR_STALL_MS 2000
#define DEADLINE_MS 2                     /* per-frame budget of the LSF call */

enum ns_state { NS_STATE_OFF, NS_STATE_ON, NS_STATE_DSP_ABSENT };

/* ---------------------------------------------------------------- the model */

struct path {
    int16_t ring[RING_FRAMES][FRAME];   /* one direction, as the service has */
    unsigned head, tail;
    int16_t frame[FRAME];               /* the frame being worked on */
    enum ns_state state;
    unsigned warnings;
    unsigned log_events;
    unsigned lost_frames;
    unsigned late_frames;
    unsigned fallback_us;
    unsigned long fallback_total_us;               /* cost of deciding to degrade */
    int drop_next;                      /* mutation hook, see NS_DROP_FRAME */
};

static void *ns_state_buf;
static int dsp_present = 1;

/* The stage as the AP sees it: hand over a frame, get one back, or time out and
 * pass the audio through untouched.  A real client would block on the LSF reply;
 * here "absent" means the call returns nothing, which is the same branch. */
static void nr_stage(struct path *p, int16_t *io)
{
    enum ns_state before = p->state;
#ifdef NS_NO_FALLBACK
    /* Mutation: a client that treats "no answer" as "silence".  The audio stops,
     * which is what the fallback exists to prevent. */
    if (!dsp_present) {
        memset(io, 0, FRAME * sizeof(int16_t));
        p->state = NS_STATE_DSP_ABSENT;
        p->warnings++;
        return;
    }
#else
    if (!dsp_present) {
        p->state = NS_STATE_DSP_ABSENT;   /* UI/log see it, audio does not */
        p->fallback_us = DEADLINE_MS * 1000;
        p->warnings++;
        if (before != p->state)
            p->log_events++;              /* one line per transition, not per frame */
        return;                            /* io untouched: audio flows */
    }
#endif
    if (ns_dsp_process(ns_state_buf, io) != 0) {
        p->state = NS_STATE_DSP_ABSENT;
        p->warnings++;
        return;
    }
    p->state = NS_STATE_ON;
#ifdef NS_DROP_FRAME
    /* Mutation: the stage eats one frame.  Nothing else changes. */
    {
        static unsigned seen;
        if (++seen == 100)
            p->drop_next = 1;
    }
#endif
}

/* producer -> ring -> stage -> ring -> consumer, one frame per iteration, no
 * threads: the ordering is the one the service guarantees, and the point of the
 * test is the sample bookkeeping, not the scheduling.  Returns the number of
 * samples that reached the consumer, which is what "nothing is lost" means. */
static unsigned run_path(const int16_t *in, int16_t *out, struct path *p)
{
    unsigned written = 0;

    memset(p, 0, sizeof(*p));
    for (int k = 0; k < NFRAMES; k++) {
        /* producer publishes a frame */
        memcpy(p->ring[p->head], in + k * FRAME, FRAME * sizeof(int16_t));
        p->head = (p->head + 1) % RING_FRAMES;

        /* the stage pulls the oldest frame, works on it, hands it back */
        memcpy(p->frame, p->ring[p->tail], FRAME * sizeof(int16_t));
        nr_stage(p, p->frame);

        /* Mutation: the stage eats one frame, so the consumer never gets it and
         * the stream is 480 samples short. */
        if (p->drop_next) {
            p->drop_next = 0;
            p->lost_frames++;
            p->tail = (p->tail + 1) % RING_FRAMES;
            p->head = (p->head + RING_FRAMES - 1) % RING_FRAMES;
            continue;
        }
        memcpy(p->ring[p->tail], p->frame, FRAME * sizeof(int16_t));

        /* consumer takes the oldest frame */
        memcpy(out + written, p->ring[p->tail], FRAME * sizeof(int16_t));
        written += FRAME;
        p->tail = (p->tail + 1) % RING_FRAMES;
    }
    return written;
}

/* -------------------------------------------------------------- the checks */

static double delay_ms(const int16_t *in, const int16_t *out, double *peak)
{
    /* Normalised cross-correlation over the first two seconds, lags 0..40 ms. */
    const int n = 2 * 100 * FRAME;
    const int maxlag = 40 * 48;
    double best = -2;
    int bestlag = 0;
    double ein = 0, eout = 0;
    for (int i = 0; i < n; i++) {
        ein += (double)in[i] * in[i];
        eout += (double)out[i] * out[i];
    }
    for (int lag = 0; lag <= maxlag; lag++) {
        double acc = 0, eo = 0;
        for (int i = 0; i + lag < n; i++) {
            acc += (double)in[i] * out[i + lag];
            eo += (double)out[i + lag] * out[i + lag];
        }
        (void)ein;
        (void)eout;
        double norm = (acc == 0) ? 0 : acc / sqrt(eo * ein + 1e-30);
        if (norm > best) {
            best = norm;
            bestlag = lag;
        }
    }
    *peak = best;
    return bestlag * 1000.0 / 48000.0;
}

int main(void)
{
    static int16_t in[NSAMPLES], out[NSAMPLES];
    static unsigned char state[64 * 1024] __attribute__((aligned(8)));
    static struct path p;
    int failures = 0;

    ns_state_buf = state;

    for (int i = 0; i < NSAMPLES; i++) {
        double t = (double)i / 48000.0;
        in[i] = (int16_t)(TONE_AMP * sin(2.0 * M_PI * TONE_HZ * t));
    }

    /* ---- §4: continuity and latency, DSP present ------------------------- */
    if (ns_dsp_init(state, sizeof(state)) != 0) {
        printf("FAIL init\n");
        return 1;
    }
    dsp_present = 1;
    unsigned written = run_path(in, out, &p);

    if (written != NSAMPLES || p.lost_frames) {
        printf("FAIL continuity: %d samples in, %u samples out, %u frame(s) "
               "lost, %u samples missing\n", NSAMPLES, written, p.lost_frames,
               NSAMPLES - written);
        failures++;
    } else {
        printf("CONTINUITY OK %d samples in, %u samples out, %u frames lost\n",
               NSAMPLES, written, p.lost_frames);
    }

    {
        double peak = 0;
        double ms = delay_ms(in, out, &peak);
        printf("%s latency: added delay %.2f ms (peak correlation %.3f), "
               "budget 30.00 ms\n", ms <= 30.0 ? "LATENCY OK" : "FAIL latency",
               ms, peak);
        if (ms > 30.0)
            failures++;
    }

    /* ---- §5: the DSP does not answer ------------------------------------- */
    dsp_present = 0;
    memset(out, 0, sizeof(out));
    {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        written = run_path(in, out, &p);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;
        p.fallback_total_us = (unsigned long)us;
        p.fallback_us = (unsigned)(us / NFRAMES);   /* per frame, measured */
    }
    if (written != NSAMPLES)
        printf("  with the DSP absent only %u of %d samples came out\n",
               written, NSAMPLES);

    unsigned lost = p.lost_frames;
    int identical = (memcmp(in, out, sizeof(in)) == 0);
    printf("%s degradation: state=%s warnings=%u log=%u "
           "fallback=%lu us in %d frames (%.0f us/frame), "
           "supervisor stall at %d ms\n",
           (identical && lost == 0 && p.state == NS_STATE_DSP_ABSENT &&
            p.log_events > 0 && p.fallback_us < SUPERVISOR_STALL_MS * 1000u)
               ? "DEGRADE OK" : "FAIL degradation",
           p.state == NS_STATE_DSP_ABSENT ? "DSP_ABSENT" : "?",
           p.warnings, p.log_events, p.fallback_total_us, NFRAMES,
           (double)p.fallback_total_us / NFRAMES, SUPERVISOR_STALL_MS);
    if (!identical) {
        printf("  audio is not the input: the AP would hear the stage's silence\n");
        failures++;
    }
    if (lost != 0 || written != NSAMPLES) {
        printf("  %u frame(s) lost with the DSP absent, %d samples missing\n",
               lost, NSAMPLES - (int)written);
        failures++;
    }
    if (p.state != NS_STATE_DSP_ABSENT || p.log_events == 0) {
        printf("  the degradation is not visible in the state or the log\n");
        failures++;
    }
    if (p.fallback_us >= SUPERVISOR_STALL_MS * 1000u) {
        printf("  fallback took %u us, the supervisor would call it a stall\n",
               p.fallback_us);
        failures++;
    }
    if (p.fallback_us > DEADLINE_MS * 1000u) {
        printf("  fallback took %u us/frame, over the %d ms budget\n",
               p.fallback_us, DEADLINE_MS);
        failures++;
    }

    printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
    return failures ? 1 : 0;
}
