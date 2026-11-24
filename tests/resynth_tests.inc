/* Resynth (src/dsp/hm.c), the third personality. Included by test_main.c after voice_tests.inc.
 * The sound itself is calibrated against Humanoid offline (calibration/): these check the module. */

#include <time.h>

/* a 6-harmonic tone at f Hz, amplitude a, as stereo blocks through the whole module */
static void rs_run(vt_t *vt, double f, float a, int frames, float *out_l)
{
    float blk[2 * 128];
    long ph = 0;                                    /* every call starts the tone at phase 0: two instances compare */
    for (int n = 0; n < frames; n += 128) {
        for (int i = 0; i < 128; i++) {
            const float v = a * tone(f, (int)(ph + i), 6, VT_SAMPLE_RATE);
            blk[2 * i] = blk[2 * i + 1] = v;
        }
        ph += 128;
        dsp_process_block_f(vt, blk, 128);
        if (out_l) for (int i = 0; i < 128 && n + i < frames; i++) out_l[n + i] = blk[2 * i];
    }
}

/* pitch of a steady signal: the autocorrelation peak (parabolic), 60..1000 Hz */
static double rs_pitch(const float *x, int n)
{
    int best = 0;
    double bv = 0.0, e0 = 0.0;
    const int lo = VT_SAMPLE_RATE / 1000, hi = VT_SAMPLE_RATE / 60;
    for (int i = 0; i < n - hi; i++) e0 += (double)x[i] * x[i];
    if (e0 <= 0.0) return 0.0;
    static double ac[VT_SAMPLE_RATE / 60 + 2];
    for (int t = lo - 1; t <= hi + 1; t++) {
        double s = 0.0;
        for (int i = 0; i < n - hi; i++) s += (double)x[i] * x[i + t];
        ac[t] = s / e0;
    }
    for (int t = lo; t <= hi; t++)
        if (ac[t] > ac[t - 1] && ac[t] >= ac[t + 1] && ac[t] > 0.9 * bv) { if (ac[t] > bv + 1e-9 || !best) { bv = ac[t]; best = t; } if (bv > 0.8) break; }
    if (!best) return 0.0;
    const double a = ac[best - 1], b = ac[best], c = ac[best + 1], den = a - 2 * b + c;
    const double lag = best + (fabs(den) > 1e-12 ? 0.5 * (a - c) / den : 0.0);
    return (double)VT_SAMPLE_RATE / lag;
}

static void test_resynth_tuning(void)
{
    /* Scale mode, C major (the default): a 215 Hz voice comes out on A3, 220 Hz; Note mode puts it on the Note */
    static float out[VT_SAMPLE_RATE];
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    CHECK(dsp_set_param(vt, "engine", "Resynth") == 0);
    rs_run(vt, 215.0, 0.2f, VT_SAMPLE_RATE, out);
    double f = rs_pitch(out + VT_SAMPLE_RATE / 2, 8192);
    if (fabs(1200.0 * log2(f / 220.0)) > 10.0) printf("  scale mode: %.2f Hz\n", f);
    CHECK(fabs(1200.0 * log2(f / 220.0)) < 10.0);
    CHECK(dsp_set_param(vt, "hm_tuning_mode", "Note") == 0 && dsp_set_param(vt, "hm_pitch_mono", "55") == 0);   /* G3 */
    rs_run(vt, 215.0, 0.2f, VT_SAMPLE_RATE, out);
    f = rs_pitch(out + VT_SAMPLE_RATE / 2, 8192);
    if (fabs(1200.0 * log2(f / 196.0)) > 10.0) printf("  note mode: %.2f Hz\n", f);
    CHECK(fabs(1200.0 * log2(f / 196.0)) < 10.0);
    /* Key D, Major: 215 Hz goes to A3 still (A is in D major); Key C#: G#3 (207.7 Hz) */
    dsp_set_param(vt, "hm_tuning_mode", "Scale");
    dsp_set_param(vt, "hm_key", "C#");
    rs_run(vt, 213.0, 0.2f, VT_SAMPLE_RATE, out);
    f = rs_pitch(out + VT_SAMPLE_RATE / 2, 8192);
    if (fabs(1200.0 * log2(f / 207.65)) > 10.0) printf("  C# major: %.2f Hz\n", f);
    CHECK(fabs(1200.0 * log2(f / 207.65)) < 10.0);
    dsp_cleanup(vt);
}

static void test_resynth_midi(void)
{
    /* MIDI mode: a held pad is the note; nothing held is silence */
    static float out[VT_SAMPLE_RATE];
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    dsp_set_param(vt, "engine", "Resynth");
    dsp_set_param(vt, "hm_tuning_mode", "MIDI");
    rs_run(vt, 200.0, 0.2f, VT_SAMPLE_RATE / 2, out);
    double e = 0;
    for (int i = VT_SAMPLE_RATE / 4; i < VT_SAMPLE_RATE / 2; i++) e += (double)out[i] * out[i];
    CHECK(e < 1e-6);
    const uint8_t on[3] = { 0x90, 64, 100 }, off[3] = { 0x80, 64, 0 };
    dsp_handle_midi(vt, on, 3);
    rs_run(vt, 200.0, 0.2f, VT_SAMPLE_RATE, out);
    const double f = rs_pitch(out + VT_SAMPLE_RATE / 2, 8192);
    if (fabs(1200.0 * log2(f / 329.63)) > 10.0) printf("  midi E4: %.2f Hz\n", f);
    CHECK(fabs(1200.0 * log2(f / 329.63)) < 10.0);
    dsp_handle_midi(vt, off, 3);
    dsp_cleanup(vt);
}

