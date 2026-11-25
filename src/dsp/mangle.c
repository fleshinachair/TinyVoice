#include "mangle.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F          3.14159265358979f
#define MG_MIN_DELAY  3.0f          /* samples between a read and the newest sample (interpolation, margin) */
#define MG_MIN_GRAIN  0.005f        /* PSOLA: no grain shorter than 5 ms */
#define MG_UNVOICED_T0 256.0f
#define MG_HEAD_MARGIN 2.0
#define MG_LAT_SLEW   0.002f
#define MG_GRAIN_SCATTER 0.002f    /* seconds: the random extra delay of each Granular grain */

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

static inline float rnd(uint32_t *s)                 /* xorshift32, uniform [0, 1) */
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x ? x : 0x9e3779b9u;
    return (float)(x >> 8) * (1.0f / 16777216.0f);
}

/* the input at absolute index idx (the newest sample is n - 1), linear interpolation */
static inline float rd(const mg_t *m, double idx)
{
    const double fl = floor(idx);
    const float fr = (float)(idx - fl);
    const long long i = (long long)fl;
    const float a = m->ring[(size_t)(i & (MG_RING - 1))], b = m->ring[(size_t)((i + 1) & (MG_RING - 1))];
    return a + (b - a) * fr;
}

static inline float hann01(float x) { const float s = sinf(PI_F * x); return s * s; }

void mg_init(mg_t *m, float sr)
{
    memset(m, 0, sizeof(*m));
    m->sr = sr > 1000.0f ? sr : 44100.0f;
    m->rng = 0x2545f491u;
    for (int i = 0; i < MG_MAX_VOICES; i++) m->v[i].rng = 0x9e3779b9u * (uint32_t)(i + 1) + 12345u;
    m->k_amp = 1.0f - expf(-1.0f / (0.010f * m->sr));     /* 10 ms voice fades */
    m->k_lat = 1.0f - expf(-1.0f / (0.050f * m->sr));
    m->vgain = 1.0f;
    m->dry_lat = MG_MIN_DELAY;
    mg_params p;
    memset(&p, 0, sizeof p);
    p.engine = MG_GRANULAR; p.snap = 1; p.unison = 1; p.grain_hz = 10.0f; p.grain_jitter = 1;
    p.fix = 1; p.filter_hz = 1000.0f; p.filter_oct = 10.0f; p.mix = 1.0f; p.mono = 1;
    mg_set(m, &p);
}

/* The note offsets in play, then every voice's place. */
static void layout(mg_t *m)
{
    const mg_params *p = &m->p;
    int nn = 0;
    if (p->note_mode == MG_NOTES_PADS) {
        if (m->n_pads > 0) {
            if (p->mono) m->notes[nn++] = m->pads[m->n_pads - 1] - 60;
            else for (int i = 0; i < m->n_pads && nn < MG_MAX_NOTES; i++) m->notes[nn++] = m->pads[i] - 60;
        }
    } else if (p->note_mode == MG_NOTES_CHORD) {
        for (int i = 0; i < m->chord_n[m->slot]; i++) m->notes[nn++] = m->chord[m->slot][i] - 60;   /* an empty slot: silent (PitchMonster) */
    } else {
        m->notes[nn++] = 0;                      /* Normal: the voice at Pitch */
    }
    m->n_notes = nn;
    int u = p->unison < 1 ? 1 : p->unison > 8 ? 8 : p->unison;
    if (nn > 0 && nn * u > MG_MAX_VOICES) u = MG_MAX_VOICES / nn < 1 ? 1 : MG_MAX_VOICES / nn;
    const int total = nn * u;
    const float pitch = p->snap ? floorf(p->pitch + 0.5f) : p->pitch;
    const float S = clampf(p->detune, 0.0f, 12.0f), T = clampf(p->time_ms, 0.0f, MG_MAX_TIME_MS) * 0.001f * m->sr;
    for (int k = 0; k < MG_MAX_VOICES; k++) {
        mg_voice *v = &m->v[k];
        const int on = k < total;
        if (on && v->amp_goal <= 0.0f && v->amp < 1e-4f) {   /* a voice coming in: start it clean */
            memset(v->g, 0, sizeof v->g);
            v->started = v->have_mark = 0;
            /* each voice at its own grain phase: voices at one pitch started in step summed coherently (+3 dB per
             * doubling, +5.9 dB at 8; PitchMonster +2.3 Granular, flat Formant) */
            v->ph[0] = rnd(&v->rng) * 0.5f; v->ph[1] = v->ph[0] + 0.5f;
            v->inc[0] = v->inc[1] = 0.0f;                 /* restarts at its first sample */
            v->until_next = rnd(&v->rng) * MG_UNVOICED_T0; /* PSOLA: its first grain (and so its mark grid) a little later */
        }
        v->amp_goal = on ? 1.0f : 0.0f;
        if (!on) continue;
        const int j = k / u, q = k % u;
        v->st = pitch + (float)m->notes[j] + (u > 1 && !p->detune_jitter ? -0.5f * S + S * (float)q / (float)(u - 1) : 0.0f);
        v->delay = u > 1 && !p->time_jitter ? T * (float)q / (float)(u - 1) : 0.0f;
        /* A note's unison voices after the first read 3..7 ms further back (fixed per voice: layout runs every
         * block), so voices at one pitch don't sum coherently: PitchMonster's identical voices add +1.0 dB at 8
         * (Formant), ours +9 before */
        v->off = q > 0 ? (0.003f + 0.004f * ((float)q * 0.618034f - floorf((float)q * 0.618034f))) * m->sr : 0.0f;
        float pos = 0.0f;                                 /* -1 left .. +1 right */
        if (p->wide) pos = u > 1 ? -1.0f + 2.0f * (float)q / (float)(u - 1) : nn > 1 ? -1.0f + 2.0f * (float)j / (float)(nn - 1) : 0.0f;
        const float th = (pos + 1.0f) * 0.25f * PI_F;
        v->pl = cosf(th) * 1.41421356f;
        v->pr = sinf(th) * 1.41421356f;
    }
    m->n_voices = total;
    m->n_unison = u;
}

