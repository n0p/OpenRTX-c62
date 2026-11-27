/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * SPDX-FileCopyrightText: Copyright 2024 Jamiexu (Jamie793)
 * SPDX-FileCopyrightText: Copyright 2024 Andrej (Tunas1337)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later AND MIT
 *
 * Based on Jamie793's MIT-licensed BK4819 driver for the Talkpod A36Plus,
 * https://github.com/Jamie793/A36Plus_Radio, as extended by Andrej in
 * https://github.com/OpenRTX/OpenRTX/pull/278.
 */

#include "drivers/baseband/BK4819.h"
#include "drivers/baseband/BK4819_priv.h"
#include "interfaces/delays.h"
#include "core/utils.h"

/*
 * Serialisation of the chip's serial bus.
 *
 * One register access is not one transfer: the command byte and the data share
 * a chip-select window (readRegChecked()/writeRegOn()), and a read-modify-write
 * is two accesses.  spi_acquire()/spi_release() only protect a single transfer,
 * and only when the device was given a mutex -- and on this target the mutex
 * cannot be the pthread_mutex_t of struct spiDevice, which is a plain uint32_t
 * under the C62's newlib.  So the exclusion lives here, around the whole
 * sequence, and the bus mutex of the SPI device stays NULL.
 *
 * Three contexts reach the chip on the radio: the RTX thread (everything
 * radio_*), the pcproto thread (the debug namespace's register read/write) and
 * the system workqueue (paCut() -> radio_disableRtx() -> REG_33).  Without
 * this, one context can slip its own transfer between the command and the data
 * of another, and the read comes from a different register than the one asked
 * for -- REG_30 and REG_33, the PA path, included.
 *
 * Two properties are deliberate:
 *  - recursive, because bk4819_maskSetRegister() takes it and then calls the
 *    two halves that take it again;
 *  - bounded, because what can hold it is a transfer that never completes
 *    (zephyrSpiBus_transfer() waits inside the driver), and the workqueue's
 *    cut must not park behind that: the access fails after
 *    BK4819_LOCK_TIMEOUT_MS and the teardown continues.
 *
 * Not built for the host tests, which drive the driver from one thread against
 * a fake bus.
 */
#ifdef __ZEPHYR__
#include <zephyr/kernel.h>

#define BK4819_LOCK_TIMEOUT_MS 50

static K_MUTEX_DEFINE(bk4819BusMutex);
static bool busLock(void)
{
    /* Nothing here is called from an interrupt today, and a mutex may not be
     * taken from one: skip the lock instead of faulting if that ever changes. */
    if (k_is_in_isr())
        return true;

    return k_mutex_lock(&bk4819BusMutex, K_MSEC(BK4819_LOCK_TIMEOUT_MS)) == 0;
}

static void busUnlock(void)
{
    if (!k_is_in_isr())
        k_mutex_unlock(&bk4819BusMutex);
}
#else
static bool busLock(void)
{
    return true;
}

static void busUnlock(void)
{
}
#endif

/*
 * Audio processing fields, with their values by enum bk4819_audioMode: voice
 * (the values bk4819_init() sets), then bypass, as the application note's
 * RF_EnterBypass() sets it.
 */
struct audioModeField {
    uint8_t reg;       ///< Register address.
    uint16_t mask;     ///< Fields set by the audio mode.
    uint16_t value[2]; ///< Field values, by audio mode.
};

static const struct audioModeField audioModeFields[] = {
    {
        .reg = 0x2B,
        .mask = BK4819_REG2B_AF_FILTERS_DISABLE,
        .value = { 0, BK4819_REG2B_AF_FILTERS_DISABLE, },
    },
    {
        .reg = 0x31,
        .mask = BK4819_REG31_COMPANDER | BK4819_REG31_VOX,
        .value = { 0, 0, },
    },
    {
        .reg = 0x47,
        .mask = BK4819_REG47_BYPASS,
        .value = { 0, BK4819_REG47_BYPASS, },
    },
    {
        .reg = 0x4B,
        .mask = BK4819_REG4B_ALC_DISABLE,
        .value = { 0, BK4819_REG4B_ALC_DISABLE, },
    },
    {
        .reg = 0x70,
        .mask = BK4819_REG70_TONE1_ENABLE | BK4819_REG70_TONE2_ENABLE,
        .value = { 0, 0, },
    },
    {
        .reg = 0x7E,
        .mask = BK4819_REG7E_TX_DCF_BW_MASK | BK4819_REG7E_RX_DCF_BW_MASK,
        .value = { BK4819_REG7E_TX_DCF_BW(BK4819_DEFAULT_TX_DCF_BW)
                       | BK4819_REG7E_RX_DCF_BW(BK4819_DEFAULT_RX_DCF_BW),
                   0, },
    },
};

