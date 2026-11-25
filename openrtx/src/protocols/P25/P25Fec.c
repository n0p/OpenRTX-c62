/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file P25Fec.c
 * @brief Forward error correction of P25 Phase 1, see P25Fec.h.
 *
 * Provenance of the codes:
 *   - Golay(23,12) generator basis: op25 op25_golay.h (GPL-3.0-or-later,
 *     KA1RBI), from the APCO/NIAC tables.
 *   - Hamming(15,11) basis and the Hamming(10,6) tables: op25
 *     op25_hamming.h, same licence.
 *   - BCH(63,16): Berlekamp-Massey + Chien search after op25 bch.cc, same
 *     field polynomial and coefficient order.
 *   - Trellis code and block interleaver: op25 p25p1_fdma.cc, from the
 *     wireshark P25 dissector (Michael Ossmann, GPL-2.0).
 *   - Reed-Solomon over GF(64): written for this firmware.
 *
 * All of them are validated by work/p25-rx (host prototype, self tests and
 * real captures) before they run on the radio; see docs/RX_P25.md.
 */

#include "protocols/P25/P25Fec.h"

#include <string.h>

/* --------------------------------------------------------------- Golay --- */

/**
 * Codeword for data bit (11 - i) set.  Bit 22 is the first transmitted bit of
 * the data field; the low 11 bits are the parity bits.
 */
static const uint32_t golayBasis[12] =
{
    0x40063A, 0x20031D, 0x1007B4, 0x0803DA, 0x0401ED, 0x0206CC,
    0x010366, 0x0081B3, 0x0046E3, 0x00254B, 0x00149F, 0x000C75,
};

/** Parity contribution of each of the 23 bit positions (12 data, 11 parity). */
static const uint16_t golaySingle[23] =
{
    0x63A, 0x31D, 0x7B4, 0x3DA, 0x1ED, 0x6CC, 0x366, 0x1B3,
    0x6E3, 0x54B, 0x49F, 0x475,
    0x001, 0x002, 0x004, 0x008, 0x010, 0x020, 0x040, 0x080, 0x100, 0x200,
    0x400,
};

/** Codeword bit for each entry of golaySingle. */
static const uint8_t golayPos[23] =
{
    22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11,
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10,
};

uint32_t p25Fec_golayEncode(uint32_t info)
{
    uint32_t cw = 0;
    for (int i = 0; i < 12; i++)
        if (info & (1u << (11 - i)))
            cw ^= golayBasis[i];
    return cw & 0x7FFFFF;
}

static uint32_t golaySyndrome(uint32_t cw)
{
    uint32_t s = cw & 0x7FF;
    for (int i = 0; i < 12; i++)
        if (cw & (1u << (22 - i)))
            s ^= golaySingle[i];
    return s;
}

int p25Fec_golayDecode(uint32_t *cw)
{
    uint32_t w = *cw & 0x7FFFFF;
    uint32_t s = golaySyndrome(w);
    if (s == 0)
    {
        *cw = w;
        return 0;
    }

    for (int i = 0; i < 23; i++)
    {
        if (golaySingle[i] == s)
        {
            *cw = w ^ (1u << golayPos[i]);
            return 1;
        }
    }
    for (int i = 0; i < 23; i++)
    {
        for (int j = i + 1; j < 23; j++)
        {
            if ((uint32_t)(golaySingle[i] ^ golaySingle[j]) == s)
            {
                *cw = w ^ (1u << golayPos[i]) ^ (1u << golayPos[j]);
                return 2;
            }
        }
    }
    for (int i = 0; i < 23; i++)
    {
        for (int j = i + 1; j < 23; j++)
        {
            uint32_t t = (uint32_t)(golaySingle[i] ^ golaySingle[j]);
            for (int k = j + 1; k < 23; k++)
            {
                if ((t ^ golaySingle[k]) == s)
                {
                    *cw = w ^ (1u << golayPos[i]) ^ (1u << golayPos[j]) ^
                          (1u << golayPos[k]);
                    return 3;
                }
            }
        }
    }
    *cw = w;
    return -1;
}

