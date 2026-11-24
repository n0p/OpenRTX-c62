/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drivers/baseband/BK1080.h"
#include "drivers/I2C/i2c_bitbang.h"
#include "interfaces/delays.h"
#include <stddef.h>

/*
 * 7-bit I2C-like address of the BK1080, as used by the chip's control byte
 * (0x80 = BK1080_I2C_ADDR << 1 | write bit).  The datasheet calls 0x80 the
 * "device ID"; it is not a standard I2C address and is why this bus needs the
 * bit-banged driver of drivers/I2C/i2c_bitbang.c rather than a hardware one.
 */
#define BK1080_I2C_ADDR 0x40U

/*
 * The 34-register configuration table of the stock C62 firmware, read out of
 * the factory image at 0x180a3514 (dumps/extract/app-cm33.bin): registers
 * 0x00 to 0x21, 16 bit big endian, written in one burst from register 0.
 * Only one value differs from the factory: REG_04 0x48C0 instead of 0x40C0,
 * which sets DE (bit 11) for the 50 us de-emphasis used in Europe instead of
 * the 75 us used in the US.  See docs/RX_WFM.md for the table decoded.
 */
static const uint8_t BK1080_InitTable[] = {
    0x00, 0x08, /* REG_00 reserved                                           */
    0x10, 0x80, /* REG_01 device ID (read only)                              */
    0x02, 0x01, /* REG_02 ENABLE                                             */
    0x00, 0x00, /* REG_03 channel, TUNE low                                  */
    0x48, 0xC0, /* REG_04 STCIEN, DE = 50 us, BLNDADJ 11                     */
    0x0A, 0x1F, /* REG_05 BAND 01, 100 kHz, volume 15                        */
    0x00, 0x2E, /* REG_06 soft mute default, SKSNR 2, SKCNT 14               */
    0x02, 0xFF, /* REG_07 factory value                                      */
    0x5B, 0x11, /* REG_08 factory value                                      */
    0x00, 0x00, /* REG_09 factory value                                      */
    0x41, 0x1E, /* REG_0A read only, factory value                           */
    0x00, 0x00, /* REG_0B read only, factory value                           */
    0xCE, 0x00, /* REG_0C factory value                                      */
    0x00, 0x00, /* REG_0D factory value                                      */
    0x00, 0x00, /* REG_0E factory value                                      */
    0x10, 0x00, /* REG_0F factory value                                      */
    0x31, 0x97, /* REG_10 Beken internal, write only                         */
    0x00, 0x00, /* REG_11 factory value                                      */
    0x13, 0xFF, /* REG_12 factory value                                      */
    0x98, 0x52, /* REG_13 factory value                                      */
    0x00, 0x00, /* REG_14 factory value                                      */
    0x00, 0x00, /* REG_15 factory value                                      */
    0x00, 0x08, /* REG_16 factory value                                      */
    0x00, 0x00, /* REG_17 factory value                                      */
    0x51, 0xE1, /* REG_18 factory value                                      */
    0x28, 0xBC, /* REG_19 (REG_25) pulsed by BK1080_Init()                   */
    0x26, 0x45, /* REG_1A factory value                                      */
    0x00, 0xE4, /* REG_1B factory value                                      */
    0x1C, 0xD8, /* REG_1C factory value                                      */
    0x3A, 0x50, /* REG_1D factory value                                      */
    0xEA, 0xE0, /* REG_1E factory value                                      */
    0x30, 0x00, /* REG_1F factory value                                      */
    0x02, 0x00, /* REG_20 factory value                                      */
    0x00, 0x00, /* REG_21 factory value                                      */
};

/* How long the chip has to answer on the bus after the power pin goes low */
static const unsigned int BK1080_POWER_POLL_MS  = 10;
static const unsigned int BK1080_POWER_POLL_MAX = 100;  /* 1 s, as the stock */
/* Calibration settling, both from the factory sequence */
static const unsigned int BK1080_CAL_SETTLE_MS  = 250;
static const unsigned int BK1080_CAL_FINISH_MS  = 60;
/* Tune and seek: STC comes up in about 60 ms, 200 ms is the guard */
static const unsigned int BK1080_STC_POLL_MS    = 5;
static const unsigned int BK1080_STC_POLL_MAX   = 40;

/**
 * \internal
 * Read one or more 16-bit registers.  The chip auto-increments the address,
 * so a burst read from REG_0A returns REG_0A and REG_0B.
 */
