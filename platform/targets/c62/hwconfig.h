/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWCONFIG_H
#define HWCONFIG_H

#define TARGET_C62

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "drivers/baseband/BK1080.h"
#include "drivers/baseband/BK4819.h"

/*
 * Display properties are encoded in the devicetree
 */
#define DISPLAY DT_CHOSEN(zephyr_display)
#define CONFIG_SCREEN_WIDTH DT_PROP(DISPLAY, width)
#define CONFIG_SCREEN_HEIGHT DT_PROP(DISPLAY, height)

#define CONFIG_PIX_FMT_RGB565

/* Screen has adjustable brightness */
#define CONFIG_SCREEN_BRIGHTNESS

/* Battery type */
#define CONFIG_BAT_LIION
#define CONFIG_BAT_NCELLS 2

#define CONFIG_M17

/*
 * TETRA automatic frequency correction, by fine tuning of the BK4819: the
 * phase error is lowest with the carrier off the nominal frequency (the
 * filters of the flat receive path are not centred), and the crystal error
 * adds to that.
 */
#define CONFIG_TETRA_AFC

/*
 * Keypad backlight: the C62 has one white LED under the keys, on/off only
 * (no PWM). It is a setting of its own, not part of the display brightness,
 * so it can be switched off at night while keeping the screen readable.
 */
#define CONFIG_KEY_BACKLIGHT

/*
 * Manual fine RX tuning, +-500 Hz in 10 Hz steps, added to the channel
 * frequency on every retune. The 10 Hz resolution is what the BK4819
 * supports (see radio_C62.cpp); TETRA's automatic correction adds to it
 * instead of replacing it.
 */
#define CONFIG_RX_FINE_OFFSET

/*
 * TETRA mode: starting taps of the demodulator's equalizer, measured on the
 * BK4819 flat RX path (REG_43 0x7e28). They undo the filters' inter-symbol
 * interference; the equalizer keeps adapting from there.
 */
/* clang-format 18 and 21 disagree on this initializer */
/* clang-format off */
#define CONFIG_TETRA_EQ_INIT \
    { 0.007f, -0.004f, 0.036f, -0.185f, 1.149f, -0.187f, 0.033f, -0.001f, 0.0f }
/* clang-format on */

/* Software gain of the M17 microphone input, set for C62_ADC_MIC_GAIN_A/D */
#define CONFIG_MIC_GAIN 13

/*
 * CSK6 codec gains in dB, analog (_A) and digital (_D). The TX DAC sets the
 * level into the BK4819 mic input for both FM and M17; never above 0 dB.
 */
#define C62_ADC_MIC_GAIN_A 6
#define C62_ADC_MIC_GAIN_D 10
#define C62_ADC_RX_GAIN_A 6
#define C62_ADC_RX_GAIN_D 20
#define C62_DAC_SPK_GAIN_A -6
#define C62_DAC_SPK_GAIN_D 0
#define C62_DAC_TX_GAIN_A -18
#define C62_DAC_TX_GAIN_D 0

/**
 * BK4819 and BK1080 device instances. Pin numbers must be kept in sync
 * with the board devicetree (gpioa/gpiob nodes).
 */
extern const struct BK4819 c62_bk4819;
extern const struct gpioDev c62_bk4819_gpio;

/* BK4819 GPIOs */
#define LNA_VHF_EN &c62_bk4819_gpio, 0
#define LNA_UHF_EN &c62_bk4819_gpio, 1
#define PA_VHF_EN &c62_bk4819_gpio, 2
#define PA_UHF_EN &c62_bk4819_gpio, 3
#define ALC_TX_LED_EN &c62_bk4819_gpio, 4

/**
 * Set the fine RX tuning offset, added to the channel frequency (Hz, 10 Hz
 * resolution). Retunes at once when receiving.
 */
void c62_setRxOffset(int32_t hz);

/**
 * @return the fine RX tuning offset, Hz.
 */
int32_t c62_getRxOffset(void);

/**
 * Switch the keypad backlight on or off.
 *
 * The C62 has a single white LED under the keys with no dimming: the LED is
 * either on or off. It follows its own setting (settings.keyBacklight), not
 * display_setBacklightLevel(), so the keys can go dark while the screen stays
 * readable.
 *
 * @param on: true to light the keys.
 */
void c62_setKeyBacklight(bool on);

extern const struct BK1080 c62_bk1080;

extern const struct gpio_dt_spec speaker_enable;
extern const struct gpio_dt_spec dtmf_enable;
extern const struct gpio_dt_spec button_ptt;
extern const struct gpio_dt_spec led_white;
extern const struct gpio_dt_spec led_green;
extern const struct gpio_dt_spec led_keyboard;
extern const struct pwm_dt_spec pwm_lcd_backlight;
extern const struct pwm_dt_spec pwm_rf_apc;
extern const struct device *adc_dev;

/*
 * Persistence policy.
 *
 * The C62 has no graceful shutdown: the power knob cuts power, so
 * state_terminate() never runs and a save on shutdown would be lost. Settings
 * and VFO changes are therefore persisted by a debounced write while the
 * radio is running. Platforms with a clean shutdown leave this undefined and
 * keep saving from state_terminate() only.
 */
#define NVMEM_DEBOUNCED_WRITE
#define NVM_WRITE_DEBOUNCE_MS 3000

#ifdef __cplusplus
}
#endif

#endif /* HWCONFIG_H */
