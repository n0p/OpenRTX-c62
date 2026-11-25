/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <interfaces/platform.h>
#include <interfaces/delays.h>
#include <interfaces/keyboard.h>
#include <hwconfig.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <interfaces/audio.h>
#include <interfaces/nvmem.h>
#include <interfaces/radio.h>
#include "drivers/SPI/spi_zephyr.h"
#include "drivers/audio/csk6_codec.h"
#ifdef C62_BASEBAND_DUMP
#include "drivers/audio/baseband_dump_c62.h"
#ifdef C62_REMOTE
#include "drivers/audio/c62_remote.h"
#endif
#endif
#ifdef C62_KISS_UART
/* Out of the dump block on purpose: the KISS build has no dump, and inside it
 * the declaration disappeared and the call became an implicit one. */
extern void kiss_uart_init(void);
#endif
#include "controller.h"
#include "dsp.h"
#include <IOMuxManager.h>
#include "core/port_ptt.h"
#include "core/settings.h"
#include "core/state.h"
#ifdef C62_PC_PROTO
void pc_proto_c62_init(void);
void pc_proto_c62_supervise(void);
#endif
#include <string.h>
#include <stdint.h>

/*
 * DMR package state in PSRAM (platform/targets/c62/psram_dmr.ld): the
 * demodulator's sample ring, the call log, the voice burst queue, the slot
 * state machine, the data reassembly buffers, the Tier III engine and the
 * transmitter's vocoder tables.  Too big for SRAM, and only touched once per
 * burst, never inside a sample loop.
 *
 * The section is NOLOAD, so nothing zeroes it but this: the PSRAM controller's
 * calibration leaves test words in the region, and a stale "already
 * initialised" flag would be read as true.  It is zeroed here, once, before
 * anything can use it; each module still rearms its own state in its reset
 * function when the mode opens.
 *
 * A separate output section, not .psram_section, because that one also holds
 * the CSK6 system heap: zeroing it wholesale would wipe it.
 */
extern uint32_t __psram_dmr_start[];
extern uint32_t __psram_dmr_end[];

static void psram_dmr_clear(void)
{
    const uintptr_t n =
        (uintptr_t)__psram_dmr_end - (uintptr_t)__psram_dmr_start;

    if (n > 0)
        memset(__psram_dmr_start, 0, n);
}

/*
 * Tuning range.
 *
 * By default the radio keeps the ranges Retevis declares for the C62
 * (VHF 137-174 MHz, UHF 400-480 MHz), which is what the TX calibration covers.
 * CONFIG_C62_UNLOCKED_TUNING opens the whole BK4819 range instead:
 * 18-660 MHz and 840-1300 MHz, with the 660-840 MHz gap it cannot reach
 * excluded, as the user asked. TX outside the calibrated bands is then the
 * user's responsibility: the radio is not type approved there and the PA
 * calibration is extrapolated from the nearest measured point.
 */
static const hwInfo_t hwInfo = {
    .name = "c62",
    .hw_version = 0,
    .uhf_band = 1,
    .vhf_band = 1,
#ifdef CONFIG_C62_UNLOCKED_TUNING
    .uhf_maxFreq = 1300,
    .uhf_minFreq = 840,
    .vhf_maxFreq = 660,
    .vhf_minFreq = 18,
#else
    .uhf_maxFreq = 480,
    .uhf_minFreq = 400,
    .vhf_maxFreq = 174,
    .vhf_minFreq = 137,
#endif
};

// ADC
#define ADC_RAW_CHARGING_THRESHOLD 2100
#define BAT_DET_SETTLE_MS 10 // Measured: settles to 1 LSB in 5 ms
static int32_t adc_vref = 0;
static bool is_battery_charging = false;

PINCTRL_DT_DEFINE(DT_NODELABEL(adc0));
PINCTRL_DT_DEFINE(DT_CHOSEN(zephyr_console));

/*
 * Raw RX baseband capture at boot.
 *
 * The dump is the only way to see what the demodulators actually receive, and
 * it takes the console UART, mutes the log and allocates a megabyte of PSRAM,
 * so it is armed only when the operator asks for it: the A/B key (the red one,
 * KEY_ESC) held while the radio is switched on. The key state is read before
 * the DSP starts, because the DSP reconfigures the pins it shares.
 */
#ifdef C62_BASEBAND_DUMP
static bool dumpRequested;
#endif

/**
 * Re-apply what the DSP firmware overwrites when it starts and sets up audio:
 * the battery ADC pins, which also read the power knob, and the console pins
 * and UART settings. Then wait for the battery reading to settle.
 */
