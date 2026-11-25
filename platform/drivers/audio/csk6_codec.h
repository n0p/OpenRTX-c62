/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CSK6_CODEC_H
#define CSK6_CODEC_H

#include <stddef.h>
#include <stdint.h>
#include <interfaces/audio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Direction of a codec device, passed as its audioDevice configuration with
 * a cast to (const void *).
 */
enum Csk6CodecMode {
    CSK6_CODEC_MODE_OUTPUT = 0, ///< Playback to the speaker or radio
    CSK6_CODEC_MODE_INPUT = 1,  ///< Capture from the microphone or radio
};

/**
 * Processing applied to the radio audio on its way to the speaker, in place.
 *
 * The callback runs in the audio worker, on every playback frame, on the
 * samples of the SOURCE_RTX to SINK_SPK path only: the radio to the speaker
 * passthrough that a mode like AM opens. It must therefore be cheap, must not
 * block, and must not touch the radio: what it gets is 48 kHz audio.
 *
 * @param samples: first sample of the block.
 * @param count: number of samples of the speaker channel.
 * @param stride: distance between consecutive samples, in samples.
 */
typedef void (*csk6codec_rxProcessor)(int16_t *samples, size_t count,
                                      size_t stride);

/**
 * C62 audio through the CSK6 codec and DSP: one 48 kHz stereo stream each way,
 * microphone/speaker on the left and the BK4819 on the right. The device
 * configuration is an enum Csk6CodecMode.
 */
extern const struct audioDriver csk6_codec_audio_driver;

/**
 * Configure the codec and open the DSP streams on the calling thread, then
 * start the audio worker. Call once, at startup.
 *
 * @return zero on success, a negative error code on failure, in which case
 * no worker runs.
 */
int csk6codec_init(void);

/**
 * Cancel streams, join the worker and release DSP audio resources. Call once,
 * at shutdown, and only after a successful csk6codec_init(); the codec can't
 * be started again.
 */
void csk6codec_terminate(void);

/**
 * Enable or disable DSP audio for a board-selected route.
 * The caller handles GPIOs and radio AF muxing, and passes only valid,
 * compatible routes.
 *
 * Bounded: the route is recorded and the worker applies it, and the call waits
 * for that only up to CSK6_ROUTE_TIMEOUT_MS. The lock itself is taken with a
 * deadline too, because the worker holds it across the DSP's shared-memory
 * calls: a DSP that does not answer costs this route, never the caller's
 * thread. A negative return means the route was not applied; the board layer
 * then leaves the path closed so that the next request tries again.
 *
 * @param source: audio source.
 * @param sink: audio destination.
 * @param enabled: true to enable, false to disable and cancel its stream.
 * @return zero when the route was applied, -ETIMEDOUT when it was not.
 */
int csk6codec_setRoute(enum AudioSource source, enum AudioSink sink,
                       bool enabled);

/**
 * Install or remove the processing of the radio audio towards the speaker.
 *
 * It is a per-sample filter, so the owner of the mode has to set it before
 * opening the SOURCE_RTX to SINK_SPK path and remove it after closing it: with
 * the path closed the worker has nothing from the radio to process, and a
 * callback left behind would only be called on a later mode's audio.
 *
 * @param processor: the callback, NULL to remove it.
 */
void csk6codec_setRxProcessor(csk6codec_rxProcessor processor);

/**
 * Install or remove the processing of the microphone audio on its way to the
 * radio, in place.
 *
 * The callback runs in the audio worker on every playback frame, on the
 * SOURCE_MIC to SINK_RTX path only, and it receives the block that is about to
 * be played to the BK4819: a mode that modulates the power amplifier instead of
 * the frequency modulator takes the voice from here and leaves silence behind,
 * so that what the chip is asked to transmit is a bare carrier. Like the
 * receive processor it must be cheap and must not block, and its owner has to
 * remove it after closing the path.
 *
 * @param processor: the callback, NULL to remove it.
 */
void csk6codec_setTxProcessor(csk6codec_rxProcessor processor);


/**
 * Peak microphone magnitude of the last captured DSP frame.
 *
 * @return peak from 0 to 32768, 0 while the microphone is not captured.
 */
uint16_t csk6codec_getMicPeak(void);

/**
 * Peak magnitude of the radio channel (the BK4819 AF output), with a peak hold
 * of about 100 ms.
 *
 * This is the level the digital modes demodulate, so it is also the number that
 * says whether the codec ADC is clipping: 32767 means the top of the range has
 * been reached and the 4FSK levels are no longer what the demodulator's slicer
 * assumes.
 *
 * @return peak from 0 to 32768, 0 while the radio is not captured.
 */
uint16_t csk6codec_getRxPeak(void);

/**
 * Buffer halves lost because the reader was late, since the last clear.
 *
 * A DMA-style stream overwrites a half its reader did not take. For a
 * demodulator that is a jump in the sample stream, so this counter is the
 * first thing to look at when bursts decode but the slot grid keeps slipping.
 *
 * @return number of lost halves.
 */
uint32_t csk6codec_getStreamMisses(void);

/**
 * Reset the lost-half counter.
 */
void csk6codec_clearStreamMisses(void);

/**
 * Peak magnitude played to the radio (SINK_RTX) since the last clear, with a
 * peak hold of about 100 ms.
 *
 * A keyed transmitter that sends nothing and one that sends silence cannot be
 * told apart from the outside; this is the number that separates them. Zero
 * while a transmission is running means the mode is not producing baseband.
 *
 * @return peak from 0 to 32768.
 */
uint16_t csk6codec_getTxPeak(void);

/**
 * Reset the transmit peak.
 */
void csk6codec_clearTxPeak(void);

/**
 * Milliseconds since the audio worker last went round its loop.
 *
 * The worker is expected to run every millisecond or two while audio is
 * running. A large value means it is stuck -- typically inside a DSP call --
 * and every stream is starving. Reported in the transmit trace.
 *
 * @return age in milliseconds, -1 if the worker never ran.
 */
int32_t csk6codec_getWorkerAge(void);

#ifdef __cplusplus
}
#endif

#endif /* CSK6_CODEC_H */
