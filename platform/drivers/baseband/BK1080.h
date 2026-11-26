/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BK1080_H
#define BK1080_H

/**
 * @file BK1080.h
 * @brief Beken BK1080 broadcast FM receiver: register access and tuning.
 *
 * The chip is a complete low-IF FM broadcast receiver with its own LNA,
 * stereo MPX decoder, soft mute and stereo audio DACs.  It has nothing in
 * common with the BK4819 transceiver next to it, and it is not on the audio
 * codec either: LOUT/ROUT drive the speaker amplifier through the analog
 * volume pot, so this driver only ever programs registers and reads status.
 * See docs/RX_WFM.md for the hardware map.
 *
 * Bus: two wires (device ID 0x80, so a 7-bit address of 0x40), bit-banged by
 * platform/drivers/I2C/i2c_bitbang.c.  Registers are 16 bit, big endian, and
 * the address auto-increments on a multi-byte transfer.
 *
 * Example (C62):
 *   BK1080_Init(&c62_bk1080);
 *   BK1080_SetFrequency(&c62_bk1080, 101100000U, BK1080_SPACE_100K);
 *   BK1080_SetMute(&c62_bk1080, false);
 */

#include <stdint.h>
#include <stdbool.h>
#include "peripherals/gpio.h"
#include "peripherals/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Value of REG_01 on a BK1080. */
#define BK1080_DEVICE_ID 0x1080U

/** Band selection, REG_05<7:6>. */
enum bk1080Band
{
    BK1080_BAND_875_108 = 0,    /**< 87.5 - 108.0 MHz, US/Europe     */
    BK1080_BAND_76_108  = 1,    /**< 76.0 - 108.0 MHz, Japan wide    */
    BK1080_BAND_76_90   = 2,    /**< 76.0 -  90.0 MHz, Japan         */
    BK1080_BAND_64_76   = 3     /**< 64.0 -  76.0 MHz, OIRT          */
};

/** Channel spacing, REG_05<5:4>. */
enum bk1080Spacing
{
    BK1080_SPACE_200K = 0,      /**< 200 kHz, US/Australia           */
    BK1080_SPACE_100K = 1,      /**< 100 kHz, Europe/Japan (CCIR)    */
    BK1080_SPACE_50K  = 2       /**< 50 kHz, offset rasters (Italy)  */
};

/** Registers this driver uses.  The chip has 0x00-0x21; see the datasheet. */
enum BK1080_Register_t
{
    BK1080_REG_00 = 0x00U,
    BK1080_REG_01_DEVICE_ID = 0x01U,
    BK1080_REG_02_POWER_CONFIGURATION = 0x02U,
    BK1080_REG_03_CHANNEL = 0x03U,
    BK1080_REG_04_SYSTEM_CONFIGURATION1 = 0x04U,
    BK1080_REG_05_SYSTEM_CONFIGURATION2 = 0x05U,
    BK1080_REG_06_SYSTEM_CONFIGURATION3 = 0x06U,
    BK1080_REG_07_TEST1 = 0x07U,
    BK1080_REG_0A_STATUS = 0x0AU,
    BK1080_REG_0B_READCHAN = 0x0BU,
    BK1080_REG_25_INTERNAL = 0x19U,
};

typedef enum BK1080_Register_t BK1080_Register_t;

// REG_02
#define BK1080_REG_02_DSMUTE   (1U << 15)   /**< Soft mute disable         */
#define BK1080_REG_02_MUTE     (1U << 14)   /**< Hard mute                 */
#define BK1080_REG_02_MONO     (1U << 13)   /**< Force mono                */
#define BK1080_REG_02_CKSEL    (1U << 12)   /**< Internal crystal          */
#define BK1080_REG_02_SKMODE   (1U << 10)   /**< Stop at the band limit    */
#define BK1080_REG_02_SEEKUP   (1U << 9)    /**< Seek upwards              */
#define BK1080_REG_02_SEEK     (1U << 8)    /**< Start a seek              */
#define BK1080_REG_02_DISABLE  (1U << 6)    /**< Power down                */
#define BK1080_REG_02_ENABLE   (1U << 0)    /**< Power up                  */

