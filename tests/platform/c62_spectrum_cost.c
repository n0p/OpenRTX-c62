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
 * runs: bk4819_setFrequency() / bk4819_tuneCached() for the PLL word,
 * bk4819_getRssi() / bk4819_getRssiFast() for the level, bk4819_getGlitch() for
 * the gate, and spectrum_store() for the engine, on top of a model of the
 * bit-banged SPI the C62 uses.
 *
 * The bus model is not a guess: its inner loop is the one in
 * zephyr/drivers/spi/spi_bitbang.c (mosi set, k_busy_wait(wait_us), clock edge,
 * k_busy_wait(wait_us), clock edge, read), with
 *
 *   wait_us = (1000000 * 2000 / frequency) / 2000 / 2
 *
 * which is the driver's own truncating division. At the radio's 200 kHz it
 * gives 2 us per half period and 4 us per bit, so a 3-byte access is 24 bits and
 * 96 us of bus. Above 1 MHz it truncates to zero and the cost of a bit is the
 * GPIO toggling alone: three gpio_pin_set_dt() calls per bit, which on this core
 * with the Zephyr STM32 driver is about 30 cycles each. That figure is an
 * estimate -- the driver is not part of this image -- so it is charged as
 * FAST_BIT_CYCLES and reported apart from the instructions, and everything else
 * here is counted or modelled from the chip's own documentation.
 *
 * The BK4819 is modelled too, and the model is the point of the exercise: it
 * keeps REG_39/38 (so the mock chip knows what it is tuned to), REG_67 with a
 * settling time (the RSSI holds the previous bin until the IF has recovered)
 * and REG_63 (the glitch indicator, which saturates over the same window). That
 * is what makes it possible to say, for a given settling time of the real chip,
 * which variant is fast and which is merely wrong: a variant that reads the RSSI
 * before the chip is ready gets the previous bin's level, and the trace error is
 * what counts it.
 *
 *   RTX=/home/n0p/src/c62/work/sweep-fast
 *   D=/home/n0p/src/c62/work/dsp-probe
 *   O=/tmp/speccost
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 \
 *     -mfloat-abi=hard -O2 -ffunction-sections -fdata-sections \
 *     -I$RTX/openrtx/include -I$RTX/platform -I$RTX/tests/platform/qemu_stub \
 *     tests/platform/c62_spectrum_cost.c \
 *     $RTX/openrtx/src/core/spectrum.c \
 *     $RTX/platform/drivers/baseband/BK4819.c \
 *     -T $D/common/mps2.ld -specs=rdimon.specs -Wl,--gc-sections \
 *     $D/common/startup.o -lrdimon -o $O/speccost.elf
 *   INSNHIST_MARKER=$(arm-none-eabi-nm $O/speccost.elf |
 *                     awk '$3=="frame_marker"{print $1}')
 *   INSNHIST_OUT=$O/h qemu-system-arm -M mps2-an505 -nographic \
 *     -semihosting-config enable=on,target=native -kernel $O/speccost.elf \
 *     -plugin file=$D/common/libinsnframe.so -d plugin
 *
 * The dump holds one line per marker with the cumulative total: the difference
 * between two consecutive lines is the instruction count of that block.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/spectrum.h"
#include "drivers/baseband/BK4819.h"
#include "peripherals/spi.h"

extern void initialise_monitor_handles(void);

/* The raster of the screen's default span: 1 MHz in 41 bins of 25 kHz */
#define SPAN_HZ  1000000U
#define STEP_HZ    25000U

/*
 * CPU cost of one bit of the fast bus: three gpio_pin_set_dt() calls at about
 * 30 cycles each, charged as cycles of the core and converted with the core
 * clock. See the header comment for why it is an estimate.
 */
#define FAST_BIT_CYCLES 90U

/*
 * Same, for the fixed part of a bus transfer: spi_acquire(), the spi_config the
 * driver rebuilds on every call, spi_context setup, and the 64-bit divisions
 * spi_bitbang_configure() does to turn the frequency into a per-bit delay. About
 * 450 cycles, also an estimate.
 */
#define FAST_XFER_CYCLES 450U

/* Long settle used as the reference of what each bin really contains */
#define REFERENCE_SETTLE_US 2000U

