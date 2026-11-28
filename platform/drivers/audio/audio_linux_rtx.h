/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_LINUX_RTX_H
#define AUDIO_LINUX_RTX_H

/**
 * @file audio_linux_rtx.h
 * @brief The emulator's transmitter and microphone drivers, for audio_linux.c.
 *
 * See audio_linux_rtx.c: the endpoints the Linux platform declared without a
 * driver, and which every digital transmitter needs to open a stream.
 */

#include "interfaces/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Playback driver for SINK_RTX. */
extern const struct audioDriver audio_linux_rtxSinkDriver;

/** The receiver's discriminator, SOURCE_RTX: the capture file if it is there,
 *  a noise floor if it is not. */
extern const struct audioDriver audio_linux_rxSourceDriver;

/** Capture driver for SOURCE_MIC. */
extern const struct audioDriver audio_linux_micDriver;

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_LINUX_RTX_H */
