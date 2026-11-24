/*
 * SPDX-FileCopyrightText: 2026 OpenRTX Contributors
 * SPDX-FileCopyrightText: 2009 Pavel Yazev
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file dmr_enc_core.c
 * @brief AMBE+2 (DMR 3600x2450) analysis: PCM -> model parameters.
 *
 * Mechanical C port of the encoder half of OpenDMR, which is Pavel Yazev's
 * fixed-point P25 IMBE implementation (GPL).  What changed:
 *
 *  - C++ class members became file-scope state (the reference had them as
 *    `static` globals before it was turned into a class, so this is going
 *    back to the original shape);
 *  - `<cstdio>`, `<cstdlib>`, `<cmath>`, the WMOPS counters, the `printf`
 *    debug dumps and the asserts are gone: there is no libc in the path;
 *  - the arithmetic operators of `basicop2.cc` are `static inline` in
 *    dmr_enc_basic.h and the whole analysis lives in one translation unit,
 *    which is what makes the calls disappear;
 *  - `div_s()` no longer aborts on a bad argument (see dmr_enc_basic.h).
 *
 * The fixed-point arithmetic is bit-exact with the reference by construction:
 * same integer operations in the same order, same tables.  The only float in
 * the encoder is in the quantiser (dmr_enc_frame.c), not here.
 *
 * Layout of this file:
 *   random  math  dsp  tables  quantisers  DC blocker  pitch LPF  pitch
 *   estimation  pitch refinement  V/UV  spectral amplitudes  frame vector
 */

#include <string.h>

#include "dmr_enc_int.h"


/* ------------------------------------------------------------------------
 * State.  The reference kept this inside `imbe_vocoder_impl`; it was file
 * scope before that (and `rand_gen.cc` still defines its own `seed`).
 * One encoder instance = one radio.
 * ------------------------------------------------------------------------ */
static Word16 prev_pitch;
static Word16 prev_prev_pitch;
static Word16 prev_e_p;
static Word16 prev_prev_e_p;
static Word16 num_harms_prev2;
static Word32 sa_prev2[NUM_HARMS_MAX + 2];
static Word32 th_max;
static Word16 v_uv_dsn[NUM_BANDS_MAX];
static Word16 wr_array[FFTLENGTH / 2 + 1];
static Word16 wi_array[FFTLENGTH / 2 + 1];
static Word16 pitch_est_buf[PITCH_EST_BUF_SIZE];
static Word16 pitch_ref_buf[PITCH_EST_BUF_SIZE];
static Word32 dc_rmv_mem;
static cmplx16_t fft_buf[FFTLENGTH];
static Word16 pe_lpf_mem[PE_LPF_ORD];


/* ======================================================================
 * Every constant table of the analysis (windows, quantisers).
 * (reference: encoder/tbls.cc)
 * ==================================================================== */
//
// Bit allocation and ancillary tables
//
static const UWord16 bit_allocation_tbl[] =
{
	0xA999, 0x9987,
	0x9988, 0x8976, 0x5000,
	0x8887, 0x7976, 0x5400,
	0x8777, 0x7876, 0x5430,
	0x7776, 0x6776, 0x5433,
	0x7666, 0x6775, 0x4434, 0x3000,
	0x7666, 0x5675, 0x4433, 0x3300,
	0x6665, 0x5665, 0x4433, 0x3320,
	0x6655, 0x5555, 0x4443, 0x3232,
	0x6555, 0x5545, 0x5433, 0x3322, 0x2000,
	0x6554, 0x4545, 0x4433, 0x3323, 0x2100,
	0x6554, 0x4545, 0x4433, 0x2321, 0x3210,
	0x5554, 0x4445, 0x4433, 0x2232, 0x1321,
	0x5544, 0x4444, 0x4432, 0x3223, 0x2122, 0x1000,
	0x5444, 0x4434, 0x4343, 0x2322, 0x2212, 0x2100,
	0x5444, 0x4433, 0x4333, 0x3232, 0x1221, 0x2210,
	0x5444, 0x3433, 0x4333, 0x3232, 0x1221, 0x2111,
	0x5443, 0x3433, 0x4333, 0x2232, 0x1221, 0x1221, 0x1000,
	0x5443, 0x3432, 0x4323, 0x2232, 0x2122, 0x1122, 0x1100,
	0x4443, 0x3432, 0x4323, 0x2223, 0x2112, 0x2112, 0x1110,
	0x4443, 0x3332, 0x4322, 0x3222, 0x3211, 0x2111, 0x2111,
	0x4443, 0x3332, 0x2332, 0x2322, 0x1321, 0x1211, 0x1211, 0x1000,
	0x4433, 0x3332, 0x2332, 0x2322, 0x1221, 0x1211, 0x1211, 0x1100,
	0x4433, 0x3332, 0x2332, 0x2322, 0x1221, 0x1211, 0x1121, 0x1100,
	0x4333, 0x3332, 0x2332, 0x2321, 0x1221, 0x1121, 0x1112, 0x1111,
	0x4333, 0x3322, 0x2322, 0x2322, 0x1122, 0x1112, 0x1111, 0x2111, 0x0000,
	0x4333, 0x3322, 0x2322, 0x2232, 0x1112, 0x2111, 0x2111, 0x0211, 0x1000,
	0x4333, 0x3322, 0x2132, 0x2213, 0x2111, 0x2211, 0x1211, 0x1021, 0x1100,
	0x4333, 0x2322, 0x2132, 0x2223, 0x2111, 0x2111, 0x1211, 0x1021, 0x1110,
	0x4333, 0x2322, 0x2132, 0x2213, 0x2111, 0x2111, 0x1211, 0x1102, 0x1111, 0x0000,
	0x4333, 0x2322, 0x2132, 0x2213, 0x2111, 0x2211, 0x1021, 0x1110, 0x2111, 0x0000,
	0x4333, 0x2322, 0x2132, 0x2113, 0x2111, 0x1221, 0x1102, 0x1111, 0x0211, 0x1000,
	0x4332, 0x2322, 0x1132, 0x2211, 0x3211, 0x1122, 0x1110, 0x2111, 0x1021, 0x1100,
	0x4332, 0x2322, 0x2113, 0x2221, 0x1221, 0x1112, 0x2111, 0x0211, 0x1002, 0x1110, 0x0000,
	0x4332, 0x2322, 0x2113, 0x2221, 0x1221, 0x1112, 0x1111, 0x0211, 0x1002, 0x1111, 0x0000,
	0x4332, 0x2322, 0x1113, 0x2221, 0x1221, 0x1112, 0x1111, 0x0211, 0x1100, 0x2111, 0x1000,
	0x4332, 0x2322, 0x1113, 0x2211, 0x1221, 0x1112, 0x2111, 0x0021, 0x1110, 0x0211, 0x1100,
	0x3332, 0x2322, 0x1113, 0x2211, 0x1221, 0x1111, 0x2211, 0x1002, 0x1111, 0x0021, 0x1110, 0x0000,
	0x3332, 0x2322, 0x1113, 0x2211, 0x1122, 0x1111, 0x1221, 0x1100, 0x2111, 0x1002, 0x1110, 0x0000,
	0x3332, 0x2322, 0x1111, 0x3221, 0x1112, 0x2111, 0x1122, 0x1110, 0x0211, 0x1000, 0x2111, 0x0000,
	0x3332, 0x2322, 0x1111, 0x3221, 0x1112, 0x2111, 0x1022, 0x1110, 0x0211, 0x1000, 0x2111, 0x1000,
	0x3332, 0x2322, 0x1111, 0x3221, 0x1112, 0x2111, 0x1022, 0x1110, 0x0211, 0x1100, 0x0211, 0x1000, 0x0000,
	0x3332, 0x2322, 0x1111, 0x3211, 0x1112, 0x2111, 0x1022, 0x1110, 0x0021, 0x1110, 0x0021, 0x1110, 0x0000,
	0x3322, 0x2321, 0x1111, 0x3221, 0x1112, 0x2111, 0x1102, 0x2111, 0x0002, 0x1111, 0x0002, 0x1111, 0x0000,
	0x3322, 0x2321, 0x1111, 0x3221, 0x1111, 0x2211, 0x1110, 0x2211, 0x1000, 0x2111, 0x1000, 0x2111, 0x0000,
	0x3322, 0x2322, 0x1111, 0x0322, 0x1111, 0x0221, 0x1111, 0x0221, 0x1100, 0x0211, 0x1100, 0x0211, 0x1000, 0x0000,
	0x3322, 0x2322, 0x1111, 0x0322, 0x1111, 0x0221, 0x1111, 0x0221, 0x1100, 0x0211, 0x1000, 0x0211, 0x1100, 0x0000,
	0x3322, 0x2322, 0x1111, 0x0322, 0x1111, 0x0221, 0x1111, 0x0221, 0x1100, 0x0211, 0x1100, 0x0021, 0x1100, 0x0000
};

static const UWord16 bit_allocation_offset_tbl[] =
{
	0x0002, 0x000E, 0x001E, 0x0032, 0x004A, 0x0066, 0x0086, 0x00AA, 0x00D2, 0x00FE, 0x012E, 0x0162
};

//
// Log Magnitude Prediction Residual Block Length
//
static const UWord32 lmprbl_tbl[] =
{
	0x11122200,
	0x11222200,
	0x12222200,
	0x22222200,
	0x22222300,
	0x22223300,
	0x22233300,
	0x22333300,
	0x23333300,
	0x33333300,
	0x33333400,
	0x33334400,
	0x33344400,
	0x33444400,
	0x34444400,
	0x44444400,
	0x44444500,
	0x44445500,
	0x44455500,
	0x44555500,
	0x45555500,
	0x55555500,
	0x55555600,
	0x55556600,
	0x55566600,
	0x55666600,
	0x56666600,
	0x66666600,
	0x66666700,
	0x66667700,
	0x66677700,
	0x66777700,
	0x67777700,
	0x77777700,
	0x77777800,
	0x77778800,
	0x77788800,
	0x77888800,
	0x78888800,
	0x88888800,
	0x88888900,
	0x88889900,
	0x88899900,
	0x88999900,
	0x89999900,
	0x99999900,
	0x99999A00,
	0x9999AA00
};

//
// Gain Quantizer Levels in signed Q5.11 format
//
static const Word16 gain_qnt_tbl[] =
{
	-5821, -5518, -5239, -4880, -4549, -4292, -4057, -3760, -3370, -2903,
	-2583, -2305, -1962, -1601, -1138,  -711,  -302,    57,   433,   795,
	 1132,  1510,  1909,  2333,  2705,  3038,  3376,  3689,  3979,  4339,
	 4754,  5129,  5435,  5695,  5991,  6300,  6596,  6969,  7342,  7751,
	 8101,  8511,  8835,  9102,  9375,  9698, 10055, 10415, 10762, 11084,
	11403, 11752, 12123, 12468, 12863, 13239, 13615, 13997, 14382, 14770,
	15301, 15849, 16640, 17809
};