/*
 * REG_43 filters by enum bk4819_bandwidth. 12.5 and 25 kHz are the 0x4048
 * and 0x3028 of the stock C62 firmware and of the UV-K5 firmware:
 * https://github.com/DualTachyon/uv-k5-firmware/blob/6f8afac8864e0349ecf8b10f91ce8f69273013db/driver/bk4819.c#L292-L299
 * 6.25 kHz is the UV-K5 custom firmware's:
 * https://github.com/egzumer/uv-k5-firmware-custom/blob/7607f0a4bd6203d1f06b70556fc1ce0d7399d6b3/driver/bk4819.c#L647-L660
 */
struct bandwidthFilter {
    uint8_t rfBw;     ///< RX RF filter bandwidth.
    uint8_t rfBwWeak; ///< RX RF filter bandwidth on weak signals.
    uint8_t txLpf2Bw; ///< TX audio LPF2 bandwidth.
};

static const struct bandwidthFilter bandwidthFilters[] = {
    [BK4819_BW_12K5] = { .rfBw = 4, .rfBwWeak = 0, .txLpf2Bw = 1, },
    [BK4819_BW_6K25] = { .rfBw = 3, .rfBwWeak = 3, .txLpf2Bw = 1, },
    [BK4819_BW_25K] = { .rfBw = 3, .rfBwWeak = 0, .txLpf2Bw = 0, },
};

/**
 * Convert a CTCSS tone to the REG_07 frequency word, in CTC1 mode, for a
 * 26 MHz crystal.
 *
 * @param freq: tone frequency, in 0.1 Hz.
 * @return REG_07 frequency word.
 */
static uint16_t ctcssWord(uint16_t freq)
{
    // 2064888 / 1000000 reduced by 8, so that the product fits 32 bits
    return ((uint32_t)freq * 258111) / 125000;
}

/**
 * Convert a beep frequency to the tone generator's units.
 *
 * @param freq: frequency, in Hz.
 * @return REG_71 value.
 */
static uint16_t scaleFreq(const uint16_t freq)
{
    return (((uint32_t)freq * 1048576u) + 50000u) / 100000u; /* with rounding */
}

/**
 * Which bus a transfer goes over.
 *
 * The chip takes up to 8 MHz on SCK (datasheet V1.0 §2.6, table 6), while the
 * radio talks to it at 200 kHz because no register access is in anyone's inner
 * loop. A sweep is the exception: 96 us for a 3-byte transfer, three times per
 * bin, is longer than the settling time it is meant to cover. The device
 * therefore carries a second SPI configuration, same pins and same protocol,
 * only a faster clock, used by the sweep and by nothing else, so a board that
 * turns out not to like the faster clock still has a working radio.
 */
static const struct spiDevice *busOf(const struct BK4819 *dev, bool fast)
{
    if (fast && (dev->spiFast != NULL))
        return dev->spiFast;

    return dev->spi;
}

/*
 * Read one register and say whether the transfer completed.
 *
 * The SPI layer returns a negative error code when the bus did not deliver
 * (spiZephyr_transfer() passes Zephyr's own), and this driver used to ignore
 * all three results while returning whatever was in an uninitialised stack
 * buffer.  A register read is the evidence the transmit path is built on --
 * radio_txPathOff() proves the PA is off by reading REG_33 -- so a failure has
 * to reach the caller, not become the value of the register.
 *
 * @return zero on success and the value in *out; a negative error code
 *         otherwise, with *out untouched.
 */
