/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * HiFi4 (venus_hifi4) matrix-vector kernel for RNNoise's GRU and dense layers.
 *
 * Selected by -DRNNOISE_DSP_KERNELS; lib/rnnoise/src/rnn.c keeps the portable
 * version and calls this one through ns_matvec().
 *
 * Two neurons per AE vector instruction.  The vectors are the *neurons*, not
 * the inputs: neuron i and neuron i+1 accumulate the same j sequence, in the
 * same order, each in its own IEEE-754 single lane, so the result is the same
 * as the scalar code sample for sample.  Nothing is reassociated and nothing is
 * fused: every product is rounded on its own (MUL.S then ADD.S), because the C
 * code it replaces has -ffp-contract=off.
 *
 * y != 0 is the output gate's recurrent term, w*state[j]*r[j], which C
 * evaluates as (w*state[j])*r[j]: two rounded products, in that order.
 */

#include <xtensa/tie/xt_hifi4.h>
#include <xtensa/tie/xt_FP.h>

#include "rnn.h"

static inline xtfloatx2 ns_splat2(float x)
{
    union { float f[2]; xtfloatx2 v; } u;
    u.f[0] = x;
    u.f[1] = x;
    return u.v;
}

static inline xtfloatx2 ns_pack2i8(rnn_weight a, rnn_weight b)
{
    union { float f[2]; xtfloatx2 v; } u;
    u.f[0] = (float)a;
    u.f[1] = (float)b;
    return u.v;
}

static void ns_matvec_tail(float *acc, const rnn_weight *w, int M, int N,
                           int stride, const float *x, const float *y)
{
    for (int i = 0; i < N; i++) {
        float sum = acc[i];
        if (y == 0) {
            for (int j = 0; j < M; j++)
                sum += w[j * stride + i] * x[j];
        } else {
            for (int j = 0; j < M; j++)
                sum += (w[j * stride + i] * x[j]) * y[j];
        }
        acc[i] = sum;
    }
}

void ns_matvec_dsp(float *acc, const rnn_weight *w, int M, int N, int stride,
                   const float *x, const float *y)
{
    int i = 0, j;

    if (N & 1) {
        /* one neuron left over: let the scalar path do it */
        ns_matvec_tail(acc + N - 1, w + N - 1, M, 1, stride, x, y);
    }

    for (i = 0; i + 1 < N; i += 2) {
        xtfloatx2 a = XT_LSX2I((const xtfloatx2 *)(acc + i), 0);
        const rnn_weight *wp = w + i;
        for (j = 0; j < M; j++, wp += stride) {
            xtfloatx2 wv = ns_pack2i8(wp[0], wp[1]);
            xtfloatx2 t = XT_MUL_SX2(wv, ns_splat2(x[j]));
            if (y != 0)
                t = XT_MUL_SX2(t, ns_splat2(y[j]));
            a = XT_ADD_SX2(a, t);
        }
        XT_SSX2I(a, (xtfloatx2 *)(acc + i), 0);
    }
}