//
// Gain Ste Size in unsigned Q0.16 format
//
static const UWord16 gain_step_size_tbl[] =
{
	  203,  263,  220,  190,  173,
	  406,  263,  440,  380,  346,
	  813,  527,  440,  760,  692,
 	  813, 1054,  881,  760,  692,
	 1625, 1054,  881, 1425, 1298,
	 1625, 1976, 1652, 1425, 1298,
	 1625, 1976, 1652, 1425, 2422,
	 3047, 1976, 1652, 2661, 2422,
	 3047, 1976, 3083, 2661, 2422,
	 3047, 3688, 3083, 2661, 2422,
	 3047, 3688, 3083, 3801, 3460,
	 3047, 3688, 3083, 3801, 3460,
	 5689, 3688, 3083, 3801, 3460,
	 5689, 3688, 4404, 3801, 3460,
	 5689, 5269, 4404, 3801, 3460,
	 5689, 5269, 4404, 3801, 3460,
	 5689, 5269, 4404, 3801, 5623,
	 5689, 5269, 4404, 6177, 5623,
	 5689, 5269, 4404, 6177, 5623,
	 8126, 5269, 4404, 6177, 5623,
	 8126, 5269, 4404, 6177, 5623,
	 8126, 5269, 4404, 6177, 5623,
	 8126, 5269, 7157, 6177, 5623,
	 8126, 5269, 7157, 6177, 5623,
	 8126, 8562, 7157, 6177, 5623,
	 8126, 8562, 7157, 6177, 5623,
	 8126, 8562, 7157, 6177, 5623,
	 8126, 8562, 7157, 6177, 5623,
	 8126, 8562, 7157, 6177, 7353,
	 8126, 8562, 7157, 6177, 7353,
	 8126, 8562, 7157, 6177, 7353,
	 8126, 8562, 7157, 6177, 7353,
	 8126, 8562, 7157, 8077, 7353,
	 8126, 8562, 7157, 8077, 7353,
	 8126, 8562, 7157, 8077, 7353,
	 8126, 8562, 7157, 8077, 7353,
	 8126, 8562, 7157, 8077, 7353,
	13206, 8562, 7157, 8077, 7353,
	13206, 8562, 7157, 8077, 7353,
	13206, 8562, 7157, 8077, 7353,
	13206, 8562, 7157, 8077, 7353,
	13206, 8562, 7157, 8077, 7353,
	13206, 8562, 7157, 8077, 7353,
	13206, 8562, 9359, 8077, 7353,
	13206, 8562, 9359, 8077, 7353,
	13206, 8562, 9359, 8077, 7353,
	13206, 8562, 9359, 8077, 7353,
	13206, 8562, 9359, 8077, 7353
};

//
// Standard Deviation of Higher Order DCT Coefficients in unsigned Q0.16 format
//
static const UWord16 hi_ord_std_tbl[] =
{
	20120, 15794, 13566, 12452, 11731, 11338, 10813, 11141, 11141
};

//
// Quantizer Step for Higher Order DCT Coefficients in unsigned Q1.15 format
//
static const UWord16 hi_ord_step_size_tbl[] =
{
	39322,	27853, 21299, 13107, 9175, 4915, 2621, 1311, 655, 328
};

//
// Speech Synthesis Window
//

//
// Squared Pitch Estimation Window 64*wi^2
//
static const Word16 wi[301] =
{
	   15,    18,    21,    25,    29,    34,    40,    46,    52,    59,
	   67,    76,    86,    96,   108,   120,   133,   148,   164,   180,
	  199,   218,   239,   261,   285,   311,   338,   367,   398,   431,
	  465,   502,   541,   582,   625,   671,   719,   769,   822,   878,
	  936,   997,  1061,  1128,  1198,  1271,  1347,  1426,  1508,  1594,
	 1683,  1775,  1871,  1970,  2073,  2179,  2289,  2403,  2520,  2641,
	 2766,  2894,  3026,  3162,  3301,  3445,  3592,  3742,  3897,  4055,
	 4216,  4382,  4551,  4723,  4899,  5078,  5261,  5447,  5636,  5828,
	 6023,  6221,  6423,  6626,  6833,  7042,  7253,  7467,  7683,  7900,
	 8120,  8341,  8564,  8788,  9014,  9241,  9468,  9696,  9925, 10154,
	10383, 10613, 10842, 11070, 11298, 11525, 11752, 11977, 12200, 12422,
	12642, 12860, 13076, 13289, 13500, 13708, 13912, 14114, 14312, 14506,
	14696, 14882, 15064, 15242, 15414, 15582, 15745, 15903, 16055, 16202,
	16343, 16478, 16608, 16731, 16848, 16958, 17062, 17159, 17250, 17333,
	17410, 17480, 17542, 17597, 17646, 17686, 17720, 17746, 17764, 17776,
	17779, 17776, 17764, 17746, 17720, 17686, 17646, 17597, 17542, 17480,
	17410, 17333, 17250, 17159, 17062, 16958, 16848, 16731, 16608, 16478,
	16343, 16202, 16055, 15903, 15745, 15582, 15414, 15242, 15064, 14882,
	14696, 14506, 14312, 14114, 13912, 13708, 13500, 13289, 13076, 12860,
	12642, 12422, 12200, 11977, 11752, 11525, 11298, 11070, 10842, 10613,
	10383, 10154,  9925,  9696,  9468,  9241,  9014,  8788,  8564,  8341,
	 8120,  7900,  7683,  7467,  7253,  7042,  6833,  6626,  6423,  6221,
	 6023,  5828,  5636,  5447,  5261,  5078,  4899,  4723,  4551,  4382,
	 4216,  4055,  3897,  3742,  3592,  3445,  3301,  3162,  3026,  2894,
	 2766,  2641,  2520,  2403,  2289,  2179,  2073,  1970,  1871,  1775,
	 1683,  1594,  1508,  1426,  1347,  1271,  1198,  1128,  1061,   997,
	  936,   878,   822,   769,   719,   671,   625,   582,   541,   502,
	  465,   431,   398,   367,   338,   311,   285,   261,   239,   218,
	  199,   180,   164,   148,   133,   120,   108,    96,    86,    76,
	   67,    59,    52,    46,    40,    34,    29,    25,    21,    18,
	   15
};

//
// Pitch Refinement Window
//
static const Word16 wr[] =
{
	  487,   570,   658,   753,   854,   962,  1076,  1197,  1325,   1460,
	 1602,  1752,  1909,  2074,  2246,  2426,  2614,  2810,  3014,   3227,
	 3447,  3675,  3912,  4157,  4410,  4671,  4941,  5219,  5505,   5799,
	 6101,  6411,  6729,  7054,  7388,  7728,  8077,  8432,  8795,   9164,
	 9541,  9923, 10312, 10707, 11108, 11515, 11927, 12344, 12766,  13192,
	13622, 14056, 14494, 14935, 15379, 15825, 16273, 16724, 17175,  17628,
	18081, 18534, 18988, 19440, 19892, 20342, 20790, 21237, 21680,  22120,
	22557, 22990, 23419, 23842, 24261, 24673, 25080, 25480, 25874,  26259,
	26638, 27008, 27370, 27722, 28066, 28400, 28724, 29037, 29340,  29632,
	29912, 30181, 30438, 30683, 30915, 31135, 31341, 31535, 31715,  31881,
	32033, 32172, 32296, 32406, 32502, 32583, 32649, 32701, 32738,  32760,
};

//
// Real Part Spectrum of Pitch Refinement Window * 256
//
static const Word16 wr_sp[] =
{
	   16,    43,    71,   102,   135,   170,   207,   247,   289,   333,
	  380,   430,   483,   538,   596,   657,   721,   787,   857,   930,
	 1006,  1085,  1168,  1254,  1343,  1435,  1531,  1630,  1733,  1840,
	 1950,  2063,  2181,  2302,  2426,  2555,  2687,  2823,  2962,  3106,
	 3253,  3404,  3559,  3718,  3881,  4047,  4218,  4392,  4570,  4751,
	 4937,  5126,  5319,  5516,  5716,  5920,  6128,  6339,  6554,  6772,
	 6994,  7219,  7447,  7679,  7914,  8152,  8393,  8637,  8884,  9134,
  	 9387,  9642,  9900, 10160, 10423, 10688, 10956, 11225, 11497, 11770,
	12045, 12322, 12601, 12881, 13162, 13444, 13728, 14012, 14298, 14584,
	14871, 15158, 15445, 15733, 16020, 16308, 16595, 16882, 17168, 17454,
	17738, 18022, 18305, 18586, 18866, 19145, 19421, 19696, 19969, 20240,
	20508, 20774, 21037, 21298, 21556, 21811, 22062, 22310, 22555, 22796,
	23033, 23267, 23496, 23722, 23943, 24159, 24372, 24579, 24782, 24980,
	25172, 25360, 25542, 25719, 25891, 26057, 26217, 26372, 26520, 26663,
	26800, 26930, 27055, 27173, 27285, 27390, 27489, 27581, 27667, 27746,
	27818, 27884, 27942, 27994, 28039, 28078, 28109, 28133, 28151, 28161,
	28165, 28161, 28151, 28133, 28109, 28078, 28039, 27994, 27942, 27884,
	27818, 27746, 27667, 27581, 27489, 27390, 27285, 27173, 27055, 26930,
	26800, 26663, 26520, 26372, 26217, 26057, 25891, 25719, 25542, 25360,
	25172, 24980, 24782, 24579, 24372, 24159, 23943, 23722, 23496, 23267,
	23033, 22796, 22555, 22310, 22062, 21811, 21556, 21298, 21037, 20774,
	20508, 20240, 19969, 19696, 19421, 19145, 18866, 18586, 18305, 18022,
	17738, 17454, 17168, 16882, 16595, 16308, 16020, 15733, 15445, 15158,
	14871, 14584, 14298, 14012, 13728, 13444, 13162, 12881, 12601, 12322,
	12045, 11770, 11497, 11225, 10956, 10688, 10423, 10160,  9900,  9642,
	 9387,  9134,  8884,  8637,  8393,  8152,  7914,  7679,  7447,  7219,
	 6994,  6772,  6554,  6339,  6128,  5920,  5716,  5516,  5319,  5126,
     4937,  4751,  4570,  4392,  4218,  4047,  3881,  3718,  3559,  3404,
     3253,  3106,  2962,  2823,  2687,  2555,  2426,  2302,  2181,  2063,
     1950,  1840,  1733,  1630,  1531,  1435,  1343,  1254,  1168,  1085,
     1006,   930,   857,   787,   721,   657,   596,   538,   483,   430,
      380,   333,   289,   247,   207,   170,   135,   102,    71,    43,
       16
};
/* ======================================================================
 * Pseudo-random generator used by the unvoiced detector.
 * (reference: encoder/rand_gen.cc)
 * ==================================================================== */

//	PURPOSE:
//				Generate pseudo-random numbers in range -1...1
//
//
//  INPUT:
//		None
//
//	OUTPUT:
//		None
//
//	RETURN:
//		        Pseudo-random number in signed Q1.16 format
//
/* ======================================================================
 * Fixed-point pow2/cos/sin/exp/log and the 32x16 multiply.
 * (reference: encoder/math_sub.cc)
 * ==================================================================== */
// Table for routine Pow2()     table[] = 2^(-1...0)

//	PURPOSE:
//		Computes  pow(2.0, x)
//
//  INPUT:
//      x -  In signed Q10.22 format
//
//	OUTPUT:
//		None
//
//	RETURN:
//		Result in signed Q14.2 format
//

//	PURPOSE:
//				Multiply a 32 bit number (L_var2) and a 16 bit
//              number (var1) returning a 32 bit result. L_var2
//              is truncated to 31 bits prior to executing the
//              multiply.
//
//  INPUT:
//              L_var2 -   A Word32 input variable
//              var1   -   A Word16 input variable
//
//	OUTPUT:
//		None
//
//	RETURN:
//		        A Word32 value
//
static Word32 L_mpy_ls(Word32 L_var2, Word16 var1)
{
	Word32 L_varOut;
	Word16 swtemp;

	swtemp = shr(extract_l(L_var2), 1);
	swtemp = (Word16)32767 & (Word16) swtemp;

	L_varOut = L_mult(var1, swtemp);
	L_varOut = L_shr(L_varOut, 15);
	L_varOut = L_mac(L_varOut, var1, extract_h(L_var2));
	return (L_varOut);
}