static int readRegChecked(const struct BK4819 *dev, const struct spiDevice *spi,
                          uint8_t reg, uint16_t *out)
{
    const uint8_t cmd = reg | BK4819_REG_READ;
    uint8_t rx[2] = { 0, 0 };
    int ret;

    if (!busLock())
        return -EBUSY;

    if (spi_acquire(spi) < 0) {
        busUnlock();
        return -EBUSY;
    }

    gpioPin_clear(&dev->scn);
    delayUs(1);

    ret = spi_send(spi, &cmd, 1);
    if (ret == 0)
        ret = spi_receive(spi, rx, sizeof(rx));

    /* Chip select goes back up whatever happened: left selected, the chip's
     * interface stays busy for whatever transfer comes next. */
    delayUs(1);
    gpioPin_set(&dev->scn);
    spi_release(spi);
    busUnlock();

    if (ret < 0)
        return ret;

    *out = ((uint16_t)rx[0] << 8) | rx[1];

    return 0;
}

/**
 * Read one register over the slow bus, without a failure channel.
 *
 * Used by the callers whose answer is a scalar (the S-meter, the glitch
 * counter): a failed read is a zero, which those keep as "no level" rather
 * than as a measurement.  Everything that acts on the value -- the register
 * read-modify-write, the PA latch, the debug protocol -- uses
 * bk4819_readRegChecked() instead.
 */
static uint16_t readRegOn(const struct BK4819 *dev, const struct spiDevice *spi,
                          uint8_t reg)
{
    uint16_t value = 0;

    (void)readRegChecked(dev, spi, reg, &value);

    return value;
}

static int writeRegOn(const struct BK4819 *dev, const struct spiDevice *spi,
                      uint8_t reg, uint16_t data)
{
    const uint8_t cmd = reg | BK4819_REG_WRITE;
    const uint8_t tx[2] = {
        (data >> 8) & 0xFF,
        data & 0xFF,
    };
    int ret;

    if (!busLock())
        return -EBUSY;

    if (spi_acquire(spi) < 0) {
        busUnlock();
        return -EBUSY;
    }

    gpioPin_clear(&dev->scn);
    delayUs(1);

    ret = spi_send(spi, &cmd, 1);
    if (ret == 0)
        ret = spi_send(spi, tx, sizeof(tx));

    delayUs(1);
    gpioPin_set(&dev->scn);
    spi_release(spi);
    busUnlock();

    return ret;
}

uint16_t bk4819_readReg(const struct BK4819 *dev, uint8_t reg)
{
    return readRegOn(dev, dev->spi, reg);
}

int bk4819_readRegChecked(const struct BK4819 *dev, uint8_t reg, uint16_t *out)
{
    return readRegChecked(dev, dev->spi, reg, out);
}

void bk4819_writeReg(const struct BK4819 *dev, uint8_t reg, uint16_t data)
{
    (void)writeRegOn(dev, dev->spi, reg, data);
}

int bk4819_maskSetRegister(const struct BK4819 *dev, uint8_t reg,
                           uint16_t mask, uint16_t value)
{
    uint16_t current = 0;
    int ret;

    /*
     * The whole read-modify-write is one transaction on the bus: without this
     * outer lock another context could write the register between this side's
     * read and its write, and the other context's bits would be thrown away.
     * Recursive, so the two accesses below take the same mutex again.
     */
    if (!busLock())
        return -EBUSY;

    ret = readRegChecked(dev, dev->spi, reg, &current);

    /*
     * A failed read still writes, with the unread bits taken as zero.
     *
     * This is the register path of REG_33, and REG_33 is what drives the PA,
     * the ALC and the LNA switches: the last chance to change a register that
     * may be keying must not be thrown away because the read before it failed.
     * A zero base also lands on the safe side of every register this function
     * touches -- REG_33 has all the pins deasserted and every pin an output,
     * REG_30 has the PA gain and the TX DSP off, REG_43 keeps the fields the
     * mask carries.  The error still reaches the caller, which is what decides
     * whether the result counts as proof.
     */
    int wr = writeRegOn(dev, dev->spi, reg, (current & ~mask) | (value & mask));

    busUnlock();

    return (ret < 0) ? ret : wr;
}