// REG_03
#define BK1080_REG_03_TUNE     (1U << 15)   /**< Start a tune              */
#define BK1080_REG_03_CHAN(v)  ((uint16_t)((v) & 0x03FFU))

// REG_04
#define BK1080_REG_04_STCIEN   (1U << 14)   /**< STC interrupt on GPIO2    */
#define BK1080_REG_04_DEBPS    (1U << 13)   /**< Bypass de-emphasis        */
#define BK1080_REG_04_DE_50US  (1U << 11)   /**< 1 = 50 us, 0 = 75 us      */
#define BK1080_REG_04_AGCD     (1U << 10)   /**< AGC disable               */
#define BK1080_REG_04_BLNDADJ(s) ((uint16_t)(((s) & 0x03U) << 6))

// REG_05
#define BK1080_REG_05_SEEKTH(v)  ((uint16_t)(((v) & 0xFFU) << 8))
#define BK1080_REG_05_BAND(b)    ((uint16_t)(((b) & 0x03U) << 6))
#define BK1080_REG_05_SPACE(s)   ((uint16_t)(((s) & 0x03U) << 4))
#define BK1080_REG_05_VOLUME(v)  ((uint16_t)((v) & 0x0FU))

// REG_06
#define BK1080_REG_06_SMUTER(r)  ((uint16_t)(((r) & 0x03U) << 14))
#define BK1080_REG_06_SMUTEA(a)  ((uint16_t)(((a) & 0x03U) << 12))
#define BK1080_REG_06_SKSNR(v)   ((uint16_t)(((v) & 0x0FU) << 4))
#define BK1080_REG_06_SKCNT(v)   ((uint16_t)((v) & 0x0FU))

// REG_07
#define BK1080_REG_07_SHIFT_FREQD 4
#define BK1080_REG_07_SHIFT_SNR 0

#define BK1080_REG_07_MASK_FREQD (0xFFFU << BK1080_REG_07_SHIFT_FREQD)
#define BK1080_REG_07_MASK_SNR (0x00FU << BK1080_REG_07_SHIFT_SNR)

#define BK1080_REG_07_GET_FREQD(x) \
    (((x) & BK1080_REG_07_MASK_FREQD) >> BK1080_REG_07_SHIFT_FREQD)
#define BK1080_REG_07_GET_SNR(x) \
    (((x) & BK1080_REG_07_MASK_SNR) >> BK1080_REG_07_SHIFT_SNR)

// REG_0A
#define BK1080_REG_0A_STC      (1U << 14)   /**< Seek/tune complete        */
#define BK1080_REG_0A_SFBL     (1U << 13)   /**< Seek failed / band limit  */
#define BK1080_REG_0A_AFCRL    (1U << 12)   /**< AFC railed: invalid chan  */
#define BK1080_REG_0A_STEN     (1U << 9)    /**< Stereo decoder indicator  */
#define BK1080_REG_0A_ST       (1U << 8)    /**< Stereo reception          */
#define BK1080_REG_0A_RSSI(v)  ((uint8_t)((v) & 0xFFU))

#define BK1080_REG_0A_GET_AFCRL(x) (((x) & BK1080_REG_0A_AFCRL) != 0)
#define BK1080_REG_0A_GET_RSSI(x)  BK1080_REG_0A_RSSI(x)

// REG_0B
#define BK1080_REG_0B_IMPC(v)    ((uint16_t)(((v) >> 10) & 0x0FU))
#define BK1080_REG_0B_READCHAN(v) ((uint16_t)((v) & 0x03FFU))

/**
 * BK1080 device data.
 *
 * Register access happens over the chip's I2C-like serial bus, accessed
 * through the generic struct i2cDevice interface of peripherals/i2c.h.  The
 * power pin is active low: clear it to power the chip up.
 */
struct BK1080
{
    const struct i2cDevice *i2c; ///< I2C bus device
    struct gpioPin pwr;          ///< Power switch, active low
};