static void svf_coefs(float fc, float sr, float *g, float *k)
{
    *g = tanf(PI_F * clampf(fc, 5.0f, 0.49f * sr) / sr);
    *k = 1.41421356f;                                     /* Butterworth */
}

void mg_set(mg_t *m, const mg_params *p)
{
    m->p = *p;
    m->p.grain_hz = clampf(p->grain_hz, 1.0f, 100.0f);
    m->p.filter_oct = clampf(p->filter_oct, 0.1f, 10.0f);
    m->p.mix = clampf(p->mix, 0.0f, 1.0f);
    layout(m);
    const float f = clampf(m->p.filter_hz, 20.0f, 20000.0f);
    const float fhp = f * exp2f(-0.5f * m->p.filter_oct), flp = f * exp2f(0.5f * m->p.filter_oct);
    svf_coefs(fhp, m->sr, &m->hp_g, &m->hp_k);
    svf_coefs(flp, m->sr, &m->lp_g, &m->lp_k);
    if (fhp <= 10.0f) m->hp_g = 0.0f;                     /* below the audio band: that side is open */
    if (flp >= 0.45f * m->sr) m->lp_g = 0.0f;
}

void mg_set_chord(mg_t *m, int slot, const int *notes, int n)
{
    if (slot < 0 || slot > 7) return;
    if (n < 0) n = 0;
    if (n > MG_MAX_NOTES) n = MG_MAX_NOTES;
    for (int i = 0; i < n; i++) m->chord[slot][i] = notes[i] < 0 ? 0 : notes[i] > 127 ? 127 : notes[i];
    m->chord_n[slot] = n;
    if (slot == m->slot) layout(m);
}

void mg_default_chords(mg_t *m)
{
    static const int c[8][4] = { {60, 64, 67, 0}, {62, 65, 69, 0}, {64, 67, 71, 0}, {65, 69, 72, 0},
                                 {67, 71, 74, 0}, {69, 72, 76, 0}, {67, 71, 74, 77}, {60, 64, 67, 71} };
    for (int i = 0; i < 8; i++) mg_set_chord(m, i, c[i], i >= 6 ? 4 : 3);
}

void mg_select_chord(mg_t *m, int slot)
{
    slot = slot < 0 ? 0 : slot > 7 ? 7 : slot;
    if (slot != m->slot) { m->slot = slot; layout(m); }
}

void mg_set_pads(mg_t *m, const int *notes, int n)
{
    if (n > 4) { notes += n - 4; n = 4; }
    int same = n == m->n_pads;
    for (int i = 0; i < n && same; i++) same = notes[i] == m->pads[i];
    if (same) return;
    for (int i = 0; i < n; i++) m->pads[i] = notes[i];
    m->n_pads = n < 0 ? 0 : n;
    layout(m);
}

