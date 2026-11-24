#include "fft.h"

#include <math.h>

void fft_init(fft_t *f, int nmax)
{
    if (nmax > FFT_MAX_N) nmax = FFT_MAX_N;
    f->nmax = nmax;
    for (int k = 0; k < nmax / 2; k++) {
        double a = 2.0 * 3.14159265358979323846 * (double)k / (double)nmax;
        f->cs[k] = (float)cos(a);
        f->sn[k] = (float)sin(a);
    }
}

void fft_run(const fft_t *f, float *re, float *im, int n, int inverse)
{
    if (n < 2 || n > f->nmax) return;
    /* bit reversal */
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    const float sg = inverse ? 1.0f : -1.0f;
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = f->nmax / len;
        for (int i = 0; i < n; i += len) {
            for (int k = 0; k < half; k++) {
                const float wr = f->cs[k * step];
                const float wi = sg * f->sn[k * step];
                const int a = i + k, b = a + half;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}