void bk4819_init(const struct BK4819 *dev)
{
    /* Configure CS as output, idle high */
    gpioPin_setMode(&dev->scn, OUTPUT);
    gpioPin_set(&dev->scn);

    /*
     * Init sequence of the A36Plus driver's bk4819_init(), except REG_36,
     * REG_47, REG_48 and REG_7D, which differ on the C62:
     * https://github.com/Jamie793/A36Plus_Radio/blob/530a6089c0dba2023c6b7aa9e13d10cd8128ba4a/Drivers/BK4819/bk4819.c#L123-L176
     */
    bk4819_writeReg(dev, 0x00, BIT(15)); // Soft Reset
    bk4819_writeReg(dev, 0x00, 0);       // Normal operation
    bk4819_writeReg(dev, 0x37, 0x1d0f);  // Power mode

    /* Rx AGC Gain */
    bk4819_writeReg(dev, 0x13, 0x3be);
    bk4819_writeReg(dev, 0x12, 0x37b);
    bk4819_writeReg(dev, 0x11, 0x27b);
    bk4819_writeReg(dev, 0x10, 0x7a);
    bk4819_writeReg(dev, 0x14, 0x19);

    bk4819_writeReg(dev, 0x49, 0x2a38);
    bk4819_writeReg(dev, 0x7B, 0x8420);
    bk4819_writeReg(dev, 0x7D, 0xe959); // Mic sensitivity
    bk4819_writeReg(dev, 0x48, 0xb3c1); // AF Gains
    bk4819_writeReg(dev, 0x1E, 0x4c58);
    bk4819_writeReg(dev, 0x1F, 0xa656);
    bk4819_writeReg(dev, 0x3E, 0xa037); // Band selection threshold
    bk4819_writeReg(dev, 0x3F, 0x07fe); // Interrupt enable
    bk4819_writeReg(dev, 0x2A, 0x7fff);
    bk4819_writeReg(dev, 0x28, 0x6b00); // Expander
    bk4819_writeReg(dev, 0x53, 59000);
    bk4819_writeReg(dev, 0x2C, 0x5705);
    bk4819_writeReg(dev, 0x4B, 0x7102); // ALC
    bk4819_setAudioMode(dev, BK4819_AUDIO_VOICE);
    bk4819_writeReg(dev, 0x77, 0x88ef);
    bk4819_writeReg(dev, 0x26, 0x13a0);
    bk4819_writeReg(dev, 0x4E, 0x6f15); // Squelch
    bk4819_writeReg(dev, 0x4F, 0x3f3e); // Ex-noise threshold for Squelch

    /* DTMF Symbol 0 - 15 Coefficient */
    bk4819_writeReg(dev, 0x09, 0x006f);
    bk4819_writeReg(dev, 0x09, 0x106b);
    bk4819_writeReg(dev, 0x09, 0x2067);
    bk4819_writeReg(dev, 0x09, 0x3062);
    bk4819_writeReg(dev, 0x09, 0x4050);
    bk4819_writeReg(dev, 0x09, 0x5047);
    bk4819_writeReg(dev, 0x09, 0x603a);
    bk4819_writeReg(dev, 0x09, 0x702c);
    bk4819_writeReg(dev, 0x09, 0x8041);
    bk4819_writeReg(dev, 0x09, 0x9037);
    bk4819_writeReg(dev, 0x09, 0xa025);
    bk4819_writeReg(dev, 0x09, 0xb017);
    bk4819_writeReg(dev, 0x09, 0xc0e4);
    bk4819_writeReg(dev, 0x09, 0xd0cb);
    bk4819_writeReg(dev, 0x09, 0xe0b5);
    bk4819_writeReg(dev, 0x09, 0xf09f);

    bk4819_writeReg(dev, 0x74, 0xfa02); // 3000Hz AF Response coefficient for Tx
    bk4819_writeReg(dev, 0x44, 0x8f88); // 300Hz AF Response coefficient for Tx
    bk4819_writeReg(dev, 0x45, 0x3201); // 300Hz AF Response coefficient for Rx
    bk4819_writeReg(dev, 0x28, 0x6b38); // Expander
    bk4819_writeReg(dev, 0x29, 0xb4cb); // Compress
    // Tx Output power Bias 0xdf = 2,8V PA Tuning 0xbf = Enable PACTKoutput + 7,26dBm
    bk4819_writeReg(dev, 0x36, 0xdfbf);
    bk4819_writeReg(dev, 0x47, 0x6040); // AF

    /*
     * Let the microphone input bias settle with only the mic ADC enabled,
     * before any audio starts: otherwise the mic AGC can stay at a low gain.
     */
    uint16_t power = bk4819_readReg(dev, 0x30);
    bk4819_writeReg(dev, 0x30, BK4819_REG30_MIC_ADC_ENABLE);
    delayMs(250);
    bk4819_writeReg(dev, 0x30, power);
}

