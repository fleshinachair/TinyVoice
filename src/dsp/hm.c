#include "hm.h"
#include "cher.h"     /* cher_mpm */

#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979f
#define TWO_PI_F 6.28318530717959f

/* Output gain of the resynthesis over the input: Humanoid's INIT measures +2.3 dB. */
#define HM_MAKEUP 1.303f
#define HM_LEVEL_CEIL 0.30f           /* the limiter's ceiling: -10.5 dBFS, under the module's headroom stage */
#define HM_OUT_CEIL 0.944f            /* Humanoid's output limiter: -0.5 dBFS */
#define HM_LEVEL_TARGET_MS 0.016f    /* the leveller's aim: -18 dBFS RMS (sung averages land a few dB under) */
/* Partials stop below this (Hz), whatever the sample rate. */
#define HM_FMAX 18000.0f

/* The table: src/dsp/hm_params.h (Humanoid's ids, units and ranges; defaults as INIT
 * except Latency, Lo here for live singing). Order = hm_param. */
#define HM_X_INFO(id, pid, label, kind, def, lo, hi, step, unit, ...) {pid, (float)(def), (float)(lo), (float)(hi)},
static const hm_param_info k_par[HM_NPARAM] = { HM_PARAM_LIST(HM_X_INFO) };

const hm_param_info *hm_param_get(int id)
{
    return (id >= 0 && id < HM_NPARAM) ? &k_par[id] : 0;
}

int hm_param_find(const char *id)
{
    for (int i = 0; i < HM_NPARAM; i++)
        if (strcmp(k_par[i].id, id) == 0) return i;
    return -1;
}

/* ------------------------------------------------------------- helpers */

static float midi_hz(float m) { return 440.0f * exp2f((m - 69.0f) / 12.0f); }
static float hz_midi(float f) { return 69.0f + 12.0f * log2f(f / 440.0f); }
static float db_lin(float d)  { return expf(d * 0.11512925f); }
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static float rnd(hm_t *h)       /* uniform 0..1 */
{
    h->rng = h->rng * 1664525u + 1013904223u;
    return (float)(h->rng >> 8) * (1.0f / 16777216.0f);
}

/* envelope grid: 40 Hz .. 20 kHz, log spaced */
#define GRID_LO 40.0f
#define GRID_OCT 8.966f               /* log2(20000/40) */
static float grid_hz(int i) { return GRID_LO * exp2f(GRID_OCT * (float)i / (float)(HM_GRID - 1)); }
static float grid_pos(float hz)
{
    if (hz <= GRID_LO) return 0.0f;
    float g = log2f(hz / GRID_LO) / GRID_OCT * (float)(HM_GRID - 1);
    return g > (float)(HM_GRID - 1) ? (float)(HM_GRID - 1) : g;
}
static float env_at(const float *env, float hz)
{
    float g = grid_pos(hz);
    int i = (int)g;
    if (i >= HM_GRID - 1) return env[HM_GRID - 1];
    float t = g - (float)i;
    return env[i] + t * (env[i + 1] - env[i]);
}

/* the power of a harmonic series on f0 read off the (unshifted) envelope */
static double env_power(const hm_t *h, float f0, float sr)
{
    int K = (int)(fminf(HM_FMAX, 0.45f * sr) / f0);
    if (K > HM_KMAX) K = HM_KMAX;
    double p = 0.0;
    for (int k = 1; k <= K; k++) p += (double)expf(env_at(h->env, (float)k * f0) * 0.23025851f);
    return p;
}

/* noise bands: 24 log-spaced bands, 80 Hz .. 20 kHz */
static float band_lo(int b) { return 80.0f * exp2f(7.966f * (float)b / (float)HM_NB); }
static int band_of(float hz)
{
    if (hz <= 80.0f) return 0;
    int b = (int)(log2f(hz / 80.0f) / 7.966f * (float)HM_NB);
    return b < 0 ? 0 : (b >= HM_NB ? HM_NB - 1 : b);
}

/* --------------------------------------------------------------- setup */

static int lag_for(int setting)
{
    /* analysis lag: the frame is centred this far behind the write head.
     * Lo / Mid / Hi = 768 / 1024 / 2048 samples (17 / 23 / 46 ms at 44.1 kHz),
     * plus one hop of overlap-add = the latency. */
    return setting <= 0 ? 768 : (setting == 1 ? 1024 : 2048);
}

static void make_saw(hm_t *h)
{
    for (int i = 0; i < HM_WT_N; i++) h->wt_src[i] = -1.0f + 2.0f * (float)i / (float)HM_WT_N;
    h->wt_src_n = HM_WT_N;
    h->wt_dirty = 1;
}

void hm_init(hm_t *h, float sample_rate)
{
    memset(h, 0, sizeof(*h));
    h->sr = sample_rate;
    h->fold_note[0] = h->fold_note[1] = -1;
    h->lv_g = 1.0f; h->lv_lim = 1.0f;  /* autolevel is off here (the A/B tools compare with Humanoid untouched); the module turns it on */
    fft_init(&h->fft, HM_NFFT);
    for (int i = 0; i < HM_NPARAM; i++) h->p[i] = k_par[i].def;
    h->mask = 0xAB5;                  /* C major, as INIT */
    h->rng = 0x12345u;

    /* Blackman-Harris (4-term) synthesis window, centred on index 0 (circular) */
    for (int i = 0; i < HM_NS; i++) {
        double x = (double)i / (double)HM_NS;         /* 0..1 with the peak at 0.5 */
        double w = 0.35875 - 0.48829 * cos(2 * 3.14159265358979 * x) + 0.14128 * cos(4 * 3.14159265358979 * x)
                 - 0.01168 * cos(6 * 3.14159265358979 * x);
        h->bh[(i + HM_NS / 2) % HM_NS] = (float)w;     /* bh[0] = the centre */
    }
    /* its spectrum (real, the window is symmetric about index 0) at fractional bins */
    for (int k = 0; k <= 2 * HM_KERN_HW * HM_KERN_OS; k++) {
        double d = (double)(k - HM_KERN_HW * HM_KERN_OS) / HM_KERN_OS, s = 0;
        for (int n = -HM_NS / 2; n < HM_NS / 2; n++)
            s += h->bh[(n + HM_NS) % HM_NS] * cos(2 * 3.14159265358979 * d * n / HM_NS);
        h->kern[k] = (float)s;
    }
    /* overlap-add: triangle over 2 hops, divided by the window */
    for (int n = -HM_HOP; n < HM_HOP; n++) {
        float tri = 1.0f - fabsf((float)n) / (float)HM_HOP;
        h->olaw[n + HM_HOP] = tri / h->bh[(n + HM_NS) % HM_NS];
    }
    for (int l = 0; l < HM_NLINE; l++)
        for (int k = 0; k <= HM_KMAX; k++) h->ph[l][k] = 0.37f * (float)(k * k);
    for (int i = 0; i < HM_GRID; i++) h->env[i] = -120.0f;
    h->lat_set = -1;
    make_saw(h);
    hmfx_init(&h->fx, sample_rate);
    h->bpm = 120.0f;
    h->ref_note = -1000;
}

