/* RNNoise host driver for the C62 evaluation: 48 kHz mono s16 raw in/out.
 * usage: rn_demo <in.raw> <out.raw> [--dry]   (--dry = passthrough)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "rnnoise.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s in.raw out.raw [--dry]\n", argv[0]); return 1; }
    FILE *fi = fopen(argv[1], "rb"), *fo = fopen(argv[2], "wb");
    if (!fi || !fo) { perror("open"); return 1; }
    int dry = (argc > 3 && strcmp(argv[3], "--dry") == 0);

    DenoiseState *st = rnnoise_create(NULL);
    const int N = rnnoise_get_frame_size();
    short *in = malloc(N * sizeof(short));
    float *fin = malloc(N * sizeof(float)), *fout = malloc(N * sizeof(float));
    short *out = malloc(N * sizeof(short));

    long frames = 0;
    while (fread(in, sizeof(short), N, fi) == (size_t)N) {
        for (int i = 0; i < N; i++) fin[i] = (float)in[i];
        if (dry) memcpy(fout, fin, N * sizeof(float));
        else     rnnoise_process_frame(st, fout, fin);
        for (int i = 0; i < N; i++) {
            float v = fout[i];
            if (v > 32767.f) v = 32767.f;
            if (v < -32768.f) v = -32768.f;
            out[i] = (short)lrintf(v);
        }
        fwrite(out, sizeof(short), N, fo);
        frames++;
    }
    fprintf(stderr, "frames=%ld state=%d\n", frames, rnnoise_get_size());
    rnnoise_destroy(st);
    return 0;
}
