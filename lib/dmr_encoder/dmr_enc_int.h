/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 * SPDX-FileCopyrightText: 2009 Pavel Yazev
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_enc_int.h
 * @brief Internal interface of the AMBE+2 encoder: analysis parameters and the
 * entry points of the two halves (analysis, frame packing).
 *
 * This is a port of the fixed-point IMBE analysis of OpenDMR's encoder
 * (`encoder/`, itself Pavel Yazev's P25 IMBE implementation) to plain C with
 * one instance and everything in one translation unit.  The header exists so
 * that the analysis (`dmr_enc_core.c`) and the model/quantiser
 * (`dmr_enc_frame.c`) can share the parameter structure without either of them
 * exposing the other's state.
 */

#ifndef DMR_ENC_INT_H
#define DMR_ENC_INT_H

#include <stddef.h>
#include <stdint.h>

#include "dmr_enc_basic.h"

/* ---- geometry of one frame (20 ms at 8 kHz) ---------------------------- */
#define FRAME 160          /**< samples per frame */
#define FFTLENGTH 256      /**< analysis FFT size */
#define PITCH_EST_FRAME 301 /**< pitch estimator window */
#define PITCH_EST_BUF_SIZE 621 /**< 2 * PITCH_EST_FRAME + FRAME - 1... as in the reference */
#define NUM_HARMS_MAX 56   /**< harmonics at 8 kHz with the lowest pitch */
#define NUM_HARMS_MIN 9
#define NUM_BANDS_MAX 12
#define MAX_BLOCK_LEN 10
#define NUM_PRED_RES_BLKS 6
#define PE_LPF_ORD 21      /**< pitch estimation low-pass, 21 taps */
#define B_NUM (NUM_HARMS_MAX - 1)
#define GAIN_QNT_TBL_SIZE 64
#define EN_BIT_STREAM_LEN (3 + 3 * 12 + 6 + 2 * 11 + 3)

/* ---- Q format constants used by the analysis --------------------------- */
#define CNST_0_9254_Q0_16 60647
#define CNST_0_33_Q0_16 0x5556
#define CNST_ONE_Q8_24 0x01000000
#define CNST_0_7_Q1_15 0x599A
#define CNST_0_4_Q1_15 0x3333
#define CNST_0_03_Q1_15 0x03D7
#define CNST_0_05_Q1_15 0x0666
#define CNST_1_125_Q8_8 0x0120
#define CNST_0_5_Q8_8 0x0080
#define CNST_0_125_Q8_8 0x0020
#define CNST_0_25_Q8_8 0x0040
#define CNST_0_8717_Q1_15 0x6F94
#define CNST_0_0031_Q1_15 0x0064
#define CNST_0_48_Q4_12 0x07AE
#define CNST_1_00_Q4_12 0x1000
#define CNST_0_85_Q4_12 0x0D9B
#define CNST_0_4_Q4_12 0x0666
#define CNST_0_05_Q4_12 0x00CD
#define CNST_0_5882_Q1_15 0x4B4B
#define CNST_0_2857_Q1_15 0x2492

#define CNST_0_5_Q1_15 0x4000
#define CNST_0_5_Q5_11 0x0400
#define CNST_1_0_Q1_15 0x7FFF
#define CNST_0_5_Q0_16 0x8000

#define X05_Q15 16384
#define ONE_Q15 32767

#define HI_BYTE(a) (((a) >> 8) & 0xFF)
#define LO_BYTE(a) ((a) & 0xFF)

/**
 * One frame of analysis parameters, as the quantiser consumes them.
 * Field for field the reference `IMBE_PARAM`.
 */
typedef struct {
    Word16 e_p;
    Word16 pitch;     /**< Q14.2 */
    Word16 ref_pitch; /**< Q8.8 */
    Word32 fund_freq;
    Word16 num_harms;
    Word16 num_bands;
    Word16 v_uv_dsn[NUM_HARMS_MAX];
    Word16 b_vec[NUM_HARMS_MAX + 3];
    Word16 bit_alloc[B_NUM + 4];
    Word16 sa[NUM_HARMS_MAX];
    Word16 l_uv;
    Word16 div_one_by_num_harm;
    Word16 div_one_by_num_harm_sh;
} imbe_param_t;

typedef struct {
    Word16 re;
    Word16 im;
} cmplx16_t;

/* ---- analysis (dmr_enc_core.c) ----------------------------------------- */
/** Zero the analysis state (the reference `imbe_vocoder_impl` constructor). */
void dmr_enc_analysis_reset(void);
/** Analyse 160 samples of 8 kHz PCM into one frame of parameters. */
void dmr_enc_analysis(imbe_param_t *out, const int16_t *pcm);

/* Optional trace for the test harnesses: when DMR_ENC_DEBUG is defined the
 * encoder copies its last frame's parameters and quantiser indices here, so a
 * host test can diff them against the reference without any printf in the
 * firmware path. */
#ifdef DMR_ENC_DEBUG
extern imbe_param_t dmr_enc_dbg_param;
extern int dmr_enc_dbg_b[9];
#endif

/* ---- model and frame packing (dmr_enc_frame.c) ------------------------- */
/** Zero the quantiser and the parameter history. */
void dmr_enc_frame_reset(void);

/** Bytes of .bss held by the two halves (analysis + model). */
size_t dmr_enc_state_bytes(void);
/**
 * Quantise one analysis frame into the 72-bit AMBE+2 frame (9 bytes,
 * canonical A|B|C order, i.e. what lib/dmr_vocoder consumes).
 */
void dmr_enc_frame(imbe_param_t *param, uint8_t frame[9]);

#endif /* DMR_ENC_INT_H */