void bk4819_setFrequency(const struct BK4819 *dev, uint32_t freq)
{
    freq = freq / 10; /* Convert to 10 Hz units */
    bk4819_writeReg(dev, 0x39, (freq >> 16) & 0xFFFF);
    bk4819_writeReg(dev, 0x38, freq & 0xFFFF);
}

void bk4819_tuneCached(const struct BK4819 *dev, uint32_t freq, uint32_t *word)
{
    const struct spiDevice *spi = busOf(dev, true);
    const uint32_t w = freq / 10U; /* 10 Hz units */

    /*
     * High half first and low half last, as bk4819_setFrequency() does: the
     * chip applies the word when the low half lands, so skipping the high write
     * when the field has not moved cannot leave the PLL on a half-updated word.
     */
    if ((word == NULL) || ((w >> 16) != (*word >> 16)))
        writeRegOn(dev, spi, 0x39, (uint16_t)((w >> 16) & 0xFFFF));

    writeRegOn(dev, spi, 0x38, (uint16_t)(w & 0xFFFF));

    if (word != NULL)
        *word = w;
}

void bk4819_enableRx(const struct BK4819 *dev)
{
    const uint16_t paths =
        BK4819_REG30_VCO_CALIB_ENABLE | BK4819_REG30_RX_LINK_ENABLE
        | BK4819_REG30_AF_DAC_ENABLE | BK4819_REG30_DISC_MODE_ENABLE
        | BK4819_REG30_PLL_VCO_ENABLE | BK4819_REG30_RX_DSP_ENABLE;

    bk4819_writeReg(dev, 0x37, BK4819_REG37_ACTIVE);
    delayUs(1);
    bk4819_writeReg(dev, 0x30, BK4819_REG30_AF_DAC_ENABLE);
    bk4819_writeReg(dev, 0x30, paths);
}

void bk4819_enableTx(const struct BK4819 *dev)
{
    const uint16_t paths = BK4819_REG30_VCO_CALIB_ENABLE | BK4819_REG30_UNDOC14
                         | BK4819_REG30_DISC_MODE_ENABLE
                         | BK4819_REG30_PLL_VCO_ENABLE
                         | BK4819_REG30_PA_GAIN_ENABLE
                         | BK4819_REG30_MIC_ADC_ENABLE
                         | BK4819_REG30_TX_DSP_ENABLE;

    bk4819_writeReg(dev, 0x37, BK4819_REG37_ACTIVE);
    delayUs(1);
    bk4819_writeReg(dev, 0x30, 0x00); /* reset */
    bk4819_writeReg(dev, 0x30, paths);
}

void bk4819_disableRtx(const struct BK4819 *dev)
{
    bk4819_writeReg(dev, 0x30, 0x00); /* reset */
    bk4819_writeReg(dev, 0x30, BK4819_REG30_AF_DAC_ENABLE);
}

void bk4819_setModulation(const struct BK4819 *dev, bool fm)
{
    /*
     * The AM detector is an undocumented value of the AF output selector (7,
     * "reserved" in the V3 register list); the FM one is the documented normal
     * AF output.
     */
    bk4819_setAfOutput(dev, fm ? BK4819_AF_FM : BK4819_AF_AM);

    /* Top step of the RX AGC gain table, and the undocumented REG_4A */
    bk4819_writeReg(dev, 0x13, fm ? BK4819_REG13_AGC_TOP_FM
                                  : BK4819_REG13_AGC_TOP_AM);
    bk4819_writeReg(dev, 0x4A, fm ? BK4819_REG4A_RX_FM : BK4819_REG4A_RX_AM);

    /*
     * AM carries no pre-emphasis at the transmitter, so the receiver must not
     * de-emphasise it, and no compander either: the expander would undo part
     * of what the AGC does. Both filters stay as the FM path leaves them
     * otherwise (300 Hz HPF, 3 kHz LPF), the HPF being what removes the DC of
     * the detected carrier.
     */
    bk4819_maskSetRegister(dev, 0x2B, BK4819_REG2B_RX_DEEMPHASIS_DISABLE,
                           fm ? 0 : BK4819_REG2B_RX_DEEMPHASIS_DISABLE);
    bk4819_maskSetRegister(dev, 0x28, BK4819_REG28_EXPANDER_MASK,
                           fm ? BK4819_REG28_EXPANDER_1_2
                              : BK4819_REG28_EXPANDER_OFF);
}

