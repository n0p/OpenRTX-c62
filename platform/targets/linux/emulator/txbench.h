/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TXBENCH_H
#define TXBENCH_H

/**
 * @file txbench.h
 * @brief Knobs the host transmit bench needs from the emulator's transmitter.
 *
 * The emulator used to have no transmitter at all: SINK_RTX and SOURCE_MIC were
 * listed as endpoints with a NULL driver, so every mode that builds its
 * waveform into a stream -- DMR, NXDN, dPMR, D-STAR, YSF, P25 -- failed in
 * audioStream_start() with -ENODEV on the host and nothing above that call was
 * ever exercised.  That is why a PTT that does nothing in five modes could not
 * be seen here: the host stopped one call short of the radio.
 *
 * audio_linux_rtx.c adds the two endpoints the C62 has (the codec's playback
 * into the PA and the microphone capture) and these are the handles a test
 * needs on top of them: how much went out, and the ability to make the stub
 * refuse, which is how a bench proves it can tell a working transmission from
 * a broken one.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Make the RTX playback driver refuse to start, the way a codec that did not
 * take the route does.
 *
 * @param rc: the error the driver returns (negative), or 0 to be honest again.
 */
void txbench_failStreamStart(int rc);

/**
 * Make the receiver's stream refuse to start, the way a codec route the worker
 * did not apply does.  The mode then gets no samples at all: it is the state
 * that used to take P25's and YSF's PTT with it (their keying decision was
 * behind the receive pass).
 *
 * @param dead: true to refuse every receive stream.
 */
void txbench_failReceiveStream(bool dead);

/**
 * Make the transmitter refuse to key, the way a driver whose PA does not come
 * up does.  Refusals are counted by radio_linux_refusedCount().
 *
 * @param refuse: true to refuse every keying.
 */
void txbench_refuseKeying(bool refuse);

/** Forget the counters and both injections. */
void txbench_clear(void);

/** Halves the RTX playback driver has taken since txbench_clear(). */
unsigned txbench_blocks(void);

/** Samples those halves carried. */
unsigned txbench_samples(void);

/** Samples the microphone source has handed out since txbench_clear(). */
unsigned txbench_micSamples(void);

/** Loudest sample the playback driver has seen, absolute value. */
int txbench_peak(void);

#ifdef __cplusplus
}
#endif

#endif /* TXBENCH_H */
