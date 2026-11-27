/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <hwconfig.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/device.h>
#include "drivers/GPIO/gpio_zephyr.h"
#include "drivers/SPI/spi_zephyr.h"
#include "drivers/I2C/i2c_bitbang.h"

/*
 * Clock of the SPI bus while the spectrum analyser sweeps, Hz. The chip's
 * interface reaches 8 MHz (BK4819 datasheet V1.0 §2.6 table 6) and the radio
 * talks to it at 200 kHz the rest of the time; this is the one knob to turn if
 * a board turns out not to like the faster clock, and it costs nothing but the
 * analyser. See docs/SPECTRUM.md §3.
 */
#ifdef CONFIG_C62_SPECTRUM_SPI_HZ
#define C62_BK4819_SWEEP_SPI_HZ CONFIG_C62_SPECTRUM_SPI_HZ
#else
#define C62_BK4819_SWEEP_SPI_HZ 1000000
#endif

/*
 * Pin mapping, must match platform/targets/c62/c62.dts:
 * BK4819: SCLK gpioa 13, SDATA gpioa 7, SCN gpioa 8 (active low)
 * BK1080: SCLK gpioa 5, SDATA gpioa 6, PWR gpiob 2 (active low)
 */
static const struct spiZephyrCfg c62_bk4819_spiCfg = {
    .bus = DEVICE_DT_GET(DT_NODELABEL(bk4819_spi)),
    .frequency = 200000,
    .flags = SPI_HALF_DUPLEX
};

SPI_ZEPHYR_DEVICE_DEFINE(c62_bk4819_spi, c62_bk4819_spiCfg, NULL)

/*
 * The same bus at the clock the sweep uses. The chip's interface takes up to
 * 8 MHz (BK4819 datasheet V1.0 §2.6 table 6: fSCK max 8 MHz, tHIGH/tLOW min
 * 25 ns), and the bus is a Zephyr bit-bang whose per-bit delay is
 * (1000000*2000/frequency)/2000/2 us: at 200 kHz that truncates to 2 us per
 * half period and gives 4 us per bit, 96 us for the three bytes of a register
 * access. Above 1 MHz the per-bit delay becomes zero and the rate is set by the
 * GPIO toggling alone (about 2 MHz on this core), which is still four times
 * inside the chip's limit. Only the sweep uses this configuration -- see
 * docs/SPECTRUM.md -- so a board that does not tolerate it costs the analyser
 * and nothing else.
 */
static const struct spiZephyrCfg c62_bk4819_fastSpiCfg = {
    .bus = DEVICE_DT_GET(DT_NODELABEL(bk4819_spi)),
    .frequency = C62_BK4819_SWEEP_SPI_HZ,
    .flags = SPI_HALF_DUPLEX
};

SPI_ZEPHYR_DEVICE_DEFINE(c62_bk4819_fastSpi, c62_bk4819_fastSpiCfg, NULL)

const struct BK4819 c62_bk4819 = { .spi = &c62_bk4819_spi,
                                   .spiFast = &c62_bk4819_fastSpi,
                                   .scn = { &GpioA, 8 } };

GPIO_BK4819_DEVICE_DEFINE(c62_bk4819_gpio, c62_bk4819)

static struct i2cBitbangState c62_bk1080_i2cState;

/*
 * Serialisation of the tuner's bit-banged bus.
 *
 * The BK1080 driver keeps the "bus held" state of a frame from one call to the
 * next (i2c_bitbang.h), so two contexts interleaving calls is not a slow read:
 * it is a frame addressed to the wrong register or a value written where
 * another one was going.  Two contexts reach it on the radio -- the RTX thread
 * (radio_getRssi() in the broadcast mode) and the pcproto thread (the debug
 * namespace's register access) -- and neither can use the pthread_mutex_t of
 * struct i2cDevice, which under this target's newlib is a plain uint32_t.
 *
 * Bounded, like the BK4819's: the access that holds it can hang, and a reader
 * that cannot have the bus has to say so instead of parking for ever.
 */
static K_MUTEX_DEFINE(c62Bk1080Mutex);

bool c62Bk1080Lock(void)
{
    if (k_is_in_isr())
        return true;

    return k_mutex_lock(&c62Bk1080Mutex, K_MSEC(C62_BUS_LOCK_TIMEOUT_MS)) == 0;
}

void c62Bk1080Unlock(void)
{
    if (!k_is_in_isr())
        k_mutex_unlock(&c62Bk1080Mutex);
}


/*
 * Clock half-period, microseconds. The BK1080 takes up to 2.5 MHz on this bus
 * (400 ns), and delayUs() below 1 ms is a busy wait on this SoC, so every
 * microsecond here is CPU time: 2 us (250 kHz, ten times slower than the chip
 * allows) costs about 0.2 ms of busy wait per status read instead of the
 * 1.1 ms that 10 us would. See docs/RX_WFM.md.
 */
static const struct i2cBitbangCfg c62_bk1080_i2cCfg = {
    .sck = { &GpioA, 5 },
    .sda = { &GpioA, 6 },
    .clkPeriod = 2,
    .state = &c62_bk1080_i2cState,
};

I2C_BITBANG_DEVICE_DEFINE(c62_bk1080_i2c, c62_bk1080_i2cCfg, NULL)

const struct BK1080 c62_bk1080 = { .i2c = &c62_bk1080_i2c,
                                   .pwr = { &GpioB, 2 } };

const struct gpio_dt_spec speaker_enable =
    GPIO_DT_SPEC_GET(DT_PATH(gpio_controls, speaker_enable), gpios);
const struct gpio_dt_spec dtmf_enable =
    GPIO_DT_SPEC_GET(DT_PATH(gpio_controls, dtmf_enable), gpios);
const struct gpio_dt_spec button_ptt =
    GPIO_DT_SPEC_GET_OR(DT_NODELABEL(button_ptt), gpios, { 0 });
const struct gpio_dt_spec led_white = GPIO_DT_SPEC_GET(DT_ALIAS(ledwhite),
                                                       gpios);
const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(ledgreen),
                                                       gpios);
const struct gpio_dt_spec led_keyboard =
    GPIO_DT_SPEC_GET(DT_NODELABEL(ledkeyboard), gpios);

const struct pwm_dt_spec pwm_lcd_backlight =
    PWM_DT_SPEC_GET(DT_NODELABEL(pwm_lcd_backlight));
const struct pwm_dt_spec pwm_rf_apc = PWM_DT_SPEC_GET(DT_NODELABEL(pwm_rf_apc));

const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc0));