void bk4819_setAmReceive(const struct BK4819 *dev, enum bk4819_amBw bw)
{
    /*
     * REG_43: 12.5 kHz mode with the 1.7 kHz IF is the stock firmware's AM
     * setting (0x0048), narrow enough for 8.33 kHz channel spacing. The wide
     * one is 25 kHz mode, where REG_43<5> = 1 doubles the 3 kHz IF to 6 kHz,
     * which fits the +-3 kHz of a 25 kHz AM channel.
     */
    const uint16_t reg43 = (bw == BK4819_AM_WIDE)
        ? BK4819_REG43_RF_BW(3) | BK4819_REG43_RF_BW_WEAK(3)
              | BK4819_REG43_AF_TX_LPF2_BW(0) | BK4819_REG43_BW_MODE(2)
              | BK4819_REG43_UNDOC3
        : BK4819_REG43_RF_BW(0) | BK4819_REG43_RF_BW_WEAK(0)
              | BK4819_REG43_AF_TX_LPF2_BW(1) | BK4819_REG43_BW_MODE(0)
              | BK4819_REG43_UNDOC3;

    bk4819_writeReg(dev, 0x43, reg43);
    bk4819_setModulation(dev, false);
}

void bk4819_setAfOutput(const struct BK4819 *dev, enum bk4819_afOutput af)
{
    bk4819_maskSetRegister(dev, 0x47, BK4819_REG47_AF_OUTPUT_MASK,
                           BK4819_REG47_AF_OUTPUT(af));
}

void bk4819_enableTxCtcss(const struct BK4819 *dev, uint16_t freq, uint8_t gain)
{
    const uint16_t mask = BK4819_REG51_CXCSS_TX_ENABLE | BK4819_REG51_CTCSS_MODE
                        | BK4819_REG51_CXCSS_TX_GAIN_MASK;
    const uint16_t value = BK4819_REG51_CXCSS_TX_ENABLE
                         | BK4819_REG51_CTCSS_MODE
                         | BK4819_REG51_CXCSS_TX_GAIN(gain);

    bk4819_maskSetRegister(dev, 0x51, mask, value);
    bk4819_writeReg(dev, 0x07, ctcssWord(freq));
}

void bk4819_enableRxCtcss(const struct BK4819 *dev, uint16_t freq)
{
    bk4819_maskSetRegister(dev, 0x51, BK4819_REG51_CTCSS_MODE,
                           BK4819_REG51_CTCSS_MODE);
    bk4819_writeReg(dev, 0x07, ctcssWord(freq));
}

void bk4819_enableTxCdcss(const struct BK4819 *dev, uint8_t negative,
                          uint8_t bits24, uint32_t code, uint8_t gain)
{
    const uint16_t mode = BK4819_REG07_MODE_CDCSS
                        | BK4819_REG07_CDCSS_BAUD_134_4HZ;
    const uint16_t high = BK4819_REG08_CDCSS_HIGH
                        | ((code >> 12) & BK4819_REG08_CDCSS_CODE_MASK);
    const uint16_t low = code & BK4819_REG08_CDCSS_CODE_MASK;
    uint16_t reg = BK4819_REG51_CXCSS_TX_ENABLE
                 | BK4819_REG51_CXCSS_TX_GAIN(gain);

    if (negative != 0)
        reg |= BK4819_REG51_TX_CDCSS_NEGATIVE;
    if (bits24 != 0)
        reg |= BK4819_REG51_CDCSS_24BIT;

    bk4819_writeReg(dev, 0x51, reg);
    bk4819_writeReg(dev, 0x07, mode);
    bk4819_writeReg(dev, 0x08, high);
    bk4819_writeReg(dev, 0x08, low);
}

void bk4819_disableCtdcss(const struct BK4819 *dev)
{
    bk4819_maskSetRegister(dev, 0x51, BK4819_REG51_CXCSS_TX_ENABLE, 0);
}

bool bk4819_getCtcss(const struct BK4819 *dev)
{
    return (bk4819_readReg(dev, 0x0C) & BK4819_REG0C_CTC1_RECEIVED) != 0;
}

