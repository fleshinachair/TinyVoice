#ifndef TINYVOICE_HMFX_H
#define TINYVOICE_HMFX_H

/* Resynth's output effects, in the reference's order after the resynthesis:
 * De-ess, Freeze (a short glitch loop, mono), Widen (chorus ensemble). The Filter
 * panel is spectral and lives in hm.c. Parameters are read from the engine's hm_t p[] (the reference units).
 * Host-free, no allocation. */

#define HMFX_CHORUS  2048            /* chorus delay line (power of two) */
#define HMFX_FREEZE  65536           /* freeze buffer, mono: up to ~1.48 s at 44.1 kHz (power of two) */

typedef struct {
    float sr;
    /* per-block constants (hmfx_begin) */
    int ds_on, fz_want;
    float ds_a, ds_att, ds_rel, ds_thr, ds_maxcut;
    float wd, ch_base, ch_dep, ch_a, ch_dinc, fz_step;
    /* de-esser */
    float ds_hp_z[2][2], ds_env, ds_full, ds_gain;
    /* widen */
    float ch_buf[2][HMFX_CHORUS];
    unsigned ch_w;
    float ch_ph, ch_hp_z[2];
    /* freeze */
    float fz_buf[HMFX_FREEZE];
    unsigned fz_w;
    int fz_on, fz_len, fz_pos;
    float fz_fade;
} hmfx_t;

void hmfx_init(hmfx_t *f, float sample_rate);
/* p: the engine's parameter array (hm_t.p); bpm: host tempo (Freeze sync).
 * hmfx_begin once per block, then hmfx_tick per sample (hmfx_process = both). */
void hmfx_begin(hmfx_t *f, const float *p, float bpm);
void hmfx_tick(hmfx_t *f, float *l, float *r);
void hmfx_process(hmfx_t *f, const float *p, float bpm, float *l, float *r, int n);

#endif
