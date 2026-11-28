/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The C++ side of the M17 interrupt harness: the firmware's own entry into the
 * transmit audio path, reached from C.
 *
 * audioPath_request() is what OpMode_M17::txState() calls the instant the PTT
 * is pressed (openrtx/src/rtx/OpMode_M17.cpp:459), and it is compiled here from
 * the real source, not copied: only the board layer underneath it is a stub
 * (m17_isr_board.cpp), because this harness has no CSK6 codec to talk to.
 */

#include <core/audio_path.h>

extern "C" pathId m17_audio_request(void)
{
    return audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
}