/* ------------------------------------------------------------- Hamming --- */

/** Parity contribution of each data bit; parity bits 3..0 are 8,4,2,1. */
static const uint8_t ham15Single[15] =
{
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x07, 0x06, 0x05, 0x03,
    0x08, 0x04, 0x02, 0x01,
};

uint16_t p25Fec_hamEncode(uint16_t info)
{
    uint16_t p = 0;
    for (int i = 0; i < 11; i++)
        if (info & (1u << (10 - i)))
            p ^= ham15Single[i];
    return (uint16_t)(((info & 0x7FF) << 4) | p);
}

int p25Fec_hamDecode(uint16_t *cw)
{
    uint16_t w = *cw & 0x7FFF;
    uint8_t s = (uint8_t)(w & 0x0F);
    for (int i = 0; i < 11; i++)
        if (w & (1u << (14 - i)))
            s ^= ham15Single[i];
    if (s == 0)
    {
        *cw = w;
        return 0;
    }
    for (int i = 0; i < 15; i++)
    {
        if (ham15Single[i] == s)
        {
            *cw = (uint16_t)(w ^ (1u << (14 - i)));
            return 1;
        }
    }
    *cw = w;
    return -1;
}

/** Hamming(10,6,3) tables as published in op25's op25_hamming.h. */
static const uint8_t hmg1063Enc[64] =
{
    0, 12, 3, 15, 7, 11, 4, 8, 11, 7, 8, 4, 12, 0, 15, 3,
    13, 1, 14, 2, 10, 6, 9, 5, 6, 10, 5, 9, 1, 13, 2, 14,
    14, 2, 13, 1, 9, 5, 10, 6, 5, 9, 6, 10, 2, 14, 1, 13,
    3, 15, 0, 12, 4, 8, 7, 11, 8, 4, 11, 7, 15, 3, 12, 0,
};

static const uint8_t hmg1063Corr[16] =
{
    0, 0, 0, 2, 0, 0, 0, 4, 0, 0, 0, 8, 1, 16, 32, 0,
};

int p25Fec_ham10Decode(uint32_t data, uint32_t parity)
{
    uint8_t d = (uint8_t)(data & 0x3F);
    uint8_t s = (uint8_t)(hmg1063Enc[d] ^ (parity & 0x0F));
    uint8_t c = hmg1063Corr[s];
    if (c & 0x30)
        return -1;
    return (d ^ c) & 0x3F;
}

/* --------------------------------------------------------------- GF(64) --- */

static uint8_t gfExp[128];
static uint8_t gfLog[64];
static bool gfReady;

static void gfInit(void)
{
    if (gfReady)
        return;
    uint8_t x = 1;
    for (int i = 0; i < 63; i++)
    {
        gfExp[i] = x;
        gfLog[x] = (uint8_t)i;
        /* Multiply by alpha, with alpha^6 = alpha + 1 (x^6 + x + 1). */
        x = (uint8_t)(x << 1);
        if (x & 0x40)
            x ^= 0x43;
    }
    gfExp[63] = 1;
    for (int i = 64; i < 128; i++)
        gfExp[i] = gfExp[i - 63];
    gfReady = true;
}

static uint8_t gfMul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0)
        return 0;
    return gfExp[gfLog[a] + gfLog[b]];
}

static uint8_t gfDiv(uint8_t a, uint8_t b)
{
    if (a == 0)
        return 0;
    return gfExp[(gfLog[a] + 63 - gfLog[b]) % 63];
}

static uint8_t gfPow(int e)
{
    e %= 63;
    if (e < 0)
        e += 63;
    return gfExp[e];
}

/* ----------------------------------------------------------- BCH(63,16) --- */