void bk4819_enableVox(const struct BK4819 *dev, uint8_t delay, uint8_t interval,
                      uint16_t thresholdOn, uint16_t thresholdOff)
{
    const uint16_t timing = BK4819_REG79_VOX_INTERVAL(interval) | thresholdOff;

    (void)delay;
    bk4819_maskSetRegister(dev, 0x31, BK4819_REG31_VOX, BK4819_REG31_VOX);
    bk4819_writeReg(dev, 0x79, timing);
    bk4819_writeReg(dev, 0x46, thresholdOn);
}

bool bk4819_getVox(const struct BK4819 *dev)
{
    return (bk4819_readReg(dev, 0x0C) & BK4819_REG0C_VOX) != 0;
}

void bk4819_setSquelch(const struct BK4819 *dev, uint8_t rssiOpen,
                       uint8_t rssiClose, uint8_t noiseOpen, uint8_t noiseClose,
                       uint8_t glitchOpen, uint8_t glitchClose)
{
    const uint16_t rssi = BK4819_REG78_RSSI_OPEN(rssiOpen)
                        | BK4819_REG78_RSSI_CLOSE(rssiClose);
    const uint16_t noiseMask = BK4819_REG4F_NOISE_CLOSE_MASK
                             | BK4819_REG4F_NOISE_OPEN_MASK;
    const uint16_t noise = BK4819_REG4F_NOISE_CLOSE(noiseClose)
                         | BK4819_REG4F_NOISE_OPEN(noiseOpen);

    bk4819_writeReg(dev, 0x78, rssi);

    // Only the thresholds: timing and reserved bits are kept
    bk4819_maskSetRegister(dev, 0x4F, noiseMask, noise);
    bk4819_maskSetRegister(dev, 0x4D, BK4819_REG4D_GLITCH_CLOSE_MASK,
                           BK4819_REG4D_GLITCH_CLOSE(glitchClose));
    bk4819_maskSetRegister(dev, 0x4E, BK4819_REG4E_GLITCH_OPEN_MASK,
                           BK4819_REG4E_GLITCH_OPEN(glitchOpen));
}

void bk4819_setAudioMode(const struct BK4819 *dev, enum bk4819_audioMode mode)
{
    const bool bypass = (mode == BK4819_AUDIO_BYPASS);

    for (size_t i = 0; i < ARRAY_SIZE(audioModeFields); i++) {
        const struct audioModeField *field = &audioModeFields[i];
        bk4819_maskSetRegister(dev, field->reg, field->mask,
                               field->value[bypass]);
    }

    if (bypass)
        return;

    /*
     * Bypass leaves these to the caller: voice puts back the values of
     * bk4819_init(), and the radio then sets its channel's.
     */
    bk4819_setTxDeviation(dev, true, BK4819_DEFAULT_TX_DEVIATION);
    bk4819_setBandwidth(dev, BK4819_BW_12K5);
    bk4819_setRxAudioGain(dev, BK4819_DEFAULT_RX_AUDIO_GAIN1,
                          BK4819_DEFAULT_RX_AUDIO_GAIN2);
    bk4819_setMicGain(dev, BK4819_DEFAULT_MIC_GAIN);
}

void bk4819_setTxDeviation(const struct BK4819 *dev, bool enable,
                           uint16_t tuning)
{
    const uint16_t mask = BK4819_REG40_TX_DEVIATION_ENABLE
                        | BK4819_REG40_TX_DEVIATION_MASK;
    uint16_t value = BK4819_REG40_TX_DEVIATION(tuning);

    if (enable)
        value |= BK4819_REG40_TX_DEVIATION_ENABLE;

    bk4819_maskSetRegister(dev, 0x40, mask, value);
}

void bk4819_setRfFilter(const struct BK4819 *dev, uint8_t bw, uint8_t weakBw)
{
    const uint16_t mask = BK4819_REG43_RF_BW_MASK
                        | BK4819_REG43_RF_BW_WEAK_MASK;
    const uint16_t value = BK4819_REG43_RF_BW(bw)
                         | BK4819_REG43_RF_BW_WEAK(weakBw);

    bk4819_maskSetRegister(dev, 0x43, mask, value);
}