__attribute__((noinline, used)) void frame_marker(void);
void frame_marker(void) { __asm__ volatile("" ::: "memory"); }
#define MARK() frame_marker()

/* ---- the virtual clock, in microseconds -------------------------------- */

/*
 * Time as the chip sees it. Advanced by the bit-bang delays, by the settle and
 * by the gate's own waits; the CPU work of the bit loop is charged separately as
 * instructions. Everything the mock chip does is a function of this.
 */
static uint32_t vnow;

/* CPU cycles burned in the modelled GPIO/transfer overhead of the fast bus */
static volatile uint32_t busCycles;

static void busyWait(unsigned int useconds)
{
    vnow += useconds;
}

void delayUs(unsigned int useconds) { vnow += useconds; }
void delayMs(unsigned int mseconds) { vnow += (uint32_t)mseconds * 1000U; }

/* ---- the band the mock chip is looking at ------------------------------ */

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

/* ---- the chip ---------------------------------------------------------- */

static uint16_t regs[256];

/* Settling model: what the real chip is assumed to need after a retune. */
static uint32_t mockSettleUs;

/* The retune in flight: when it was written, the level before it, and after. */
static uint32_t tuneAt;
static int16_t  levelBefore;
static int16_t  levelAfter;

/* Fast bus: 0 when the configuration is the radio's 200 kHz one */
static int busFast;

/* Byte at the output of an SPI transfer */
static uint8_t lastCmd;
static uint8_t pending[2];
static uint8_t pendingBytes;
static uint32_t transferCount;

static uint16_t mockRead(uint8_t reg)
{
    if (reg == 0x67)
    {
        /* The IF has not recovered yet: the register still holds the old bin */
        const int16_t dbm = (vnow < (tuneAt + mockSettleUs)) ? levelBefore
                                                            : levelAfter;
        return (uint16_t)((dbm + 160) * 2);
    }

    if (reg == 0x63)
    {
        /* Glitch counter: saturated over the same window as the IF recovery */
        return (vnow < (tuneAt + mockSettleUs)) ? 0x00FFU : 0x0010U;
    }

    return regs[reg];
}

/* ---- the bus ----------------------------------------------------------- */

static int bitBangTransfer(const struct spiDevice *dev, const void *txBuf,
                           void *rxBuf, const size_t size)
{
    (void)dev;

    const uint8_t *tx = (const uint8_t *)txBuf;
    uint8_t *rx = (uint8_t *)rxBuf;
    const unsigned waitUs = busFast ? 0U : 2U;

    transferCount++;

    if (busFast)
        busCycles += FAST_XFER_CYCLES;

    /* gpio_pin_configure_dt(): one direction change per transfer */
    volatile int reconfigured = 1;
    (void)reconfigured;

    if ((tx != NULL) && (size == 1))
    {
        lastCmd = tx[0];
        pendingBytes = 0;
    }
    else if ((tx != NULL) && (size == 2) && ((lastCmd & 0x80) == 0))
    {
        const uint8_t reg = lastCmd & 0x7F;
        const uint16_t value = (uint16_t)((tx[0] << 8) | tx[1]);

        regs[reg] = value;

        /*
         * A write of the low half is what the chip acts on: it is the last one
         * of the pair, in this driver and in the UV-K5 firmware alike.
         */
        if (reg == 0x38)
        {
            const uint32_t freq = ((((uint32_t)regs[0x39] & 0x7FFFU) << 16)
                                   | regs[0x38]) * 10U;

            levelBefore = levelAfter;
            if (levelBefore == 0)
                levelBefore = mockLevel(freq);

            levelAfter = mockLevel(freq);
            tuneAt     = vnow;
        }
        else if (reg == 0x39)
        {
            const uint32_t freq = ((((uint32_t)regs[0x39] & 0x7FFFU) << 16)
                                   | regs[0x38]) * 10U;

            levelBefore = levelAfter;
            if (levelBefore == 0)
                levelBefore = mockLevel(freq);

            levelAfter = mockLevel(freq);
            tuneAt     = vnow;
        }

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

            busyWait(waitUs);

            if (rx != NULL)
                in = (uint8_t)((in << 1) | ((out >> bit) & 1));

            busyWait(waitUs);

            /* Two clock edges and, on the fast bus, three GPIO calls */
            if (busFast)
                busCycles += FAST_BIT_CYCLES;
        }

        if (rx != NULL)
            rx[byte] = in;
    }

    return 0;
}