int p25Fec_bchDecode(uint64_t *cw)
{
    gfInit();
    int S[23];
    int elp[24][22], D[23], L[24], uLu[24], locn[12], reg[12];
    int i, j, U, q, count;
    bool syn = false;
    uint64_t w = *cw;

    for (i = 1; i <= 22; i++)
    {
        int s = 0;
        for (j = 0; j <= 62; j++)
            if ((w >> j) & 1)
                s ^= gfExp[(i * j) % 63];
        if (s)
            syn = true;
        S[i] = (s == 0) ? -1 : gfLog[s];
    }
    if (!syn)
        return 0;

    L[0] = 0; uLu[0] = -1; D[0] = 0; elp[0][0] = 0;
    L[1] = 0; uLu[1] = 0; D[1] = S[1]; elp[1][0] = 1;
    for (i = 1; i <= 21; i++)
    {
        elp[0][i] = -1;
        elp[1][i] = 0;
    }
    U = 0;

    do
    {
        U++;
        if (D[U] == -1)
        {
            L[U + 1] = L[U];
            for (i = 0; i <= L[U]; i++)
            {
                elp[U + 1][i] = elp[U][i];
                elp[U][i] = (elp[U][i] == 0) ? -1 : gfLog[elp[U][i]];
            }
        }
        else
        {
            q = U - 1;
            while (D[q] == -1 && q > 0)
                q--;
            if (q > 0)
            {
                j = q;
                do
                {
                    j--;
                    if (D[j] != -1 && uLu[q] < uLu[j])
                        q = j;
                } while (j > 0);
            }
            L[U + 1] = (L[U] > L[q] + U - q) ? L[U] : (L[q] + U - q);
            for (i = 0; i <= 21; i++)
                elp[U + 1][i] = 0;
            for (i = 0; i <= L[q]; i++)
                if (elp[q][i] != -1)
                    elp[U + 1][i + U - q] =
                        gfExp[(D[U] + 63 - D[q] + elp[q][i]) % 63];
            for (i = 0; i <= L[U]; i++)
            {
                elp[U + 1][i] ^= elp[U][i];
                elp[U][i] = (elp[U][i] == 0) ? -1 : gfLog[elp[U][i]];
            }
        }
        uLu[U + 1] = U - L[U + 1];

        if (U < 22)
        {
            D[U + 1] = (S[U + 1] == -1) ? 0 : gfExp[S[U + 1]];
            for (i = 1; i <= L[U + 1]; i++)
                if (S[U + 1 - i] != -1 && elp[U + 1][i] != 0)
                    D[U + 1] ^= gfExp[(S[U + 1 - i] +
                                       gfLog[elp[U + 1][i]]) % 63];
            D[U + 1] = (D[U + 1] == 0) ? -1 : gfLog[D[U + 1]];
        }
    } while (U < 22 && L[U + 1] <= 11);

    U++;
    if (L[U] > 11)
        return -1;

    for (i = 0; i <= L[U]; i++)
        elp[U][i] = (elp[U][i] == 0) ? -1 : gfLog[elp[U][i]];

    for (i = 1; i <= L[U]; i++)
        reg[i] = elp[U][i];
    count = 0;
    for (i = 1; i <= 63; i++)
    {
        q = 1;
        for (j = 1; j <= L[U]; j++)
        {
            if (reg[j] != -1)
            {
                reg[j] = (reg[j] + j) % 63;
                q ^= gfExp[reg[j]];
            }
        }
        if (q == 0)
        {
            if (count < 12)
                locn[count] = 63 - i;
            count++;
        }
    }
    if (count != L[U])
        return -1;
    for (i = 0; i < L[U] && i < 12; i++)
        w ^= (uint64_t)1 << locn[i];
    *cw = w;
    return L[U];
}

/* -------------------------------------------------------- Reed-Solomon --- */