void hm_reset(hm_t *h)
{
    memset(h->in_ring, 0, sizeof h->in_ring);
    memset(h->dry_ring, 0, sizeof h->dry_ring);
    memset(h->out_ring, 0, sizeof h->out_ring);
    memset(h->fr_prev, 0, sizeof h->fr_prev);
    h->cnt = 0; h->voiced = 0; h->lp_dev = 0; h->ref_note = -1000; h->env_valid = 0;
    h->gate_env = 0; h->gate_g = 0; h->gate_hold = 0;
    h->lv_pow = 0.0f; h->lv_ms = 0.0f; h->lv_g = 1.0f; h->lv_lim = 1.0f;
}

void hm_set(hm_t *h, int id, float v)
{
    if (id < 0 || id >= HM_NPARAM || v != v) return;
    h->p[id] = clampf(v, k_par[id].lo, k_par[id].hi);
}

float hm_get(const hm_t *h, int id) { return (id >= 0 && id < HM_NPARAM) ? h->p[id] : 0.0f; }

void hm_set_mask(hm_t *h, unsigned m) { h->mask = m & 0xFFFu; }

void hm_set_bpm(hm_t *h, float bpm) { if (bpm > 20.0f && bpm < 400.0f) h->bpm = bpm; }

void hm_set_wavetable(hm_t *h, const float *cycle, int n)
{
    if (!cycle || n < 2) return;
    if (n > HM_WT_SRC) n = HM_WT_SRC;
    for (int i = 0; i < n; i++) h->wt_src[i] = (cycle[i] == cycle[i]) ? clampf(cycle[i], -4.0f, 4.0f) : 0.0f;
    h->wt_src_n = n;
    h->wt_dirty = 1;
}

void hm_note_on(hm_t *h, int note)
{
    hm_note_off(h, note);
    if (h->n_held == 8) { memmove(h->midi_held, h->midi_held + 1, 7 * sizeof(int)); h->n_held = 7; }
    h->midi_held[h->n_held++] = note;
}
void hm_note_off(hm_t *h, int note)
{
    for (int i = 0; i < h->n_held; i++)
        if (h->midi_held[i] == note) {
            memmove(h->midi_held + i, h->midi_held + i + 1, (size_t)(h->n_held - i - 1) * sizeof(int));
            h->n_held--; i--;
        }
}
void hm_all_notes_off(hm_t *h) { h->n_held = 0; }

int hm_latency(const hm_t *h) { return lag_for((int)lrintf(h->p[HM_LATENCY])) + HM_HOP; }
float hm_f0(const hm_t *h) { return h->voiced ? h->f0 : 0.0f; }
float hm_target(const hm_t *h) { return h->tgt_hz; }

/* ----------------------------------------------------------- wavetable */

/* Chebyshev polynomial T_n(x) */
static float cheb(int n, float x)
{
    float t0 = 1.0f, t1 = x;
    if (n == 0) return t0;
    for (int i = 1; i < n; i++) { float t2 = 2.0f * x * t1 - t0; t0 = t1; t1 = t2; }
    return t1;
}

static void build_wavetable(hm_t *h)
{
    const float shape = h->p[HM_SHAPE], stretch = h->p[HM_STRETCH];
    float peak = 1e-9f;
    for (int i = 0; i < h->wt_src_n; i++) peak = fmaxf(peak, fabsf(h->wt_src[i]));
    /* Stretch: a phase-distortion knee, the phase reaching ky at kx and the rest of the cycle after.
     * Up to 0.5 the cycle's midpoint moves to `stretch` (kx = s, ky = 0.5); above it the cycle is
     * squeezed into the first 1 - s and nearly held after (ky = 1 - 2 (1 - s)^2), saturating at 0.9.
     * Measured on Humanoid's saw within 0.35 dB over 24 harmonics from 0.1 to 1.0 (the plain skew,
     * symmetric about 0.5, was 5-9 dB too dark at 0.9-1). */
    const float sc = fminf(fmaxf(stretch, 0.02f), 0.9f);
    const float kx = sc <= 0.5f ? sc : 1.0f - sc;
    const float ky = sc <= 0.5f ? 0.5f : 1.0f - 2.0f * (1.0f - sc) * (1.0f - sc);
    for (int i = 0; i < HM_WT_N; i++) {
        float ph = (float)i / (float)HM_WT_N, q;
        q = ph < kx ? ky * ph / kx : ky + (1.0f - ky) * (ph - kx) / (1.0f - kx);
        float pos = q * (float)h->wt_src_n;
        int a = (int)pos;
        float t = pos - (float)a;
        float x = h->wt_src[a % h->wt_src_n] * (1.0f - t) + h->wt_src[(a + 1) % h->wt_src_n] * t;
        x /= peak;
        /* Shape: a Chebyshev series whose coefficients move from the identity (T1 alone) to the
         * preset's Odd/Even values (measured: this, not a waveform crossfade, matches Shape 0.5) */
        if (shape > 0.0f) {
            float y = 0.0f;
            for (int c = 0; c < 7; c++)
                y += ((c == 0 ? 1.0f - shape : 0.0f) + shape * h->p[HM_CHEB0 + c]) * cheb(c + 1, x);
            x = y;
        }
        h->wt[i] = x;
    }
    /* harmonic levels: FFT of the cycle */
    for (int i = 0; i < HM_WT_N; i++) { h->re[i] = h->wt[i]; h->im[i] = 0.0f; }
    fft_run(&h->fft, h->re, h->im, HM_WT_N, 0);
    float mx = 1e-20f;
    float pw[HM_KMAX + 1];
    for (int k = 1; k <= HM_KMAX; k++) {
        pw[k] = h->re[k] * h->re[k] + h->im[k] * h->im[k];
        if (pw[k] > mx) mx = pw[k];
    }
    for (int k = 1; k <= HM_KMAX; k++) h->wt_db[k] = 10.0f * log10f(pw[k] / mx + 1e-12f);
    h->wt_for[0] = shape; h->wt_for[1] = stretch;
    for (int c = 0; c < 7; c++) h->wt_for[2 + c] = h->p[HM_CHEB0 + c];
    h->wt_dirty = 0;
}