static void restore_dsp_overwrites(void)
{
    const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    struct uart_config config;

    int adc = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(adc0)),
                                  PINCTRL_STATE_DEFAULT);
    int pins = pinctrl_apply_state(
        PINCTRL_DT_DEV_CONFIG_GET(DT_CHOSEN(zephyr_console)),
        PINCTRL_STATE_DEFAULT);
    /* The driver keeps its configuration, which the DSP leaves alone */
    int uart = 0;
    bool restoreUart = true;
#ifdef C62_BASEBAND_DUMP
    /* An armed baseband dump owns the console UART and its baud rate */
    restoreUart = !dumpRequested;
#endif
    if (restoreUart) {
        uart = uart_config_get(console, &config);
        if (uart == 0)
            uart = uart_configure(console, &config);
    } else {
        (void)config;
        (void)console;
    }
    if ((adc != 0) || (pins != 0) || (uart != 0))
        printk("Restore after DSP start failed: adc=%d pins=%d uart=%d\n", adc,
               pins, uart);

    sleepFor(0, BAT_DET_SETTLE_MS);
}

static int battery_init(void)
{
    if (!device_is_ready(adc_dev)) {
        printk("ADC device not ready\n");
        return -1;
    }

    // Configure ADC channel
    struct adc_channel_cfg channel_cfg = {
        .gain = ADC_GAIN_1,
        .reference = ADC_REF_INTERNAL,
        .channel_id = 2, // ADC2 channel (pin B08)
    };

    int ret = adc_channel_setup(adc_dev, &channel_cfg);
    if (ret < 0) {
        printk("Failed to setup ADC channel: %d\n", ret);
        return -2;
    }

    channel_cfg.channel_id = 1; // ADC1 channel (Charge detection)

    ret = adc_channel_setup(adc_dev, &channel_cfg);
    if (ret < 0) {
        printk("Failed to setup ADC channel: %d\n", ret);
        return -2;
    }

    // Read ADC reference voltage if supported
    adc_vref = adc_ref_internal(adc_dev);

    return 0;
}

#ifdef C62_BASEBAND_DUMP
static bool captureKeyHeld(void)
{
    /* Two samples, a few tens of milliseconds apart: the key has to be held,
     * not caught on a bounce */
    if ((kbd_getKeys() & KEY_ESC) == 0)
        return false;
    sleepFor(0, 20);
    return (kbd_getKeys() & KEY_ESC) != 0;
}
#endif

