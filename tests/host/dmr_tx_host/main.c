/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file main.c
 * @brief The DMR transmit sequence, on the host, decoded by the receiver.
 *
 * The sequence the bench drives on the radio -- TUNE(DMR), PTT down for three
 * seconds, PTT up -- is walked here with the firmware's own pieces:
 *
 *   - the slot machine of the mode, rtx/DmrTxPlan.h (the same header
 *     OpMode_DMR::txSlot() runs, not a copy of it);
 *   - the burst builders and the modulator, protocols/DMR/dmr_tx.c;
 *   - the AMBE+2 encoder hook, protocols/DMR/dmr_enc.c over lib/dmr_encoder.
 *
 * What comes out is the baseband the mode would hand to the BK4819, 30 ms of
 * slots at 48 kHz, and it is written as raw int16 discriminator audio so the
 * repository's reference receiver (work/dmr-rx, build/dmr_rx --format s16) can
 * decode it.  A transmission that "keys" but carries no valid bursts is the
 * failure this exists to make impossible, so run.sh reads the receiver's report
 * and fails unless it accepted the voice bursts and the terminator.
 *
 * Two things beyond the audio:
 *
 *   - the call has to end by itself.  The mode used to have no transition into
 *     the terminator, so a released PTT left it generating silence for ever
 *     and the guard cut the transmission from outside; the sequence is checked
 *     for "two headers, voice, one terminator, tail, done" and for the unkey
 *     reaching DMRTX_DONE within TAIL_SLOTS+2 slots.
 *   - no thread may stay blocked.  The same loop is run in a pthread against a
 *     sink that stops consuming, and the thread has to come back and join:
 *     that is what putHalf() reporting the sync result is for.
 *
 *   tests/host/dmr_tx_host/run.sh          (from the repository root)
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rtx/DmrTxPlan.h"
#include "protocols/DMR/dmr_enc.h"
#include "protocols/DMR/dmr_tx.h"

/* One 30 ms half of the baseband stream at 48 kHz (OpMode_DMR.hpp) */
#define HALF_SAMPLES 1440
/* Symbols per half: 144 at 10 samples per symbol */
#define SPS 10
/* Guard around the 132 dibits of a burst inside the 144 of a slot */
#define BURST_DIBITS DMR_TX_BURST_DIBITS
#define GUARD_SYMS ((HALF_SAMPLES / SPS - BURST_DIBITS) / 2)
/* Headers of a call, and its tail of silence after the terminator */
#define TX_HEADERS 2
#define TAIL_SLOTS 2
/* 3 s of speech at 30 ms per slot */
#define PTT_SLOTS 100
/* The transmit level of the mode (same figure, so the decoder sees the same) */
#define TX_LEVEL (23000.0f * 0.3165f * 1944.0f / 2400.0f)
#define TX_INVERT true

static int16_t slot[HALF_SAMPLES];     /* one rendered slot               */
static uint8_t burst[BURST_DIBITS];    /* dibits of the burst in the slot */
static dmr_mod_t mod;
static dmr_tx_t tx;
static uint8_t ambe[3][9];             /* the frames of one burst         */
static int16_t silence[160];           /* what a muted microphone gives   */
static FILE *out;                      /* the baseband, s16               */

static unsigned countHeader, countVoice, countTerm, countTail, countDone;

/* ---------------------------------------------------------------- audio -- */

/* Render one slot: the burst in its guard, silence elsewhere (renderHalf). */
static void renderSlot(const uint8_t *dibits, bool carry)
{
    for (unsigned s = 0; s < HALF_SAMPLES / SPS; s++) {
        const uint8_t *d = NULL;

        if (carry && (s >= GUARD_SYMS) && (s < GUARD_SYMS + BURST_DIBITS))
            d = &dibits[s - GUARD_SYMS];
        dmr_mod_dibits(&mod, d, 1, slot + s * SPS);
    }
    if (out != NULL)
        fwrite(slot, sizeof(int16_t), HALF_SAMPLES, out);
}

/** The sink: a call that is being consumed answers "played". */
static bool consuming(void)
{
    return true;
}

/* ----------------------------------------------------------------- slots -- */

/*
 * One slot of the call, exactly as OpMode_DMR::txSlot() runs it: the planner
 * decides, the burst builders fill the dibits, the modulator renders.
 *
 * @param sinkAlive: the stream is still consuming.  When it is not, the slot
 *                   is the end of the over, as putHalf() makes it.
 * @return true when the call is over.
 */
static bool oneSlot(dmrTxCall_t *c, bool ptt, bool ending, uint8_t timeslot,
                    bool sinkAlive)
{
    const unsigned step = dmrTxNextSlot(c, ptt, ending, timeslot, TAIL_SLOTS);
    bool carry = false;

    switch (step) {
        case DMRTX_HEADER:
            dmr_tx_header(&tx, burst);
            carry = true;
            countHeader++;
            break;

        case DMRTX_VOICE:
            /* Three frames per burst, from the muted microphone: silence
             * encoded, which is what the bench keys with */
            for (unsigned i = 0; i < 3; i++) {
                if (!dmrEnc_encode(silence, ambe[i]))
                    memset(ambe[i], 0, sizeof(ambe[i]));
            }
            dmr_tx_voice(&tx, (const uint8_t (*)[9])ambe, burst);
            carry = true;
            countVoice++;
            break;

        case DMRTX_TERMINATOR:
            dmr_tx_terminator(&tx, burst);
            carry = true;
            countTerm++;
            break;

        case DMRTX_DONE:
            /* The last tail slot goes out with the rest, then the mode leaves */
            renderSlot(burst, false);
            countDone++;
            return true;

        default:
            countTail++;
            break;
    }

    renderSlot(burst, carry);

    /* putHalf(): the sink's answer is the mode's way out of a dead stream */
    return !sinkAlive;
}

