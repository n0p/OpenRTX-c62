/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The C62 panel (ST7735R on SPI1), drawn with a deadline.
 *
 * The initialisation of the controller -- reset, sleep out, the register
 * sequence -- stays where it was: in the driver of the SDK, run at boot
 * (POST_KERNEL) and reached through display_blanking_off().  What this file
 * owns is the frame path, and it does not go through display_write() any more.
 *
 * Why: display_write() ends in spi_transceive(), which waits for the transfer
 * with k_sem_take(&ctx->sync, K_FOREVER).  When the transfer does not complete
 * -- and on this board it is started by the CSK6 SPI driver through a DMA
 * channel it picks from a shared pool, with the HAL's error swallowed by the
 * driver's transceive() -- the UI thread never comes back.  The trace of that
 * freeze is in docs/RX_AIRBAND.md; the deadline is in core/display_flush.h.
 *
 * So the frames are sent here, six transfers at a time (command + parameters
 * for CASET, for RASET, and the command followed by the pixels), each one
 * submitted asynchronously and waited for with a timeout.  A frame that misses
 * its deadline is dropped and counted; the thread that draws keeps drawing.
 */

#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <interfaces/display.h>
#include <core/blackbox.h>
#include <core/display_flush.h>
#include <hwconfig.h>
#include <errno.h>
#include <string.h>
#include <math.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display, LOG_LEVEL_DBG);

// ST7735R is a color display using RGB565 format (16 bits per pixel)
#define FB_SIZE (CONFIG_SCREEN_HEIGHT * CONFIG_SCREEN_WIDTH * 2)

/* The three opcodes a frame needs; the controller's init stays in the driver. */
#define ST7735R_CMD_CASET 0x2A
#define ST7735R_CMD_RASET 0x2B
#define ST7735R_CMD_RAMWR 0x2C

static const struct device *displayDev;

/*
 * The panel's bus and its DC line, taken straight from the device tree so they
 * are the same ones the driver initialised the controller with.
 *
 * Two identical bus specifications, and the pair is the recovery: the driver
 * skips reconfiguring the peripheral when the spi_config it is handed is the
 * same object it saw last (spi_context_configured() is a pointer comparison),
 * so forcing a *different* object through it re-runs SPI_Control() -- which
 * resets the controller and clears the vendor HAL's "busy" flag.  That is what
 * brings the panel back after a transfer was lost, without touching the SDK
 * and without a reboot.
 */
static const struct spi_dt_spec panelBusA =
    SPI_DT_SPEC_GET(DT_CHOSEN(zephyr_display),
                    SPI_OP_MODE_MASTER | SPI_WORD_SET(8));
static const struct spi_dt_spec panelBusB =
    SPI_DT_SPEC_GET(DT_CHOSEN(zephyr_display),
                    SPI_OP_MODE_MASTER | SPI_WORD_SET(8));

static const struct spi_dt_spec *panelBus = &panelBusA;

static const struct gpio_dt_spec panelDc =
    GPIO_DT_SPEC_GET(DT_CHOSEN(zephyr_display), cmd_data_gpios);

// Copy of the frame sent to the panel, filled before every transfer, so it
// can live in PSRAM (not zeroed at boot) to save SRAM.
__attribute__((section(".psram_section"))) static uint8_t shadowBuffer[FB_SIZE];

/* Completion of the transfer in flight, given by the driver's callback. */
static K_SEM_DEFINE(panelDone, 0, 1);
static volatile int panelStatus;

/* The last frame was dropped: used to mark the first one in the breadcrumbs. */
static bool panelDropping;

static void panelComplete(const struct device *dev, int status, void *user)
{
    (void)dev;
    (void)user;

    panelStatus = status;
    k_sem_give(&panelDone);
}

static uint32_t panelNowMs(void *ctx)
{
    (void)ctx;
    return k_uptime_get_32();
}

static int panelSubmit(void *ctx, bool isData, const void *buf, size_t len)
{
    (void)ctx;

    struct spi_buf txBuf = { .buf = (void *)buf, .len = len };
    struct spi_buf_set txBufs = { .buffers = &txBuf, .count = 1 };

    /*
     * A token left by a transfer that completed after its deadline would be
     * read as the completion of this one, so the semaphore starts empty on
     * every transfer: a late completion costs one dropped frame, never a false
     * "done" that would leave the panel half written.  The status starts as a
     * failure for the same reason: anything read without a fresh completion is
     * a failure, not a success.
     */
    k_sem_reset(&panelDone);
    panelStatus = -EIO;

    gpio_pin_set_dt(&panelDc, isData ? 0 : 1);

    int ret = spi_transceive_async(panelBus->bus, &panelBus->config, &txBufs,
                                   NULL, panelComplete, NULL);
    return (ret < 0) ? ret : 0;
}

static int panelWait(void *ctx, uint32_t timeoutMs)
{
    (void)ctx;

    if (k_sem_take(&panelDone, K_MSEC(timeoutMs)) != 0)
        return -EAGAIN;

    return (panelStatus == 0) ? 0 : -EIO;
}

static void panelAbandon(void *ctx)
{
    (void)ctx;

    /*
     * spi_release() balances the context lock the driver took for the
     * transfer that never completed (spi_context_unlock_unconditionally(), and
     * it also drives CS high), which is what keeps the *next* frame from
     * parking on that lock.
     */
    (void)spi_release(panelBus->bus, &panelBus->config);
    k_sem_reset(&panelDone);
}