/* ---- Granular ---- */

static void tap_restart(mg_t *m, mg_voice *v, int k)
{
    const mg_params *p = &m->p;
    float st = v->st;
    if (p->detune_jitter) st += (rnd(&v->rng) - 0.5f) * clampf(p->detune, 0.0f, 12.0f);
    const float r = clampf(exp2f(st / 12.0f), 1.0f / 16.0f, 4.0f);
    const float rate = p->grain_hz * (p->grain_jitter ? 0.5f + rnd(&v->rng) : 1.0f);
    float base = p->time_jitter ? rnd(&v->rng) * clampf(p->time_ms, 0.0f, MG_MAX_TIME_MS) * 0.001f * m->sr : v->delay;
    /* every grain reads up to 2 ms further back at random: PitchMonster's Granular is not a clean delay at 0 st (null
     * -16 dB against the input) and its identical voices barely add (+1.6 dB at 8): its grains scatter like this */
    base += rnd(&v->rng) * MG_GRAIN_SCATTER * m->sr + v->off;
    float w = fabsf(1.0f - r) * m->sr / rate;
    const float room = (float)(MG_RING - 64) - MG_MIN_DELAY;
    if (base > room) base = room;
    if (base + w > room) w = room - base;
    v->r[k] = r;
    v->inc[k] = rate / m->sr;
    v->w[k] = w;
    v->base[k] = base + MG_MIN_DELAY;
}

static float granular(mg_t *m, mg_voice *v)
{
    float out = 0.0f;
    for (int k = 0; k < 2; k++) {
        if (v->inc[k] <= 0.0f) tap_restart(m, v, k);
        const float ph = v->ph[k];
        /* r > 1 reads ahead: the delay falls from base + w to base over the grain; r < 1 it rises */
        const float d = v->base[k] + (v->r[k] > 1.0f ? 1.0f - ph : ph) * v->w[k];
        /* sin^1.5: between Hann (the taps sum to 1 when they read the same sample) and a sine window (their powers
         * sum to 1 when they read different ones). Shifted, the taps are uncorrelated: power 0.85, -0.7 dB under 0 st;
         * PitchMonster -0.8 on a real vocal (Hann was -1.9) */
        const float s = sinf(PI_F * ph), wnd = s * sqrtf(s);
        out += wnd * rd(m, m->n - 1.0 - (double)d);
        v->ph[k] = ph + v->inc[k];
        if (v->ph[k] >= 1.0f) { v->ph[k] -= 1.0f; tap_restart(m, v, k); }
    }
    return out;
}

/* ---- Formant / Vocode: TD-PSOLA on the long ring, from this voice's delay ---- */