void bk4819_setTxAudioFilter(const struct BK4819 *dev, uint8_t bw)
{
    bk4819_maskSetRegister(dev, 0x43, BK4819_REG43_AF_TX_LPF2_BW_MASK,
                           BK4819_REG43_AF_TX_LPF2_BW(bw));
}

void bk4819_setBandwidth(const struct BK4819 *dev, enum bk4819_bandwidth bw)
{
    /*
     * The enum indexes bandwidthFilters[] and it arrives from a mode profile
     * and, upstream of that, from a channel record: clamp it, so a value the
     * chip does not have becomes 12.5 kHz instead of a field written from
     * whatever is behind the table.
     */
    if ((unsigned)bw > (unsigned)BK4819_BW_25K)
        bw = BK4819_BW_12K5;

    const struct bandwidthFilter *filters = &bandwidthFilters[bw];
    const uint16_t mask = BK4819_REG43_RF_BW_MASK | BK4819_REG43_RF_BW_WEAK_MASK
                        | BK4819_REG43_AF_TX_LPF2_BW_MASK
                        | BK4819_REG43_BW_MODE_MASK | BK4819_REG43_UNDOC3;
    const uint16_t value = BK4819_REG43_RF_BW(filters->rfBw)
                         | BK4819_REG43_RF_BW_WEAK(filters->rfBwWeak)
                         | BK4819_REG43_AF_TX_LPF2_BW(filters->txLpf2Bw)
                         | BK4819_REG43_BW_MODE(bw) | BK4819_REG43_UNDOC3;

    bk4819_maskSetRegister(dev, 0x43, mask, value);
}

void bk4819_setRxAudioGain(const struct BK4819 *dev, uint8_t gain1,
                           uint8_t gain2)
{
    const uint16_t mask = BK4819_REG48_AF_RX_GAIN1_MASK
                        | BK4819_REG48_AF_RX_GAIN2_MASK;
    const uint16_t value = BK4819_REG48_AF_RX_GAIN1(gain1)
                         | BK4819_REG48_AF_RX_GAIN2(gain2);

    bk4819_maskSetRegister(dev, 0x48, mask, value);
}

void bk4819_setMicGain(const struct BK4819 *dev, uint8_t gain)
{
    bk4819_maskSetRegister(dev, 0x7D, BK4819_REG7D_MIC_GAIN_MASK,
                           BK4819_REG7D_MIC_GAIN(gain));
}

int16_t bk4819_getRssi(const struct BK4819 *dev)
{
    return ((bk4819_readReg(dev, 0x67) & BK4819_REG67_RSSI_MASK) / 2) - 160;
}

int16_t bk4819_getRssiFast(const struct BK4819 *dev)
{
    const uint16_t raw = readRegOn(dev, busOf(dev, true), 0x67)
                       & BK4819_REG67_RSSI_MASK;

    return (int16_t)((raw / 2) - 160);
}

uint8_t bk4819_getGlitch(const struct BK4819 *dev)
{
    return (uint8_t)(readRegOn(dev, busOf(dev, true), 0x63) & 0xFF);
}

uint16_t bk4819_getMicLevel(const struct BK4819 *dev)
{
    return (bk4819_readReg(dev, 0x6F) & BK4819_REG6F_AF_AMPLITUDE_MASK) * 2;
}

void bk4819_beepStart(const struct BK4819 *dev, uint16_t freq, bool tuningGain)
{
    const uint16_t tone = BK4819_REG70_TONE1_ENABLE
                        | BK4819_REG70_TONE1_GAIN(96);
    const uint16_t paths = BK4819_REG30_AF_DAC_ENABLE
                         | BK4819_REG30_DISC_MODE_ENABLE
                         | BK4819_REG30_TX_DSP_ENABLE;

    (void)tuningGain;
    bk4819_writeReg(dev, 0x50, 0x3B20);
    bk4819_setAfOutput(dev, BK4819_AF_BEEP);
    bk4819_writeReg(dev, 0x70, tone);
    bk4819_writeReg(dev, 0x71, scaleFreq(freq));
    bk4819_writeReg(dev, 0x30, 0);
    bk4819_writeReg(dev, 0x30, paths);
    bk4819_writeReg(dev, 0x50, 0x3B20);
}

void bk4819_beepStop(const struct BK4819 *dev)
{
    bk4819_writeReg(dev, 0x50, 0xBB20);
}