static int rsSyndromes(const uint8_t *sym, int n, int npar, uint8_t *S)
{
    gfInit();
    bool any = false;
    for (int j = 1; j <= npar; j++)
    {
        uint8_t s = 0;
        for (int i = 0; i < n; i++)
            s ^= gfMul(sym[i], gfPow(j * (n - 1 - i)));
        S[j - 1] = s;
        if (s)
            any = true;
    }
    return any ? 1 : 0;
}

int p25Fec_rsCheck(const uint8_t *sym, int n, int k)
{
    uint8_t S[20];
    return rsSyndromes(sym, n, n - k, S) ? -1 : 0;
}

int p25Fec_rsDecode(uint8_t *sym, int n, int k)
{
    int npar = n - k;
    uint8_t S[20];
    if (npar > 20 || npar <= 0)
        return -1;
    if (!rsSyndromes(sym, n, npar, S))
        return 0;

    /* Berlekamp-Massey: error locator polynomial. */
    uint8_t C[21] = {1}, B[21] = {1}, T[21];
    int L = 0, m = 1;
    uint8_t b = 1;
    for (int step = 0; step < npar; step++)
    {
        uint8_t d = S[step];
        for (int i = 1; i <= L; i++)
            d ^= gfMul(C[i], S[step - i]);
        if (d == 0)
        {
            m++;
            continue;
        }
        uint8_t coef = gfDiv(d, b);
        memcpy(T, C, sizeof(C));
        for (int i = 0; i + m <= npar; i++)
            C[i + m] ^= gfMul(coef, B[i]);
        if (2 * L <= step)
        {
            L = step + 1 - L;
            memcpy(B, T, sizeof(B));
            b = d;
            m = 1;
        }
        else
        {
            m++;
        }
    }
    if (L == 0 || 2 * L > npar)
        return -1;

    /* Chien search: position i has locator X = alpha^(n-1-i), the root of the
     * locator polynomial is X^-1. */
    int pos[16], nerr = 0;
    for (int i = 0; i < n && nerr < L; i++)
    {
        uint8_t X = gfPow(i - (n - 1));
        uint8_t v = 0, xp = 1;
        for (int j = 0; j <= L; j++)
        {
            v ^= gfMul(C[j], xp);
            xp = gfMul(xp, X);
        }
        if (v == 0)
            pos[nerr++] = i;
    }
    if (nerr != L)
        return -1;

    /* Error magnitudes: solve S_j = sum_e e_e * X_e^j for j = 1..L. */
    uint8_t A[17][18];
    for (int j = 0; j < L; j++)
    {
        for (int e = 0; e < L; e++)
            A[j][e] = gfPow((j + 1) * (n - 1 - pos[e]));
        A[j][L] = S[j];
    }
    for (int col = 0; col < L; col++)
    {
        int piv = -1;
        for (int r = col; r < L; r++)
            if (A[r][col])
            {
                piv = r;
                break;
            }
        if (piv < 0)
            return -1;
        if (piv != col)
            for (int c = 0; c <= L; c++)
            {
                uint8_t t = A[col][c];
                A[col][c] = A[piv][c];
                A[piv][c] = t;
            }
        uint8_t inv = gfDiv(1, A[col][col]);
        for (int c = col; c <= L; c++)
            A[col][c] = gfMul(A[col][c], inv);
        for (int r = 0; r < L; r++)
        {
            if (r == col || A[r][col] == 0)
                continue;
            uint8_t f = A[r][col];
            for (int c = col; c <= L; c++)
                A[r][c] ^= gfMul(f, A[col][c]);
        }
    }
    for (int e = 0; e < L; e++)
        sym[pos[e]] ^= A[e][L];
    return L;
}

/* ------------------------------------------------------------- Trellis --- */