static int wavetable_stale(const hm_t *h)
{
    if (h->wt_dirty) return 1;
    if (h->wt_for[0] != h->p[HM_SHAPE] || h->wt_for[1] != h->p[HM_STRETCH]) return 1;
    for (int c = 0; c < 7; c++) if (h->wt_for[2 + c] != h->p[HM_CHEB0 + c]) return 1;
    return 0;
}

/* ------------------------------------------------------------- analysis */

static float ring_at(const hm_t *h, int idx_from_write)   /* idx <= 0: samples before the write head */
{
    return h->in_ring[(h->w + (unsigned)idx_from_write) & (HM_RING - 1)];
}

/* Pitch at the frame centre (lag la behind the write head). */
static void track_pitch(hm_t *h, int la)
{
    const int n2 = HM_DEC_N;
    double e = 0.0;
    for (int i = 0; i < n2; i++) {
        int s = -la - n2 + 2 * i;
        float v = 0.5f * (ring_at(h, s) + ring_at(h, s + 1));
        h->dec[i] = v;
        e += (double)v * v;
    }
    const float rms = sqrtf((float)(e / n2));
    /* The search reaches an octave under Range Lo (58 Hz at the least: the frame holds 2.2 periods
     * there) and up to 1.6 kHz whatever Range Hi says. Humanoid tracks a steady voice down to ~D2 with
     * Range Lo at C3 or C4 (it plays those notes ~47 c sharp, a quirk not copied) and up to E6 with
     * Range Hi at A4; a hard floor at Range Lo silenced a baritone (97-151 Hz) for 68 % of a phrase
     * at the INIT range, where Humanoid held 86 %. */
    float fmin = fmaxf(midi_hz(h->p[HM_RANGE_LO] - 12.0f) * 0.97f, 58.0f);
    float fmax = fmaxf(midi_hz(h->p[HM_RANGE_HI]) * 1.03f, 1600.0f);
    float f0 = 0.0f, clar = 0.0f;
    int ok = rms > 1e-4f && cher_mpm(h->dec, n2, h->sr * 0.5f, fmin, fmax, h->nsdf, &f0, &clar);
    if (ok && clar > 0.6f && f0 >= fmin && f0 <= fmax) {
        float m = hz_midi(f0);
        /* an octave jump with no onset must persist 2 hops to count */
        if (h->voiced && fabsf(fabsf(m - h->midi_med[2]) - 12.0f) < 0.6f && h->oct_wait < 2) {
            h->oct_wait++;
            m = h->midi_med[2];
        } else {
            h->oct_wait = 0;
        }
        if (!h->voiced) { h->midi_med[0] = h->midi_med[1] = m; }
        h->midi_med[0] = h->midi_med[1]; h->midi_med[1] = h->midi_med[2]; h->midi_med[2] = m;
        float a = h->midi_med[0], b = h->midi_med[1], c = h->midi_med[2], md;
        md = (a > b) ? ((b > c) ? b : (a > c ? c : a)) : ((a > c) ? a : (b > c ? c : b));
        /* the median only when it agrees: a real step must not be delayed by it */
        if (fabsf(md - m) > 0.5f) md = m;
        h->f0 = midi_hz(md);
        h->voiced = 1;
    } else {
        h->voiced = 0;
        h->oct_wait = 0;
    }
}

static void analyse_spectrum(hm_t *h, int la)
{
    const float sr = h->sr;
    int W;
    if (h->voiced) {
        W = (int)(4.0f * sr / h->f0);
        int wmax = 2 * la < HM_NFFT ? 2 * la : HM_NFFT;
        if (W > wmax) W = wmax;
        if (W < 512) W = 512;
    } else {
        W = 1024;
    }
    W &= ~1;
    memset(h->re, 0, sizeof h->re);
    memset(h->im, 0, sizeof h->im);
    float wsum = 0.0f;
    /* Hann centred on the frame centre, rotated so the centre is index 0 */
    for (int n = -W / 2; n < W / 2; n++) {
        float w = 0.5f + 0.5f * cosf(TWO_PI_F * (float)n / (float)W);
        wsum += w;
        h->re[(n + HM_NFFT) & (HM_NFFT - 1)] = ring_at(h, -la + n) * w;
    }
    fft_run(&h->fft, h->re, h->im, HM_NFFT, 0);
    const float ascale = 2.0f / wsum;
    const float binhz = sr / (float)HM_NFFT;
    const int nb = HM_NFFT / 2;
    /* power per bin, reused as scratch in im[] */
    for (int k = 0; k < nb; k++) h->im[k] = h->re[k] * h->re[k] + h->im[k] * h->im[k];
    float *pw = h->im;

    /* harmonics */
    h->nh = 0;
    if (h->voiced) {
        const float f0 = h->f0, f0b = f0 / binhz;
        int K = (int)(fminf(HM_FMAX, 0.45f * sr) / f0);
        if (K > HM_KMAX) K = HM_KMAX;
        for (int k = 1; k <= K; k++) {
            float b = (float)k * f0b;
            int lo = (int)(b - 0.35f * f0b), hi = (int)(b + 0.35f * f0b) + 1;
            if (lo < 1) lo = 1;
            if (hi > nb - 2) hi = nb - 2;
            int pk = lo;
            for (int i = lo; i <= hi; i++) if (pw[i] > pw[pk]) pk = i;
            float lev, dev = 0.0f;
            if (pk > lo && pk < hi) {
                float a = 10.0f * log10f(pw[pk - 1] + 1e-30f), c0 = 10.0f * log10f(pw[pk] + 1e-30f),
                      c = 10.0f * log10f(pw[pk + 1] + 1e-30f);
                float den = a - 2.0f * c0 + c;
                float d = fabsf(den) > 1e-9f ? 0.5f * (a - c) / den : 0.0f;
                d = clampf(d, -0.5f, 0.5f);
                lev = c0 - 0.25f * (a - c) * d;
                dev = ((float)pk + d) / b - 1.0f;
            } else {
                int bi = (int)lrintf(b);
                lev = 10.0f * log10f(pw[bi] + 1e-30f);
            }
            h->h_db[k] = lev + 20.0f * log10f(ascale);
            h->h_dev[k] = clampf(dev, -0.03f, 0.03f);
        }
        h->nh = K;
        /* only the inharmonicity: remove the deviation the partials share (it is pitch
         * movement within the frame, which the tuner handles) */
        double sw = 0.0, sd = 0.0;
        for (int k = 1; k <= K; k++) {
            double w = (double)expf(h->h_db[k] * 0.11512925f);
            sw += w; sd += w * h->h_dev[k];
        }
        const float common = sw > 0.0 ? (float)(sd / sw) : 0.0f;
        for (int k = 1; k <= K; k++) h->h_dev[k] = clampf(h->h_dev[k] - common, -0.03f, 0.03f);
    }

    /* residual noise per band: bins away from the harmonics */
    float acc[HM_NB], cnt[HM_NB];
    memset(acc, 0, sizeof acc); memset(cnt, 0, sizeof cnt);
    float w2 = 0.0f;
    for (int n = -W / 2; n < W / 2; n++) {
        float w = 0.5f + 0.5f * cosf(TWO_PI_F * (float)n / (float)W);
        w2 += w * w;
    }
    for (int k = 2; k < nb; k++) {
        float f = (float)k * binhz;
        if (f < 80.0f || f > 20000.0f) continue;
        if (h->voiced) {
            /* with a 4-period Hann window the midpoint between two harmonics is a null of both:
             * read the noise there, or leakage counts as noise (a clean tone measured -48 dB) */
            float r = f / h->f0, fr = r - floorf(r);
            if (fabsf(fr - 0.5f) > 0.12f) continue;
        }
        int b = band_of(f);
        acc[b] += pw[k];
        cnt[b] += 1.0f;
    }
    for (int b = 0; b < HM_NB; b++)
        h->nz[b] = cnt[b] > 0 ? acc[b] / cnt[b] / w2 : (b > 0 ? h->nz[b - 1] : 0.0f);

    /* envelope on the grid: through the harmonics (voiced) or the noise bands */
    if (h->voiced && h->nh > 0) {
        const float f0 = h->f0;
        for (int i = 0; i < HM_GRID; i++) {
            float f = grid_hz(i), r = f / f0, d;
            if (r <= 1.0f) d = h->h_db[1];
            else if (r >= (float)h->nh) d = h->h_db[h->nh] - 12.0f * log2f(r / (float)h->nh);
            else {
                int k = (int)r;
                float t = r - (float)k;
                d = h->h_db[k] + t * (h->h_db[k + 1] - h->h_db[k]);
            }
            h->env_new[i] = d;
        }
    } else {
        for (int i = 0; i < HM_GRID; i++) {
            float nzp = h->nz[band_of(grid_hz(i))];
            h->env_new[i] = 10.0f * log10f(nzp + 1e-24f);
        }
    }
    /* Smoothing (timbre): frame-to-frame averaging of the envelope */
    float a = clampf(h->p[HM_SMOOTHING] * h->p[HM_SMOOTH_TIMBRE], 0.0f, 0.95f) * 0.9f;
    if (!h->env_valid) a = 0.0f;
    for (int i = 0; i < HM_GRID; i++) h->env[i] = a * h->env[i] + (1.0f - a) * h->env_new[i];
    h->env_valid = h->voiced;
}

