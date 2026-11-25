/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost harness for the spectrum analyser: bare metal on QEMU (mps2-an505,
 * Cortex-M33), instructions counted with the TCG plugin of
 * work/dsp-probe/common/insnframe.c between frame markers.
 *
 * What it measures is one point of a sweep, built from the real code the radio
 * runs: bk4819_setFrequency() (two register writes, REG_39 then REG_38),
 * bk4819_getRssi() (one read of REG_67) and spectrum_store() (the engine), on
 * top of a model of the Zephyr bit-banged SPI the C62 uses. The model is not a
 * guess: its inner loop is the one in zephyr/drivers/spi/spi_bitbang.c
 * (mosi set, k_busy_wait(wait_us), clock edge, k_busy_wait(wait_us), clock
 * edge, read), with wait_us = 2, which is what
 * (1000000 * 2000 / 200000) / 2000 / 2 truncates to for the 200 kHz the C62
 * asks for: 4 us per bit, not 5. A 3-byte transfer is therefore 24 bits and
 * 96 us of bus time, and that is reported as wall clock next to the
 * instructions, because k_busy_wait() is time the core spends waiting, not
 * work it does.
 *
 * The BK4819 is modelled too: the register file keeps REG_39/38 (so the mock
 * chip knows what it is tuned to) and answers REG_67 from a synthetic band with
 * two carriers, so the trace the harness prints is a real sweep of the engine
 * against a front end that exists only here.
 *
 *   RTX=/home/n0p/src/c62/work/spectrum
 *   D=/home/n0p/src/c62/work/dsp-probe
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 \
 *     -mfloat-abi=hard -O2 -ffunction-sections -fdata-sections \
 *     -I$RTX/openrtx/include -I$RTX/platform \
 *     tests/platform/c62_spectrum_cost.c \
 *     $RTX/openrtx/src/core/spectrum.c \
 *     $RTX/platform/drivers/baseband/BK4819.c \
 *     -T $D/common/mps2.ld -specs=rdimon.specs -Wl,--gc-sections \
 *     $D/common/startup.o -lrdimon -o /tmp/speccost.elf
 *   INSNHIST_MARKER=$(arm-none-eabi-nm /tmp/speccost.elf |
 *                     awk '$3=="frame_marker"{print $1}')
 *   INSNHIST_OUT=/tmp/h qemu-system-arm -M mps2-an505 -nographic \
 *     -semihosting-config enable=on,target=native -kernel /tmp/speccost.elf \
 *     -plugin file=$D/common/libinsnframe.so -d plugin
 *
 * The histogram holds one "TOTAL" line per marker; the difference between two
 * consecutive lines is the instructions of that block.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/spectrum.h"
#include "drivers/baseband/BK4819.h"
#include "peripherals/spi.h"

extern void initialise_monitor_handles(void);

/* newlib + -specs=rdimon: the delay between two points, microseconds */
#define SETTLE_US   400U

/* Bins swept in the measured block: the default span and step of the screen */
#define SPAN_HZ  1000000U
#define STEP_HZ    25000U

/* Sweeps measured: enough for the marker histogram to be readable */
#define SWEEPS 10

__attribute__((noinline, used)) void frame_marker(void);
void frame_marker(void) { __asm__ volatile("" ::: "memory"); }
#define MARK() frame_marker()

/* ---- what the CPU spends waiting instead of working -------------------- */

static volatile uint32_t busyUs;

/*
 * k_busy_wait() of the Zephyr bit-bang driver. On the radio it is a calibrated
 * loop that burns the core for the whole period; in the emulator it returns at
 * once, so its microseconds are accumulated and reported separately, next to
 * the instruction count. delayUs() is the same kind of wait (a busy wait below
 * one millisecond on this SoC).
 */
static void busyWait(unsigned int useconds)
{
    busyUs += useconds;
}

void delayUs(unsigned int useconds) { busyWait(useconds); }
void delayMs(unsigned int mseconds) { (void)mseconds; }

/* ---- the BK4819, as far as a sweep can tell ---------------------------- */

static uint16_t regs[256];
static uint16_t mockRead(uint8_t reg);
static uint32_t transferCount;

/*
 * The band the mock chip is looking at: a noise floor and two carriers, with
 * the 15 dB per bin skirt of one IF filter (~9 kHz in the wide profile) across
 * a 25 kHz raster. Frequencies are the ones the harness sweeps.
 */