// Table for routine cos_fxp()
static const Word16 cos_table[129] =
{
	32767, 32766, 32758, 32746, 32729, 32706, 32679, 32647, 32610,
	32568, 32522, 32470, 32413, 32352, 32286, 32214, 32138, 32058,
	31972, 31881, 31786, 31686, 31581, 31471, 31357, 31238, 31114,
	30986, 30853, 30715, 30572, 30425, 30274, 30118, 29957, 29792,
	29622, 29448, 29269, 29086, 28899, 28707, 28511, 28311, 28106,
	27897, 27684, 27467, 27246, 27020, 26791, 26557, 26320, 26078,
	25833, 25583, 25330, 25073, 24812, 24548, 24279, 24008, 23732,
	23453, 23170, 22884, 22595, 22302, 22006, 21706, 21403, 21097,
	20788, 20475, 20160, 19841, 19520, 19195, 18868, 18538, 18205,
	17869, 17531, 17190, 16846, 16500, 16151, 15800, 15447, 15091,
	14733, 14373, 14010, 13646, 13279, 12910, 12540, 12167, 11793,
	11417, 11039, 10660, 10279,  9896,  9512,  9127,  8740,  8351,
	7962,   7571,  7180,  6787,  6393,  5998,  5602,  5205,  4808,
	4410,   4011,  3612,  3212,  2811,  2411,  2009,  1608,  1206,
	804,     402,     0
};

//	PURPOSE:
//				Computes the cosine of x whose value is expressed in radians/PI.
//
//  INPUT:
//              x - argument in Q1.15
//
//	OUTPUT:
//		None
//
//	RETURN:
//		        Result in Q1.15
//
static Word16 cos_fxp(Word16 x)
{
	Word16 tx, ty;
	Word16 sign;
	Word16 index1,index2;
	Word16 m;
	Word16 temp;

	sign = 0;
	if(x < 0)
		tx = negate(x);
	else
		tx = x;

	// if angle > pi/2, cos(angle) = -cos(pi-angle)
	if(tx > X05_Q15)
	{
		tx = sub(ONE_Q15,tx);
		sign = -1;
	}
	// convert input to be within range 0-128
	index1 = shr(tx,7);
	index2 = add(index1,1);

	if (index1 == 128)
		return (Word16)0;

	m = sub(tx,shl(index1,7));
	// convert decimal part to Q15
	m = shl(m,8);

	temp = sub(cos_table[index2],cos_table[index1]);
	temp = mult(m,temp);
	ty   = add(cos_table[index1],temp);

	if(sign)
		return negate(ty);
	else
		return ty;
}

//	PURPOSE:
//				Computes the sinus of x whose value is expressed in radians/PI.
//
//  INPUT:
//              x - argument in Q1.15
//
//	OUTPUT:
//		None
//
//	RETURN:
//		        Result in Q1.15
//
static Word16 sin_fxp(Word16 x)
{
	Word16 tx, ty;
	Word16 sign;

	sign = 0;
	if(x < 0)
	{
		tx = negate(x);
		sign = 1;
	}
	else
		tx = x;

	if(tx > X05_Q15)
		tx = sub(tx, X05_Q15);
	else
		tx = sub(X05_Q15,tx);

	ty = cos_fxp(tx);

	if(sign)
		return negate(ty);
	else
		return ty;
}

// Table for routine sqrt_l_exp()
// table[] = sqrt((i+16)*2^-6) * 2^15, i.e. sqrt(x) scaled Q15
static const Word16 sqrt_table[49] =
{
	16384, 16888, 17378, 17854, 18318, 18770, 19212, 19644, 20066, 20480,
	20886, 21283, 21674, 22058, 22435, 22806, 23170, 23530, 23884, 24232,
	24576, 24915, 25249, 25580, 25905, 26227, 26545, 26859, 27170, 27477,
	27780, 28081, 28378, 28672, 28963, 29251, 29537, 29819, 30099, 30377,
	30652, 30924, 31194, 31462, 31727, 31991, 32252, 32511, 32767
};

//	PURPOSE:
//				Computes sqrt(L_x), where  L_x is positive.
//
//  INPUT:
//              L_x  - argument in Q1.31
//              *exp - pointer to save denormalization exponent
//	OUTPUT:
//		        Right shift to be applied to result, Q16.0
//
//	RETURN:
//		        Result in Q1.31
//              Right shift should be applied to it!
//
static Word32 sqrt_l_exp(Word32 L_x,  Word16 *exp)
{
	Word16 e, i, a, tmp;
	Word32 L_y;

	if(L_x <= (Word32)0)
	{
		*exp = 0;
		return (Word32)0;
	}

	e    = norm_l(L_x) & 0xFFFE;     // get next lower EVEN norm. exp
	L_x  = L_shl(L_x, e);            // L_x is normalized to [0.25..1)
	*exp = e >> 1;                   // return 2*exponent (or Q1)

	L_x  = L_shr(L_x, 9);
	i    = extract_h(L_x);           // Extract b25-b31, 16 <= i <= 63 because of normalization
	L_x  = L_shr(L_x, 1);
	a    = extract_l(L_x);           // Extract b10-b24
	a    = a & (Word16)0x7fff;

	i    = sub(i, 16);               // 0 <= i <= 47

	L_y  = L_deposit_h(sqrt_table[i]);                // table[i] << 16
	tmp  = sub(sqrt_table[i], sqrt_table[i + 1]);     // table[i] - table[i+1])
	L_y  = L_msu(L_y, tmp, a);                        // L_y -= tmp*a*2

	return L_y;
}

// Table for routine Log2()
static const Word16 log_table[33] =
{
	0,      1455,  2866,  4236,  5568,  6863,  8124,  9352, 10549, 11716,
	12855, 13967, 15054, 16117, 17156, 18172, 19167, 20142, 21097, 22033,
	22951, 23852, 24735, 25603, 26455, 27291, 28113, 28922, 29716, 30497,
	31266, 32023, 32767
};

//	PURPOSE:
//				Computes log2 of x
//
//  INPUT:
//              x  - argument in Q14.2
//
//	OUTPUT:
//              None
//
//	RETURN:
//		        Result in Q10.22
//
static Word32 Log2(Word16 x)
{
	Word16 exp, i, a, tmp;
	Word32 L_y;

	if(x <= (Word16)0)
		return 0;

	exp = norm_s(x);
	x   = shl(x, exp);

	i = shr(x, 9);                              // Extract b15-b9
	a = shl(x & 0x1FF, 6);                      // Extract b8-b0
	i = sub (i, 32);

	L_y = L_deposit_h(log_table[i]);            // table[i] << 16
	tmp = sub(log_table[i], log_table[i + 1]);  // table[i] - table[i+1]
	L_y = L_msu(L_y, tmp, a);                   // L_y -= tmp*a*2

	L_y = L_shr(L_y, 9);

	exp = sub(12, exp);
	L_y = L_add(L_y, L_deposit_h(shl(exp, 6)));

	return L_y;
}
/* ======================================================================
 * Inverse DCT, DCT, the 256-point complex FFT and the twiddles.
 * (reference: encoder/dsp_sub.cc)
 * ==================================================================== */
//	PURPOSE:
//				Perform inverse DCT
//
//
//  INPUT:
//              in     -  pointer to input data
//              m_lim  -  input data's size
//              i_lim  -  result's size
//              out    -  pointer to save result
//
//	OUTPUT:
//		None
//
//	RETURN:
//		        Saved in out result of conversion
//
static void idct(Word16 *in, Word16 m_lim, Word16 i_lim, Word16 *out)
{
	UWord16 angl_step, angl_intl, angl_intl_2;
	UWord16 angl_acc;
	Word32  sum;
	Word16  i, m;

	if(m_lim == 1)
	{
		angl_intl   = CNST_0_5_Q1_15;
		angl_intl_2 = CNST_1_0_Q1_15;
	}
	else
	{
		angl_intl   = div_s ((Word16) CNST_0_5_Q5_11, m_lim << 11); // calculate 0.5/m_lim
		angl_intl_2 = shl(angl_intl, 1);
	}

	angl_step = angl_intl;
	for(i = 0; i < i_lim; i++)
	{
		sum = 0;
		angl_acc = angl_step;
		for(m = 1; m < m_lim; m++)
		{
			sum = L_add(sum, L_shr( L_mult(in[m], cos_fxp(angl_acc)), 7));
			angl_acc += angl_step;
		}
		sum = L_add(sum, L_shr( L_deposit_h(in[0]), 8));
		out[i] = extract_l(L_shr_r (sum, 8));
		angl_step += angl_intl_2;
	}
}

//	PURPOSE:
//				Perform DCT
//
//
//  INPUT:
//              in     -  pointer to input data
//              m_lim  -  input data's size
//              i_lim  -  result's size
//              out    -  pointer to save result
//
//	OUTPUT:
//		None
//
//	RETURN:
//		        Saved in out result of conversion
//
static void dct(Word16 *in, Word16 m_lim, Word16 i_lim, Word16 *out)
{
	UWord16 angl_step, angl_intl, angl_intl_2, angl_begin;
	UWord16 angl_acc;
	Word32  sum;
	Word16  i, m;

	if(m_lim == 1)
	{
		angl_intl   = CNST_0_5_Q1_15;
		angl_intl_2 = CNST_1_0_Q1_15;
	}
	else
	{
		angl_intl   = div_s ((Word16) CNST_0_5_Q5_11, m_lim << 11); // calculate 0.5/m_lim
		angl_intl_2 = shl(angl_intl, 1);
	}

	// Calculate first coefficient
	sum = 0;
	for(m = 0; m < m_lim; m++)
		sum = L_add(sum, L_deposit_l(in[m]));
	out[0] = extract_l(L_mpy_ls(sum, angl_intl_2));

	// Calculate the others coefficients
	angl_begin = angl_intl;
	angl_step  = angl_intl_2;
	for(i = 1; i < i_lim; i++)
	{
		sum = 0;
		angl_acc = angl_begin;
		for(m = 0; m < m_lim; m++)
		{
			sum = L_add(sum, L_deposit_l(mult(in[m], cos_fxp(angl_acc))));
			angl_acc += angl_step;
		}
		out[i] = extract_l(L_mpy_ls(sum, angl_intl_2));

		angl_step  += angl_intl_2;
		angl_begin += angl_intl;
	}
}

static void fft_init(void)
{
	Word16 i, fft_len2, shift, step, theta;

	fft_len2 = shr(FFTLENGTH, 1);
	shift    = norm_s(fft_len2);
	step     = shl(2, shift);
	theta    = 0;

	for(i = 0; i <= fft_len2; i++)
	{
		wr_array[i] = cos_fxp(theta);
		wi_array[i] = sin_fxp(theta);
		if(i >= (fft_len2 - 1))
			theta = ONE_Q15;
		else
			theta = add(theta, step);
	}
}

//  	Subroutine FFT: Fast Fourier Transform
// ***************************************************************
// * Replaces data by its DFT, if isign is 1, or replaces data   *
// * by inverse DFT times nn if isign is -1.  data is a complex  *
// * array of length nn, input as a real array of length 2*nn.   *
// * nn MUST be an integer power of two.  This is not checked    *
// * The real part of the number should be in the zeroeth        *
// * of data , and the imaginary part should be in the next      *
// * element.  Hence all the real parts should have even indeces *
// * and the imaginary parts, odd indeces.			             *
// *                                                             *
// * Data is passed in an array starting in position 0, but the  *
// * code is copied from Fortran so uses an internal pointer     *
// * which accesses position 0 as position 1, etc.		         *
// *                                                             *
// * This code uses e+jwt sign convention, so isign should be    *
// * reversed for e-jwt.                                         *
// ***************************************************************
//
// Q values:
// datam1 - Q14
// isign  - Q15

#define	SWAP(a,b) temp1 = (a);(a) = (b); (b) = temp1