/* The Filter panel, measured: near brick-wall HPF / LPF (about 300 dB/oct), resonance in dB
 * (positive: a peak 0.3 oct inside the passband, 0.88 x res dB; negative: the slope moves inward,
 * 1.3 oct and 75 dB/oct at -80) and a mid band that is a Gaussian on log frequency (sigma =
 * 0.387 (width/12)^0.64 oct). Returns dB at f. */
static float filter_db(const hm_t *h, float f)
{
    float d = 0.0f;
    const float lf2 = log2f(fmaxf(f, 1.0f));
    for (int side = 0; side < 2; side++) {
        const float fc = midi_hz(side ? h->p[HM_LPF] : h->p[HM_HPF]);
        const float res = side ? h->p[HM_LPF_RES] : h->p[HM_HPF_RES];
        /* a filter parked at its end stop does nothing, unless its resonance is set: Humanoid's negative-resonance
         * roll-off is heard from an LPF at maximum (Vocal Enhancer, LPF Res -39.5: its top 4-7 dB under ours) */
        if (res == 0.0f && ((side == 0 && fc < 16.0f) || (side == 1 && fc > 0.48f * h->sr))) continue;
        const float x = side ? log2f(fc) - lf2 : lf2 - log2f(fc);    /* octaves into the passband */
        /* Measured on Humanoid (a saw at 82 Hz, Transform 1): the edge is a brick wall (-80 dB 0.08 octave
         * past it); a negative resonance is a roll-off from 1.4 octaves inside the passband, 0.85 |res| dB
         * per octave (res -40: -10 dB an octave in, -27 at 0.6; res -80: -50 at 0.6); a positive one is a
         * peak of res dB just inside the edge (0.15 octave), within 1-2 dB from 11 to 24 dB. */
        float cut = x < 0.0f ? 1000.0f * -x : 0.0f;
        if (res < 0.0f && x < 1.4f) cut += 0.85f * -res * (1.4f - x);
        d -= fminf(100.0f, cut);
        if (res > 0.0f) {
            float u = x - 0.15f;
            d += res * expf(-u * u / 0.5f);
        }
    }
    const float mg = h->p[HM_MID_GAIN];
    if (fabsf(mg) > 0.01f) {
        /* a Gaussian in log frequency, sigma 0.0164 octave per unit of Width (measured 8..60, within 1 %) */
        const float sg = 0.01645f * clampf(h->p[HM_MID_WIDTH], 4.0f, 80.0f);
        const float u = lf2 - log2f(midi_hz(h->p[HM_MID_FREQ]));
        d += mg * expf(-u * u / (2.0f * sg * sg));
    }
    return d;
}

/* Sharpen, measured on Humanoid: a fixed curve (from a flat-spectrum input) plus a partial
 * flattening of the input's spectral tilt (from a vowel), both scaled by Sharpen / 100; Bias moves
 * the lows. Loudness-neutral: the lines are renormalised afterwards. Adds dB to `out` (grid). */
static void sharpen_curve(const hm_t *h, float *out)
{
    const float s = h->p[HM_SHARPEN] / 100.0f, bias = h->p[HM_SHARPEN_BIAS];
    if (s <= 0.0f && bias == 0.0f) { for (int i = 0; i < HM_GRID; i++) out[i] = 0.0f; return; }
    /* fixed part at Sharpen 80 (dB re 2 kHz), log-frequency points */
    static const float fx[9] = {110, 330, 990, 1980, 2970, 4950, 7920, 10890, 20000};
    static const float gx[9] = {-5.6f, -5.6f, -1.7f, 0.0f, 2.6f, -0.8f, -0.8f, -0.8f, -0.8f};
    /* tilt of the envelope, dB per octave, 150 Hz .. 6 kHz */
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
    for (int i = 0; i < HM_GRID; i++) {
        float f = grid_hz(i);
        if (f < 150.0f || f > 6000.0f) continue;
        double x = (double)log2f(f / 1000.0f), y = h->env[i];
        sx += x; sy += y; sxx += x * x; sxy += x * y; n++;
    }
    float slope = (n > 2) ? (float)((n * sxy - sx * sy) / (n * sxx - sx * sx + 1e-12)) : 0.0f;
    slope = clampf(slope, -24.0f, 12.0f);
    for (int i = 0; i < HM_GRID; i++) {
        float f = grid_hz(i), g;
        if (f <= fx[0]) g = gx[0];
        else {
            int j = 0;
            while (j < 7 && f > fx[j + 1]) j++;
            float t = log2f(f / fx[j]) / log2f(fx[j + 1] / fx[j]);
            g = gx[j] + clampf(t, 0.0f, 1.0f) * (gx[j + 1] - gx[j]);
        }
        float d = g * (s / 0.8f) - 0.21f * s * slope * log2f(f / 1000.0f);
        if (f < 600.0f) d -= 1.58f * bias * (f < 330.0f ? 1.0f : (600.0f - f) / 270.0f);
        out[i] = d;
    }
}

