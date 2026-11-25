/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file sim_control.h
 * @brief Scriptable control channel of the C62 simulator.
 *
 * The emulator's readline shell is for a person: it prompts, echoes and keeps a
 * history. A test needs something that answers one command with one bounded
 * answer and never blocks, so the simulator build (PLATFORM_LINUX_SIM) replaces
 * the shell with a line protocol, enabled by `OPENRTX_SIM_HEADLESS=1` (stdin)
 * and/or `OPENRTX_SIM_SOCKET=/path/to/socket`:
 *
 *     key DOWN            press and release DOWN, then wait for the UI
 *     keys DOWN DOWN P2   several presses in a row
 *     key P2 long         hold it past the long-press threshold
 *     ptt on|off|toggle    the PTT line
 *     volume 12           turn the analog volume pot
 *     knob left|right     KNOB_LEFT / KNOB_RIGHT events
 *     screenshot [file]   PNG of the screen, auto-numbered without a name
 *     dump [text|json]    what the screen shows and what the radio is set to
 *     sync                wait until the UI has drawn the queued keys
 *     sleep 250           milliseconds
 *     quit
 *
 * Every command answers with zero or more payload lines and closes with
 *
 *     #end ok
 *
 * or `#end err <message>`, which is the only line a script has to look for.
 * docs/SIMULATOR.md has the reference and tools/simulator/tour.py the worked
 * example.
 */

#ifndef SIM_CONTROL_H
#define SIM_CONTROL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * True when the control channel was asked for in the environment.
 *
 * OPENRTX_SIM_HEADLESS set to a non-empty value other than "0" reads commands
 * from stdin; OPENRTX_SIM_SOCKET names a UNIX socket to create and accept one
 * client at a time on.
 */
bool sim_control_wanted(void);

/** Start the control thread. Called from emulator_start() instead of the CLI. */
void sim_control_start(void);

#ifdef __cplusplus
}
#endif

#endif /* SIM_CONTROL_H */