/* ------------------------------------------------------------ scenarios -- */

/* The bench sequence: TUNE(DMR), PTT down 3 s, PTT up, until the call ends. */
static int runSequence(const char *path)
{
    dmrTxCall_t call;
    unsigned slots = 0, afterUnkey = 0;
    bool ended = false;

    out = fopen(path, "wb");
    if (out == NULL) {
        perror(path);
        return -1;
    }

    /* TUNE: a group call on talkgroup 9, colour code 1, timeslot 1 */
    dmr_tx_setup(&tx, 1234567u, 9u, true, 1);
    dmr_mod_init(&mod, TX_LEVEL, TX_INVERT);
    dmrEnc_reset();
    dmrTxCall_begin(&call, TX_HEADERS);

    /* PTT down: three seconds of voice */
    for (unsigned i = 0; i < PTT_SLOTS; i++) {
        if (oneSlot(&call, true, false, 1, consuming()))
            break;
        slots++;
    }

    /* PTT up: the terminator and the tail, and the mode has to get there */
    for (unsigned i = 0; i < TAIL_SLOTS + 4; i++) {
        afterUnkey++;
        if (oneSlot(&call, false, true, 1, consuming())) {
            ended = true;
            break;
        }
        slots++;
    }
    fclose(out);
    out = NULL;

    printf("== secuencia TUNE(DMR) -> PTT 3 s -> unkey\n");
    printf("   slots            %u (%.2f s)\n", slots + afterUnkey,
           (slots + afterUnkey) * 0.03);
    printf("   cabeceras        %u\n", countHeader);
    printf("   rafagas de voz   %u\n", countVoice);
    printf("   terminador       %u\n", countTerm);
    printf("   slots de cola    %u\n", countTail);
    printf("   fin tras unkey   %s (%u slots)\n", ended ? "si" : "NO",
           afterUnkey);

    if (countHeader != TX_HEADERS) {
        printf("FALLO: %u cabeceras, se esperaban %u\n", countHeader,
               TX_HEADERS);
        return -1;
    }
    /* One burst per 60 ms TDMA frame: at most every second 30 ms slot */
    if (countVoice > PTT_SLOTS / 2 + 1) {
        printf("FALLO: %u rafagas en %u slots: mas de una por trama TDMA\n",
               countVoice, PTT_SLOTS);
        return -1;
    }
    if (countVoice < PTT_SLOTS / 3) {
        printf("FALLO: solo %u rafagas de voz en 3 s\n", countVoice);
        return -1;
    }
    if (countTerm != 1) {
        printf("FALLO: %u terminadores, se esperaba 1 (la llamada no termina "
               "sola)\n", countTerm);
        return -1;
    }
    if (!ended || (afterUnkey > TAIL_SLOTS + 3)) {
        printf("FALLO: el unkey no termino la llamada (%u slots, maximo %u)\n",
               afterUnkey, TAIL_SLOTS + 3);
        return -1;
    }
    return 0;
}

/* A sink that stops consuming, while the call runs in its own thread. */
struct stalled
{
    pthread_mutex_t lock;
    bool            stopped;
};

static void *sequenceThread(void *arg)
{
    struct stalled *s = arg;
    dmrTxCall_t call;

    dmr_tx_setup(&tx, 1234567u, 9u, true, 1);
    dmr_mod_init(&mod, TX_LEVEL, TX_INVERT);
    dmrEnc_reset();
    dmrTxCall_begin(&call, TX_HEADERS);

    for (unsigned i = 0; i < PTT_SLOTS; i++) {
        pthread_mutex_lock(&s->lock);
        const bool alive = !s->stopped;
        pthread_mutex_unlock(&s->lock);

        if (oneSlot(&call, true, false, 1, alive))
            break;
    }
    return NULL;
}

static int runStalled(void)
{
    struct stalled s = { PTHREAD_MUTEX_INITIALIZER, false };
    pthread_t th;
    bool joined = false;

    out = NULL;
    if (pthread_create(&th, NULL, sequenceThread, &s) != 0) {
        printf("FALLO: no se pudo crear el hilo\n");
        return -1;
    }

    /* Let it get into the call, then stop the sink for good */
    struct timespec pause = { 0, 50 * 1000 * 1000 };
    nanosleep(&pause, NULL);
    pthread_mutex_lock(&s.lock);
    s.stopped = true;
    pthread_mutex_unlock(&s.lock);

    /* The loop must notice on the next slot and leave; joining is the proof */
    struct timespec limit;
    clock_gettime(CLOCK_MONOTONIC, &limit);
    limit.tv_sec += 2;

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (pthread_tryjoin_np(th, NULL) == 0) {
            joined = true;
            break;
        }
        if ((now.tv_sec > limit.tv_sec) ||
            (now.tv_sec == limit.tv_sec && now.tv_nsec >= limit.tv_nsec))
            break;
        struct timespec step = { 0, 2 * 1000 * 1000 };
        nanosleep(&step, NULL);
    }

    printf("== sink que deja de consumir\n");
    printf("   el bucle sale    %s\n", joined ? "si" : "NO (hilo colgado)");
    if (!joined) {
        printf("FALLO: el hilo de transmision no volvio\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "dmr_tx_baseband.s16";
    int ret = 0;

    if (runSequence(path) != 0)
        ret = 1;

    countHeader = countVoice = countTerm = countTail = countDone = 0;
    if (runStalled() != 0)
        ret = 1;

    printf("%s\n", ret == 0 ? "SECUENCIA CORRECTA" : "FALLOS");
    return ret;
}