static void fft(Word16 *datam1, Word16 nn, Word16 isign)
{
	Word16 n, mmax, m, j, istep, i;
	Word16 wr, wi, temp1;
	Word32 L_tempr, L_tempi;
	Word16 *data;
	Word32 L_temp1, L_temp2;
	Word16 index, index_step;

	//  Use pointer indexed from 1 instead of 0
	data = &datam1[-1];

	n = shl(nn,1);
	j = 1;
	for( i = 1; i < n; i+=2 )
	{
		if ( j > i)
		{
			SWAP(data[j],data[i]);
			SWAP(data[j+1],data[i+1]);
		}
		m = nn;
		while ( m >= 2 && j > m )
		{
			j = sub(j,m);
			m = shr(m,1);
		}
		j = add(j,m);
	}
	mmax = 2;

	// initialize index step
	index_step = nn;

	while ( n > mmax)
	{
		istep = shl(mmax,1);  // istep = 2 * mmax

		index = 0;
		index_step = shr(index_step,1);

		wr = ONE_Q15;
		wi = 0;
		for ( m = 1; m < mmax; m+=2)
		{
			for ( i = m; i <= n; i += istep)
			{
				j = i + mmax;

				// tempr = wr * data[j] - wi * data[j+1]
				L_temp1 = L_shr(L_mult(wr,data[j]),1);
				L_temp2 = L_shr(L_mult(wi,data[j+1]),1);
				L_tempr = L_sub(L_temp1,L_temp2);

				// tempi = wr * data[j+1] + wi * data[j]
				L_temp1 = L_shr(L_mult(wr,data[j+1]),1);
				L_temp2 = L_shr(L_mult(wi,data[j]),1);
				L_tempi = L_add(L_temp1,L_temp2);

				// data[j] = data[i] - tempr
				L_temp1 = L_shr(L_deposit_h(data[i]),1);
				data[j] = round_h(L_sub(L_temp1,L_tempr));

				// data[i] += tempr
				data[i] = round_h(L_add(L_temp1,L_tempr));

				// data[j+1] = data[i+1] - tempi
				L_temp1 = L_shr(L_deposit_h(data[i+1]),1);
				data[j+1] = round_h(L_sub(L_temp1,L_tempi));

				// data[i+1] += tempi
				data[i+1] = round_h(L_add(L_temp1,L_tempi));
			}
			index = add(index,index_step);
			wr = wr_array[index];
			if (isign < 0)
				wi = negate(wi_array[index]);
			else
				wi = wi_array[index];
		}
		mmax = istep;
	}
}
/* ======================================================================
 * Bit allocation tables accessors.
 * (reference: encoder/aux_sub.cc)
 * ==================================================================== */
//	PURPOSE:
//		Return pointer to bit allocation array
//      according to the number of harmonics
//
//  INPUT:
//		num_harms - The number of harmonics
//
//	OUTPUT:
//		None
//
//	RETURN:
//		Pointer to bits allocation array
//
static const UWord16 *get_bit_allocation_arr(Word16 num_harms)
{
    UWord16 offset_in_word;
    Word16 index;

	if(num_harms == NUM_HARMS_MIN)
		return &bit_allocation_tbl[0];
	else
	{
		index = num_harms - NUM_HARMS_MIN - 1;
		offset_in_word = bit_allocation_offset_tbl[index >> 2] + ((3 + (index >> 2)) * (index & 0x3));
		return &bit_allocation_tbl[offset_in_word];
	}
}

//	PURPOSE:
//		Unpack bit allocation table's item
//
//  INPUT:
//		num_harms - The number of harmonics
//      ptr       - Pointer to buffer to place bit allocation data
//
//	OUTPUT:
//		Unpacked bit allocation table
//
//	RETURN:
//		None
//
static void get_bit_allocation(Word16 num_harms, Word16 *ptr)
{
	const UWord16 *bat_ptr;
    UWord16 tmp;
    Word16 i;

	bat_ptr = get_bit_allocation_arr(num_harms);

	for(i = 0; i < num_harms - 1; i += 4)
	{
		tmp = *bat_ptr++;
		ptr[3] = tmp & 0xF; tmp >>= 4;
		ptr[2] = tmp & 0xF; tmp >>= 4;
		ptr[1] = tmp & 0xF; tmp >>= 4;
		ptr[0] = tmp & 0xF;
		ptr += 4;
	}
}

//	PURPOSE:
//		Set the elements of a 16 bit input vector to zero.
//
//  INPUT:
//		vec       - Pointer to vector
//      n         - size of vec
//
//	OUTPUT:
//		None
//
//	RETURN:
//		None
//
static void v_zap(Word16 *vec, Word16 n)
{
	while(n--)
		*vec++ = 0;
}

//	PURPOSE:
//		Copy the contents of one 16 bit input vector to another
//
//  INPUT:
//		vec1      - Pointer to the destination vector
//		vec2      - Pointer to the source vector
//      n         - size of data should be copied
//
//	OUTPUT:
//		Copy of the source vector
//
//	RETURN:
//		None
//

//	PURPOSE:
//			Compute the sum of square magnitude of a 16 bit input vector
//		    with saturation and truncation.  Output is a 32 bit number.
//
//	INPUT:
//		vec       - Pointer to the vector
//      n         - size of input vectors
//
//	OUTPUT:
//		none
//
//	RETURN:
//		32 bit long signed integer result
//

//	PURPOSE:
//		Copy the contents of one 16 bit input vector to another with shift
//
//  INPUT:
//		vec1      - Pointer to the destination vector
//		vec2      - Pointer to the source vector
//      scale     - right shift factor
//      n         - size of data should be copied
//
//	OUTPUT:
//		Copy of the source vector
//
//	RETURN:
//		None
//
/* ======================================================================
 * Scalar quantisers for the spectral amplitudes.
 * (reference: encoder/qnt_sub.cc)
 * ==================================================================== */
//	PURPOSE:
//		Dequantize by quantizer step size
//
//
//  INPUT:
//		qval         - quantized value
//      step_size    - step size used to quantize in unsigned Q0.16 format
//      bit_num      - the number of bits
//
//	OUTPUT:
//		None
//
//	RETURN:
//		Quantized Value in signed (bit_num).16 format
//
static Word32 deqnt_by_step(Word16 qval, UWord16 step_size, Word16 bit_num)
{
	Word32 res;

	if(bit_num == 0)
		return (Word32)0;

	res = (Word32)step_size * (qval -  (1 << (bit_num - 1)));
	res = L_add(res, ((Word32)step_size * CNST_0_5_Q0_16) >> 16);

	return res;
}

//	PURPOSE:
//		Quantize by quantizer step size
//
//
//  INPUT:
//		val          - value to be quantized in Q5.10 format
//      step_size    - step size used to quantize in unsigned Q0.16 format
//      bit_num      - the number of bits
//
//	OUTPUT:
//		None
//
//	RETURN:
//		Quantized Value in integer
//
static Word16 qnt_by_step(Word16 val, UWord16 step_size, Word16 bit_num)
{
	Word16 index, min_val, max_val;
	Word16 q_index, shift, tmp;

	shift = norm_s(step_size);

	tmp = div_s(0x4000, shl(step_size, shift));     // Remark: To get result in Qxx.16 format it is necessary left shift tmp by (shift + 3)
	q_index = shr_r(mult(val, tmp), sub(9, shift)); // q_index here is rounded to the nearest integer

	max_val = 1 << (bit_num - 1);
	min_val = negate(max_val);

	if(q_index < min_val)
		index = 0;
	else if(q_index >= max_val)
		index = (1 << bit_num) - 1;
	else
		index = max_val + q_index;

	return index;
}

//	PURPOSE:
//		Quantize by table
//
//
//  INPUT:
//		val          - value to be quantized
//      q_tbl        - pointer to table
//      q_tbl_size   - size of table
//
//	OUTPUT:
//		None
//
//	RETURN:
//		Quantized Value in integer
//
static Word16 tbl_quant(Word16 val, Word16 *q_tbl, Word16 q_tbl_size)
{
	Word16 min_index, max_index, index;

	min_index = 0;
	max_index = q_tbl_size - 1;

	if(val >= q_tbl[max_index])
		return max_index;

	if(val <= q_tbl[min_index])
		return min_index;

	while(max_index - min_index != 1)
	{
		index = min_index + ((max_index - min_index) >> 1);

		if(q_tbl[index] > val)
			max_index = index;
		else
			min_index = index;
	}

	if(q_tbl[max_index] - val <= val - q_tbl[min_index])
		return max_index;
	else
		return min_index;
}
/* ======================================================================
 * DC blocker of the analysis input.
 * (reference: encoder/dc_rmv.cc)
 * ==================================================================== */
#define CNST_0_99_Q1_15   0x7EB8

//	PURPOSE:
//		High-pass filter to remove DC
//
//
//  INPUT:
//		*sigin  - pointer to input signal buffer
//      *sigout - pointer to output signal buffer
//      *mem    - pointer to filter's memory element
//       len    - number of input signal samples
//
//	OUTPUT:
//		None
//
//	RETURN:
//       Saved filter state in mem
//
static void dc_rmv(Word16 *sigin, Word16 *sigout, Word32 *mem, Word16 len)
{
	Word32 L_tmp, L_mem;

	L_mem = *mem;
	while(len--)
	{
		L_tmp = L_deposit_h(*sigin++);
		L_mem = L_add(L_mem, L_tmp);
		*sigout++ = round_h(L_mem);
		L_mem = L_mpy_ls(L_mem, CNST_0_99_Q1_15);
		L_mem = L_sub(L_mem, L_tmp);
	}
	*mem = L_mem;
}
/* ======================================================================
 * Pitch estimation low-pass.
 * (reference: encoder/pe_lpf.cc)
 * ==================================================================== */
static const Word16 lpf_coef[PE_LPF_ORD] =
{
	 -94,  -92,  185,   543,  288, -883, -1834, -495, 3891, 9141, 11512,
	9141, 3891, -495, -1834, -883,  288,   543,  185,  -92,  -94
};

//	PURPOSE:
//		Low-pass filter for pitch estimator
//
//
//  INPUT:
//		*sigin  - pointer to input signal buffer
//      *sigout - pointer to output signal buffer
//      *mem    - pointer to filter's memory element
//       len    - number of input signal samples
//
//	OUTPUT:
//		None
//
//	RETURN:
//       Saved filter state in mem
//
static void pe_lpf(Word16 *sigin, Word16 *sigout, Word16 *mem, Word16 len)
{
	Word16 i;
	Word32 L_sum;

	while(len--)
	{
		for(i = 0; i < PE_LPF_ORD - 1; i++)
			mem[i] = mem[i + 1];
		mem[PE_LPF_ORD - 1] = *sigin++;

		L_sum = 0;
		for(i = 0; i < PE_LPF_ORD; i++)
			L_sum = L_mac(L_sum, mem[i], lpf_coef[i]);

		*sigout++ = round_h(L_sum);
	}
}
/* ======================================================================
 * Open-loop pitch estimation.
 * (reference: encoder/pitch_est.cc)
 * ==================================================================== */