static const char *rs_square_b64(void)
{
    /* hm_wt: 512 int16 samples, base64 (a square) */
    static char b64[4 * ((2 * 512 + 2) / 3) + 1];
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char raw[1024];
    for (int i = 0; i < 512; i++) {
        const int16_t v = i < 256 ? 30000 : -30000;
        raw[2 * i] = (unsigned char)((uint16_t)v & 0xFF);
        raw[2 * i + 1] = (unsigned char)((uint16_t)v >> 8);
    }
    int o = 0;
    for (int i = 0; i < 1024; i += 3) {
        const unsigned a = raw[i], b = i + 1 < 1024 ? raw[i + 1] : 0, c = i + 2 < 1024 ? raw[i + 2] : 0;
        const unsigned w = (a << 16) | (b << 8) | c;
        b64[o++] = tab[(w >> 18) & 63];
        b64[o++] = tab[(w >> 12) & 63];
        b64[o++] = i + 1 < 1024 ? tab[(w >> 6) & 63] : '=';
        b64[o++] = i + 2 < 1024 ? tab[w & 63] : '=';
    }
    b64[o] = '\0';
    return b64;
}

static void test_resynth_state(void)
{
    /* a preset's wavetable travels in the state; a restored instance plays the same samples */
    static char st[8192];
    static float a_out[VT_SAMPLE_RATE / 2], b_out[VT_SAMPLE_RATE / 2];
    vt_t *a = dsp_init(VT_SAMPLE_RATE), *b = dsp_init(VT_SAMPLE_RATE);
    dsp_set_param(a, "engine", "Resynth");
    CHECK(dsp_set_param(a, "hm_wt", rs_square_b64()) == 0);
    CHECK(dsp_set_param(a, "hm_wt", "not base64!") == -1);           /* refused, the table stays */
    dsp_set_param(a, "hm_wave", "Preset");
    dsp_set_param(a, "hm_transform", "1");
    dsp_set_param(a, "hm_scale", "Custom");
    dsp_set_param(a, "hm_mask", "145");                              /* C E G */
    const int n = dsp_get_param(a, "state", st, sizeof st);
    CHECK(n > 0 && strstr(st, "\"hm_wt\":\"") != NULL && strstr(st, "\"engine\":\"Resynth\"") != NULL);
    CHECK(strstr(st, "\"hm_freeze\"") == NULL);                       /* Glitch is a performance toggle */
    CHECK(dsp_set_param(b, "state", st) == 0);
    rs_run(a, 215.0, 0.2f, VT_SAMPLE_RATE / 2, a_out);
    rs_run(b, 215.0, 0.2f, VT_SAMPLE_RATE / 2, b_out);
    int same = 1;
    for (int i = 0; i < VT_SAMPLE_RATE / 2; i++) same &= a_out[i] == b_out[i];
    CHECK(same);
    char w[2048];
    CHECK(dsp_get_param(b, "hm_wt", w, sizeof w) > 0 && strcmp(w, rs_square_b64()) == 0);
    dsp_cleanup(a); dsp_cleanup(b);
}

static void test_resynth_isolation(void)
{
    /* the Vocoder and Voice FX are bit-identical whatever the Resynth knobs say */
    static float x[VT_SAMPLE_RATE / 2], y[VT_SAMPLE_RATE / 2];
    for (int e = 0; e < 2; e++) {
        vt_t *a = dsp_init(VT_SAMPLE_RATE), *b = dsp_init(VT_SAMPLE_RATE);
        const char *eng = e ? "VoiceFX" : "Vocoder";
        dsp_set_param(a, "engine", eng); dsp_set_param(b, "engine", eng);
        dsp_set_param(a, "mode", "Snap"); dsp_set_param(b, "mode", "Snap");
        for (int id = VT_P_HM_FIRST; id <= VT_P_HM_WAVE; id++) {
            const vt_param_def *d = dsp_param_def(id);
            char v[32];
            if (d->type == VT_PTYPE_ENUM) snprintf(v, sizeof v, "%d", d->num_options - 1);
            else snprintf(v, sizeof v, "%g", (double)d->max);
            dsp_set_param(b, d->key, v);
        }
        rs_run(a, 215.0, 0.2f, VT_SAMPLE_RATE / 2, x);
        rs_run(b, 215.0, 0.2f, VT_SAMPLE_RATE / 2, y);
        int same = 1;
        for (int i = 0; i < VT_SAMPLE_RATE / 2; i++) same &= x[i] == y[i];
        CHECK(same);
        dsp_cleanup(a); dsp_cleanup(b);
    }
}