/* ---- the GPIO stubs ---------------------------------------------------- */

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

static const struct spiDevice mockSlowSpi = {
    .transfer = bitBangTransfer, .priv = NULL, .mutex = NULL,
};

static const struct spiDevice mockFastSpi = {
    .transfer = bitBangTransfer, .priv = NULL, .mutex = NULL,
};

static const struct BK4819 chip = {
    .spi = &mockSlowSpi,
    .spiFast = &mockFastSpi,
    .scn = { .port = &fakePort, .pin = 8 },
};

/* ---- the variants ------------------------------------------------------ */

typedef struct
{
    const char *name;
    bool     fastBus;     ///< spiFast (the analyser's 1 MHz configuration)
    bool     cached;      ///< skip the high half of the PLL word
    uint32_t settleUs;    ///< fixed wait, 0 = poll REG_63
    uint8_t  gatePolls;   ///< polls of REG_63 when settleUs is 0
    uint8_t  gateStepUs;  ///< time between two of those polls
} variant_t;

static const variant_t variants[] = {
    { "V0 base (200 kHz, 400 us sleep)", false, false, 400U,  0,  0 },
    { "V1 fast bus (400 us sleep)",      true,  false, 400U,  0,  0 },
    { "V2 fast bus + half cache",        true,  true,  400U,  0,  0 },
    { "V3 + 200 us",                     true,  true,  200U,  0,  0 },
    { "V4 + 100 us",                     true,  true,  100U,  0,  0 },
    { "V5 + 50 us",                      true,  true,   50U,  0,  0 },
    { "V6 + no settle",                  true,  true,    0U,  0,  0 },
    { "V7 gate, 10 us steps",            true,  true,    0U, 40, 10 },
    { "V8 gate, 20 us steps",            true,  true,    0U, 20, 20 },
};

#define VARIANT_COUNT (sizeof(variants) / sizeof(variants[0]))

/*
 * One point, as the driver does it: the PLL word, the wait and the level.
 *
 * This mirrors radio_spectrumPoint() of platform/drivers/baseband/radio_C62.cpp
 * (without the LNA switch, which costs nothing inside a band). Keeping it here
 * instead of calling the driver keeps the harness buildable without Zephyr; the
 * calls it makes are the driver's own.
 */
static void sweepPoint(const variant_t *v, uint32_t freq, uint32_t *word,
                       int16_t *dbm, uint32_t *spentUs)
{
    const uint32_t t0 = vnow;

    if (v->cached)
        bk4819_tuneCached(&chip, freq, word);
    else
        bk4819_setFrequency(&chip, freq);

    if (v->settleUs > 0)
    {
        busyWait(v->settleUs);
    }
    else if (v->gatePolls > 0)
    {
        for (uint8_t i = 0; i < v->gatePolls; i++)
        {
            if (bk4819_getGlitch(&chip) < 255U)
                break;

            busyWait(v->gateStepUs);
        }
    }

    *dbm = v->fastBus ? bk4819_getRssiFast(&chip) : bk4819_getRssi(&chip);
    *spentUs = vnow - t0;
}

/* ---- the measurements -------------------------------------------------- */

static spectrum_t scan;

/* LEVELS[route][bin] of the reference sweep, per mock settling time */
static int16_t reference[SPECTRUM_MAX_BINS];

static uint32_t bins;

/**
 * Sweep the raster once with one variant, against a chip that settles in
 * mockSettle us.
 *
 * @param sweepUs: filled with the wall clock of the whole sweep, us.
 * @param worstErr: filled with the worst bin error against the reference,
 *                  in dB (the trace is in dBm).
 * @param badBins: filled with the number of bins more than 1 dB out.
 */
