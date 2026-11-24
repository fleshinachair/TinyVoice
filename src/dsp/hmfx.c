#include "hmfx.h"
#include "hm.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979f

static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float flush(float v) { return fabsf(v) < 1e-20f ? 0.0f : v; }

void hmfx_init(hmfx_t *f, float sample_rate)
{
    memset(f, 0, sizeof(*f));
    f->sr = sample_rate;
    f->ds_gain = 1.0f;
}




void hmfx_begin(hmfx_t *f, const float *p, float bpm)
{
    const float sr = f->sr;
    /* de-esser: a split-band compressor on the top (above ~5 kHz) */
    f->ds_on = p[HM_DEESS] > 0.05f;
    f->ds_a = expf(-2.0f * PI_F * 5000.0f / sr);
    f->ds_att = expf(-1.0f / (sr * fmaxf(p[HM_DEESS_ATTACK], 0.001f)));
    f->ds_rel = expf(-1.0f / (sr * 0.08f));
    /* sibilant when the top holds more than a share of the energy; higher Sensitivity
     * = a lower share (measured: 0.6 cuts more than 0.2) */
    f->ds_thr = 0.65f - clampf(p[HM_DEESS_SENS], 0.05f, 0.6f);
    f->ds_maxcut = powf(10.0f, -p[HM_DEESS] / 20.0f);
    /* widen: two modulated delays in anti-phase, high-passed at Cutoff, mixed by Widen */
    /* the side level follows Widen^1.17, -4 dB under the mid at 1 (measured: -18.7 / -11.7 / -4.0 dB at .25 / .5 / 1) */
    f->wd = p[HM_WIDEN] > 0.0f ? 0.725f * powf(p[HM_WIDEN], 1.17f) : 0.0f;
    f->ch_base = 0.012f * sr;
    f->ch_dep = 0.004f * sr;
    /* Cutoff barely changes Humanoid's width (measured): ~160 .. 250 Hz here */
    f->ch_a = expf(-2.0f * PI_F * (200.0f * powf(10.0f, (p[HM_WIDEN_CUTOFF] - 0.9f) * 1.0f)) / sr);
    f->ch_dinc = 2.0f * PI_F * clampf(p[HM_WIDEN_RATE], 0.05f, 20.0f) * 0.25f / sr;
    /* freeze: the last Buffer ms (or a tempo division) loops while on */
    int len;
    if (p[HM_FREEZE_SYNC] > 0.5f && bpm > 1.0f) {
        int idx = (int)lrintf(clampf(p[HM_FREEZE_SYNCLEN], 0.0f, 6.0f));   /* 1/64 .. 1/1 of a bar */
        len = (int)(sr * 240.0f / bpm * (float)(1 << idx) / 64.0f);
    } else {
        len = (int)(sr * p[HM_FREEZE_MS] * 0.001f);
    }
    if (len < 32) len = 32;
    if (len > HMFX_FREEZE - 1) len = HMFX_FREEZE - 1;
    f->fz_want = p[HM_FREEZE] > 0.5f;
    if (f->fz_want && !f->fz_on) { f->fz_len = len; f->fz_pos = 0; }
    f->fz_on = f->fz_want;
    f->fz_step = 1.0f / (0.005f * sr);
}

void hmfx_tick(hmfx_t *f, float *pl, float *pr)
{
    float l = *pl, r = *pr;
    if (f->ds_on) {
        float lo[2], hi[2], x[2];
        x[0] = l; x[1] = r;
        for (int c = 0; c < 2; c++) {
            f->ds_hp_z[c][0] = flush((1.0f - f->ds_a) * x[c] + f->ds_a * f->ds_hp_z[c][0]);
            lo[c] = f->ds_hp_z[c][0];
            hi[c] = x[c] - lo[c];
        }
        float eh = 0.5f * (hi[0] * hi[0] + hi[1] * hi[1]);
        float ef = 0.5f * (l * l + r * r);
        f->ds_env = flush(eh > f->ds_env ? f->ds_att * f->ds_env + (1 - f->ds_att) * eh : f->ds_rel * f->ds_env + (1 - f->ds_rel) * eh);
        f->ds_full = flush(f->ds_rel * f->ds_full + (1 - f->ds_rel) * ef);
        float ratio = f->ds_full > 1e-12f ? f->ds_env / f->ds_full : 0.0f;
        float over = clampf((ratio - f->ds_thr) / (1.0f - f->ds_thr), 0.0f, 1.0f);
        float g = 1.0f - over * (1.0f - f->ds_maxcut);
        f->ds_gain = g < f->ds_gain ? f->ds_att * f->ds_gain + (1 - f->ds_att) * g : f->ds_rel * f->ds_gain + (1 - f->ds_rel) * g;
        l = lo[0] + hi[0] * f->ds_gain;
        r = lo[1] + hi[1] * f->ds_gain;
    }
    /* freeze (mono) */
    if (!f->fz_want) {
        f->fz_buf[f->fz_w & (HMFX_FREEZE - 1)] = 0.5f * (l + r);
        f->fz_w++;
    }
    f->fz_fade = clampf(f->fz_fade + (f->fz_want ? f->fz_step : -f->fz_step), 0.0f, 1.0f);
    if (f->fz_fade > 0.0f) {
        unsigned src = f->fz_w - (unsigned)f->fz_len + (unsigned)f->fz_pos;
        const float v = f->fz_buf[src & (HMFX_FREEZE - 1)], g = f->fz_fade;
        l = (1 - g) * l + g * v;
        r = (1 - g) * r + g * v;
        if (f->fz_want && ++f->fz_pos >= f->fz_len) f->fz_pos = 0;
    }
    /* widen */
    const float m = 0.5f * (l + r);
    f->ch_buf[0][f->ch_w & (HMFX_CHORUS - 1)] = m;
    f->ch_w++;
    if (f->wd > 0.001f) {
        const float s = sinf(f->ch_ph);
        float wet[2];
        for (int c = 0; c < 2; c++) {
            float d = f->ch_base + (c ? -1.0f : 1.0f) * f->ch_dep * s;
            float pos = (float)(f->ch_w - 1) - d;
            unsigned i0 = (unsigned)floorf(pos);
            float t = pos - floorf(pos);
            float v = f->ch_buf[0][i0 & (HMFX_CHORUS - 1)] * (1 - t) + f->ch_buf[0][(i0 + 1) & (HMFX_CHORUS - 1)] * t;
            f->ch_hp_z[c] = flush(f->ch_a * f->ch_hp_z[c] + (1 - f->ch_a) * v);
            wet[c] = v - f->ch_hp_z[c];
        }
        const float sd = 0.5f * (wet[0] - wet[1]) * 1.6f * f->wd;   /* side only: the mono sum stays clean */
        l += sd;
        r -= sd;
    }
    f->ch_ph += f->ch_dinc;
    if (f->ch_ph > 2.0f * PI_F) f->ch_ph -= 2.0f * PI_F;
    *pl = l; *pr = r;
}

void hmfx_process(hmfx_t *f, const float *p, float bpm, float *l, float *r, int n)
{
    hmfx_begin(f, p, bpm);
    for (int i = 0; i < n; i++) hmfx_tick(f, &l[i], &r[i]);
}