/**
 * Everything REG_07, REG_0A and REG_0B say about the current channel.
 */
struct bk1080Status
{
    uint16_t deviceId;  ///< REG_01, 0x1080 on a BK1080
    uint16_t chan;      ///< REG_0B READCHAN<9:0>, channel the tuner is on
    uint8_t  rssi;      ///< REG_0A RSSI<7:0>, dBuV (0..75)
    uint8_t  snr;       ///< REG_07 SNR<3:0>
    uint16_t freqd;     ///< REG_07 FREQD<11:0>, 148 Hz units
    uint8_t  impc;      ///< REG_0B IMPC<3:0>, impulse counter
    bool     stc;       ///< Seek/tune complete
    bool     sfbl;      ///< Seek failed / band limit
    bool     afcrl;     ///< AFC railed: no usable channel, audio soft muted
    bool     stereo;    ///< ST: pilot present, signal received in stereo
    bool     stereoSync;///< STEN: stereo decoder locked
};

/**
 * Power the chip up, check its identity and write the configuration table.
 *
 * The power pin is driven low and REG_01 is polled until the chip answers
 * 0x1080, instead of the fixed one-second wait of the stock firmware: the
 * chip answers as soon as its reference clock is stable, which is normally
 * tens of milliseconds.  The table, the 250 ms wait, the REG_25 calibration
 * pulse and the 60 ms wait are the factory sequence, see docs/RX_WFM.md.
 *
 * @param dev: pointer to device data.
 * @return true if the chip answered and was configured.
 */
bool BK1080_Init(const struct BK1080 *dev);

/**
 * Power the chip down (ENABLE and DISABLE set, then the power pin released).
 *
 * @param dev: pointer to device data.
 */
void BK1080_PowerOff(const struct BK1080 *dev);

/**
 * Read a chip register.
 *
 * @param dev: pointer to device data.
 * @param reg: register address.
 * @return register value.
 */
uint16_t BK1080_ReadRegister(const struct BK1080 *dev, uint8_t reg);

/**
 * Write a chip register.
 *
 * @param dev: pointer to device data.
 * @param reg: register address.
 * @param value: value to be written.
 */
void BK1080_WriteRegister(const struct BK1080 *dev, uint8_t reg,
                          uint16_t value);

/**
 * Tune a frequency.
 *
 * The band is chosen from the frequency: 64-76 MHz uses OIRT, everything
 * else the 76-108 MHz band, which is what the stock firmware does and the
 * only way to reach the 85.0-87.5 MHz part of the band this build offers.
 * The channel, band and spacing are written, TUNE is raised, STC polled and
 * TUNE cleared.
 *
 * @param dev: pointer to device data.
 * @param freq: frequency, in Hz.
 * @param spacing: channel raster.
 * @return true if the tune completed.
 */
bool BK1080_SetFrequency(const struct BK1080 *dev, uint32_t freq,
                         enum bk1080Spacing spacing);

/**
 * Tune a channel index directly (REG_03 CHAN).
 *
 * @param dev: pointer to device data.
 * @param chan: channel index inside the band.
 * @param band: band the index is relative to.
 * @param spacing: channel raster.
 * @return true if the tune completed.
 */
bool BK1080_TuneChannel(const struct BK1080 *dev, uint16_t chan,
                        enum bk1080Band band, enum bk1080Spacing spacing);

/**
 * Mute or unmute the audio output (REG_02 MUTE, the hard mute; the chip's
 * soft mute on weak signals is a separate feature and stays enabled).
 *
 * @param dev: pointer to device data.
 * @param mute: true to mute.
 */
void BK1080_SetMute(const struct BK1080 *dev, bool mute);

/**
 * Force mono (REG_02 MONO).  Stereo is the default and the chip blends to
 * mono by itself as the signal degrades.
 *
 * @param dev: pointer to device data.
 * @param mono: true to force mono.
 */
void BK1080_SetMono(const struct BK1080 *dev, bool mono);