static float psola(mg_t *m, mg_voice *v, int vocode)
{
    const mg_params *p = &m->p;
    const double now = m->n;
    const float sr = m->sr;

    /* Start a new grain every output period. */
    v->until_next -= 1.0f;
    if (v->until_next <= 0.0f) {                                /* (a new voice waits a random fraction of a period first) */
        float st = v->st;
        if (p->detune_jitter) st += (rnd(&v->rng) - 0.5f) * clampf(p->detune, 0.0f, 12.0f);
        const float d = (p->time_jitter ? rnd(&v->rng) * clampf(p->time_ms, 0.0f, MG_MAX_TIME_MS) * 0.001f * sr : v->delay) + v->off;
        const double at = now - (double)d;                       /* this voice's "now" */
        /* the hop of its newest sample (at - 1): `at` itself can be the first sample of a hop not written yet, and a
         * grain read as unvoiced there gets the 256-sample unvoiced period, which lands every later grain on a hop
         * boundary too: the voice stuck at ratio 1 for good (seen) */
        const long long hop = (long long)floor((at - 1.0) / MG_HOP);
        const float t0 = m->t0h[(size_t)(hop & (MG_RING / MG_HOP - 1))];
        const int voiced = t0 > 1.0f;
        const float T0 = voiced ? t0 : MG_UNVOICED_T0;
        float pr = 1.0f;
        if (voiced) pr = vocode ? (440.0f * exp2f((60.0f + st - 69.0f) / 12.0f)) / (sr / t0) : exp2f(st / 12.0f);
        pr = clampf(pr, 1.0f / 16.0f, 4.0f);
        /* Fix On: the formant is the knob's alone; Off: it follows the shift */
        const float fst = p->formant + (p->fix ? 0.0f : st);
        const float r = voiced ? clampf(exp2f(fst / 12.0f), 0.25f, 4.0f) : clampf(exp2f(p->formant / 12.0f), 0.25f, 4.0f);
        const float Tout = T0 / pr;
        /* A grain is two input periods, played in 2 T0 / r: the formant moves by r and the grain still holds two
         * periods. (Reading r x 2 periods into a 2 T0 grain, as psola.c does up to r = 2, put the input's own pitch
         * inside each grain at r times: at Formant +19 the low harmonics were 10-13 dB under PitchMonster's.) */
        float span = 2.0f * T0;
        float len = span / r;
        if (len < MG_MIN_GRAIN * sr) { len = MG_MIN_GRAIN * sr; span = len * r; }
        const float max_span = 0.25f * (float)MG_RING;
        if (span > max_span) { len *= max_span / span; span = max_span; }
        float lat_t = fabsf(span - len) * 0.5f + 0.5f * T0 + 2.0f;
        if (!v->started) { v->lat = lat_t; v->started = 1; v->until_next = 0.0f; }   /* until_next <= 0 here anyway */
        else if (lat_t > v->lat) v->lat = lat_t;
        else v->lat += (lat_t - v->lat) * MG_LAT_SLEW * Tout;   /* per grain: the psola.c slew, per sample */
        const double ideal = at + 0.5 * len - (double)v->lat;
        if (!v->have_mark || !voiced || fabs(ideal - v->mark) > 4.0 * T0) v->mark = ideal;
        else v->mark += floor((ideal - v->mark) / T0 + 0.5) * T0;
        v->have_mark = 1;
        {   /* read-head protection: every read stays behind the newest sample */
            const double limit = now - 1.0 - MG_HEAD_MARGIN - (r > 1.0f ? (double)(len * (r - 1.0f)) : 0.0);
            const double start = v->mark - span * 0.5;
            if (start > limit) {
                if (voiced) v->mark -= ceil((start - limit) / T0) * T0;
                else v->mark = limit + span * 0.5;
            }
        }
        for (int k = 0; k < MG_MAX_GRAINS; k++) {
            mg_grain *g = &v->g[k];
            if (g->active) continue;
            g->start = v->mark - span * 0.5;
            g->rate = r;
            g->len = len;
            g->t = 0.0f;
            /* PitchMonster's level follows the ratio: -3 dB an octave down (grains as loud as at 0 st, fewer of them),
             * +1.5 dB an octave up (measured on a real vocal: -24 st -6.0, -12 -3.3, +12 +1.7, +24 +3.4) */
            g->gain = sqrtf(2.0f * T0 / len) * (pr > 1.0f ? 1.0f / sqrtf(sqrtf(pr)) : 1.0f);
            g->active = 1;
            break;
        }
        v->until_next += Tout;
    }
    float out = 0.0f;
    for (int k = 0; k < MG_MAX_GRAINS; k++) {
        mg_grain *g = &v->g[k];
        if (!g->active) continue;
        out += hann01(g->t / g->len) * g->gain * rd(m, g->start + (double)(g->t * g->rate));
        g->t += 1.0f;
        if (g->t >= g->len) g->active = 0;
    }
    return out;
}

static inline float svf_hp(float x, float g, float k, float *s)
{
    const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const float v3 = x - s[1];
    const float v1 = a1 * s[0] + a2 * v3, v2 = s[1] + a2 * s[0] + a3 * v3;
    s[0] = 2.0f * v1 - s[0]; s[1] = 2.0f * v2 - s[1];
    return x - k * v1 - v2;
}

static inline float svf_lp(float x, float g, float k, float *s)
{
    const float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    const float v3 = x - s[1];
    const float v1 = a1 * s[0] + a2 * v3, v2 = s[1] + a2 * s[0] + a3 * v3;
    s[0] = 2.0f * v1 - s[0]; s[1] = 2.0f * v2 - s[1];
    return v2;
}

