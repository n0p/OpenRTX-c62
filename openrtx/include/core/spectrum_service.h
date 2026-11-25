/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SPECTRUM_SERVICE_H
#define SPECTRUM_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/spectrum.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The radio side of the spectrum analyser: the sweep, the moment it may run and
 * what it does to the receiver while it runs.
 *
 * Where it runs matters. A point is a PLL write, a settle and an RSSI read, and
 * the tick of that clock is 0.6 ms (docs/SPECTRUM.md); doing it from the UI
 * thread would mean retuning the receiver under the RTX thread, which reads
 * the RSSI of the same chip every pass and would see each bin as a squelch
 * opening. So the sweep runs in the RTX thread, in the slot where the mode
 * would normally be fed audio: while a sweep is in flight the mode is not
 * updated at all, its RSSI filter is not fed, and the receiver is muted, so the
 * squelch cannot chatter on the bins and nothing reaches the speaker.
 *
 * Between sweeps the receiver is parked on the centre frequency, unmuted, with
 * the normal RTX path running: the mode's own squelch decides whether there is
 * a carrier there, the audio plays if there is one, and while a carrier holds
 * the sweep stays parked (the "pause" of the screen). That is what keeps the
 * analyst's squelch and audio working: the sweep is only ever a guest between
 * two normal receptions.
 *
 * Locking: none. spectrumService_open() and the setters run in the UI thread
 * with state_mutex held, poll() runs in the RTX thread which does not take it;
 * what crosses the boundary is a handful of aligned words and a restart flag,
 * the same contract core/vfo_search_service.c uses. The trace is read by the
 * screen straight from the engine: a bin is one aligned 16-bit word, so a frame
 * can show a bin that was swept while it was being drawn, and the raster only
 * changes at the start of a sweep.
 */

/** Spans the quick menu offers, Hz. */
#define SPECTRUM_SPAN_COUNT 9

/** One completed sweep of this span fits in the frame budget of the panel. */
typedef struct
{
    uint32_t centre;      ///< Centre of the span, Hz
    uint32_t span;        ///< Span really swept, Hz
    uint32_t step;        ///< Step really used, Hz (may be coarser than asked)
    uint32_t low;         ///< First bin, Hz
    uint32_t high;        ///< Last bin, Hz
    uint32_t tuned;       ///< Frequency the receiver was on, Hz
    uint32_t peakFreq;    ///< Strongest bin of the last sweep, Hz
    uint32_t sweeps;      ///< Sweeps completed since the screen opened
    uint32_t pointUs;     ///< Measured time per point, average, us
    uint32_t sweepUs;     ///< Measured time of the last sweep, us
    uint16_t bins;        ///< Bins of the raster
    uint16_t index;       ///< Bin being swept
    int16_t  peakDbm;     ///< Level of that bin, dBm
    uint32_t settleUs;    ///< Settling time asked for, us (0 = chip's gate)
    uint32_t gatePolls;   ///< Average glitch polls per point, 0 if unused
    bool     gateOn;      ///< The glitch gate is doing the waiting
    bool     paused;      ///< Parked on a carrier, audio on
    bool     valid;       ///< At least one sweep is on screen
    bool     hold;        ///< Maximum hold enabled
} spectrumStatus_t;

/**
 * Start the analyser on a frequency.
 *
 * Called by the UI thread when the screen opens. Saves nothing: the receiver
 * is only ever retuned through the driver's own tuning word, so the channel in
 * state.channel and the RTX configuration are untouched and the frequency to
 * come back to is the one the radio already had.
 *
 * @param centre: centre of the span, Hz, on the VFO frequency.
 * @return false if the analyser cannot run now: the receiver is not receiving
 *         (off or transmitting), or the mode owns the discriminator (a digital
 *         mode needs continuous samples and has its own squelch).
 */
bool spectrumService_open(uint32_t centre);

/**
 * Stop the analyser and give the receiver back.
 *
 * The RTX thread retunes to the frequency the radio had before the analyser
 * opened, restores the filters and the audio, at the next pass.
 */
void spectrumService_close(void);

/** True between open() and close(): the screen is on. */
bool spectrumService_active(void);

/**
 * One pass of the analyser, from the RTX thread.
 *
 * @return true if this pass belongs to the sweep (the caller must skip the
 *         normal RSSI read and the mode update); false when the receiver is
 *         parked on the centre and the normal path must run, which is how the
 *         squelch and the audio keep working between sweeps.
 */
bool spectrumService_poll(void);

/** Read the trace and the raster. Valid until the next close(). */
const spectrum_t *spectrumService_trace(void);

/** Snapshot of everything the screen shows. */
void spectrumService_status(spectrumStatus_t *out);

/* ---- what the quick menu changes ---------------------------------------- */

uint8_t spectrumService_spanIndex(void);
void    spectrumService_setSpanIndex(uint8_t index);
uint8_t spectrumService_stepIndex(void);
void    spectrumService_setStepIndex(uint8_t index);

/** Step really used, Hz: the engine raises it to fit the panel. */
uint32_t spectrumService_effectiveStep(void);

bool spectrumService_hold(void);
void spectrumService_setHold(bool on);
void spectrumService_clearHold(void);
void spectrumService_resetPeak(void);

/**
 * Settling time the sweep waits after each retune, us: 0 is the chip's own
 * glitch indicator (docs/SPECTRUM.md §3), anything else a fixed wait.
 *
 * The table is { 0, 50, 100, 200, 400 } us, so the operator can compare what the
 * chip asks for against the datasheet's worst case on the radio itself, which
 * is the only place where the real settling time of a unit can be seen.
 */
uint8_t  spectrumService_settleIndex(void);
void     spectrumService_setSettleIndex(uint8_t index);
uint32_t spectrumService_settleUs(void);

/** Park the span on a frequency again (the "CENTRA" shortcut). */
void spectrumService_centreOn(uint32_t freq);

/** Move the centre by a number of steps, positive or negative. */
void spectrumService_nudge(int32_t steps);

/* ---- human readable ----------------------------------------------------- */

/** Name of a span entry, e.g. "1M", for the menu row and the status line. */
const char *spectrumService_spanName(uint8_t index);

/** Name of a step entry, e.g. "25k". */
const char *spectrumService_stepName(uint8_t index);

/**
 * One line with the whole trace, for the PC protocol.
 *
 * The hook the remote control would carry on its serial channel: the format is
 * "sp <centre> <step> <bins> <dbm> <dbm> ...", one level per bin in the order
 * of the raster, the same numbers the screen draws. Nothing calls it: wiring it
 * needs a command in the remote control's table (platform/drivers/audio/
 * c62_remote.c, the "rr"/"rw" commands are the model) and a reply packet
 * through baseband_dump_send_text(). See docs/SPECTRUM.md §6.
 *
 * @param buf: destination.
 * @param len: its size.
 * @return false if there is no trace or the buffer is too small.
 */
bool spectrumService_traceLine(char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SPECTRUM_SERVICE_H */