/* ---------------------------------------------------------------- tuner */

static int nearest_in_mask(float m, unsigned mask)
{
    int c = (int)lrintf(m), best = c;
    float bd = 1e9f;
    if ((mask & 0xFFFu) == 0) return c;
    for (int n = c - 6; n <= c + 6; n++) {
        int pc = ((n % 12) + 12) % 12;
        if (!((mask >> pc) & 1u)) continue;
        float d = fabsf(m - (float)n);
        if (d < bd) { bd = d; best = n; }
    }
    return best;
}

/* fc of the deviation high-pass from Quantize (normalised as the plugin does) */
static float fc_quantize(float q)
{
    float n = clampf(2.26886f * q - 0.0676f, 0.0f, 1.0f);
    static const float xn[4] = {0.0f, 0.5f, 0.84f, 1.0f};
    /* ln of 3.51, 9.73, 25.2, 55.8 Hz: the cutoffs of this hop-rate one-pole that pass Humanoid's
     * measured 79 / 41 / 13 / 3 % of a 5.5 Hz vibrato (solved for the discrete filter) */
    static const float lf[4] = {1.2556f, 2.2752f, 3.2268f, 4.0218f};
    int i = n < 0.5f ? 0 : (n < 0.84f ? 1 : 2);
    float t = (n - xn[i]) / (xn[i + 1] - xn[i]);
    return expf(lf[i] + t * (lf[i + 1] - lf[i]));
}

/* A held pad, moved by whole octaves to within one octave of the voice (m, MIDI): a pad an octave up still sounds an
 * octave up, but Move's pads (E5 and up) no longer throw a voice sung around A3 two octaves away. Once folded it stays
 * put until the voice is more than 12.8 semitones from it, so a voice wandering near the edge doesn't flip octaves. */
static int fold_to_voice(hm_t *h, int slot, int note, float m)
{
    int n = note;
    if (h->fold_note[slot] == note) n = note + h->fold_off[slot];
    if (h->fold_note[slot] != note || fabsf((float)n - m) > 12.8f) {
        n = note;
        while ((float)n - m > 12.0f) n -= 12;
        while ((float)n - m < -12.0f) n += 12;
    }
    h->fold_note[slot] = note;
    h->fold_off[slot] = n - note;
    return n;
}

/* Returns the number of targets (0..2) and fills the output pitches (MIDI). */
static int tune(hm_t *h, float *out_midi, int *base_note)
{
    int mode = (int)lrintf(h->p[HM_TUNING_MODE]);
    if (mode == HM_MODE_MIDI && h->n_held == 0 && h->midi_idle_tune) mode = HM_MODE_SCALE;    /* no pad: keep tuning to the scale */
    if (!h->voiced) return 0;
    const float m = hz_midi(h->f0);
    if (h->p[HM_SCALE_BYPASS] > 0.5f && mode == HM_MODE_SCALE) {
        out_midi[0] = m; base_note[0] = (int)lrintf(m);
        return 1;
    }
    int ref = mode == HM_MODE_SCALE ? nearest_in_mask(m, h->mask) : (int)lrintf(m);
    if (h->ref_note < -100) h->lp_dev = m - (float)ref;
    else if (ref != h->ref_note) h->lp_dev += (float)(h->ref_note - ref);
    h->ref_note = ref;
    const float d = m - (float)ref;
    /* the corrected part of the deviation: its low-passed value; the higher cutoff of
     * Quantize and Flatten x Robotify wins */
    float fc = fc_quantize(h->p[HM_QUANTIZE]);
    float rf = h->p[HM_ROBOTIFY] * h->p[HM_FLATTEN];
    if (rf > 0.0f) {
        float ff = rf >= 0.999f ? 1e6f : 9.73f * rf / (1.0f - rf);
        if (ff > fc) fc = ff;
    }
    float al = 1.0f - expf(-TWO_PI_F * fc * (float)HM_HOP / h->sr);
    h->lp_dev += al * (d - h->lp_dev);
    const float keep = d - h->lp_dev;

    int nt = 0;
    if (mode == HM_MODE_SCALE) {
        base_note[0] = ref; nt = 1;
    } else if (mode == HM_MODE_NOTE) {
        base_note[0] = (int)lrintf(h->p[HM_PITCH_MONO]); nt = 1;
        if (h->p[HM_DUOPHONIC] > 0.5f) { base_note[1] = (int)lrintf(h->p[HM_PITCH_DUO]); nt = 2; }
    } else {
        for (int i = h->n_held - 1; i >= 0 && nt < 2; i--) { base_note[nt] = fold_to_voice(h, nt, h->midi_held[i], m); nt++; }
    }
    for (int i = 0; i < nt; i++) out_midi[i] = (float)base_note[i] + keep;
    return nt;
}

/* --------------------------------------------------------------- synthesis */

static void add_partial(hm_t *h, float bin, float amp, float ph)
{
    const float cr = 0.5f * amp * cosf(ph), ci = 0.5f * amp * sinf(ph);
    int k0 = (int)ceilf(bin - (float)HM_KERN_HW), k1 = (int)floorf(bin + (float)HM_KERN_HW);
    for (int k = k0; k <= k1; k++) {
        float d = ((float)k - bin) * (float)HM_KERN_OS + (float)(HM_KERN_HW * HM_KERN_OS);
        int di = (int)d;
        if (di < 0 || di >= 2 * HM_KERN_HW * HM_KERN_OS) continue;
        float t = d - (float)di;
        float kv = h->kern[di] + t * (h->kern[di + 1] - h->kern[di]);
        int kp = ((k % HM_NS) + HM_NS) % HM_NS, kn = ((-k % HM_NS) + HM_NS) % HM_NS;
        h->sre[kp] += cr * kv; h->sim[kp] += ci * kv;     /* positive frequency */
        h->sre[kn] += cr * kv; h->sim[kn] -= ci * kv;     /* its mirror */
    }
}

