#ifndef TINYVOICE_FFT_H
#define TINYVOICE_FFT_H

/* In-place iterative radix-2 complex FFT, sizes 2..FFT_MAX_N (a power of two).
 * Tables live in the caller's fft_t (no globals, no allocation): fft_init once,
 * then any size up to the one given there. Host-free. */

#define FFT_MAX_LOG2 12
#define FFT_MAX_N    (1 << FFT_MAX_LOG2)   /* 4096 */

typedef struct {
    int   nmax;
    float cs[FFT_MAX_N / 2];   /* cos(2 pi k / nmax) */
    float sn[FFT_MAX_N / 2];   /* sin(2 pi k / nmax) */
} fft_t;

void fft_init(fft_t *f, int nmax);

/* Forward (sign -1, no scaling) or inverse (sign +1, no scaling) transform of
 * n complex points held as separate re/im arrays. n <= nmax, a power of two. */
void fft_run(const fft_t *f, float *re, float *im, int n, int inverse);

#endif