/** For each transmitted bit, the bit of the de-interleaved block it carries. */
static const uint8_t trellisDeinterleave196[196] =
{
      0,   1,   2,   3,  52,  53,  54,  55, 100, 101, 102, 103,
    148, 149, 150, 151,   4,   5,   6,   7,  56,  57,  58,  59,
    104, 105, 106, 107, 152, 153, 154, 155,   8,   9,  10,  11,
     60,  61,  62,  63, 108, 109, 110, 111, 156, 157, 158, 159,
     12,  13,  14,  15,  64,  65,  66,  67, 112, 113, 114, 115,
    160, 161, 162, 163,  16,  17,  18,  19,  68,  69,  70,  71,
    116, 117, 118, 119, 164, 165, 166, 167,  20,  21,  22,  23,
     72,  73,  74,  75, 120, 121, 122, 123, 168, 169, 170, 171,
     24,  25,  26,  27,  76,  77,  78,  79, 124, 125, 126, 127,
    172, 173, 174, 175,  28,  29,  30,  31,  80,  81,  82,  83,
    128, 129, 130, 131, 176, 177, 178, 179,  32,  33,  34,  35,
     84,  85,  86,  87, 132, 133, 134, 135, 180, 181, 182, 183,
     36,  37,  38,  39,  88,  89,  90,  91, 136, 137, 138, 139,
    184, 185, 186, 187,  40,  41,  42,  43,  92,  93,  94,  95,
    140, 141, 142, 143, 188, 189, 190, 191,  44,  45,  46,  47,
     96,  97,  98,  99, 144, 145, 146, 147, 192, 193, 194, 195,
     48,  49,  50,  51,
};

/** Codeword for (state, input dibit); the next state is the dibit itself. */
static const uint8_t trellisNext[4][4] =
{
    {0x2, 0xC, 0x1, 0xF},
    {0xE, 0x0, 0xD, 0x3},
    {0x9, 0x7, 0xA, 0x4},
    {0x5, 0xB, 0x6, 0x8},
};

int p25Fec_trellisDecode(const uint8_t *bits, uint8_t out[12])
{
    enum { STEPS = 49 };
    int metric[4], nextMetric[4];
    uint8_t prevOf[4][STEPS];

    for (int s = 0; s < 4; s++)
    {
        metric[s] = (s == 0) ? 0 : 1 << 20;
        memset(prevOf[s], 0, STEPS);
    }

    for (int k = 0; k < STEPS; k++)
    {
        int r[4];
        for (int b = 0; b < 4; b++)
            r[b] = bits[trellisDeinterleave196[4 * k + b]] & 1;

        for (int ns = 0; ns < 4; ns++)
        {
            int bestM = 1 << 20, bestS = 0;
            for (int s = 0; s < 4; s++)
            {
                int cw = trellisNext[s][ns];
                int bm = 0;
                for (int b = 0; b < 4; b++)
                    bm += ((cw >> (3 - b)) & 1) ^ r[b];
                int m = metric[s] + bm;
                if (m < bestM)
                {
                    bestM = m;
                    bestS = s;
                }
            }
            nextMetric[ns] = bestM;
            prevOf[ns][k] = (uint8_t)bestS;
        }
        for (int s = 0; s < 4; s++)
            metric[s] = nextMetric[s];
    }

    int st = 0;
    for (int s = 1; s < 4; s++)
        if (metric[s] < metric[st])
            st = s;

    uint8_t dibits[STEPS];
    for (int k = STEPS - 1; k >= 0; k--)
    {
        dibits[k] = (uint8_t)st;
        st = prevOf[st][k];
    }

    int errors = 0, s2 = 0;
    memset(out, 0, 12);
    for (int k = 0; k < STEPS; k++)
    {
        int d = dibits[k];
        if (k < 48)
            out[k >> 2] |= (uint8_t)(d << (6 - 2 * (k & 3)));
        int cw = trellisNext[s2][d];
        for (int b = 0; b < 4; b++)
            errors += ((cw >> (3 - b)) & 1) ^
                      (bits[trellisDeinterleave196[4 * k + b]] & 1);
        s2 = d;
    }
    return errors;
}