static const UWord16 min_max_tbl[203] =
{
	0x0008, 0x0009, 0x000a, 0x000c, 0x000d, 0x000e, 0x000f, 0x0010, 0x0012, 0x0013,
	0x0014, 0x0115, 0x0216, 0x0218, 0x0319, 0x041a, 0x051b, 0x061c, 0x061e, 0x071f,
	0x0820, 0x0921, 0x0a22, 0x0a24, 0x0b25, 0x0c26, 0x0d27, 0x0e28, 0x0e2a, 0x0f2b,
	0x102c, 0x112d, 0x122e, 0x1230, 0x1331, 0x1432, 0x1533, 0x1634, 0x1636, 0x1737,
	0x1838, 0x1939, 0x1a3a, 0x1a3c, 0x1b3d, 0x1c3e, 0x1d3f, 0x1e40, 0x1e42, 0x1f43,
	0x2044, 0x2145, 0x2246, 0x2248, 0x2349, 0x244a, 0x254b, 0x264c, 0x264e, 0x274f,
	0x2850, 0x2951, 0x2a52, 0x2a54, 0x2b55, 0x2c56, 0x2d57, 0x2e58, 0x2e5a, 0x2f5b,
	0x305c, 0x315d, 0x325e, 0x3260, 0x3361, 0x3462, 0x3563, 0x3664, 0x3666, 0x3767,
	0x3868, 0x3969, 0x3a6a, 0x3a6c, 0x3b6d, 0x3c6e, 0x3d6f, 0x3e70, 0x3e72, 0x3f73,
	0x4074, 0x4175, 0x4276, 0x4278, 0x4379, 0x447a, 0x457b, 0x467c, 0x467e, 0x477f,
	0x4880, 0x4981, 0x4a82, 0x4a84, 0x4b85, 0x4c86, 0x4d87, 0x4e88, 0x4e8a, 0x4f8b,
	0x508c, 0x518d, 0x528e, 0x5290, 0x5391, 0x5492, 0x5593, 0x5694, 0x5696, 0x5797,
	0x5898, 0x5999, 0x5a9a, 0x5a9c, 0x5b9d, 0x5c9e, 0x5d9f, 0x5ea0, 0x5ea2, 0x5fa3,
	0x60a4, 0x61a5, 0x62a6, 0x62a8, 0x63a9, 0x64aa, 0x65ab, 0x66ac, 0x66ae, 0x67af,
	0x68b0, 0x69b1, 0x6ab2, 0x6ab4, 0x6bb5, 0x6cb6, 0x6db7, 0x6eb8, 0x6eba, 0x6fbb,
	0x70bc, 0x71bd, 0x72be, 0x72c0, 0x73c1, 0x74c2, 0x75c3, 0x76c4, 0x76c6, 0x77c7,
	0x78c8, 0x79c9, 0x7aca, 0x7aca, 0x7bca, 0x7cca, 0x7dca, 0x7eca, 0x7eca, 0x7fca,
	0x80ca, 0x81ca, 0x82ca, 0x82ca, 0x83ca, 0x84ca, 0x85ca, 0x86ca, 0x86ca, 0x87ca,
	0x88ca, 0x89ca, 0x8aca, 0x8aca, 0x8bca, 0x8cca, 0x8dca, 0x8eca, 0x8eca, 0x8fca,
	0x90ca, 0x91ca, 0x92ca, 0x92ca, 0x93ca, 0x94ca, 0x95ca, 0x96ca, 0x96ca, 0x97ca,
	0x98ca, 0x99ca, 0x9aca
};

static void pitch_est_init(void)
{
	prev_pitch = prev_prev_pitch = 158; // 100
	prev_e_p = prev_prev_e_p = 0;
}

/* Autocorrelation of the windowed signal.  Three calls per frame, ~130 lags
 * each over ~250 samples: this is where the reference spends most of its
 * time, because every term goes through four saturating ETSI operators.
 *
 * It is bit-exact, for three reasons that the code below depends on:
 *
 *  - every term is a square, so the series is non-decreasing and the
 *    reference's saturating L_add can only clamp at MAX_32 and stay there;
 *    the comparison at the end does exactly that;
 *  - L_mult saturates only on (-32768)*(-32768), and `sigin` is the input
 *    multiplied by a positive Q15 window (mult_r), so it cannot reach -32768;
 *  - L_shr is applied to each term by the reference, and for a positive value
 *    ((2p) >> s) == (p >> (s-1)), which is why the doubling disappears.
 */
static Word32 autocorr(const Word16 *sigin, Word16 shift, Word16 scale_shift)
{
	const int sh = scale_shift - 1;   /* -1 when there is no scaling */
	const int n = PITCH_EST_FRAME - shift;
	int64_t acc = 0;

	if (sh < 0) {
		for (int i = 0; i < n; i++)
			acc += 2 * (int64_t)((int32_t)sigin[i] * (int32_t)sigin[i + shift]);
	} else {
		for (int i = 0; i < n; i++)
			acc += (int32_t)sigin[i] * (int32_t)sigin[i + shift] >> sh;
	}

	return (acc > MAX_32) ? MAX_32 : (Word32)acc;
}

static void e_p(Word16 *sigin, Word16 *res_buf)
{
	Word16 i, j, den_part_acc, tmp;
	Word32 L_sum, L_num, L_den, L_e0, L_tmp;
	Word16 sig_wndwed[PITCH_EST_FRAME];
	Word32 corr[259];
	Word16 index_beg, index_step;
	Word16 scale_shift;

	// Windowing input signal s * wi^2
	for(i = 0 ; i < PITCH_EST_FRAME; i++)
		sig_wndwed[i] = mult_r(sigin[i], wi[i]);

	L_sum = 0;
	for(i = 0 ; i < PITCH_EST_FRAME; i++)
		L_sum = L_add(L_sum, L_mpy_ls( L_mult(sigin[i], sigin[i]), wi[i]) );     // sum(s^2 * wi^2)

	// Check for the overflow
	if(L_sum == MAX_32)
	{
		// Recalculate with scaling
		L_sum = 0;
		for(i = 0 ; i < PITCH_EST_FRAME; i++)
			L_sum = L_add(L_sum, L_mpy_ls( L_shr(L_mult(sigin[i], sigin[i]), 5), wi[i]));
		scale_shift = 5;
	}
	else
		scale_shift = 0;

	L_e0 = 0;
	for(i = 0 ; i < PITCH_EST_FRAME; i++)
		L_e0 = L_add(L_e0, L_shr( L_mult(sig_wndwed[i], sig_wndwed[i]), scale_shift));              // sum(s^2 * wi^4)

    // Calculate correlation for time shift in range 21...150 with step 0.5
	// For integer shifts
	for(tmp = 21, i = 0; tmp <= 150; tmp++, i += 2)
		corr[i] = autocorr(sig_wndwed, tmp, scale_shift);
	// For intermediate shifts
	for(i = 1; i < 258; i += 2)
		corr[i] = L_shr( L_add(corr[i - 1], corr[i + 1]), 1);

	// variable to calculate 1 - P * sum(wi ^4) in denominator
	den_part_acc = CNST_0_8717_Q1_15;

	index_step = 42;               // Note: 42 = 21 in Q15.1 format, so index_step will be used also as p in Q15.1 format
	index_beg  = 0;
	L_e0 = L_shr(L_e0, 7);         // divide by 64 to compensate wi scaling
	// p = 21...122 by step 0.5
	for(i = 0; i < 203; i++)
	{

		// Calculate sum( corr ( n * p) )
		L_tmp = 0;
		j = index_beg;
		while(j <= 258)
		{
			L_tmp = L_add(L_tmp, corr[j]);
			j += index_step;
		}

		L_tmp = L_shr(L_tmp, 6);       // compensate wi scaling
		L_tmp = L_add(L_tmp, L_e0);    // For n = 0
		L_tmp = L_tmp * index_step;
		L_num = L_sub(L_sum, L_tmp);

		index_beg++;
		index_step++;

		L_den = L_mpy_ls(L_sum, den_part_acc);

		if(L_num < L_den && L_den != 0)
		{
			//res_buf[i] = (Word16)((double)L_num/(double)L_den * 4096.);  // Q4.12

			if(L_num <= 0)
				res_buf[i] = 0;
			else
			{
				tmp = norm_l(L_den);
				tmp = div_s(extract_h(L_shl(L_num, tmp)), extract_h(L_shl(L_den, tmp)));
				res_buf[i] = shr(tmp, 3);  // convert to Q4.12
			}
		}
		else if(L_num >= L_den)
		{
			res_buf[i] = CNST_1_00_Q4_12;
			//res_buf[i] = (Word16)((double)L_num/(double)L_den * 4096.);  // Q4.12
		}
		else
			res_buf[i] = CNST_1_00_Q4_12;

		den_part_acc = sub(den_part_acc, CNST_0_0031_Q1_15);
	}
}

static void pitch_est(imbe_param_t *imbe_param, Word16 *frames_buf)
{
	Word16 e_p_arr0[203], e_p_arr1[203], e_p_arr2[203], e1p1_e2p2_est_save[203];
	Word16 min_index, max_index, p, i, p_index;
	UWord16 tmp=0, p_fp;
	UWord32 UL_tmp;
	Word16 e_p_cur, pb, pf, ceb, s_tmp;
	Word16 cef_est, cef, p0_est, p0, p1, p2, p1_max_index, p2_max_index, e1p1_e2p2_est;
        Word16 e_p_arr2_min[203];

	// Calculate E(p) function for current and two future frames
	e_p(&frames_buf[0], e_p_arr0);

	// Look-Back Pitch Tracking
	min_index = HI_BYTE(min_max_tbl[prev_pitch]);
	max_index = LO_BYTE(min_max_tbl[prev_pitch]);

	p = pb = min_index;
	e_p_cur = e_p_arr0[min_index];
	while(++p <= max_index)
		if(e_p_arr0[p] < e_p_cur)
		{
			e_p_cur = e_p_arr0[p];
			pb = p;
		}
	ceb = add(e_p_cur, add(prev_e_p, prev_prev_e_p));

	if(ceb <= CNST_0_48_Q4_12)
	{
		prev_prev_pitch = prev_pitch;
		prev_pitch      = pb;
		prev_prev_e_p   = prev_e_p;
		prev_e_p        = e_p_arr0[pb];

		imbe_param->pitch = pb + 42;  // Result in Q15.1 format
		imbe_param->e_p = prev_e_p;
		return;
	}

	// Look-Ahead Pitch Tracking
	e_p(&frames_buf[FRAME],     e_p_arr1);
	e_p(&frames_buf[2 * FRAME], e_p_arr2);

	p0_est = p0 = 0;
	cef_est = e_p_arr0[p0] + e_p_arr1[p0] + e_p_arr2[p0];

            p1 = 0;
            while(p1 < 203)
            {
                        p2 = HI_BYTE(min_max_tbl[p1]);
                        p2_max_index = LO_BYTE(min_max_tbl[p1]);
                        s_tmp = e_p_arr2[p1];
                        while(p2 <= p2_max_index)
                        {
                                   if(e_p_arr2[p2] < s_tmp)
                                               s_tmp = e_p_arr2[p2];
                                   p2++;
                        }
                        e_p_arr2_min[p1] = s_tmp;
                        p1++;
            }
            while(p0 < 203)
            {
                        e1p1_e2p2_est = e_p_arr1[p0] + e_p_arr2_min[p0];
                        p1 = HI_BYTE(min_max_tbl[p0]);
                        p1_max_index = LO_BYTE(min_max_tbl[p0]);
                        while(p1 <= p1_max_index)
                        {
                                   if(add(e_p_arr1[p1], e_p_arr2_min[p1]) < e1p1_e2p2_est)
                                               e1p1_e2p2_est = add(e_p_arr1[p1], e_p_arr2_min[p1]);
                                   p1++;
                        }
                        e1p1_e2p2_est_save[p0] = e1p1_e2p2_est;
                        cef = add(e_p_arr0[p0], e1p1_e2p2_est);
                        if(cef < cef_est)
                        {
                                   cef_est = cef;
                                   p0_est  = p0;
                        }
                        p0++;
            }

	pf = p0_est;
	// Sub-multiples analysis
	if(pf >= 42)  // Check if Sub-multiples are possible
	{
		if(pf < 84)
			i = 1;
		else if(pf < 126)
			i = 2;
		else if(pf < 168)
			i = 3;
		else
			i = 4;

		p_fp = (pf + 42) << 8; // Convert pitch estimation from array index to unsigned Q7.19 format

		while(i--)
		{
			switch(i)
			{
				case 0:
					tmp = p_fp >> 1;                     // P_est/2
				break;

				case 1:
					UL_tmp = (UWord32)p_fp * 0x5555;     // P_est/3
					tmp = UL_tmp >> 16;
				break;

				case 2:
					tmp = p_fp >> 2;                     // P_est/4
				break;

				case 3:
					UL_tmp = (UWord32)p_fp * 0x3333;     // P_est/5
					tmp = UL_tmp >> 16;
				break;
			}

			p_index = ((tmp + 0x0080) >> 8) - 42;       // Convert fixed-point pitch value to integer array index with rounding

			cef = add(e_p_arr0[p_index], e1p1_e2p2_est_save[p_index]);

			if(cef <= CNST_0_85_Q4_12 && mult_r(cef, CNST_0_5882_Q1_15) <= cef_est)  // 1/1.7 = 0.5882
			{
				pf = p_index;
				break;
			}

			if(cef <= CNST_0_4_Q4_12 && mult_r(cef, CNST_0_2857_Q1_15) <= cef_est)   // 1/3.5 = 0.2857
			{
				pf = p_index;
				break;
			}

			if(cef <= CNST_0_05_Q4_12)
			{
				pf = p_index;
				break;
			}
		}
	}

	cef = add(e_p_arr0[pf], e1p1_e2p2_est_save[pf]);

	if(ceb <= cef)
		p = pb;
	else
		p = pf;

	prev_prev_pitch = prev_pitch;
	prev_pitch      = p;
	prev_prev_e_p   = prev_e_p;
	prev_e_p        = e_p_arr0[p];

	imbe_param->pitch = p + 42;  // Result in Q15.1 format
	imbe_param->e_p = prev_e_p;
}
/* ======================================================================
 * Closed-loop pitch refinement.
 * (reference: encoder/pitch_ref.cc)
 * ==================================================================== */