typedef struct { float f0; float gain; float wa, wb; } line_t;   /* wa > 0: path A rides on this line */

static void hop(hm_t *h)
{
    const float sr = h->sr;
    const int lat = (int)lrintf(h->p[HM_LATENCY]);
    if (lat != h->lat_set) { h->lat_set = lat; h->la = lag_for(lat); }
    const int la = h->la;
    if (wavetable_stale(h)) build_wavetable(h);

    track_pitch(h, la);
    analyse_spectrum(h, la);

    float out_midi[2];
    int base[2];
    int nt = tune(h, out_midi, base);
    h->tgt_hz = nt > 0 ? midi_hz((float)base[0]) : 0.0f;

    /* Warble: vibrato on the output pitch. Mode 0 = LFO (rate x 10.1 Hz, measured), mode 1 = the
     * fast "FM" one (8 + 32 x rate Hz as far as a pitch tracker can follow it). A negative rate is
     * tempo synced (Humanoid stood still with the transport stopped): 1/1 .. 1/16 of a bar. */
    const float rate = h->p[HM_WARBLE_RATE];
    float wr;
    if (rate < 0.0f) {
        int idx = (int)lrintf(-rate * 4.0f);                         /* 0..4: 1, 1/2, 1/4, 1/8, 1/16 bar */
        wr = h->bpm / 240.0f * (float)(1 << idx);
    } else {
        wr = h->p[HM_WARBLE_MODE] < 0.5f ? 10.1f * rate : 8.0f + 32.0f * rate;
    }
    h->warble_ph += TWO_PI_F * wr * (float)HM_HOP / sr;
    if (h->warble_ph > TWO_PI_F) h->warble_ph -= TWO_PI_F;
    const float wob = h->p[HM_WARBLE] * 1.3f * sinf(h->warble_ph);

    /* lines: per target the main line, a Note Shift line and the Octave lines */
    line_t ln[HM_NLINE];
    memset(ln, 0, sizeof ln);
    const float ns = h->p[HM_NOTE_SHIFT], oc = h->p[HM_OCTAVE], sm = h->p[HM_SYNTH_MIX];
    const int mode = (int)lrintf(h->p[HM_TUNING_MODE]);
    /* Synth Mix: the pitch-corrected signal (path A) and the resynthesis (path B) are different
     * signals in Humanoid, crossfaded linearly (measured with the two at different pitches: A x (1 - sm),
     * B x sm, within 0.1 dB from 0.1 to 0.9). On the same pitch they interfere: about -4.8 dB at 0.5
     * on a voice, varying per harmonic and with the pitch; modelled as one phase between the paths,
     * 108 degrees at Transform 0 relaxing to 90 (a power sum, the average measured) at Transform 1. */
    const float wa = 1.0f - sm, wb = sm;
    /* Note Shift TRANSPOSES path B (Humanoid: at Synth Mix 1 only the shifted voice is heard, at 0.5 the
     * voice and the shifted line at -6 dB each); the Octave lines follow the shifted pitch. */
    const int shifting = fabsf(ns) > 0.01f;
    const float sub_g = oc < -0.01f ? 0.52f * (-oc) : 0.0f;
    /* octave up, measured on the even harmonics: 0.5 -> 0.6, 1 -> 0.8, 1.5 -> 2.4 (it climbs past 1) */
    const float up_g = oc <= 0.01f ? 0.0f : oc <= 0.5f ? 1.2f * oc : oc <= 1.0f ? 0.6f + 0.4f * (oc - 0.5f) : 0.8f + 3.2f * (oc - 1.0f);
    const float abph = (108.0f - 18.0f * (h->p[HM_TRANSFORM] > 0.0f ? powf(h->p[HM_TRANSFORM], 1.7f) : 0.0f)) * (PI_F / 180.0f);
    const float abc = cosf(abph), abs_ = sinf(abph);
    for (int t = 0; t < nt; t++) {
        float mm = out_midi[t] + wob;
        float keep = out_midi[t] - (float)base[t];
        line_t *M0 = &ln[t * 4 + 0];
        M0->f0 = midi_hz(mm); M0->gain = 1.0f; M0->wa = wa; M0->wb = shifting ? 0.0f : wb;
        float bm = mm;                                   /* path B's pitch */
        if (shifting) {
            float sn = (float)base[t] + ns;
            if (mode == HM_MODE_SCALE) sn = (float)nearest_in_mask(sn, h->mask);
            bm = sn + keep + wob;
            ln[t * 4 + 1].f0 = midi_hz(bm); ln[t * 4 + 1].gain = 1.0f; ln[t * 4 + 1].wb = wb;
        }
        if (sub_g > 0.0f) { ln[t * 4 + 2].f0 = 0.5f * midi_hz(bm); ln[t * 4 + 2].gain = sub_g; ln[t * 4 + 2].wb = wb; }
        if (up_g > 0.0f)  { ln[t * 4 + 3].f0 = 2.0f * midi_hz(bm); ln[t * 4 + 3].gain = up_g; ln[t * 4 + 3].wb = wb; }
    }

    /* synthesis spectrum */
    memset(h->sre, 0, sizeof h->sre);
    memset(h->sim, 0, sizeof h->sim);
    const float rob = h->p[HM_ROBOTIFY];
    const float metal = rob * h->p[HM_METALLIC];
    const float emph = rob * h->p[HM_EMPHASIS];
    const float fshift = exp2f(h->p[HM_FORMANT]);
    const float tr = h->p[HM_TRANSFORM];
    const float tw = tr > 0.0f ? powf(tr, 1.7f) : 0.0f;
    const float binsz = (float)HM_NS / sr;
    const int filt = h->p[HM_HPF] > 16.5f || midi_hz(h->p[HM_LPF]) < 0.48f * sr || fabsf(h->p[HM_MID_GAIN]) > 0.01f
                     || h->p[HM_HPF_RES] != 0.0f || h->p[HM_LPF_RES] != 0.0f;
    float lv[HM_KMAX + 1], lva[HM_KMAX + 1];
    float env_s[HM_GRID];
    sharpen_curve(h, env_s);
    for (int i = 0; i < HM_GRID; i++) env_s[i] += h->env[i];

    /* Loudness: Humanoid holds every line at the voice's loudness wherever it is moved (Note mode from -7 to +24 st:
     * within ~1 dB), so the reference is the voice's own harmonic series. Reading it at the line's harmonics instead
     * lost 6-14 dB going up (fewer harmonics under the envelope). The Octave lines keep their measured ratio to the
     * line they follow. */
    const double pvoice = h->env_valid ? env_power(h, h->f0 > 0.0f ? h->f0 : 150.0f, sr) : 0.0;
    for (int l = 0; l < HM_NLINE; l++) {
        line_t *L = &ln[l];
        if (L->f0 <= 0.0f || L->gain <= 0.0f || (L->wa <= 0.0f && L->wb <= 0.0f) || !h->env_valid) {
            for (int k = 1; k <= HM_KMAX; k++) h->fr_prev[l][k] = 0.0f;
            continue;
        }
        int K = (int)(fminf(HM_FMAX, 0.45f * sr) / L->f0);
        if (K > HM_KMAX) K = HM_KMAX;
        /* voice levels from the (formant-shifted) envelope. Humanoid keeps the loudness of
         * each line whatever Formant and Emphasis do: the reference is the unshifted envelope. */
        double pv = 0.0, pw = 0.0, pref = 0.0;
        float mx = -1e9f;
        for (int k = 1; k <= K; k++) {
            float d = env_at(env_s, (float)k * L->f0 / fshift);
            lv[k] = d;
            if (d > mx) mx = d;
            pw += (double)expf((float)(h->wt_db[k]) * 0.23025851f);
        }
        {
            const int role = l & 3;                       /* 0 main, 1 note shift, 2 octave down, 3 octave up */
            if (role < 2) {
                pref = pvoice;
            } else {
                const float fb = role == 2 ? 2.0f * L->f0 : 0.5f * L->f0;     /* the line it follows */
                pref = env_power(h, L->f0, sr) * pvoice / (env_power(h, fb, sr) + 1e-30);
            }
        }
        /* Emphasis lifts the weak harmonics: the levels are pulled toward the loudest by 20 % */
        if (emph > 0.0f)
            for (int k = 1; k <= K; k++) lv[k] += 0.2f * emph * (mx - lv[k]);
        for (int k = 1; k <= K; k++) pv += (double)expf((float)(lv[k]) * 0.23025851f);
        {
            float fix = 10.0f * log10f((float)(pref / (pv + 1e-30)));
            for (int k = 1; k <= K; k++) lv[k] += fix;
            pv = pref;
        }
        /* path A (the voice line only) keeps the voice levels */
        for (int k = 1; k <= K; k++) lva[k] = lv[k];
        /* Transform: dB morph toward the wavetable at the same total power (path B) */
        if (tw > 0.0f) {
            float g = 10.0f * log10f((float)(pv / (pw + 1e-30)));
            double pm = 0.0;
            for (int k = 1; k <= K; k++) {
                lv[k] = (1.0f - tw) * lv[k] + tw * (h->wt_db[k] + g);
                pm += (double)expf((float)(lv[k]) * 0.23025851f);
            }
            float fix = 10.0f * log10f((float)(pv / (pm + 1e-30)));
            for (int k = 1; k <= K; k++) lv[k] += fix;
        }
        for (int k = 1; k <= K; k++) {
            float dev = (k <= h->nh) ? h->h_dev[k] * (1.0f - metal) : 0.0f;
            float f = (float)k * L->f0 * (1.0f + dev);
            if (f >= fminf(HM_FMAX, 0.45f * sr)) { h->fr_prev[l][k] = 0.0f; continue; }
            float fp = h->fr_prev[l][k] > 0.0f ? h->fr_prev[l][k] : f;
            h->ph[l][k] += PI_F * (fp + f) * (float)HM_HOP / sr;
            h->ph[l][k] -= TWO_PI_F * floorf(h->ph[l][k] / TWO_PI_F);
            h->fr_prev[l][k] = f;
            const float fg = filt ? db_lin(filter_db(h, f)) : 1.0f;
            if (L->wa > 0.0f) {
                /* A and B as one partial: wb B + wa A e^(j phase) */
                const float b = L->wb * db_lin(lv[k]), a = L->wa * db_lin(lva[k]);
                const float re = b + a * abc, im = a * abs_;
                add_partial(h, f * binsz, L->gain * fg * sqrtf(re * re + im * im), h->ph[l][k] + atan2f(im, re));
            } else {
                add_partial(h, f * binsz, L->gain * L->wb * fg * db_lin(lv[k]), h->ph[l][k]);
            }
        }
        for (int k = K + 1; k <= HM_KMAX; k++) h->fr_prev[l][k] = 0.0f;
    }

    /* inverse FFT and overlap-add, centred on the analysis time */
    fft_run(&h->fft, h->sre, h->sim, HM_NS, 1);
    const unsigned centre = h->w - (unsigned)la;
    const float sc = 1.0f / (float)HM_NS;
    for (int n = -HM_HOP; n < HM_HOP; n++) {
        float v = h->sre[(n + HM_NS) % HM_NS] * sc * h->olaw[n + HM_HOP];
        h->out_ring[(centre + (unsigned)n) & (HM_OUTRING - 1)] += v;
    }

    /* residual noise: band-shaped, random phase, sine-windowed overlap-add */
    /* path B's Denoise (measured 32.5 dB HNR at Robotify 1); path A's is weaker (27.3 dB) */
    const float rdn = (rob * h->p[HM_DENOISE]) * (rob * h->p[HM_DENOISE]);
    const float nb_db = -8.9f - 16.7f * rdn + 0.7f * rob, na_db = -8.9f - 10.0f * rdn + 0.7f * rob;
    float ngain_db = 10.0f * log10f(wb * wb * powf(10.0f, nb_db / 10.0f) + wa * wa * powf(10.0f, na_db / 10.0f) + 1e-12f);
    if (nt == 0 && (h->voiced || mode == HM_MODE_MIDI)) ngain_db = -200.0f;    /* MIDI mode with no note: silence */
    if (h->voiced && nt == 0 && mode != HM_MODE_MIDI) ngain_db = -200.0f;
    const float ng = db_lin(ngain_db);
    const int M = 2 * HM_HOP;
    memset(h->nre, 0, sizeof h->nre);
    memset(h->nim, 0, sizeof h->nim);
    if (ng > 1e-6f) {
        for (int k = 1; k < M / 2; k++) {
            float f = (float)k * sr / (float)M / fshift;
            if (f < 80.0f) continue;
            float s = h->nz[band_of(f)];
            float m = sqrtf((float)M * s) * ng;
            if (filt) m *= db_lin(filter_db(h, f * fshift));
            float a = TWO_PI_F * rnd(h);
            h->nre[k] = m * cosf(a); h->nim[k] = m * sinf(a);
            h->nre[M - k] = h->nre[k]; h->nim[M - k] = -h->nim[k];
        }
        fft_run(&h->fft, h->nre, h->nim, M, 1);
        for (int n = 0; n < M; n++) {
            float w = sinf(PI_F * ((float)n + 0.5f) / (float)M);
            h->out_ring[(centre - (unsigned)HM_HOP + (unsigned)n) & (HM_OUTRING - 1)] += h->nre[n] * w / (float)M;
        }
    }
    (void)band_lo;
}