/* ----------------------------------------------------------------- CRC --- */
uint16_t p25Fec_crc16(const uint8_t *data, int bytes)
{
    const uint32_t poly = (1u << 12) | (1u << 5) | 1u;
    uint32_t crc = 0;
    for (int i = 0; i < bytes; i++)
    {
        uint8_t bits = data[i];
        for (int j = 0; j < 8; j++)
        {
            uint8_t bit = (uint8_t)((bits >> (7 - j)) & 1);
            crc = ((crc << 1) | bit) & 0x1FFFF;
            if (crc & 0x10000)
                crc = (crc & 0xFFFF) ^ poly;
        }
    }
    crc ^= 0xFFFF;
    return (uint16_t)(crc & 0xFFFF);
}

/* -------------------------------------------------------------- encoders --- */

/*
 * BCH(63,16): the generator polynomial is the product of the minimal
 * polynomials of alpha^1..alpha^22 over GF(64) with x^6 + x + 1, degree 47,
 * i.e. exactly the roots p25Fec_bchDecode() takes syndromes on.  It was
 * derived from that root set (not from a table in a standard) and is checked
 * two ways in tests/host/p25_tx: every codeword has zero syndromes for the 22
 * roots, and the decoder returns zero corrections for them.
 */
#define BCH_GENERATOR 0xCD930BDD3B2BULL
#define BCH_PARITY_BITS 47

uint64_t p25Fec_bchEncode(uint16_t info)
{
    uint64_t v = (uint64_t)info << BCH_PARITY_BITS;

    for (int i = 62; i >= BCH_PARITY_BITS; i--)
        if ((v >> i) & 1u)
            v ^= BCH_GENERATOR << (i - BCH_PARITY_BITS);

    return ((uint64_t)info << BCH_PARITY_BITS) | v;
}

void p25Fec_rsEncode(const uint8_t *info, int k, uint8_t *cw, int n)
{
    int npar = n - k;

    gfInit();

    /* Generator g(x) = prod (x - alpha^j), j = 1..npar, coefficients in
     * descending powers. */
    uint8_t g[21];
    int deg = 0;

    g[0] = 1;
    for (int j = 1; j <= npar; j++)
    {
        const uint8_t a = gfPow(j);
        const uint8_t lead = g[deg];

        /* Descending powers: new[i] = old[i] + a * old[i - 1], the leading
         * coefficient stays 1 and the new constant is a * old[deg]. */
        for (int i = deg; i >= 1; i--)
            g[i] ^= gfMul(g[i - 1], a);
        g[deg + 1] = gfMul(lead, a);
        deg++;
    }

    /* Long division of info(x) * x^npar by g(x): the remainder is the parity */
    uint8_t rem[21];

    memset(rem, 0, sizeof(rem));
    for (int i = 0; i < k; i++)
    {
        uint8_t fb = (uint8_t)(info[i] ^ rem[0]);

        memmove(&rem[0], &rem[1], (size_t)npar);
        rem[npar - 1] = 0;
        if (fb != 0)
            for (int j = 0; j < npar; j++)
                rem[j] ^= gfMul(g[j + 1], fb);
    }

    memcpy(cw, info, (size_t)k);
    memcpy(&cw[k], rem, (size_t)npar);
}

void p25Fec_trellisEncode(const uint8_t in[12], uint8_t bits[196])
{
    enum { STEPS = 49 };
    uint8_t block[196];
    int state = 0;

    for (int k = 0; k < STEPS; k++)
    {
        /* 48 dibits of payload, then one flush dibit of zero */
        int d = 0;

        if (k < 48)
            d = (in[k >> 2] >> (6 - 2 * (k & 3))) & 3;

        int cw = trellisNext[state][d];

        for (int b = 0; b < 4; b++)
            block[4 * k + b] = (uint8_t)((cw >> (3 - b)) & 1u);

        state = d;
    }

    /* The table maps a de-interleaved position to the air position it travels
     * in, which is how the decoder reads it: r[b] = bits[table[4k + b]]. */
    for (int i = 0; i < 196; i++)
        bits[trellisDeinterleave196[i]] = block[i];
}