/**
 * Set the digital volume, 0 (mute) to 15 (0 dBFS).
 *
 * The C62 also has an analog volume pot after the chip, which is the one the
 * operator turns; this stays at full scale, as in the stock firmware.
 *
 * @param dev: pointer to device data.
 * @param volume: 0..15.
 */
void BK1080_SetVolume(const struct BK1080 *dev, uint8_t volume);

/**
 * Select the de-emphasis time constant (REG_04 DE).
 *
 * @param dev: pointer to device data.
 * @param fifty: true for 50 us (Europe, Australia, Japan), false for 75 us.
 */
void BK1080_SetDeemphasis50us(const struct BK1080 *dev, bool fifty);

/**
 * Enable or disable the soft mute on weak signals (REG_02 DSMUTE).
 *
 * @param dev: pointer to device data.
 * @param enable: false to disable the soft mute.
 */
void BK1080_SetSoftMute(const struct BK1080 *dev, bool enable);

/**
 * Set the RSSI seek threshold (REG_05 SEEKTH), 0 to 255 dBuV.
 *
 * @param dev: pointer to device data.
 * @param threshold: minimum RSSI for a channel to qualify as a station.
 */
void BK1080_SetSeekThreshold(const struct BK1080 *dev, uint8_t threshold);

/**
 * Set the SNR and impulse-noise seek qualifiers (REG_06 SKSNR, SKCNT).
 * Zero disables that qualifier.
 *
 * @param dev: pointer to device data.
 * @param snr: SNR threshold, 0 (off) to 15 (strictest).
 * @param count: impulse counter threshold, 0 (off) to 15 (strictest).
 */
void BK1080_SetSeekQuality(const struct BK1080 *dev, uint8_t snr,
                           uint8_t count);

/**
 * Start a hardware seek.  The chip sweeps the band itself and sets STC when
 * it stops; poll BK1080_SeekPoll() to find out where.
 *
 * @param dev: pointer to device data.
 * @param up: true to seek upwards, false downwards.
 * @param wrap: true to wrap at the band edges and keep looking.
 */
void BK1080_SeekStart(const struct BK1080 *dev, bool up, bool wrap);

/**
 * Poll a seek.
 *
 * @param dev: pointer to device data.
 * @param channel: filled with READCHAN when the seek has finished.
 * @return true if the seek finished on a valid channel, false if it is still
 * running, if it hit the band limit or if it failed.
 */
bool BK1080_SeekPoll(const struct BK1080 *dev, uint16_t *channel);

/**
 * Abort a seek and clear STC/SF-BL, so that the next tune can start.
 *
 * @param dev: pointer to device data.
 */
void BK1080_SeekStop(const struct BK1080 *dev);

/**
 * Read the status registers and fill a status structure.
 *
 * @param dev: pointer to device data.
 * @param st: structure to fill.
 * @return true if the bus transfers succeeded.
 */
bool BK1080_ReadStatus(const struct BK1080 *dev, struct bk1080Status *st);

/**
 * Lowest and highest frequency of a band, in Hz.
 */
uint32_t BK1080_BandLowHz(enum bk1080Band band);
uint32_t BK1080_BandHighHz(enum bk1080Band band);

/**
 * Channel index of a frequency inside a band, for REG_03 CHAN.
 *
 * @return the index, or 0xFFFF if the frequency is outside the band.
 */
uint16_t BK1080_FreqToChannel(uint32_t freq, enum bk1080Band band,
                              enum bk1080Spacing spacing);

/*
 * Names kept from the first version of this driver, so that code written
 * against it (and the broadcast FM pull request it came from) still builds.
 */
static inline void BK1080_Mute(const struct BK1080 *dev, bool mute)
{
    BK1080_SetMute(dev, mute);
}

static inline uint32_t BK1080_GetFreqLoLimit(uint8_t band)
{
    return BK1080_BandLowHz((enum bk1080Band)(band % 4));
}

static inline uint32_t BK1080_GetFreqHiLimit(uint8_t band)
{
    return BK1080_BandHighHz((enum bk1080Band)(band % 4));
}

#ifdef __cplusplus
}
#endif

#endif /* BK1080_H */