static int bk1080Read(const struct BK1080 *dev, uint8_t reg, uint8_t *buf,
                      size_t len)
{
    uint8_t ctrl = (uint8_t)((reg << 1) | 1U);
    int ret;

    i2c_acquire(dev->i2c);
    ret = i2c_write(dev->i2c, BK1080_I2C_ADDR, &ctrl, 1, false);
    if (ret == 0)
        ret = i2c_read(dev->i2c, BK1080_I2C_ADDR, buf, len, true);
    i2c_release(dev->i2c);

    return ret;
}

/**
 * \internal
 * Write one or more 16-bit registers, in one frame.
 */
static int bk1080Write(const struct BK1080 *dev, uint8_t reg,
                       const uint8_t *buf, size_t len)
{
    uint8_t frame[1 + 2 * 0x22];
    size_t n = (len > (sizeof(frame) - 1)) ? (sizeof(frame) - 1) : len;
    int ret;

    frame[0] = (uint8_t)(reg << 1);
    for (size_t i = 0; i < n; i++)
        frame[1 + i] = buf[i];

    i2c_acquire(dev->i2c);
    ret = i2c_write(dev->i2c, BK1080_I2C_ADDR, frame, n + 1, true);
    i2c_release(dev->i2c);

    return ret;
}

static int bk1080WriteReg(const struct BK1080 *dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[2] = { (uint8_t)(value >> 8), (uint8_t)value };

    return bk1080Write(dev, reg, buf, 2);
}

/**
 * \internal
 * Read a register for a read-modify-write.  A failed transfer reads as 0,
 * which sets the bits a setter wants to change and leaves the rest alone.
 */
static uint16_t bk1080ReadReg(const struct BK1080 *dev, uint8_t reg)
{
    uint8_t buf[2] = { 0, 0 };

    if (bk1080Read(dev, reg, buf, 2) != 0)
        return 0;

    return ((uint16_t)buf[0] << 8) | buf[1];
}

/**
 * \internal
 * Set or clear a mask of bits in a register, leaving the rest as it was.
 */
static void bk1080Update(const struct BK1080 *dev, uint8_t reg, uint16_t mask,
                         uint16_t value)
{
    uint16_t old = bk1080ReadReg(dev, reg);

    bk1080WriteReg(dev, reg, (uint16_t)((old & ~mask) | (value & mask)));
}

/**
 * \internal
 * Wait for the seek/tune complete flag.
 *
 * @return true if STC came up within the timeout.
 */
static bool bk1080WaitStc(const struct BK1080 *dev)
{
    for (unsigned int i = 0; i < BK1080_STC_POLL_MAX; i++) {
        if ((bk1080ReadReg(dev, BK1080_REG_0A_STATUS) &
             BK1080_REG_0A_STC) != 0)
            return true;

        delayMs(BK1080_STC_POLL_MS);
    }

    return false;
}

uint16_t BK1080_ReadRegister(const struct BK1080 *dev, uint8_t reg)
{
    return bk1080ReadReg(dev, reg);
}

void BK1080_WriteRegister(const struct BK1080 *dev, uint8_t reg, uint16_t value)
{
    bk1080WriteReg(dev, reg, value);
}

bool BK1080_Init(const struct BK1080 *dev)
{
    uint16_t id = 0;

    /*
     * The bus is bit-banged: its pins have to be outputs before the first
     * clock edge, and nothing else in the firmware initialises them.
     */
    i2cBitbang_init(dev->i2c);

    gpioPin_setMode(&dev->pwr, OUTPUT);
    gpioPin_clear(&dev->pwr);           /* power pin is active low */

    /*
     * The stock firmware waits a flat second here.  The chip answers as soon
     * as its reference clock is stable, so poll instead: normally the first
     * or second attempt succeeds, and the loop keeps the same one-second
     * bound for the boards where it does not.
     */
    for (unsigned int i = 0; i < BK1080_POWER_POLL_MAX; i++) {
        delayMs(BK1080_POWER_POLL_MS);
        id = bk1080ReadReg(dev, BK1080_REG_01_DEVICE_ID);
        if (id == BK1080_DEVICE_ID)
            break;
    }

    if (id != BK1080_DEVICE_ID) {
        BK1080_PowerOff(dev);
        return false;
    }

    bk1080Write(dev, BK1080_REG_00, BK1080_InitTable,
                sizeof(BK1080_InitTable));

    delayMs(BK1080_CAL_SETTLE_MS);

    /*
     * Calibration restart: clear bit 7 of REG_25 and set it again.  The
     * factory table leaves 0x28BC, so this writes 0x283C and then 0x28BC.
     * (The first version of this driver pulsed bit 15, 0xA8BC, which the
     * stock image never sets.)
     */
    bk1080WriteReg(dev, BK1080_REG_25_INTERNAL, 0x283CU);
    bk1080WriteReg(dev, BK1080_REG_25_INTERNAL, 0x28BCU);

    delayMs(BK1080_CAL_FINISH_MS);

    return true;
}