/* ----------------------------------------------------------------- process */

void hm_begin(hm_t *h)
{
    h->k_ig = db_lin(h->p[HM_INPUT_GAIN]);
    /* Humanoid's Output tops out at +9 dB (+12 measured +9.0; -24..+9 exact) */
    h->k_og = db_lin(fminf(h->p[HM_OUTPUT_GAIN], 9.0f)) * HM_MAKEUP;
    h->k_dw = h->p[HM_DRY_WET];
    h->k_lat = hm_latency(h);
    h->k_thr = db_lin(h->p[HM_GATE]);
    h->k_gate_on = h->p[HM_GATE] > -79.5f;
    h->k_grel = expf(-1.0f / (h->sr * 0.005f));
    h->k_gclose = 1.0f / (h->sr * fmaxf(h->p[HM_GATE_ATTACK], 0.001f));
    h->k_gopen = 1.0f / (h->sr * fmaxf(h->p[HM_GATE_RELEASE], 0.001f));
    h->k_ghold = (int)(h->sr * h->p[HM_GATE_HOLD]);
    hmfx_begin(&h->fx, h->p, h->bpm);
    h->k_lv_p  = 1.0f - expf(-1.0f / (0.020f * h->sr));    /* short-term power */
    h->k_lv_up = 1.0f - expf(-1.0f / (0.250f * h->sr));    /* the level reference: a slow RMS (250 ms) */
    h->k_lv_dn = h->k_lv_up;
    h->k_lv_g  = 1.0f - expf(-1.0f / (0.150f * h->sr));
    h->k_lim_a = 1.0f - expf(-1.0f / (0.0004f * h->sr));   /* the peak limiter: 0.4 ms attack, 120 ms release */
    h->k_lim_r = 1.0f - expf(-1.0f / (0.120f * h->sr));
}

