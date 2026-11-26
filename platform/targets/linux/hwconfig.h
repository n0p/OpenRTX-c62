/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWCONFIG_H
#define HWCONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Module17 hardware info flags, required by Module17 UI emulator target */
enum Mod17Flags {
    MOD17_FLAGS_HMI_PRESENT = 1,
    MOD17_FLAGS_SOFTPOT = 2,
};

/* Screen has adjustable brightness */
#define CONFIG_SCREEN_BRIGHTNESS

/* Battery type */
#define CONFIG_BAT_LIPO
#define CONFIG_BAT_NCELLS 2

/* Device supports M17 mode */
#define CONFIG_M17

/*
 * The C62 simulator (docs/SIMULATOR.md).
 *
 * The host emulator is otherwise a generic Linux radio: 160x128, M17, GPS and
 * an RTC. Built as the C62's simulator it also has the two features of the
 * radio that change what the interface shows, so that the screens the tour
 * walks are the screens the operator sees:
 *
 *  - CONFIG_KEY_BACKLIGHT: the keypad backlight, its own Settings row and its
 *    quick menu row (the C62 has a single white LED under the keys);
 *  - CONFIG_RX_FINE_OFFSET: the manual fine RX tuning, +-500 Hz in 10 Hz
 *    steps, a quick menu row and a row of the TETRA page.
 *
 * CONFIG_APRS and CONFIG_KISS come as -D from meson.build, together with the
 * sources those two bring, exactly as the radio's Kconfig does it.
 */
#ifdef PLATFORM_LINUX_SIM
#define CONFIG_KEY_BACKLIGHT
#define CONFIG_RX_FINE_OFFSET

/**
 * The radio's own way of saving: one debounced write of the settings and the
 * VFO, 3 s after the last change (core/state.c), because the C62 cuts power
 * with the volume knob and never runs state_terminate().  The simulator has to
 * save the same way or a test cannot tell a change that survived a power cut
 * from one the graceful shutdown wrote on the way out
 * (tools/simulator/settings_persist.py).  Its flash is
 * `$XDG_STATE_HOME/OpenRTX/state.bin`, see drivers/NVM/nvmem_linux.c.
 */
#define NVMEM_DEBOUNCED_WRITE

/**
 * Keypad backlight of the C62 simulator: one LED, on or off, implemented by
 * platform/targets/linux/platform.c. The radio's declaration is
 * platform/targets/c62/hwconfig.h.
 */
void c62_setKeyBacklight(bool on);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HWCONFIG_H */