void platform_init_csk6()
{
    /* Zeros the NOLOAD DMR PSRAM section: see psram_dmr_clear() above */
    psram_dmr_clear();

    /* Initialise non volatile memory manager */
    nvm_init();

    // Configure the PTT key as input with pull-up
    gpio_pin_configure_dt(&button_ptt, GPIO_INPUT);

    // Configure light pins as outputs and set initial state
    gpio_pin_configure_dt(&led_white, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&led_keyboard, GPIO_OUTPUT_INACTIVE);

    /* Init ADC for Battery reading */
    battery_init();

    /* C62 can boot with power button off when charging.
     * That's why we put a wait here until the power button is switched on.
     */
    do {
        sleepFor(0, 10);
    } while (!platform_pwrButtonStatus());

    /* Enable keyboard backlight: the UI turns it off later if the setting says so */
    c62_setKeyBacklight(true);

#ifdef C62_BASEBAND_DUMP
    /* The keypad is scanned here, before the DSP reconfigures its pins */
    if (captureKeyHeld())
        dumpRequested = true;
#endif

    /* Initialise BK4819 SPI bus and transceiver */
    spiZephyr_init(c62_bk4819.spi);
    bk4819_init(&c62_bk4819);

    /*
     * A radio must not start up transmitting (docs/TX_SAFETY.md): before the
     * DSP, the audio, the codec or any thread exists, the PA enable lines are
     * driven low and the chip's own latch (REG_33) is read back to prove it.
     * The stock latch survived the reset on the radio that came back hot, so
     * this is the first thing done once the transceiver answers.
     */
    if (radio_paForceOff())
        printk("PA: off at boot, REG_33 latch clear\n");
    else
        printk("PA: STILL KEYED AT BOOT: REG_33 latch set\n");

    /*
     * Start the DSP only now, after the BK4819 microphone input has settled.
     * It changes pins while it starts and sets up audio: restore them after
     * audio_init(). Nothing reads the battery ADC before then.
     */
    int ret = lsf_dsp_init();
    if (ret == 0)
        ret = lsf_controller_init();

    /* Initialise audio, which runs on the DSP */
    if (ret == 0)
        audio_init();

    restore_dsp_overwrites();

#ifdef C62_BASEBAND_DUMP
    /*
     * After the console restore, which the dump overrides with its own baud
     * rate. Only when the A/B key asked for a capture: an idle dump would mute
     * the log and hold a megabyte of PSRAM for nothing.
     *
     * The message goes out before baseband_dump_init() mutes the log.
     */
    if (ret == 0)
        printk("C62: baseband dump %s (hold A/B at power on to record)\n",
               dumpRequested ? "armed" : "off");
    if ((ret == 0) && dumpRequested)
        baseband_dump_init();
#ifdef C62_REMOTE
    /* After the dump: replies are dump control packets */
    if ((ret == 0) && dumpRequested)
        c62_remote_init();
#endif
#endif /* C62_BASEBAND_DUMP */
#ifdef C62_KISS_UART
    /*
     * The KISS stream owns the console UART and mutes the log: this has to
     * happen after everything that prints, and it takes the PTT pad, which is
     * why the option is off by default. See drivers/audio/c62_kiss.c.
     */
    if (ret == 0)
        kiss_uart_init();
#endif
#ifndef C62_BASEBAND_DUMP
    /* Built without the dump: say so, it is the first thing to check when a
     * capture does not produce anything. */
    if (ret == 0)
        printk("C62: baseband dump not built in (-DC62_BASEBAND_DUMP=ON)\n");
#endif

    if (ret != 0)
        printk("DSP start failed, no audio: %d\n", ret);

#ifdef C62_PC_PROTO
    /*
     * The CPS protocol takes the console UART: from here on the log goes into
     * its ring and still reaches the UART while no client is linked, so a
     * serial terminal sees the boot exactly as before.  Started after the dump
     * because it pauses the dump while a client is linked.
     */
    if (ret == 0)
        pc_proto_c62_init();
#endif

    /*
     * Safe state before the watchdog is armed: the PA enable lines are driven
     * low and the transceiver is out of transmit, so a reset -- the watchdog's,
     * or any other -- cannot leave the radio transmitting. The GPIOs come up as
     * inputs at reset, which is already off; this also covers a warm reset that
     * did not reload the pin configuration.
     */
    radio_disableRtx();

    /* Last: from here a stuck firmware resets instead of transmitting for ever */
    int wdtRet = platform_wdtStart(2000);
    if (wdtRet != 0)
        printk("C62: watchdog unavailable (%d): a stuck TX cannot reset\n",
               wdtRet);
    else
        printk("C62: watchdog armed, 2 s\n");
}

void platform_terminate()
{
    /*
     * C62 does not have a proper power on/off mechanism and the MCU is
     * always powered. Thus, for turn off, perform a system reset.
     *
     * With the charger connected the chip stays powered through the reset:
     * stop the DSP and reset the peripherals along with the core, or the
     * DSP's UART interrupt halts the next boot.
     */
    lsf_dsp_terminate();
    sys_reboot(SYS_REBOOT_COLD);
}

/**
 * Read battery voltage using ADC2 on pin B08
 * @return: Battery voltage in millivolts (mV), or 0 on error
 */
uint16_t platform_getVbat()
{
#define ADC_NUM_CHANNELS 2
    static int16_t adc_raw_buffer[ADC_NUM_CHANNELS];
    static struct adc_sequence sequence = {
        /* individual channels will be added below */
        /* BIT(2) Channel 2 (BAT_DET), BIT(1) Channel 1 (Charge detection) */
        .channels = BIT(2) | BIT(1),
        .buffer = adc_raw_buffer,
        /* buffer size in bytes, not number of samples */
        .buffer_size = sizeof(adc_raw_buffer),
        .resolution = 11, // 11-bit resolution
    };

    // Reset ADC channels to read
    sequence.channels = BIT(1) | BIT(2);

    // Read ADC value
    int ret = adc_read(adc_dev, &sequence);
    if (ret < 0) {
        printk("Failed to read ADC: %d\n", ret);
        return 0;
    }

    if (adc_raw_buffer[0] > ADC_RAW_CHARGING_THRESHOLD) {
        if (!is_battery_charging) {
            is_battery_charging = true;
            printk("\n** Charging started **\n");
        }
    } else {
        if (is_battery_charging) {
            is_battery_charging = false;
            printk("\n** Charging stopped **\n");
        }
    }

    int32_t bat_det_value =
        adc_raw_buffer[1]
        - 2048; // Adjust for 11-bit ADC with bipolar range (-2048 to 2047)
    adc_raw_to_millivolts(adc_vref, ADC_GAIN_1, 11 /* adb resolution */,
                          &bat_det_value);

    // Adapt for voltage divider 200K and 100K
    uint32_t battery_voltage_mv = bat_det_value * 3;

    return (uint16_t)battery_voltage_mv;
}