#define PITCH_REF_FRAME  221
#define MIN_INDEX         50

static void pitch_ref(imbe_param_t *imbe_param, cmplx16_t *fft_buf)
{
	Word16 i, j, index_a_save, pitch_est, tmp, shift, index_wr, up_lim;
	cmplx16_t sp_rec[FFTLENGTH/2];
	Word32 fund_freq, fund_freq_2, fund_freq_acc_a, fund_freq_acc_b, fund_freq_acc, L_tmp, amp_re_acc, amp_im_acc, L_sum, L_diff_min;
	Word16 ha, hb, index_a, index_b, index_tbl[20], it_ind, re_tmp, im_tmp, pitch_cand=0;
	Word32 fund_freq_cand=0;

	pitch_est = shl(imbe_param->pitch, 7);                      // Convert to Q8.8
	pitch_est = sub(pitch_est, CNST_1_125_Q8_8);                // Sub 1.125 = 9/8

	L_diff_min = MAX_32;
	for(i = 0; i < 19; i++)
	{
		shift = norm_s(pitch_est);
		tmp = shl(pitch_est, shift);
		tmp = div_s(0x4000, tmp);
		fund_freq = L_shl(tmp, shift + 11);

		fund_freq_acc   = fund_freq;
		fund_freq_2     = L_shr(fund_freq, 1);
		fund_freq_acc_a = L_sub(fund_freq, fund_freq_2);
		fund_freq_acc_b = L_add(fund_freq, fund_freq_2);

        // Calculate upper limit for spectrum reconstruction
		up_lim = extract_h(L_shr((UWord32)CNST_0_9254_Q0_16 * pitch_est, 1));  // 0.9254/fund_freq
		up_lim = sub(up_lim, CNST_0_5_Q8_8);                                   // sub 0.5
		up_lim = up_lim & 0xFF00;                                              // extract fixed part
		up_lim = mult(up_lim, extract_h(fund_freq));
		up_lim = shr(up_lim, 1);

		index_b = 0;
		while(index_b <= up_lim)
		{
			ha = extract_h(fund_freq_acc_a);
			hb = extract_h(fund_freq_acc_b);
			index_a = (ha >> 8) + ((ha & 0xFF)?1:0);
			index_b = (hb >> 8) + ((hb & 0xFF)?1:0);

			if(index_b >= MIN_INDEX)
			{
				L_tmp = L_shl(L_deposit_h(index_a), 8);
				L_tmp = L_sub(L_tmp, fund_freq_acc);
				L_tmp = L_add(L_tmp, 0x00020000);   // for rounding purpose
				L_tmp = L_shr(L_tmp, 2);

				index_a_save = index_a;
				it_ind = 0;

				amp_re_acc = amp_im_acc = 0;
				while(index_a < index_b)
				{
					index_wr = extract_h(L_tmp);
					if(index_wr < 0 && (L_tmp & 0xFFFF)) // truncating for negative number
						index_wr = add(index_wr, 1);
					index_wr = add(index_wr, 160);
					index_tbl[it_ind++] = index_wr;
					if(index_wr >= 0 && index_wr <= 320)
					{
						amp_re_acc = L_mac(amp_re_acc, fft_buf[index_a].re, wr_sp[index_wr]);
						amp_im_acc = L_mac(amp_im_acc, fft_buf[index_a].im, wr_sp[index_wr]);
					}

					index_a++;
					L_tmp = L_add(L_tmp, 0x400000);
				}

				it_ind = 0;
				index_a = index_a_save;
				while(index_a < index_b)
				{
					index_wr = index_tbl[it_ind++];
					if(index_wr < 0 || index_wr > 320)
					{
						sp_rec[index_a].im = sp_rec[index_a].re = 0;
					}
					else
					{
						sp_rec[index_a].im = mult(mult(extract_h(amp_im_acc), wr_sp[index_wr]), 0x6666);
						sp_rec[index_a].re = mult(mult(extract_h(amp_re_acc), wr_sp[index_wr]), 0x6666);
					}

					index_a++;
				}
			}

			fund_freq_acc_a = L_add(fund_freq_acc_a, fund_freq);
			fund_freq_acc_b = L_add(fund_freq_acc_b, fund_freq);
			fund_freq_acc   = L_add(fund_freq_acc,   fund_freq);
		}

		L_sum = 0;
		for(j = MIN_INDEX; j <= up_lim; j++)
		{
			re_tmp = sub(fft_buf[j].re, sp_rec[j].re);
			im_tmp = sub(fft_buf[j].im, sp_rec[j].im);
			L_sum = L_mac(L_sum, re_tmp, re_tmp);
			L_sum = L_mac(L_sum, im_tmp, im_tmp);
		}

		if(L_sum < L_diff_min)
		{
			L_diff_min     = L_sum;
			pitch_cand     = pitch_est;
			fund_freq_cand = fund_freq;
		}

		pitch_est = add(pitch_est,  CNST_0_125_Q8_8); // Add 0.125 = 1/8
	}

	imbe_param->ref_pitch = pitch_cand;
	imbe_param->fund_freq = fund_freq_cand;
}
/* ======================================================================
 * Voiced/unvoiced decision per harmonic.
 * (reference: encoder/v_uv_det.cc)
 * ==================================================================== */
#define CNST_0_5625_Q1_15   0x4800
#define CNST_0_45_Q1_15     0x3999
#define CNST_0_1741_Q1_15   0x164A
#define CNST_0_1393_Q1_15   0x11D5
#define CNST_0_99_Q1_15     0x7EB8
#define CNST_0_01_Q1_15     0x0148
#define CNST_0_0025_Q1_15   0x0051
#define CNST_0_25_Q1_15     0x2000
#define CNST_PI_4_Q1_15     0x6488
#define CNST_0_55_Q4_12     0x08CD


extern int frame_cnt;

static void pitch_ref_init(void)
{
	v_zap(v_uv_dsn, NUM_BANDS_MAX);
	th_max = 0;
}

static Word16 voiced_sa_calc(Word32 num, Word16 den)
{
	Word16 tmp;
	Word32 L_tmp;

	L_tmp = L_mpy_ls(num, den);
	L_tmp = sqrt_l_exp(L_tmp, &tmp);
	L_tmp = L_shr(L_tmp, tmp - 3);

	//L_tmp =0;
	//return (Word16)(2*256.0*sqrt(2*(double)num/(double)den));
	return extract_h(L_tmp);
}

static Word16 unvoiced_sa_calc(Word32 num, Word16 den)
{
	Word16 shift, tmp;
	Word32 L_tmp;

	shift = norm_s(den);
	tmp   = div_s(0x4000, shl(den, shift));
	L_tmp = L_shl(L_mpy_ls(num, tmp), shift + 2);
	L_tmp = sqrt_l_exp(L_tmp, &tmp);
	L_tmp = L_shr(L_tmp, tmp - 2 + 6);
	L_tmp = L_mpy_ls(L_tmp, 0x4A76);

	//L_tmp =0;
	//return (Word16)(2*0.1454 * sqrt(2*256*(double)num/(double)den));
	return extract_h(L_tmp);
}