void mg_tick(mg_t *m, float in, float t0, float *out_l, float *out_r)
{
    const mg_params *p = &m->p;
    if (!(in == in) || in > 4.0f || in < -4.0f) in = 0.0f;
    m->ring[(size_t)((long long)m->n & (MG_RING - 1))] = in;
    if (((long long)m->n & (MG_HOP - 1)) == 0)
        m->t0h[(size_t)(((long long)m->n / MG_HOP) & (MG_RING / MG_HOP - 1))] = t0 > 1.0f ? t0 : 0.0f;
    m->n += 1.0;

    /* each unison voice 1/sqrt(U) (PitchMonster's click copies: -3 / -6 / -9 dB at 2 / 4 / 8), each chord note 1/N^(1/4):
     * a chord gets louder by 1.5 dB per doubling of its notes (measured +1.5 / +3 / +4.5 at 2 / 4 / 8 notes) */
    const float goal = m->n_voices > 0 ? 1.0f / (sqrtf((float)m->n_unison) * sqrtf(sqrtf((float)m->n_notes))) : 0.0f;
    m->vgain += (goal - m->vgain) * m->k_amp;
    float wl = 0.0f, wr = 0.0f;
    for (int k = 0; k < MG_MAX_VOICES; k++) {
        mg_voice *v = &m->v[k];
        v->amp += (v->amp_goal - v->amp) * m->k_amp;
        if (v->amp < 1e-5f && v->amp_goal <= 0.0f) { v->amp = 0.0f; continue; }
        const float y = (p->engine == MG_GRANULAR ? granular(m, v) : psola(m, v, p->engine == MG_VOCODE)) * v->amp;
        wl += y * v->pl;
        wr += y * v->pr;
    }
    wl *= m->vgain;
    wr *= m->vgain;
    if (p->filter_on) {
        if (m->hp_g > 0.0f) { wl = svf_hp(wl, m->hp_g, m->hp_k, m->hs[0]); wr = svf_hp(wr, m->hp_g, m->hp_k, m->hs[1]); }
        if (m->lp_g > 0.0f) { wl = svf_lp(wl, m->lp_g, m->lp_k, m->ls[0]); wr = svf_lp(wr, m->lp_g, m->lp_k, m->ls[1]); }
        for (int c = 0; c < 2; c++) for (int j = 0; j < 2; j++) {
            if (fabsf(m->hs[c][j]) < 1e-20f) m->hs[c][j] = 0.0f;
            if (fabsf(m->ls[c][j]) < 1e-20f) m->ls[c][j] = 0.0f;
        }
    }
    /* the dry, lined up with the wet (voice 0's latency in the PSOLA cores) */
    const float lat_goal = p->engine == MG_GRANULAR ? MG_MIN_DELAY : (m->v[0].started ? m->v[0].lat : MG_MIN_DELAY);
    m->dry_lat += (lat_goal - m->dry_lat) * m->k_lat;
    const float dry = rd(m, m->n - 1.0 - (double)m->dry_lat);
    const float dg = fminf(1.0f, 2.0f * (1.0f - p->mix)), wg = fminf(1.0f, 2.0f * p->mix);
    const float vol = powf(10.0f, p->volume_db / 20.0f);
    *out_l = (dg * dry + wg * wl) * vol;
    *out_r = (dg * dry + wg * wr) * vol;
}

float mg_latency(const mg_t *m) { return m->dry_lat; }
int   mg_voices(const mg_t *m) { return m->n_voices; }

int mg_chords_parse(mg_t *m, const char *s)
{
    if (!s) return -1;
    int slot = 0, n = 0, notes[MG_MAX_NOTES];
    for (int i = 0; i < 8; i++) m->chord_n[i] = 0;
    const char *q = s;
    while (*q && slot < 8) {
        if (*q == ';') { mg_set_chord(m, slot, notes, n); slot++; n = 0; q++; continue; }
        if (*q == ',' || *q == ' ') { q++; continue; }
        char *end;
        const long v = strtol(q, &end, 10);
        if (end == q) return -1;
        if (n < MG_MAX_NOTES) notes[n++] = (int)v;
        q = end;
    }
    if (slot < 8) mg_set_chord(m, slot, notes, n);
    layout(m);
    return 0;
}

int mg_chords_format(const mg_t *m, char *buf, int len)
{
    int o = 0;
    if (len < 1) return -1;
    buf[0] = 0;
    for (int s = 0; s < 8; s++) {
        for (int i = 0; i < m->chord_n[s]; i++) {
            const int w = snprintf(buf + o, (size_t)(len - o), i ? ",%d" : "%d", m->chord[s][i]);
            if (w < 0 || w >= len - o) return -1;
            o += w;
        }
        if (s < 7) {
            if (o + 1 >= len) return -1;
            buf[o++] = ';'; buf[o] = 0;
        }
    }
    return o;
}