static int16_t mockLevel(uint32_t freq)
{
    static const struct { uint32_t hz; int16_t dbm; } carriers[] = {
        { 145525000U, -68 },
        { 145200000U, -91 },
    };

    int16_t level = -105;

    for (size_t i = 0; i < sizeof(carriers) / sizeof(carriers[0]); i++)
    {
        uint32_t d = (freq > carriers[i].hz) ? (freq - carriers[i].hz)
                                            : (carriers[i].hz - freq);
        uint32_t bins = d / STEP_HZ;

        if (bins > 3)
            continue;

        int16_t candidate = (int16_t)(carriers[i].dbm - (int16_t)(15 * bins));

        if (candidate > level)
            level = candidate;
    }

    return level;
}

/* ---- the bus and the chip --------------------------------------------- */


/*
 * One transfer of the bit-banged bus, with the structure of
 * zephyr/drivers/spi/spi_bitbang.c: half duplex, the data pin reconfigured for
 * the direction, then two clock edges and two k_busy_wait(wait_us) per bit,
 * with wait_us = 2 for the 200 kHz the C62 asks for (the driver's own division
 * truncates 5/2, so the real bit period is 4 us).
 */
static uint8_t lastCmd;
static uint8_t pending[2];
static uint8_t pendingBytes;

static int bitBangTransfer(const struct spiDevice *dev, const void *txBuf,
                           void *rxBuf, const size_t size)
{
    (void)dev;

    const uint8_t *tx = (const uint8_t *)txBuf;
    uint8_t *rx = (uint8_t *)rxBuf;

    transferCount++;

    /* gpio_pin_configure_dt(): one direction change per transfer */
    volatile int reconfigured = 1;
    (void)reconfigured;

    /*
     * The protocol of the chip: one command byte (address | 0x80 to read), then
     * either two bytes of data going out or two bytes coming back.
     */
    if ((tx != NULL) && (size == 1))
    {
        lastCmd = tx[0];
        pendingBytes = 0;
    }
    else if ((tx != NULL) && (size == 2) && ((lastCmd & 0x80) == 0))
    {
        regs[lastCmd & 0x7F] = (uint16_t)((tx[0] << 8) | tx[1]);
        pendingBytes = 0;
    }
    else if ((rx != NULL) && (size == 2))
    {
        const uint16_t value = mockRead((uint8_t)(lastCmd & 0x7F));
        pending[0] = (uint8_t)(value >> 8);
        pending[1] = (uint8_t)(value & 0xFF);
        pendingBytes = 2;
    }

    for (size_t byte = 0; byte < size; byte++)
    {
        uint8_t in = 0;
        uint8_t out = (tx != NULL) ? tx[byte]
                                   : ((pendingBytes > byte) ? pending[byte] : 0);

        for (int bit = 7; bit >= 0; bit--)
        {
            volatile int d = (out >> bit) & 1;    /* mosi set */
            (void)d;

            busyWait(2);                          /* k_busy_wait(wait_us) */

            /* first clock edge, sampled on the falling one */
            if (rx != NULL)
                in = (uint8_t)((in << 1) | ((out >> bit) & 1));

            busyWait(2);                          /* k_busy_wait(wait_us) */

            /* second clock edge */
        }

        if (rx != NULL)
            rx[byte] = in;
    }

    return 0;
}

/* ---- the BK4819, and the GPIO stubs it needs --------------------------- */

static int  fakeMode(const struct gpioDev *dev, const uint8_t pin,
                     const uint16_t mode)
{
    (void)dev; (void)pin; (void)mode;
    return 0;
}

static void fakeSet(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev; (void)pin;
}

static void fakeClear(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev; (void)pin;
}

static bool fakeRead(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev; (void)pin;
    return false;
}

static const struct gpioApi fakeApi = {
    .mode = fakeMode, .set = fakeSet, .clear = fakeClear, .read = fakeRead
};

static const struct gpioDev fakePort = { .api = &fakeApi, .priv = NULL };

static const struct spiDevice mockSpi = {
    .transfer = bitBangTransfer,
    .priv     = NULL,
    .mutex    = NULL,
};

static const struct BK4819 chip = {
    .spi = &mockSpi,
    .scn = { .port = &fakePort, .pin = 8 },
};