//=============================================================================
//
// Voiced/Unvoiced Determination & Spectral Amplitudes Estimation
//
//=============================================================================
static void v_uv_det(imbe_param_t *imbe_param, cmplx16_t *fft_buf)
{
	Word16 i, j, index_a_save, tmp, index_wr;
	Word32 fund_freq, fund_freq_2, fund_freq_acc_a, fund_freq_acc_b, fund_freq_acc, fund_fr_acc, L_tmp, amp_re_acc, amp_im_acc;
	Word16 ha, hb, index_a, index_b, index_tbl[30], it_ind, re_tmp, im_tmp, re_tmp2, im_tmp2, sc_coef;
	Word32 M_num[NUM_HARMS_MAX], M_num_sum, M_den_sum, D_num, D_den, th_lf, th_hf, th0, fund_fr_step, M_fcn_num, M_fcn_den;
	Word16 sp_rec_re, sp_rec_im, M_fcn;
	Word16 band_cnt, num_harms_cnt, uv_harms_cnt,  Dk;
	Word16 num_harms, num_bands, dsn_thr=0;
	Word16 thr[NUM_BANDS_MAX], M_den[NUM_HARMS_MAX], b1_vec;

	(void)thr;  /* filled in by the reference and never read on either side */
	fund_freq = imbe_param->fund_freq;

	tmp = shr( add( shr(imbe_param->ref_pitch, 1),  CNST_0_25_Q8_8), 8);     // fix(pitch_cand / 2 + 0.5)
	num_harms = extract_h((UWord32)CNST_0_9254_Q0_16 * tmp);                 // fix(0.9254 * fix(pitch_cand / 2 + 0.5))
	if(num_harms < NUM_HARMS_MIN)
		num_harms = NUM_HARMS_MIN;
	else if(num_harms > NUM_HARMS_MAX)
		num_harms = NUM_HARMS_MAX;

	if(num_harms <= 36)
		num_bands = extract_h((UWord32)(num_harms + 2) * CNST_0_33_Q0_16);   // fix((L+2)/3)
	else
		num_bands = NUM_BANDS_MAX;

	imbe_param->num_harms = num_harms;
	imbe_param->num_bands = num_bands;

	//=========================================================================
	//
	// M(th) function calculation
	//
	//=========================================================================
	for(j = 0, th_lf = 0; j < 64; j++)
	{
		th_lf = L_mac(th_lf, fft_buf[j].re, fft_buf[j].re);
		th_lf = L_mac(th_lf, fft_buf[j].im, fft_buf[j].im);
	}
	for(j = 64, th_hf = 0; j < 128; j++)
	{
		th_hf = L_mac(th_hf, fft_buf[j].re, fft_buf[j].re);
		th_hf = L_mac(th_hf, fft_buf[j].im, fft_buf[j].im);
	}
	th0 = L_add(th_lf, th_hf);

	if(th0 > th_max)
		th_max = L_shr(L_add(th_max, th0), 1);
	else
		th_max = L_add(L_mpy_ls(th_max, CNST_0_99_Q1_15), L_mpy_ls(th0, CNST_0_01_Q1_15));

	M_fcn_num = L_add(th0, L_mpy_ls(th_max, CNST_0_0025_Q1_15));
	M_fcn_den = L_add(th0, L_mpy_ls(th_max, CNST_0_01_Q1_15));
	if(M_fcn_den == 0)
		M_fcn = CNST_0_25_Q1_15;
	else
	{
		tmp       = norm_l(M_fcn_den);
		M_fcn_den = L_shl(M_fcn_den, tmp);
		M_fcn_num = L_shl(M_fcn_num, tmp);

		M_fcn = div_s(extract_h(M_fcn_num), extract_h(M_fcn_den));

		if(th_lf < (L_tmp= L_add(L_shl(th_hf, 2), th_hf)))           // compare th_lf < 5*th_hf
		{
			tmp       = norm_l(L_tmp);
			M_fcn_den = L_shl(L_tmp, tmp);
			th_lf     = L_shl(th_lf, tmp);

			tmp   = div_s(extract_h(th_lf), extract_h(M_fcn_den));
			L_tmp = sqrt_l_exp(L_deposit_h(tmp),  &tmp);
			if(tmp)
				L_tmp = L_shr(L_tmp, tmp);
			M_fcn = mult(M_fcn, extract_h(L_tmp));
		}
	}
	// ========================================================================
	fund_fr_step  = L_shl(L_mpy_ls(fund_freq, CNST_PI_4_Q1_15), 2);  // mult by PI

	uv_harms_cnt    = 0;
	b1_vec          = 0;
	band_cnt        = 0;
	num_harms_cnt   = 0;
	D_num = D_den   = 0;

	fund_fr_acc     = 0;
	fund_freq_acc   = fund_freq;
	fund_freq_2     = L_shr(fund_freq, 1);
	fund_freq_acc_a = L_sub(fund_freq, fund_freq_2);
	fund_freq_acc_b = L_add(fund_freq, fund_freq_2);
	for(j = 0; j < num_harms; j++)
	{
		ha = extract_h(fund_freq_acc_a);
		hb = extract_h(fund_freq_acc_b);
		index_a = (ha >> 8) + ((ha & 0xFF)?1:0);
		index_b = (hb >> 8) + ((hb & 0xFF)?1:0);

		L_tmp = L_shl(L_deposit_h(index_a), 8);
		L_tmp = L_sub(L_tmp, fund_freq_acc);
		L_tmp = L_add(L_tmp, 0x00020000);   // for rounding purpose
		L_tmp = L_shr(L_tmp, 2);

		index_a_save = index_a;
		it_ind = 0;

		// =========== v/uv determination threshold function ==
		if(num_harms_cnt == 0)   // calculate one time per band
		{
			if(imbe_param->e_p > CNST_0_55_Q4_12 && band_cnt >= 1)
				dsn_thr = 0;
			else if(v_uv_dsn[band_cnt] == 1)
				dsn_thr = mult(M_fcn, sub(CNST_0_5625_Q1_15, mult(CNST_0_1741_Q1_15, extract_h(fund_fr_acc))));
			else
				dsn_thr = mult(M_fcn, sub(CNST_0_45_Q1_15,   mult(CNST_0_1393_Q1_15, extract_h(fund_fr_acc))));

			fund_fr_acc = L_add(fund_fr_acc, fund_fr_step);

			thr[band_cnt] = dsn_thr;
		}
		// ====================================================

		M_den_sum  = 0;
		amp_re_acc = amp_im_acc = 0;
		while(index_a < index_b)
		{
			index_wr = extract_h(L_tmp);
			if(index_wr < 0 && (L_tmp & 0xFFFF)) // truncating for negative number
				index_wr = add(index_wr, 1);
			index_wr = add(index_wr, 160);
			index_tbl[it_ind++] = index_wr;
			if(index_wr >= 0 && index_wr <= 320)
			{
				amp_re_acc = L_mac(amp_re_acc, fft_buf[index_a].re, wr_sp[index_wr]);
				amp_im_acc = L_mac(amp_im_acc, fft_buf[index_a].im, wr_sp[index_wr]);
				M_den_sum  = L_add(M_den_sum, mult(wr_sp[index_wr], wr_sp[index_wr]));
			}

			index_a++;
			L_tmp = L_add(L_tmp, 0x400000);
		}
		sc_coef = div_s(0x4000, extract_l(L_shr(M_den_sum, 1)));
		im_tmp2 = mult(extract_h(amp_im_acc), sc_coef);
		re_tmp2 = mult(extract_h(amp_re_acc), sc_coef);

		M_num_sum = 0;
		it_ind    = 0;
		index_a   = index_a_save;
		while(index_a < index_b)
		{
			index_wr = index_tbl[it_ind++];
			if(index_wr < 0 || index_wr > 320)
				sp_rec_re = sp_rec_im = 0;
			else
			{
				sp_rec_im = mult( im_tmp2, wr_sp[index_wr]);
				sp_rec_re = mult( re_tmp2, wr_sp[index_wr]);
			}

			re_tmp = sub(fft_buf[index_a].re, sp_rec_re);
			im_tmp = sub(fft_buf[index_a].im, sp_rec_im);
			D_num = L_mac(D_num, re_tmp, re_tmp);
			D_num = L_mac(D_num, im_tmp, im_tmp);

			M_num_sum = L_mac(M_num_sum, fft_buf[index_a].re, fft_buf[index_a].re);
			M_num_sum = L_mac(M_num_sum, fft_buf[index_a].im, fft_buf[index_a].im);

			index_a++;
		}

		M_den[j] = sc_coef;
		M_num[j] = M_num_sum;
		D_den    = L_add(D_den, M_num_sum);

		if(++num_harms_cnt == 3 && band_cnt < num_bands - 1)
		{
			b1_vec <<= 1;

			if(D_den > D_num && D_den != 0)
			{
				tmp = norm_l(D_den);
				Dk  = div_s(extract_h(L_shl(D_num, tmp)), extract_h(L_shl(D_den, tmp)));
			}
			else
				Dk = MAX_16;

			if(Dk < dsn_thr)
			{
				// voiced band
				v_uv_dsn[band_cnt] = 1;
				b1_vec |= 1;
				i = j - 2;
				while(i <= j)
				{
					imbe_param->sa[i] = voiced_sa_calc(M_num[i], M_den[i]);
					imbe_param->v_uv_dsn[i] = 1;
					i++;
				}
			}
			else
			{
				// unvoiced band
				v_uv_dsn[band_cnt] = 0;
				i = j - 2;
				while(i <= j)
				{
					imbe_param->sa[i] = unvoiced_sa_calc(M_num[i], index_b - index_a_save);
					imbe_param->v_uv_dsn[i] = 0;
					uv_harms_cnt++;
					i++;
				}
			}

			D_num = D_den = 0;
			num_harms_cnt = 0;
			band_cnt++;
		}

		fund_freq_acc_a = L_add(fund_freq_acc_a, fund_freq);
		fund_freq_acc_b = L_add(fund_freq_acc_b, fund_freq);
		fund_freq_acc   = L_add(fund_freq_acc,   fund_freq);
	}

	if(num_harms_cnt)
	{
		b1_vec <<= 1;
		if(D_den > D_num && D_den != 0)
		{
			tmp = norm_l(D_den);
			Dk= div_s(extract_h(L_shl(D_num, tmp)), extract_h(L_shl(D_den, tmp)));
		}
		else
			Dk = MAX_16;

		if(Dk < dsn_thr)
		{
			// voiced band
			v_uv_dsn[band_cnt] = 1;
			b1_vec |= 1;

			i = num_harms - num_harms_cnt;
			while(i < num_harms)
			{
				imbe_param->sa[i] = voiced_sa_calc(M_num[i], M_den[i]);
				imbe_param->v_uv_dsn[i] = 1;
				i++;
			}
		}
		else
		{
			// unvoiced band
			v_uv_dsn[band_cnt] = 0;
			i = num_harms - num_harms_cnt;
			while(i < num_harms)
			{
				imbe_param->sa[i] = unvoiced_sa_calc(M_num[i], index_b - index_a_save);
				imbe_param->v_uv_dsn[i] = 0;
				uv_harms_cnt++;
				i++;
			}
		}
	}

	imbe_param->l_uv = uv_harms_cnt;

	imbe_param->b_vec[1] = b1_vec;                                       // Save encoded voiced/unvoiced decision
	imbe_param->b_vec[0] = shr( sub(imbe_param->ref_pitch, 0x1380), 7);  // Pitch encode  fix(2*pitch - 39)

}
/* ======================================================================
 * Spectral amplitude quantisation.
 * (reference: encoder/sa_encode.cc)
 * ==================================================================== */
static void sa_encode_init(void)
{
	Word16 i;
	num_harms_prev2 = 30;
	for(i = 0; i < NUM_HARMS_MAX + 2; i++)
		sa_prev2[i] = 0;
}