static void runVariant(const variant_t *v, uint32_t mockSettle,
                       uint32_t *sweepUs, int16_t *worstErr, uint16_t *badBins)
{
    busFast = v->fastBus;
    mockSettleUs = mockSettle;

    uint32_t word = 0;
    uint32_t total = 0;
    int16_t worst = 0;
    uint16_t bad = 0;

    spectrum_startSweep(&scan);

    for (uint16_t i = 0; i < bins; i++)
    {
        int16_t dbm;
        uint32_t spent;

        sweepPoint(v, spectrum_binFreq(&scan, i), &word, &dbm, &spent);
        spectrum_store(&scan, dbm);
        total += spent;
    }

    for (uint16_t i = 0; i < bins; i++)
    {
        int16_t err = (int16_t)(spectrum_level(&scan, i) - reference[i]);

        if (err < 0)
            err = (int16_t)-err;

        if (err > worst)
            worst = err;

        if (err > 2) /* more than 1 dB */
            bad++;
    }

    *sweepUs = total;
    *worstErr = worst;
    *badBins = bad;
}

int main(void)
{
    initialise_monitor_handles();

    bins = spectrum_configure(&scan, 145500000U, SPAN_HZ, STEP_HZ)
               ? spectrum_bins(&scan) : 0;

    if (bins == 0)
    {
        printf("raster failed\n");
        return 1;
    }

    printf("spectrum cost: %u bins, %u Hz step\n", (unsigned)bins,
           (unsigned)scan.scan.step);
    printf("bus: 200 kHz = 4 us/bit (2 us per half period, the driver's own "
           "division), fast = 0 + %u cycles/bit\n", (unsigned)FAST_BIT_CYCLES);

    /*
     * Reference: what every bin really contains, with a settling time long
     * enough that no variant can be right by accident (2 ms).
     */
    mockSettleUs = 0;
    {
        /* Warm start: the chip is tuned to the centre before anything runs */
        const uint32_t centreWord = scan.scan.centre / 10U;

        regs[0x39] = (uint16_t)((centreWord >> 16) & 0xFFFF);
        regs[0x38] = (uint16_t)(centreWord & 0xFFFF);
        levelAfter = mockLevel(scan.scan.centre);
        levelBefore = levelAfter;
        tuneAt = 0;
    }
    {
        const variant_t ref = { "ref", false, false, REFERENCE_SETTLE_US, 0 };
        uint32_t word = 0;

        spectrum_startSweep(&scan);

        for (uint16_t i = 0; i < bins; i++)
        {
            int16_t dbm;
            uint32_t spent;

            sweepPoint(&ref, spectrum_binFreq(&scan, i), &word, &dbm, &spent);
            reference[i] = dbm;
        }
    }

    /*
     * Two chip behaviours: one that settles in 150 us (a small step with the
     * synthesizer already locked, the case this work assumes) and the
     * datasheet's 300 us worst case. A variant is only a win if it is right in
     * both.
     */
    static const uint32_t chipSettling[] = { 150U, 300U };
    static const char *const chipNames[] = { "150 us", "300 us" };

    for (size_t c = 0; c < sizeof(chipSettling) / sizeof(chipSettling[0]); c++)
    {
        printf("\nmock chip settles in %s:\n", chipNames[c]);
        printf("  %-30s %8s %8s %8s %9s %7s %6s %5s\n", "variant", "wait/pto",
               "cpu/pto", "us/pto", "us/sweep", "pts/s", "err", "bad");

        for (size_t i = 0; i < VARIANT_COUNT; i++)
        {
            uint32_t sweep;
            int16_t worst;
            uint16_t bad;

            busCycles = 0;
            runVariant(&variants[i], chipSettling[c], &sweep, &worst, &bad);

            /* The GPIO overhead of the fast bus, as time at 300 MHz */
            const uint32_t cpuUs = busCycles / 3000U;

            printf("  %-30s %8u %8u %8u %9u %7u %6d %5u\n",
                   variants[i].name,
                   (unsigned)(sweep / bins),
                   (unsigned)(cpuUs / bins),
                   (unsigned)((sweep + cpuUs) / bins),
                   (unsigned)(sweep + cpuUs),
                   (unsigned)((bins * 1000000U) / (sweep + cpuUs)),
                   (int)worst, (unsigned)bad);
        }
    }

    printf("\ntrace (reference, every 4th bin):");
    for (uint16_t i = 0; i < bins; i += 4)
        printf(" %d", (int)reference[i]);
    printf("\n");

    return 0;
}