uint16_t p25Fec_ham10Encode(uint32_t data)
{
    uint8_t d = (uint8_t)(data & 0x3F);

    return (uint16_t)((d << 4) | (hmg1063Enc[d] & 0x0F));
}

/* ------------------------------------------------- HDU word: Golay(18,6) --- */

/*
 * The (23,12) code of this file is stored with the data at the top of the
 * word, which is convenient for the IMBE path.  The HDU word wants it the
 * other way round (Wallace's form, the one op25 and dsd use: data in the low
 * twelve bits, check bits above), so the division is written here directly on
 * the 12-bit data word instead of reusing the basis table.
 */
#define GOLAY_POLY 0xAE3u

/** The 24-bit extended codeword of a 12-bit data word, data in bits 11..0. */
static uint32_t golay24Word(uint32_t data)
{
    uint32_t cw = data & 0xFFFu;

    for (int i = 0; i < 12; i++)
    {
        if (cw & 1u)
            cw ^= GOLAY_POLY;
        cw >>= 1;
    }

    cw &= 0x7FFu;                     /* eleven check bits */
    uint32_t word = (cw << 12) | (data & 0xFFFu);

    /* Overall parity of the 23 bits in bit 23 */
    uint32_t ones = word;

    ones ^= ones >> 16;
    ones ^= ones >> 8;
    ones ^= ones >> 4;
    ones ^= ones >> 2;
    ones ^= ones >> 1;

    return word | ((ones & 1u) << 23);
}

void p25Fec_golay18Encode(uint32_t hex, uint8_t air[18])
{
    /*
     * The shortened code's twelve-bit data word puts the *last* transmitted
     * information bit in bit 11 and the first in bit 6, with bits 5..0 the
     * implicit zeros: the symbol goes in bit-reversed, which is what op25 and
     * dsd's decoder expect (checked against dsd's Golay24::decode_6 on all 64
     * symbols, tests/host/p25_tx and the real capture in docs/TX_P25.md).
     */
    uint32_t rev = 0;

    for (int i = 0; i < 6; i++)
        rev = (rev << 1) | ((hex >> i) & 1u);

    const uint32_t word = golay24Word(rev << 6);

    for (int i = 0; i < 6; i++)
        air[i] = (uint8_t)((hex >> (5 - i)) & 1u);

    /* Parity, in the order the air carries it: check bit 0 first, the overall
     * parity bit last (bit 12 of the word up to bit 23). */
    for (int i = 0; i < 12; i++)
        air[6 + i] = (uint8_t)((word >> (12 + i)) & 1u);
}

int p25Fec_golay18Decode(const uint8_t air[18], uint32_t *hex)
{
    uint32_t rx = 0;

    /* The 18 received bits are six data then twelve parity: rebuild the 24-bit
     * word and compare it with the 64 codewords of the code.  Minimum
     * distance 8 means the nearest one within three bits is unique. */
    for (int i = 0; i < 18; i++)
        rx = (rx << 1) | (air[i] & 1u);

    int best = 99;
    uint32_t bestHex = 0;

    for (uint32_t h = 0; h < 64; h++)
    {
        uint8_t cwAir[18];
        uint32_t w = 0;

        p25Fec_golay18Encode(h, cwAir);
        for (int i = 0; i < 18; i++)
            w = (w << 1) | (cwAir[i] & 1u);

        uint32_t d = w ^ rx;
        int dist = 0;

        while (d != 0)
        {
            dist += (int)(d & 1u);
            d >>= 1;
        }

        if (dist < best)
        {
            best = dist;
            bestHex = h;
        }
    }

    if (best > 3)
        return -1;

    *hex = bestHex;
    return best;
}
