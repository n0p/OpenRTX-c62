/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Cost harness for the broadcast FM receiver: bare metal on QEMU
 * (mps2-an505, Cortex-M33), instructions counted with the TCG plugin of
 * work/dsp-probe/common/insnframe.c between frame markers.
 *
 * Broadcast FM has no software demodulator -- the BK1080 does the demodulation
 * and its audio goes straight to the amplifier -- so the only thing the CM33
 * does is drive the bit-banged serial bus and read status registers.  This
 * measures exactly that: one register write, one status read, and the channel
 * arithmetic of core/wfm.c.  With those numbers the cost of the mode at its
 * 10 Hz polling rate follows by multiplication.
 *
 * The GPIO layer is stubbed (a fake gpioApi whose read() returns 1, so every
 * ACK poll succeeds on its first read) and delayUs()/delayMs() are no-ops: the
 * figure is instructions, not the 10 us half-period of the clock, which is
 * wall-clock time the CPU spends idle in a delay, not work.
 *
 *   RTX=../rx-wfm
 *   D=../../work/dsp-probe            # mps2.ld, startup.o, libinsnframe.so
 *   arm-none-eabi-gcc -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 \
 *     -mfloat-abi=hard -O2 -ffunction-sections -fdata-sections \
 *     -I$RTX/openrtx/include -I$RTX/platform \
 *     tests/platform/c62_wfm_cost.c \
 *     $RTX/openrtx/src/core/wfm.c $RTX/platform/drivers/baseband/BK1080.c \
 *     $RTX/platform/drivers/I2C/i2c_bitbang.c \
 *     -T $D/common/mps2.ld -specs=rdimon.specs -Wl,--gc-sections \
 *     $D/common/startup.o -lrdimon -o /tmp/wfmcost.elf
 *   INSNHIST_MARKER=$(arm-none-eabi-nm /tmp/wfmcost.elf |
 *                     awk '$3=="frame_marker"{print $1}')
 *   INSNHIST_OUT=/tmp/h qemu-system-arm -M mps2-an505 -nographic \
 *     -semihosting-config enable=on,target=native -kernel /tmp/wfmcost.elf \
 *     -plugin file=$D/common/libinsnframe.so -d plugin
 *
 * The histogram holds one "TOTAL" line per marker; the difference between two
 * consecutive lines is the instructions of that block.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/wfm.h"
#include "drivers/baseband/BK1080.h"
#include "drivers/I2C/i2c_bitbang.h"

/* newlib + -specs=rdimon: without this, printf goes nowhere */
extern void initialise_monitor_handles(void);

/* The mode polls the tuner this often; see radio_C62.cpp */
#define POLL_PER_SECOND 10

/* Iterations per measured block */
#define N 1000

__attribute__((noinline, used)) void frame_marker(void);
void frame_marker(void) { __asm__ volatile("" ::: "memory"); }
#define MARK() frame_marker()

/* ---- platform stubs ---------------------------------------------------- */

/*
 * The bit-banged bus waits with delayUs(), which on this SoC is a busy wait
 * below 1 ms (platform/mcu/CSK6011B/drivers/delays.c): the microseconds
 * accumulated here are CPU time, so they are reported next to the instruction
 * counts.  In the emulator they return at once, which keeps the two figures
 * separate.
 */
static volatile uint32_t busyUs;
static volatile uint32_t delayCalls;

void delayUs(unsigned int useconds) { busyUs += useconds; delayCalls++; }
void delayMs(unsigned int mseconds) { (void)mseconds; }

static int fakeMode(const struct gpioDev *dev, const uint8_t pin,
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

/*
 * SDA reads low: the chip acknowledges every byte, so the ACK poll of
 * i2cBitbang_writeByte ends on its first read and the transfer goes through.
 * (Reading high means "no ACK" and aborts the frame after the address byte,
 * which would measure one byte instead of a whole transfer.)  The data bytes
 * then read as zero, which is fine for a cost measurement.
 */
static bool fakeRead(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev; (void)pin;
    return false;
}

static const struct gpioApi fakeApi = {
    .mode = fakeMode, .set = fakeSet, .clear = fakeClear, .read = fakeRead
};

static const struct gpioDev fakePort = { .api = &fakeApi, .priv = NULL };
static const struct gpioPin sckPin = { .port = &fakePort, .pin = 5 };
static const struct gpioPin sdaPin = { .port = &fakePort, .pin = 6 };
static const struct gpioPin pwrPin = { .port = &fakePort, .pin = 2 };

static struct i2cBitbangState i2cState;
static const struct i2cBitbangCfg i2cCfg = {
    .sck = { .port = &fakePort, .pin = 5 },
    .sda = { .port = &fakePort, .pin = 6 },
    .clkPeriod = 2,            /* same as platform/targets/c62/hwconfig.c */
    .state = &i2cState,
};

I2C_BITBANG_DEVICE_DEFINE(wfmBus, i2cCfg, NULL)

static const struct BK1080 tuner = { .i2c = &wfmBus, .pwr = { &fakePort, 2 } };

static volatile uint32_t sink;

int main(void)
{
    struct bk1080Status st;
    char buf[12];

    (void)sckPin;
    (void)sdaPin;
    (void)pwrPin;

    initialise_monitor_handles();

    /*
     * Single operations first: the busy-wait figures below come from here,
     * the instruction counts from the identical loops further down (a loop of
     * one operation cannot be folded by the compiler, and N of them give a
     * per-operation average the plugin can measure between markers).
     */
    busyUs = 0;
    delayCalls = 0;
    BK1080_WriteRegister(&tuner, BK1080_REG_02_POWER_CONFIGURATION, 0x0201);
    printf("write2: busy_us=%lu half_delays=%lu\r\n", (unsigned long)busyUs,
           (unsigned long)delayCalls);

    busyUs = 0;
    delayCalls = 0;
    BK1080_ReadStatus(&tuner, &st);
    printf("status: busy_us=%lu half_delays=%lu\r\n", (unsigned long)busyUs,
           (unsigned long)delayCalls);
    sink += st.rssi;

    /* N x one 16-bit register write: one frame, two data bytes */
    MARK();
    for (int i = 0; i < N; i++) {
        BK1080_WriteRegister(&tuner, BK1080_REG_02_POWER_CONFIGURATION,
                             (uint16_t)i);
    }
    MARK();

    /* N x one status read: REG_07 (2 bytes) + REG_0A/0B (4 bytes) */
    MARK();
    for (int i = 0; i < N; i++) {
        BK1080_ReadStatus(&tuner, &st);
        sink += st.rssi + st.chan;
    }
    MARK();

    /* The arithmetic the mode does per poll: snap, channel, name, level */
    MARK();
    for (int i = 0; i < N; i++) {
        freq_t f = 85000000u + (freq_t)(i % 231) * WFM_STEP_HZ;
        f = wfm_snap(f, WFM_STEP_HZ);
        sink += wfm_chan(f);
        sink += (uint32_t)wfm_formatFreq(f, buf, sizeof(buf))[0];
        sink += (uint32_t)wfm_dbuVToDbm((uint8_t)(i % 76));
    }
    MARK();

    /* A whole-band channel conversion, for the seek/scan arithmetic */
    MARK();
    for (int i = 0; i < 231; i++)
        sink += wfm_chan(wfm_chanFreq((uint16_t)i));
    MARK();

    printf("done %lu\r\n", (unsigned long)sink);
    return 0;
}