void BK1080_PowerOff(const struct BK1080 *dev)
{
    /*
     * ENABLE and DISABLE both set: power down.  The registers keep their
     * state while VIO is there.  Then release the supply switch.
     */
    bk1080WriteReg(dev, BK1080_REG_02_POWER_CONFIGURATION,
                   BK1080_REG_02_ENABLE | BK1080_REG_02_DISABLE |
                   BK1080_REG_02_SEEKUP);
    gpioPin_set(&dev->pwr);
}

uint32_t BK1080_BandLowHz(enum bk1080Band band)
{
    switch (band) {
        case BK1080_BAND_875_108:
            return 87500000U;
        case BK1080_BAND_76_108:
        case BK1080_BAND_76_90:
            return 76000000U;
        case BK1080_BAND_64_76:
        default:
            return 64000000U;
    }
}

uint32_t BK1080_BandHighHz(enum bk1080Band band)
{
    switch (band) {
        case BK1080_BAND_875_108:
        case BK1080_BAND_76_108:
            return 108000000U;
        case BK1080_BAND_76_90:
            return 90000000U;
        case BK1080_BAND_64_76:
        default:
            return 76000000U;
    }
}

static uint32_t spacingHz(enum bk1080Spacing spacing)
{
    switch (spacing) {
        case BK1080_SPACE_200K:
            return 200000U;
        case BK1080_SPACE_50K:
            return 50000U;
        case BK1080_SPACE_100K:
        default:
            return 100000U;
    }
}

uint16_t BK1080_FreqToChannel(uint32_t freq, enum bk1080Band band,
                              enum bk1080Spacing spacing)
{
    uint32_t lo = BK1080_BandLowHz(band);
    uint32_t hi = BK1080_BandHighHz(band);
    uint32_t step = spacingHz(spacing);

    if (freq < lo || freq > hi)
        return 0xFFFFU;

    uint32_t chan = (freq - lo + (step / 2)) / step;

    /* CHAN is ten bits: the last channel of a band must still fit */
    if (chan > 0x03FFU)
        return 0xFFFFU;

    return (uint16_t)chan;
}

bool BK1080_TuneChannel(const struct BK1080 *dev, uint16_t chan,
                        enum bk1080Band band, enum bk1080Spacing spacing)
{
    /*
     * Band and spacing live in REG_05 next to the seek threshold and the
     * volume: read-modify-write so that a threshold set by the caller and the
     * full-scale volume of the factory table survive a retune.  The stock
     * firmware writes the whole register (0x0A5F / 0x0ADF), which resets both.
     */
    bk1080Update(dev, BK1080_REG_05_SYSTEM_CONFIGURATION2,
                 BK1080_REG_05_BAND(3) | BK1080_REG_05_SPACE(3),
                 BK1080_REG_05_BAND(band) | BK1080_REG_05_SPACE(spacing));

    /*
     * STC is cleared by clearing TUNE or SEEK, and must be clear to tune. Only
     * SEEK is touched: the caller's MUTE (a retune is normally muted) stays as
     * it is.
     */
    bk1080Update(dev, BK1080_REG_02_POWER_CONFIGURATION, BK1080_REG_02_SEEK, 0);

    bk1080WriteReg(dev, BK1080_REG_03_CHANNEL, BK1080_REG_03_CHAN(chan));
    bk1080WriteReg(dev, BK1080_REG_03_CHANNEL,
                   BK1080_REG_03_CHAN(chan) | BK1080_REG_03_TUNE);

    bool ok = bk1080WaitStc(dev);

    /* TUNE low again: this completes the tune and clears STC */
    bk1080WriteReg(dev, BK1080_REG_03_CHANNEL, BK1080_REG_03_CHAN(chan));

    return ok;
}

bool BK1080_SetFrequency(const struct BK1080 *dev, uint32_t freq,
                         enum bk1080Spacing spacing)
{
    enum bk1080Band band = (freq < 76000000U) ? BK1080_BAND_64_76
                                              : BK1080_BAND_76_108;

    uint16_t chan = BK1080_FreqToChannel(freq, band, spacing);
    if (chan == 0xFFFFU)
        return false;

    return BK1080_TuneChannel(dev, chan, band, spacing);
}

void BK1080_SetMute(const struct BK1080 *dev, bool mute)
{
    bk1080Update(dev, BK1080_REG_02_POWER_CONFIGURATION, BK1080_REG_02_MUTE,
                 mute ? BK1080_REG_02_MUTE : 0);
}