uint8_t platform_getMicLevel()
{
    /* The meter is shown only in the M17 and DMR modes: scale it by the
     * software gain to show what the encoder sees, so full scale is clipping */
    return MIN((csk6codec_getMicPeak() * CONFIG_MIC_GAIN) >> 7, UINT8_MAX);
}

uint8_t platform_getVolumeLevel()
{
    return 0;
}

int8_t platform_getChSelector()
{
    return 0;
}

/*
 * Application watchdog, see platform_wdtStart().
 *
 * CONFIG_WATCHDOG and the ap_wdt node in c62.dts arm it; the driver quantises
 * the window to its own table (0.5 s, 1 s, 4 s). Nothing else in the firmware
 * touches it: it is only ever refreshed by the main thread, so a firmware that
 * stops scheduling that thread -- or stops reaching it, because everything is
 * stuck behind a wedged lock -- reboots instead of sitting there with the PA
 * keyed.
 */
static const struct device *const wdt_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(ap_wdt));
static bool wdtArmed;
static int wdtChannel;

int platform_wdtStart(uint32_t windowMs)
{
    if ((wdt_dev == NULL) || !device_is_ready(wdt_dev))
        return -ENODEV;

    struct wdt_timeout_cfg cfg = {
        .window =
            {
                .min = 0,
                .max = windowMs,
            },
        .callback = NULL,
        .flags = WDT_FLAG_RESET_SOC,
    };

    int channel = wdt_install_timeout(wdt_dev, &cfg);
    if (channel < 0)
        return channel;

    int ret = wdt_setup(wdt_dev, WDT_OPT_PAUSE_IN_SLEEP);
    if (ret < 0)
        return ret;

    wdtChannel = channel;
    wdtArmed = true;
    wdt_feed(wdt_dev, wdtChannel);
    return 0;
}

void platform_wdtFeed(void)
{
    if (wdtArmed)
        wdt_feed(wdt_dev, wdtChannel);

#ifdef C62_PC_PROTO
    /*
     * Next to the watchdog, because it is the same question one level up: this
     * thread is known to be running (it just fed the hardware), so it is the
     * one that can notice the transport thread is not.  Cheap: two loads and a
     * comparison, and the report is throttled inside.
     */
    pc_proto_c62_supervise();
#endif
}

#if defined(C62_REMOTE) || defined(CONFIG_C62_BENCH_PROTO)
/*
 * Forced PTT, for the remote control and for the bench protocol: in both
 * builds the PTT pad carries a UART RX and the transmitter is keyed by whoever
 * is holding the cable.  -1 returns the pad (docs/ACCESSORY.md).
 */
static volatile int pttOverride = -1;

void platform_pttOverride(int state)
{
    pttOverride = state;
}
#endif

/*
 * ---------------------------------------------------------------------------
 * The accessory port: one pad, three modes (docs/ACCESSORY.md)
 * ---------------------------------------------------------------------------
 *
 * GPIOA_15 is the accessory jack's PTT contact and UART2 RXD at the same time.
 * The pad stays muxed to the UART -- that is how the PC talks to the radio --
 * and the switch is then read from the *receiver's* side: a character that the
 * UART framed as a break or without a stop bit is a line held low through a
 * whole character time, and a run of those longer than PORT_PTT_HOLD_MS is a
 * finger on the switch (core/port_ptt.h).  The GPIO input is not the source:
 * with the pad on UART2 RXD its DATAIN does not follow the line (it reads low
 * always, which is what the first port-mode build did in hardware).
 *
 * One instance, fed by one context: the protocol transport, every 1 ms, because
 * it is the only one that may consume the UART FIFO.  Everybody else reads the
 * verdict.
 */
static portPttState_t portPtt;

uint8_t c62_portMode(void)
{
    /* Read without the state mutex, as the status and the transmit helpers do:
     * a mode that is one tick old is still the right answer, and a lock here
     * would put the key scan behind the UI thread. */
    const uint8_t mode = state.settings.pc_port;

    return settings_portModeValid(mode) ? mode : (uint8_t)PORT_MODE_AUTO;
}

portPttState_t *c62_portPttState(void)
{
    return &portPtt;
}

bool c62_portPadLevel(void)
{
    /* The device tree declares the key active low, so the logical level is 1
     * while the pad is grounded.  Only a witness, and the whole truth in
     * "Never": with the pad muxed to UART2 RXD this bit does not follow the
     * line, which is why the detector in "Auto" does not use it. */
    return gpio_pin_get_dt(&button_ptt) != 0;
}