void hm_tick(hm_t *h, float x, float *outl, float *outr)
{
    if (x != x) x = 0.0f;
    h->dry_ring[h->w & (HM_RING - 1)] = x;
    x *= h->k_ig;
    if (h->k_gate_on) {
        float a = fabsf(x);
        h->gate_env = a > h->gate_env ? a : h->k_grel * h->gate_env + (1.0f - h->k_grel) * a;
        if (h->gate_env < 1e-20f) h->gate_env = 0.0f;
        if (h->gate_env > h->k_thr) { h->gate_hold = h->k_ghold; h->gate_g = fminf(1.0f, h->gate_g + h->k_gopen); }
        else if (h->gate_hold > 0) h->gate_hold--;
        else h->gate_g = fmaxf(0.0f, h->gate_g - h->k_gclose);
        x *= h->gate_g;
    }
    h->in_ring[h->w & (HM_RING - 1)] = x;
    h->w++;
    if (++h->cnt >= HM_HOP) { h->cnt = 0; hop(h); }
    const unsigned r = h->w - (unsigned)h->k_lat;
    float y = h->out_ring[r & (HM_OUTRING - 1)];
    h->out_ring[r & (HM_OUTRING - 1)] = 0.0f;
    if (fabsf(y) < 1e-20f) y = 0.0f;
    const float dry = h->dry_ring[r & (HM_RING - 1)];
    float l = y, rr = y;
    hmfx_tick(&h->fx, &l, &rr);
    /* the makeup takes the dry too (Dry/Wet 0 measured +2 dB); Out Gain only the wet (Dry/Wet 0.5, Out -12: the dry
     * unchanged, the wet 12 dB down; it cut Vocal Enhancer's dry with its wet, 6 dB under Humanoid) */
    const float dg = (1.0f - h->k_dw) * HM_MAKEUP, wg = h->k_dw * h->k_og;
    float ol = dg * dry + wg * l, orr = dg * dry + wg * rr;
    float ceil = HM_OUT_CEIL;
    if (h->autolevel) {
        /* Leveller: the mix is held at HM_LEVEL_TARGET (RMS) whatever the preset or the input level. The reference is a
         * slow RMS that cannot jump more than 4x (6 dB) in one step, so the clicks a synth path makes at note onsets don't
         * set it (they held the gain down for seconds), and it stands still in pauses, so the gain neither pumps on a
         * phrase onset nor lifts the room between phrases. Out Gain comes before it and is levelled away: imported
         * presets use it to even out their own loudness in Humanoid (32 of 282, down to -12.5 dB), which applied after
         * the leveller left them that much quieter than the rest. */
        const float p = 0.5f * (ol * ol + orr * orr);
        h->lv_pow += (p - h->lv_pow) * h->k_lv_p;
        if (h->lv_pow < 1e-20f) h->lv_pow = 0.0f;
        if (h->lv_pow > 1e-6f) {                                  /* above -60 dBFS: there is sound to level */
            if (h->lv_ms < 1e-9f) h->lv_ms = h->lv_pow;               /* the first sound sets the reference */
            const float pin = h->lv_pow > 4.0f * h->lv_ms ? 4.0f * h->lv_ms : h->lv_pow;
            h->lv_ms += (pin - h->lv_ms) * (pin > h->lv_ms ? h->k_lv_up : h->k_lv_dn);
            float goal = sqrtf(HM_LEVEL_TARGET_MS / h->lv_ms);
            goal = goal < 0.25f ? 0.25f : goal > 24.0f ? 24.0f : goal;   /* -12..+27.6 dB */
            h->lv_g += (goal - h->lv_g) * h->k_lv_g;
        }
        ol *= h->lv_g; orr *= h->lv_g;
        ceil = HM_LEVEL_CEIL;
    }
    /* A peak limiter: anything past the ceiling is pulled down at once and let back up over 120 ms. Under Auto Level
     * the gain is a guess at the start of a note (it can be 20 dB too high for the first half second); without it this
     * is Humanoid's own output limiter (peaks held at about -0.5 dBFS: a +19 dB filter resonance came out at -0.7). */
    const float pk = fmaxf(fabsf(ol), fabsf(orr));
    const float need = pk > ceil ? ceil / pk : 1.0f;
    h->lv_lim += (need - h->lv_lim) * (need < h->lv_lim ? h->k_lim_a : h->k_lim_r);
    if (need < h->lv_lim) h->lv_lim = need;                        /* never let a sample through over the ceiling */
    *outl = ol * h->lv_lim;
    *outr = orr * h->lv_lim;
}

void hm_process(hm_t *h, const float *in, float *outl, float *outr, int n)
{
    hm_begin(h);
    for (int i = 0; i < n; i++) hm_tick(h, in[i], &outl[i], &outr[i]);
}