static void test_resynth_numeric_safety(void)
{
    /* every input kind x three extreme knob sets: finite, under the -6.1 dBFS ceiling, no subnormals after */
    static float blk[2 * 128];
    for (int set = 0; set < 3; set++) {
        for (int kind = 0; kind < 6; kind++) {
            vt_t *vt = dsp_init(VT_SAMPLE_RATE);
            dsp_set_param(vt, "engine", "Resynth");
            for (int id = VT_P_HM_FIRST; id <= VT_P_HM_WAVE; id++) {
                const vt_param_def *d = dsp_param_def(id);
                char v[32];
                if (d->transient) continue;
                if (d->type == VT_PTYPE_ENUM) snprintf(v, sizeof v, "%d", set == 0 ? 0 : set == 1 ? d->num_options - 1 : d->num_options / 2);
                else snprintf(v, sizeof v, "%g", (double)(set == 0 ? d->min : set == 1 ? d->max : 0.5f * (d->min + d->max)));
                dsp_set_param(vt, d->key, v);
            }
            if (set == 1) { const uint8_t on[3] = { 0x90, 60, 100 }; dsp_handle_midi(vt, on, 3); }
            float peak = 0.0f;
            int finite = 1, sub = 0;
            unsigned rng = 1u;
            for (int bn = 0; bn < 520; bn++) {
                for (int i = 0; i < 128; i++) {
                    const int n = bn * 128 + i;
                    float v;
                    rng = rng * 1664525u + 1013904223u;
                    const float nz = (float)(rng >> 8) / 8388608.0f - 1.0f;
                    switch (kind) {
                    case 0: v = 0.3f * tone(180.0, n, 6, VT_SAMPLE_RATE); break;
                    case 1: v = 0.9f * nz; break;
                    case 2: v = (n / 100) % 2 ? 1.0f : -1.0f; break;
                    case 3: v = 0.5f + 0.4f * tone(300.0, n, 1, VT_SAMPLE_RATE); break;
                    case 4: v = n % 4410 == 0 ? 1.0f : 0.0f; break;
                    default: v = 0.0f; break;
                    }
                    if (bn >= 360) v = 0.0f;                                  /* then 1.2 s of silence */
                    blk[2 * i] = blk[2 * i + 1] = v;
                }
                dsp_process_block_f(vt, blk, 128);
                for (int i = 0; i < 256; i++) {
                    if (!isfinite(blk[i])) finite = 0;
                    if (fabsf(blk[i]) > peak) peak = fabsf(blk[i]);
                    if (bn >= 360 && blk[i] != 0.0f && fabsf(blk[i]) < 1.17549435e-38f) sub = 1;
                }
            }
            if (!finite || peak > TINYVOICE_OUTPUT_HEADROOM + 1e-3f || sub) printf("  set %d kind %d: finite %d peak %.3f sub %d\n", set, kind, finite, (double)peak, sub);
            CHECK(finite && peak <= TINYVOICE_OUTPUT_HEADROOM + 1e-3f && !sub);
            dsp_cleanup(vt);
        }
    }
}

static void test_resynth_cpu(void)
{
    /* the heaviest settings (Duo, Note Shift, both Octave lines' worth, Transform, every effect) on a low voice */
    static float blk[2 * 128];
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    dsp_set_param(vt, "engine", "Resynth");
    dsp_set_param(vt, "hm_tuning_mode", "Note"); dsp_set_param(vt, "hm_tuning_duophonic", "On");
    dsp_set_param(vt, "hm_pitch_shift", "7"); dsp_set_param(vt, "hm_octaving", "1.5"); dsp_set_param(vt, "hm_transform", "0.6");
    dsp_set_param(vt, "hm_synth_mix", "0.5"); dsp_set_param(vt, "hm_wavetable_shape", "0.5"); dsp_set_param(vt, "hm_widen", "1");
    dsp_set_param(vt, "hm_deess", "12"); dsp_set_param(vt, "hm_lpf_freq", "110"); dsp_set_param(vt, "hm_hpf_freq", "40");
    dsp_set_param(vt, "hm_warble_depth", "0.5"); dsp_set_param(vt, "hm_lookahead", "Hi"); dsp_set_param(vt, "hm_min_f0", "40");
    const int nb = 2000;
    clock_t t0 = clock();
    for (int bn = 0; bn < nb; bn++) {
        for (int i = 0; i < 128; i++) blk[2 * i] = blk[2 * i + 1] = 0.3f * tone(90.0, bn * 128 + i, 6, VT_SAMPLE_RATE);
        dsp_process_block_f(vt, blk, 128);
    }
    const double us = 1e6 * (double)(clock() - t0) / CLOCKS_PER_SEC / nb;
    printf("  Resynth, heaviest settings: %.0f us per 128-frame block (native)\n", us);
    CHECK(us < 600.0);
    dsp_cleanup(vt);
}