void c62_portPadToUart(bool uart)
{
    /*
     * GPIO first: configuring the pin sets the direction and the pull-up, and
     * it muxes the pad to its default function, which is plain GPIO.  The mode
     * register (direction and pull) is separate from the function selector and
     * survives the switch to UART2 RXD, so the line stays pulled up while the
     * UART owns it -- that is what makes a pad with nothing plugged into it
     * read high instead of floating.
     */
    gpio_pin_configure_dt(&button_ptt, GPIO_INPUT);

    if (uart) {
        IOMuxManager_PinConfigure(CSK_IOMUX_PAD_A, 15, CSK_IOMUX_FUNC_ALTER3);
        /*
         * ...and the pull-up once more, after the function switch.  The pad
         * mode bits (16..18: PULL_DN, PULL_UP, PULL_FRC) live in the same
         * register as the function selector and IOMuxManager_PinConfigure()
         * only rewrites bits 0..4, but the rest level of the line is the one
         * thing this mode cannot get wrong, so it is asserted last and by
         * name rather than by surviving an unrelated write.
         */
        IOMuxManager_ModeConfigure(CSK_IOMUX_PAD_A, 15,
                                   HAL_IOMUX_PULLUP_MODE);
    }

    /* A press cannot survive the change of owner: whatever was held belongs to
     * the old mode, and the new one has to see a release before it keys. */
    port_ptt_reset(&portPtt);
}

bool platform_getPttStatus()
{
#if defined(C62_REMOTE) || defined(CONFIG_C62_BENCH_PROTO)
    if (pttOverride >= 0)
        return pttOverride != 0;
#endif

#if defined(C62_REMOTE)
    /* The pad is the remote control's UART RX and there is no button to read. */
    return false;
#else
    const uint8_t mode = c62_portMode();
    const portPttPlan_t plan = port_ptt_plan(mode);

    /*
     * Never read the raw level in "Auto": while the protocol listens the pad is
     * the UART's, its GPIO input does not follow the line, and every byte the
     * client sends dips it anyway.  There the answer is the detector's, only.
     */
    if (plan.useDetector) {
#ifdef C62_PC_PROTO
        /*
         * The transport thread is the one context allowed to drain the UART,
         * and the press evidence lives in the characters it frames.  Draining
         * from here would steal the client's bytes, so this reads the verdict
         * and nothing else.  A transport that stops therefore stops the
         * accessory PTT with it -- pc_proto_c62_supervise() says so in the log.
         */
        return port_ptt_keyed(mode, port_ptt_pressed(&portPtt), false);
#else
        /*
         * Without the PC protocol nobody muxes the pad: it is a plain GPIO with
         * its pull-up, so here the level *is* the evidence, and the 30 ms rule
         * still keeps a bounce from keying.
         */
        port_ptt_update(&portPtt, c62_portPadLevel(),
                        (uint32_t)k_uptime_get_32());
        return port_ptt_keyed(mode, port_ptt_pressed(&portPtt), false);
#endif
    }

    if (plan.useButton)
        return port_ptt_keyed(mode, false, c62_portPadLevel());

    return false;
#endif
}

bool platform_pwrButtonStatus()
{
    /* Return true if battery voltage is above 3V.
     * If a voltage lower than 3V would be used for comparison,
     * the radio would take longer to detect the power off when
     * the charger is connected.
    */
    return platform_getVbat() > 3000;
}

void c62_setKeyBacklight(bool on)
{
    gpio_pin_set_dt(&led_keyboard, on ? 1 : 0);
}

void platform_ledOn(led_t led)
{
    switch (led) {
        case WHITE:
            gpio_pin_set_dt(&led_white, 1);
            break;
        case GREEN:
            gpio_pin_set_dt(&led_green, 1);
            break;
        case RED:
            // Controlled by PA activation
            break;
        default:
            break;
    }
}

void platform_ledOff(led_t led)
{
    switch (led) {
        case WHITE:
            gpio_pin_set_dt(&led_white, 0);
            break;
        case GREEN:
            gpio_pin_set_dt(&led_green, 0);
            break;
        case RED:
            // Controlled by PA dactivation
            break;
        default:
            break;
    }
}

void platform_beepStart(uint16_t freq)
{
    /*
     * No BK4819 beeps: they come out of its AF output, which the speaker
     * mux doesn't select, and bk4819_beepStart() stops RX until the next
     * retune.
     */
    (void)freq;
}

void platform_beepStop()
{
}

const hwInfo_t *platform_getHwInfo()
{
    return &hwInfo;
}