static const displayFlushIo_t panelIo = {
    .submit = panelSubmit,
    .wait = panelWait,
    .abandon = panelAbandon,
    .nowMs = panelNowMs,
    .ctx = NULL,
};

/**
 * Send @p rowCount rows of RGB565 pixels starting at @p startRow.
 *
 * Same wire traffic as the SDK driver's write path: CASET, RASET, RAMWR and
 * the pixels, each command with its parameters as a separate transfer.
 */
static void panelWrite(uint16_t startRow, uint16_t rowCount,
                       const uint8_t *pixels)
{
    /*
     * The frame after a dropped one goes out through the other bus
     * specification, which is what makes the driver reconfigure -- and so
     * reset -- the peripheral and clear the HAL's busy flag.  Without it a lost
     * DMA leaves the controller thinking it is still transferring and every
     * later frame is dropped, even though the UI thread is alive.
     */
    if (panelDropping)
        panelBus = (panelBus == &panelBusA) ? &panelBusB : &panelBusA;

    const uint16_t lastCol = CONFIG_SCREEN_WIDTH - 1;
    const uint16_t lastRow = startRow + rowCount - 1;

    const uint8_t casetCmd = ST7735R_CMD_CASET;
    const uint8_t rasetCmd = ST7735R_CMD_RASET;
    const uint8_t ramwrCmd = ST7735R_CMD_RAMWR;
    const uint8_t caset[4] = { 0x00, 0x00,
                               (uint8_t)(lastCol >> 8), (uint8_t)(lastCol & 0xFF) };
    const uint8_t raset[4] = { (uint8_t)(startRow >> 8), (uint8_t)(startRow & 0xFF),
                               (uint8_t)(lastRow >> 8), (uint8_t)(lastRow & 0xFF) };

    const displayChunk_t chunks[6] = {
        { &casetCmd, 1, false },
        { caset, sizeof(caset), true },
        { &rasetCmd, 1, false },
        { raset, sizeof(raset), true },
        { &ramwrCmd, 1, false },
        { pixels, (size_t)rowCount * CONFIG_SCREEN_WIDTH * 2, true },
    };

    bool ok = display_flush_frame(&panelIo, chunks, 6, DISP_FLUSH_TIMEOUT_MS);

    /*
     * One breadcrumb per run of dropped frames, not one per frame: at 40 Hz a
     * wedged bus would otherwise flush the whole ring in 32 frames and lose
     * the mode change that led to it.
     */
    if (!ok && !panelDropping)
        bb_mark(BB_M_DISP_DROP);

    panelDropping = !ok;
}

void display_init()
{
    LOG_INF("Display init");
    displayDev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

    if (!device_is_ready(displayDev)) {
        LOG_ERR("Display device not ready");
        return;
    }

    if (!spi_is_ready_dt(panelBus)) {
        LOG_ERR("Panel SPI bus not ready");
        return;
    }

    if (gpio_pin_configure_dt(&panelDc, GPIO_OUTPUT) != 0)
        LOG_ERR("Panel DC line not available");

    // Turn display on
    display_blanking_off(displayDev);
    LOG_INF("Display init done");
}

void display_terminate()
{
    if (displayDev != NULL) {
        display_blanking_on(displayDev);
    }
}

void display_renderRows(uint8_t startRow, uint8_t endRow, void *fb)
{
    if (startRow >= CONFIG_SCREEN_HEIGHT || endRow >= CONFIG_SCREEN_HEIGHT
        || startRow > endRow) {
        return;
    }

    uint8_t *frameBuffer = (uint8_t *)fb;
    uint16_t rowSize = CONFIG_SCREEN_WIDTH * 2; // 2 bytes per pixel
    uint16_t rowCount = endRow - startRow + 1;

    // Copy data to shadow buffer with format conversion if needed
    // This example assumes input buffer is already in RGB565 format
    memcpy(shadowBuffer, frameBuffer + startRow * rowSize, rowSize * rowCount);

    panelWrite(startRow, rowCount, shadowBuffer);
}

void display_render(void *fb)
{
    uint16_t *frameBuffer = (uint16_t *)fb;
    uint16_t *shadowBuffer16 = (uint16_t *)shadowBuffer;

    // Process each 16-bit pixel
    for (size_t i = 0; i < FB_SIZE / sizeof(uint16_t); i++) {
        // Extract the 16-bit RGB565 value
        // Store in shadow buffer with byte order swapped
        // This fixes endianness issues which can cause color problems
        shadowBuffer16[i] = __builtin_bswap16(frameBuffer[i]);
    }

    panelWrite(0, CONFIG_SCREEN_HEIGHT, shadowBuffer);
}

void display_setBacklightLevel(uint8_t level)
{
    if (level > 100)
        level = 100;

    // Calculate duty cycle (1000us period = 1kHz) on pin A02
    uint32_t period_us = 1000; // 1kHz PWM frequency
    uint32_t adjusted_brightness_percent = 0;

    if (level > 0) {
        // Apply non-linear mapping for better low-end brightness control
        adjusted_brightness_percent = roundf(level * level / 105.0) + 5;
    }

    uint32_t pulse_us = (period_us * adjusted_brightness_percent) / 100;

    int ret = pwm_set_dt(&pwm_lcd_backlight, PWM_USEC(period_us),
                         PWM_USEC(pulse_us));

    if (ret < 0) {
        LOG_ERR("Failed to set display PWM: %d", ret);
    }

    /* The keypad LED has its own setting: c62_setKeyBacklight() */
}