static void sa_encode(imbe_param_t *imbe_param)
{
	Word16 gain_vec[6], gain_r[6];
	UWord16 index, i, j, num_harms;
	Word16 *ba_ptr, *t_vec_ptr, *b_vec_ptr, *gss_ptr, *sa_ptr;
	Word16 t_vec[NUM_HARMS_MAX], c_vec[MAX_BLOCK_LEN];
	UWord32 lmprbl_item;
	Word16 bl_len, step_size, num_bits, tmp, ro_coef, si_coef, tmp1;
	UWord32 k_coef, k_acc;
	Word32 sum, tmp_word32, vec32_tmp[NUM_HARMS_MAX], *vec32_ptr;

	num_harms = imbe_param->num_harms;

	// Calculate num_harms_prev2/num_harms. Result save in unsigned format Q8.24
	if(num_harms == num_harms_prev2)
		k_coef = (Word32)CNST_ONE_Q8_24;
	else if(num_harms > num_harms_prev2)
		k_coef = (Word32)div_s(num_harms_prev2 << 9, num_harms << 9) << 9;
	else
	{
		// imbe_param->num_harms < num_harms_prev2
		k_coef = 0;
		tmp = num_harms_prev2;
		while(tmp > num_harms)
		{
			tmp -= num_harms;
			k_coef += (Word32)CNST_ONE_Q8_24;
		}
		k_coef += (Word32)div_s(tmp << 9, num_harms << 9) << 9;
	}

    // Calculate prediction coefficient
	if(num_harms <= 15)
		ro_coef = CNST_0_4_Q1_15;
	else if(num_harms <= 24)
		ro_coef = num_harms * CNST_0_03_Q1_15 - CNST_0_05_Q1_15;
	else
		ro_coef = CNST_0_7_Q1_15;

	for(i = num_harms_prev2 + 1; i < NUM_HARMS_MAX + 2; i++)
		sa_prev2[i] = sa_prev2[num_harms_prev2];

	k_acc     = k_coef;
	sum       = 0;
	sa_ptr    = imbe_param->sa;
	vec32_ptr = vec32_tmp;
	for(i = 0; i < num_harms; i++)
	{
		index   = (UWord16)(k_acc >> 24);                    // Get integer part
		si_coef = (Word16)((k_acc - ((UWord32)index << 24)) >> 9); // Get fractional part

		if(si_coef == 0)
		{
			tmp_word32 = L_mpy_ls(sa_prev2[index], ro_coef);                        // sa_prev2 here is in Q10.22 format
			*vec32_ptr++ = L_sub(Log2(*sa_ptr++), tmp_word32);
			sum = L_add(sum, sa_prev2[index]);                                      // sum in Q10.22 format
		}
		else
		{
			tmp_word32 = L_mpy_ls(sa_prev2[index], sub(0x7FFF, si_coef));
			sum = L_add(sum, tmp_word32);
			*vec32_ptr  = L_sub(Log2(*sa_ptr++), L_mpy_ls(tmp_word32, ro_coef));

			tmp_word32 = L_mpy_ls(sa_prev2[index + 1], si_coef);
			sum = L_add(sum, tmp_word32);
			*vec32_ptr = L_sub(*vec32_ptr, L_mpy_ls(tmp_word32, ro_coef));

			vec32_ptr++;
		}

		k_acc += k_coef;
	}

	imbe_param->div_one_by_num_harm_sh = tmp = norm_s(num_harms);
	imbe_param->div_one_by_num_harm = tmp1 = div_s(0x4000, num_harms << tmp); // calculate 1/num_harms with scaling for better pricision
	                                                                          // save result to use late
	sum = L_shr(L_mpy_ls(L_mpy_ls(sum, ro_coef), tmp1), (14 - tmp));

	for(i = 0; i < num_harms; i++)
		t_vec[i] = extract_h(L_shl(L_add(vec32_tmp[i], sum), 5));		                     // t_vec has Q5.11 format
    //////////////////////////////////////////////
	//
	//  Encode T vector
	//
	//////////////////////////////////////////////
	index = num_harms - NUM_HARMS_MIN;

	// Unpack bit allocation table's item
	get_bit_allocation(num_harms, imbe_param->bit_alloc);
	lmprbl_item = lmprbl_tbl[index];

	// Encoding the Higher Order DCT Coefficients
	t_vec_ptr = t_vec;
	b_vec_ptr = &imbe_param->b_vec[8];
	ba_ptr    = &imbe_param->bit_alloc[5];
	for(i = 0;  i < NUM_PRED_RES_BLKS; i++)
	{
		bl_len = (lmprbl_item >> 28) & 0xF; lmprbl_item <<= 4;

		dct(t_vec_ptr, bl_len, bl_len, c_vec);
		gain_vec[i] = c_vec[0];

		for(j = 1; j < bl_len; j++)
		{
			num_bits = *ba_ptr++;
			if(num_bits)
			{
				step_size = extract_h(((Word32)hi_ord_std_tbl[j - 1] * hi_ord_step_size_tbl[num_bits - 1]) << 1);
				*b_vec_ptr = qnt_by_step(c_vec[j], step_size, num_bits);
			}
			else
				*b_vec_ptr = 0;

			b_vec_ptr++;
		}
		t_vec_ptr += bl_len;
	}

	// Encoding the Gain Vector
	dct(gain_vec, NUM_PRED_RES_BLKS, NUM_PRED_RES_BLKS, gain_r);

	b_vec_ptr = &imbe_param->b_vec[2];
	ba_ptr    = &imbe_param->bit_alloc[0];
	gss_ptr   = (Word16 *)&gain_step_size_tbl[index * 5];

	*b_vec_ptr++ = tbl_quant(gain_r[0], (Word16 *)&gain_qnt_tbl[0], GAIN_QNT_TBL_SIZE);
	for(j = 1; j < 6; j++)
		*b_vec_ptr++ = qnt_by_step(gain_r[j], *gss_ptr++, *ba_ptr++);

	//////////////////////////////////////////////
	//
	//  Decode T vector
	//
	//////////////////////////////////////////////
	ba_ptr    = imbe_param->bit_alloc;
	b_vec_ptr = &imbe_param->b_vec[2];

	// Decoding the Gain Vector. gain_vec has signed Q5.11 format
	gss_ptr = (Word16 *)&gain_step_size_tbl[index * 5];
	gain_vec[0] = gain_qnt_tbl[*b_vec_ptr++];

	for(i = 1; i < 6; i++)
		gain_vec[i] = extract_l(L_shr(deqnt_by_step(*b_vec_ptr++, *gss_ptr++, *ba_ptr++), 5));

	idct(gain_vec, NUM_PRED_RES_BLKS, NUM_PRED_RES_BLKS, gain_r);

	v_zap(t_vec, NUM_HARMS_MAX);
	lmprbl_item = lmprbl_tbl[index];

	// Decoding the Higher Order DCT Coefficients
	t_vec_ptr = t_vec;
	for(i = 0;  i < NUM_PRED_RES_BLKS; i++)
	{
		bl_len = (lmprbl_item >> 28) & 0xF; lmprbl_item <<= 4;
		v_zap(c_vec, MAX_BLOCK_LEN);
		c_vec[0] = gain_r[i];
		for(j = 1; j < bl_len; j++)
		{
			num_bits = *ba_ptr++;
			if(num_bits)
			{
				step_size = extract_h(((Word32)hi_ord_std_tbl[j - 1] * hi_ord_step_size_tbl[num_bits - 1]) << 1);
				c_vec[j]  = extract_l(L_shr(deqnt_by_step(*b_vec_ptr, step_size, num_bits), 5));
			}
			else
				c_vec[j] = 0;

			b_vec_ptr++;
		}

		idct(c_vec, bl_len, bl_len, t_vec_ptr);
		t_vec_ptr += bl_len;
	}

	//////////////////////////////////////////////
	//
	//  Reconstruct Spectral Amplitudes
	//
	//////////////////////////////////////////////
	k_acc = k_coef;
	vec32_ptr = vec32_tmp;

	for(i = num_harms_prev2 + 1; i < NUM_HARMS_MAX + 2; i++)
		sa_prev2[i] = sa_prev2[num_harms_prev2];

	for(i = 0; i < num_harms; i++)
	{
		index   = (UWord16)(k_acc >> 24);                    // Get integer part
		si_coef = (Word16)((k_acc - ((UWord32)index << 24)) >> 9); // Get fractional part

		if(si_coef == 0)
		{
			tmp_word32 = L_mpy_ls(sa_prev2[index], ro_coef);                        // sa_prev2 here is in Q10.22 format
			*vec32_ptr++ = L_add(L_shr(L_deposit_h(t_vec[i]), 5), tmp_word32);     // Convert t_vec to Q10.22 and add ...
		}
		else
		{
			tmp_word32 = L_mpy_ls(sa_prev2[index], sub(0x7FFF, si_coef));
			*vec32_ptr  = L_add(L_shr(L_deposit_h(t_vec[i]), 5), L_mpy_ls(tmp_word32, ro_coef));

			tmp_word32 = L_mpy_ls(sa_prev2[index + 1], si_coef);
			*vec32_ptr = L_add(*vec32_ptr, L_mpy_ls(tmp_word32, ro_coef));

			vec32_ptr++;
		}

		k_acc += k_coef;
	}

	for(i = 1; i <= num_harms; i++)
		sa_prev2[i] = L_sub(vec32_tmp[i - 1], sum);

	num_harms_prev2 = num_harms;
}
/* ======================================================================
 * Parameters -> 88-bit channel vector (P25 layout).
 * (reference: encoder/ch_encode.cc)
 * ==================================================================== */
static void encode_frame_vector(imbe_param_t *imbe_param, Word16 *frame_vector)
{
	Word16 bit_stream[EN_BIT_STREAM_LEN], index0, bit_thr, bit_mask, i;
	Word16 vec_num, num_harms, num_bands, tmp;
	Word16 *ba_ptr, *b_ptr;

	num_harms = imbe_param->num_harms;
	num_bands = imbe_param->num_bands;

	v_zap(frame_vector, 8);

	// Unpack bit allocation table's item
	get_bit_allocation(num_harms, imbe_param->bit_alloc);

	// Priority Scanning
	index0   = 0;
	ba_ptr   = imbe_param->bit_alloc;
	bit_thr  = (num_harms == 0xb)?9:ba_ptr[0];
	bit_mask = shl(1, bit_thr - 1);

	while(index0 < EN_BIT_STREAM_LEN - (num_bands - 3))
	{
		b_ptr = &imbe_param->b_vec[3];
		for(i = 0; i < num_harms - 1; i++)
			if(bit_thr && bit_thr <= ba_ptr[i])
				bit_stream[index0++] = (b_ptr[i] & bit_mask)?1:0;

		bit_thr--;
		bit_mask = shr(bit_mask, 1);
	}

	frame_vector[0] = shl(imbe_param->b_vec[0] & 0xFC, 4) | (imbe_param->b_vec[2] & 0x38);

	index0 = 0;
	frame_vector[0] |= (bit_stream[index0++])?4:0;
	frame_vector[0] |= (bit_stream[index0++])?2:0;
	frame_vector[0] |= (bit_stream[index0++])?1:0;

	for(vec_num = 1; vec_num <= 3; vec_num++)
	{
		tmp = 0;
		for(i = 0; i < 12; i++)
		{
			tmp <<= 1;
			tmp |= bit_stream[index0++];
		}
		frame_vector[vec_num] = tmp;
	}

	index0 -= num_bands + 2;

	bit_mask = shl(1, num_bands - 1);
	for(i = 0; i < num_bands; i++)
	{
		bit_stream[index0++] = (imbe_param->b_vec[1] & bit_mask)?1:0;
		bit_mask >>= 1;
	}

	bit_stream[index0++] = (imbe_param->b_vec[2] & 0x04)?1:0;
	bit_stream[index0++] = (imbe_param->b_vec[2] & 0x02)?1:0;

	index0 -= num_bands + 2;

	for(vec_num = 4; vec_num <= 6; vec_num++)
	{
		tmp = 0;
		for(i = 0; i < 11; i++)
		{
			tmp <<= 1;
			tmp |= bit_stream[index0++];
		}
		frame_vector[vec_num] = tmp;
	}

	frame_vector[7] = shl(imbe_param->b_vec[0] & 0x03, 1) | shl(imbe_param->b_vec[2] & 0x01, 3);
	frame_vector[7] |= (bit_stream[index0++])?0x40:0;
	frame_vector[7] |= (bit_stream[index0++])?0x20:0;
	frame_vector[7] |= (bit_stream[index0++])?0x10:0;
	frame_vector[7] |= (imbe_param->b_vec[num_harms + 2])?0x01:0;
}
/* ======================================================================
 * The analysis itself and its initialisation.
 * (reference: encoder/encode.cc)
 * ==================================================================== */

static void encode_init(void)
{
	v_zap(pitch_est_buf, PITCH_EST_BUF_SIZE);
	v_zap(pitch_ref_buf, PITCH_EST_BUF_SIZE);
	v_zap(pe_lpf_mem, PE_LPF_ORD);
	pitch_est_init();
	fft_init();
	dc_rmv_mem = 0;
	sa_encode_init();
	pitch_ref_init();
}

static void encode(imbe_param_t *imbe_param, Word16 *frame_vector, Word16 *snd)
{
	Word16 i;
	Word16 *wr_ptr, *sig_ptr;

	for(i = 0; i < PITCH_EST_BUF_SIZE - FRAME; i++)
	{
		pitch_est_buf[i] = pitch_est_buf[i + FRAME];
		pitch_ref_buf[i] = pitch_ref_buf[i + FRAME];
	}

	dc_rmv(snd, &pitch_ref_buf[PITCH_EST_BUF_SIZE - FRAME], &dc_rmv_mem, FRAME);
	pe_lpf(&pitch_ref_buf[PITCH_EST_BUF_SIZE - FRAME], &pitch_est_buf[PITCH_EST_BUF_SIZE - FRAME], pe_lpf_mem, FRAME);

	pitch_est(imbe_param, pitch_est_buf);

    //
	// Speech windowing and FFT calculation
	//
	wr_ptr  = (Word16 *)wr;
	sig_ptr = &pitch_ref_buf[40];
	for(i = 146; i < 256; i++)
	{
		fft_buf[i].re = mult(*sig_ptr++, *wr_ptr++);
		fft_buf[i].im = 0;
	}
	fft_buf[0].re = *sig_ptr++;
	fft_buf[0].im = 0;
	wr_ptr--;
	for(i = 1; i < 111; i++)
	{
		fft_buf[i].re = mult(*sig_ptr++, *wr_ptr--);
		fft_buf[i].im = 0;
	}
	for(i = 111; i < 146; i++)
		fft_buf[i].re = fft_buf[i].im = 0;

	fft((Word16 *)&fft_buf, FFTLENGTH, 1);

	pitch_ref(imbe_param, fft_buf);
	v_uv_det(imbe_param, fft_buf);
	sa_encode(imbe_param);
	encode_frame_vector(imbe_param, frame_vector);
}

/* ------------------------------------------------------------------------
 * Entry points of the analysis half.
 * ------------------------------------------------------------------------ */

void dmr_enc_analysis_reset(void)
{
    encode_init();
}

void dmr_enc_analysis(imbe_param_t *out, const int16_t *pcm)
{
    /* The reference hands `encode()` an 8-word P25 channel vector that DMR
     * does not use (dmr_enc_frame.c builds the 72 bits from the parameters),
     * and a non-const pointer to the samples that it only reads. */
    static Word16 frame_vector[8];

    encode(out, frame_vector, (Word16 *)(uintptr_t)pcm);
}

size_t dmr_enc_state_bytes(void)
{
    extern size_t dmr_enc_frame_state_bytes(void);
    return sizeof(pitch_est_buf) + sizeof(pitch_ref_buf) + sizeof(pe_lpf_mem) +
           sizeof(fft_buf) + sizeof(wr_array) + sizeof(wi_array) +
           sizeof(sa_prev2) + sizeof(v_uv_dsn) + 32 +
           dmr_enc_frame_state_bytes();
}