void BK1080_SetMono(const struct BK1080 *dev, bool mono)
{
    bk1080Update(dev, BK1080_REG_02_POWER_CONFIGURATION, BK1080_REG_02_MONO,
                 mono ? BK1080_REG_02_MONO : 0);
}

void BK1080_SetVolume(const struct BK1080 *dev, uint8_t volume)
{
    bk1080Update(dev, BK1080_REG_05_SYSTEM_CONFIGURATION2,
                 BK1080_REG_05_VOLUME(0x0F), BK1080_REG_05_VOLUME(volume));
}

void BK1080_SetDeemphasis50us(const struct BK1080 *dev, bool fifty)
{
    bk1080Update(dev, BK1080_REG_04_SYSTEM_CONFIGURATION1,
                 BK1080_REG_04_DE_50US,
                 fifty ? BK1080_REG_04_DE_50US : 0);
}

void BK1080_SetSoftMute(const struct BK1080 *dev, bool enable)
{
    bk1080Update(dev, BK1080_REG_02_POWER_CONFIGURATION, BK1080_REG_02_DSMUTE,
                 enable ? 0 : BK1080_REG_02_DSMUTE);
}

void BK1080_SetSeekThreshold(const struct BK1080 *dev, uint8_t threshold)
{
    bk1080Update(dev, BK1080_REG_05_SYSTEM_CONFIGURATION2,
                 BK1080_REG_05_SEEKTH(0xFF), BK1080_REG_05_SEEKTH(threshold));
}

void BK1080_SetSeekQuality(const struct BK1080 *dev, uint8_t snr, uint8_t count)
{
    bk1080Update(dev, BK1080_REG_06_SYSTEM_CONFIGURATION3,
                 BK1080_REG_06_SKSNR(0x0F) | BK1080_REG_06_SKCNT(0x0F),
                 BK1080_REG_06_SKSNR(snr) | BK1080_REG_06_SKCNT(count));
}

void BK1080_SeekStart(const struct BK1080 *dev, bool up, bool wrap)
{
    uint16_t bits = BK1080_REG_02_SEEK;

    if (up)
        bits |= BK1080_REG_02_SEEKUP;
    if (!wrap)
        bits |= BK1080_REG_02_SKMODE;

    bk1080Update(dev, BK1080_REG_02_POWER_CONFIGURATION,
                 BK1080_REG_02_SEEK | BK1080_REG_02_SEEKUP |
                 BK1080_REG_02_SKMODE, bits);
}

bool BK1080_SeekPoll(const struct BK1080 *dev, uint16_t *channel)
{
    uint16_t status = bk1080ReadReg(dev, BK1080_REG_0A_STATUS);

    if ((status & BK1080_REG_0A_STC) == 0)
        return false;       /* still sweeping */

    if (channel != NULL) {
        uint16_t readchan = bk1080ReadReg(dev, BK1080_REG_0B_READCHAN);
        *channel = BK1080_REG_0B_READCHAN(readchan);
    }

    /* Valid only if the chip did not report a failure or a band limit */
    return ((status & BK1080_REG_0A_SFBL) == 0);
}

void BK1080_SeekStop(const struct BK1080 *dev)
{
    bk1080Update(dev, BK1080_REG_02_POWER_CONFIGURATION, BK1080_REG_02_SEEK, 0);
}

bool BK1080_ReadStatus(const struct BK1080 *dev, struct bk1080Status *st)
{
    uint8_t raw[4];
    uint8_t test[2];

    if (st == NULL)
        return false;

    if (bk1080Read(dev, BK1080_REG_07_TEST1, test, 2) != 0)
        return false;

    if (bk1080Read(dev, BK1080_REG_0A_STATUS, raw, 4) != 0)
        return false;

    uint16_t status = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t readchan = ((uint16_t)raw[2] << 8) | raw[3];
    uint16_t test1 = ((uint16_t)test[0] << 8) | test[1];

    st->snr = BK1080_REG_07_GET_SNR(test1);
    st->freqd = (uint16_t)BK1080_REG_07_GET_FREQD(test1);
    st->stc = (status & BK1080_REG_0A_STC) != 0;
    st->sfbl = (status & BK1080_REG_0A_SFBL) != 0;
    st->afcrl = (status & BK1080_REG_0A_AFCRL) != 0;
    st->stereoSync = (status & BK1080_REG_0A_STEN) != 0;
    st->stereo = (status & BK1080_REG_0A_ST) != 0;
    st->rssi = BK1080_REG_0A_GET_RSSI(status);
    st->chan = BK1080_REG_0B_READCHAN(readchan);
    st->impc = (uint8_t)BK1080_REG_0B_IMPC(readchan);
    st->deviceId = BK1080_DEVICE_ID;

    return true;
}