/* ---- the sweep --------------------------------------------------------- */

static spectrum_t scan;

/*
 * REG_67 read back: the real chip answers with ((dBm + 160) * 2). The mock
 * derives the frequency from the PLL registers so that the level the engine
 * stores depends on what the sweep really wrote, not on what it meant to.
 */
static uint16_t mockRead(uint8_t reg)
{
    if (reg == 0x67)
    {
        uint32_t freq = ((((uint32_t)regs[0x39] & 0x7FFFU) << 16)
                         | regs[0x38]) * 10U;
        int16_t dbm = mockLevel(freq);
        return (uint16_t)((dbm + 160) * 2);
    }

    return regs[reg];
}

int main(void)
{
    initialise_monitor_handles();

    const uint32_t bins = spectrum_configure(&scan, 145500000U, SPAN_HZ, STEP_HZ)
                              ? spectrum_bins(&scan)
                              : 0;

    if (bins == 0)
    {
        printf("raster failed\n");
        return 1;
    }

    printf("spectrum cost: %u bins, %u Hz step, %u us settle\n",
           (unsigned)bins, (unsigned)scan.scan.step, (unsigned)SETTLE_US);

    /* Warm up: the first sweep pays nothing extra, but keep it out of the count */
    spectrum_startSweep(&scan);
    for (uint16_t i = 0; i < bins; i++)
    {
        bk4819_setFrequency(&chip, spectrum_binFreq(&scan, i));
        busyWait(SETTLE_US);
        spectrum_store(&scan, bk4819_getRssi(&chip));
    }

    /*
     * Three measured blocks, each one between two markers: a single point, a
     * single bus transfer, and a whole sweep (the unit the screen asks for).
     * The histogram of the TCG plugin gives the instructions of each block; the
     * bus time and the settle are counted here, because on the radio they are
     * time the core spends waiting rather than working.
     */
    uint32_t pointTransfers;
    uint32_t pointBusUs;

    busyUs = 0;
    transferCount = 0;
    MARK();
    bk4819_setFrequency(&chip, 145500000U);
    busyWait(SETTLE_US);
    (void)bk4819_getRssi(&chip);
    MARK();
    pointTransfers = transferCount;
    pointBusUs = busyUs;

    uint32_t xferTransfers;
    uint32_t xferBusUs;

    busyUs = 0;
    transferCount = 0;
    uint8_t cmd = 0x67 | 0x80;
    uint8_t reply[2];
    MARK();
    spi_send(&mockSpi, &cmd, 1);
    spi_receive(&mockSpi, reply, 2);
    MARK();
    xferTransfers = transferCount;
    xferBusUs = busyUs;

    uint32_t sweepTransfers;
    uint32_t sweepBusUs;

    busyUs = 0;
    transferCount = 0;
    MARK();
    for (int sweep = 0; sweep < SWEEPS; sweep++)
    {
        spectrum_startSweep(&scan);

        for (uint16_t i = 0; i < bins; i++)
        {
            bk4819_setFrequency(&chip, spectrum_binFreq(&scan, i));
            busyWait(SETTLE_US);
            spectrum_store(&scan, bk4819_getRssi(&chip));
        }
    }
    MARK();
    sweepTransfers = transferCount;
    sweepBusUs = busyUs;

    printf("point: %u transfers, %u us of bus and settle\n",
           (unsigned)pointTransfers, (unsigned)pointBusUs);
    printf("transfer: %u calls, %u us for 3 bytes\n",
           (unsigned)xferTransfers, (unsigned)xferBusUs);
    printf("sweep: %u transfers, %u us each (%u us total)\n",
           (unsigned)(sweepTransfers / (bins * SWEEPS)),
           (unsigned)(sweepBusUs / (bins * SWEEPS)),
           (unsigned)(sweepBusUs / SWEEPS));
    printf("sweep: %u points/s at this settle\n",
           (unsigned)((bins * 1000000U) / (sweepBusUs / SWEEPS)));

    /* The trace the mock front end produced, so the scaling is visible */
    printf("trace:");
    for (uint16_t i = 0; i < bins; i += 4)
        printf(" %d", (int)spectrum_level(&scan, i));
    printf("\npeak: %d dBm at %lu Hz\n", (int)spectrum_peak(&scan),
           (unsigned long)spectrum_peakFreq(&scan));

    return 0;
}
