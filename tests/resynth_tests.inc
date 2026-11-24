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
    dsp_set_param(vt, "hm_midi_idle", "Silent");                  /* Humanoid's way: no pad, no sound (the default Tunes instead, below) */
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

/* The Move-side MIDI behaviour: pads folded to the voice, Latch, and a voice that keeps tuning with no pad held */
static void test_resynth_pads(void)
{
    static float out[VT_SAMPLE_RATE];
    double f;
    const uint8_t hi[3] = { 0x90, 88, 100 }, hi_off[3] = { 0x80, 88, 0 }, e4[3] = { 0x90, 64, 100 }, e4_off[3] = { 0x80, 64, 0 };

    /* octave fold: a pad at E6 (88) over a voice at G3 plays E4 (64), the octave within one of the voice, not E6 */
    {
        vt_t *vt = dsp_init(VT_SAMPLE_RATE);
        dsp_set_param(vt, "engine", "Resynth");
        dsp_set_param(vt, "hm_tuning_mode", "MIDI");
        dsp_handle_midi(vt, hi, 3);
        rs_run(vt, 200.0, 0.2f, VT_SAMPLE_RATE, out);
        f = rs_pitch(out + VT_SAMPLE_RATE / 2, 8192);
        if (fabs(1200.0 * log2(f / 329.63)) > 15.0) printf("  folded E6 -> %.2f Hz\n", f);
        CHECK(fabs(1200.0 * log2(f / 329.63)) < 15.0);
        dsp_handle_midi(vt, hi_off, 3);
        dsp_cleanup(vt);
    }
    /* Latch: a pad let go keeps playing; Latch Off ends it (and, with No Pad = Silent, the sound) */
    {
        vt_t *vt = dsp_init(VT_SAMPLE_RATE);
        dsp_set_param(vt, "engine", "Resynth");
        dsp_set_param(vt, "hm_tuning_mode", "MIDI");
        dsp_set_param(vt, "hm_midi_idle", "Silent");
        dsp_set_param(vt, "latch", "On");
        rs_run(vt, 200.0, 0.2f, 2048, out);
        dsp_handle_midi(vt, e4, 3);
        rs_run(vt, 200.0, 0.2f, 4096, out);
        dsp_handle_midi(vt, e4_off, 3);
        rs_run(vt, 200.0, 0.2f, VT_SAMPLE_RATE, out);
        f = rs_pitch(out + VT_SAMPLE_RATE / 2, 8192);
        if (fabs(1200.0 * log2(f / 329.63)) > 15.0) printf("  latched E4 -> %.2f Hz\n", f);
        CHECK(fabs(1200.0 * log2(f / 329.63)) < 15.0);
        dsp_set_param(vt, "latch", "Off");
        rs_run(vt, 200.0, 0.2f, VT_SAMPLE_RATE / 2, out);
        double e = 0;
        for (int i = VT_SAMPLE_RATE / 4; i < VT_SAMPLE_RATE / 2; i++) e += (double)out[i] * out[i];
        CHECK(e < 1e-6);
        dsp_cleanup(vt);
    }
    /* No Pad = Tune (the default): with nothing held MIDI mode keeps the voice on the scale: 213 Hz -> A3 in C major */
    {
        vt_t *vt = dsp_init(VT_SAMPLE_RATE);
        dsp_set_param(vt, "engine", "Resynth");
        dsp_set_param(vt, "hm_tuning_mode", "MIDI");
        dsp_set_param(vt, "hm_scale", "Major");
        dsp_set_param(vt, "hm_key", "C");
        rs_run(vt, 213.0, 0.2f, VT_SAMPLE_RATE, out);
        f = rs_pitch(out + VT_SAMPLE_RATE / 2, 8192);
        if (fabs(1200.0 * log2(f / 220.0)) > 15.0) printf("  no pad -> %.2f Hz\n", f);
        CHECK(fabs(1200.0 * log2(f / 220.0)) < 15.0);
        dsp_handle_midi(vt, e4, 3);                                /* a pad takes over from the scale */
        rs_run(vt, 213.0, 0.2f, VT_SAMPLE_RATE, out);
        f = rs_pitch(out + VT_SAMPLE_RATE / 2, 8192);
        CHECK(fabs(1200.0 * log2(f / 329.63)) < 15.0);
        dsp_handle_midi(vt, e4_off, 3);
        dsp_cleanup(vt);
    }
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

/* Resynth's preset bank (rsbank.c) and its browser (rs_preset / rs_preset_count / rs_preset_name) */
#include "rsbank.h"

static float rs_get(vt_t *vt, const char *key)
{
    char b[64];
    return dsp_get_param(vt, key, b, sizeof b) > 0 ? (float)atof(b) : -999.0f;
}

static void test_resynth_bank(void)
{
    char b[256];
    /* the loader: Resynth presets of this module only, a state stored as an object or as a string, sorted by name
     * case-insensitively, a missing name taken from the file, escapes decoded; Vocoder states, other modules,
     * broken files and non-.json files skipped */
    rsbank_t *bk = rsbank_load("tests/fixtures/rs_default", "tests/fixtures/rs_presets");
    CHECK(bk && bk->count[1] == 4 && bk->count[0] == 4);
    if (!bk || bk->count[1] != 4) { rsbank_free(bk); return; }
    CHECK(strcmp(bk->e[1][0].name, "alpha") == 0 && strcmp(bk->e[1][1].name, "Bravo") == 0);
    CHECK(strcmp(bk->e[1][2].name, "Caf\xc3\xa9 \"Q\"") == 0 && strcmp(bk->e[1][3].name, "g") == 0);
    CHECK(strstr(bk->e[1][0].state, "\"hm_robotify\":0.4") != NULL && bk->e[1][0].state[0] == '{');   /* unescaped */
    {
        rsbank_t *none = rsbank_load("tests/fixtures/no_such_folder", NULL);
        CHECK(none && none->count[0] == 0 && none->count[1] == 0);
        rsbank_free(none);
    }

    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    /* before the bank arrives: nothing to choose, and choosing is harmless */
    CHECK(dsp_get_param(vt, "rs_preset_count", b, sizeof b) > 0 && strcmp(b, "0") == 0);
    CHECK(dsp_get_param(vt, "rs_preset_name", b, sizeof b) > 0 && strcmp(b, "(loading)") == 0);
    CHECK(dsp_set_param(vt, "rs_preset", "2") == 0 && (int)rs_get(vt, "rs_preset") == 0);
    CHECK(dsp_set_rs_bank(vt, bk) == 0);
    /* two banks: Default (shipped) is listed first, sorted; Imported is the user's folder */
    CHECK(dsp_get_param(vt, "rs_bank", b, sizeof b) > 0 && strcmp(b, "Default") == 0);
    CHECK(dsp_get_param(vt, "rs_preset_count", b, sizeof b) > 0 && strcmp(b, "4") == 0);
    CHECK(dsp_get_param(vt, "rs_preset_name", b, sizeof b) > 0 && strcmp(b, "Alpha shipped") == 0);
    CHECK(dsp_set_param(vt, "rs_preset", "3") == 0 && dsp_get_param(vt, "rs_preset_name", b, sizeof b) > 0 && strcmp(b, "Zed first") == 0);
    CHECK(fabsf(rs_get(vt, "hm_robotify") - 0.9f) < 1e-6f);                                     /* a Default preset loads like any */
    CHECK(dsp_set_param(vt, "rs_bank", "Imported") == 0 && (int)rs_get(vt, "rs_preset") == 0);   /* a new list starts at its first */
    CHECK(dsp_get_param(vt, "rs_bank", b, sizeof b) > 0 && strcmp(b, "Imported") == 0);
    {
        rsbank_t *again = rsbank_load("tests/fixtures/rs_default", "tests/fixtures/rs_presets");
        CHECK(dsp_set_rs_bank(vt, again) == -1);           /* one handover; the caller keeps the second */
        rsbank_free(again);
    }
    CHECK(dsp_get_param(vt, "rs_preset_count", b, sizeof b) > 0 && strcmp(b, "4") == 0);

    /* choosing loads it over Resynth's defaults: nothing of the last preset or of a hand on a knob carries over */
    dsp_set_param(vt, "hm_scale_hold", "Off");              /* (Hold Scale is tested below: here the preset's own notes apply) */
    dsp_set_param(vt, "engine", "Vocoder");
    dsp_set_param(vt, "hm_transform", "0.9");
    dsp_set_param(vt, "hm_dry_wet", "0.3");
    CHECK(dsp_set_param(vt, "rs_preset", "0") == 0);
    CHECK(dsp_get_param(vt, "engine", b, sizeof b) > 0 && strcmp(b, "Resynth") == 0);
    CHECK(fabsf(rs_get(vt, "hm_robotify") - 0.4f) < 1e-6f);
    CHECK(fabsf(rs_get(vt, "hm_transform") - dsp_param_def(dsp_param_index("hm_transform"))->def) < 1e-6f);
    CHECK(fabsf(rs_get(vt, "hm_dry_wet") - dsp_param_def(dsp_param_index("hm_dry_wet"))->def) < 1e-6f);
    CHECK(dsp_get_param(vt, "rs_preset_name", b, sizeof b) > 0 && strcmp(b, "alpha") == 0);
    CHECK(dsp_set_param(vt, "rs_preset", "1") == 0);
    CHECK(fabsf(rs_get(vt, "hm_transform") - 0.7f) < 1e-6f && (int)rs_get(vt, "hm_mask") == 1);
    CHECK(fabsf(rs_get(vt, "hm_robotify") - dsp_param_def(dsp_param_index("hm_robotify"))->def) < 1e-6f);
    CHECK(dsp_get_param(vt, "hm_scale", b, sizeof b) > 0 && strcmp(b, "Custom") == 0);
    CHECK(dsp_get_param(vt, "rs_preset_name", b, sizeof b) > 0 && strcmp(b, "Bravo") == 0);
    CHECK(dsp_set_param(vt, "rs_preset", "99") == 0 && (int)rs_get(vt, "rs_preset") == 3);      /* clamps to the last */
    CHECK(dsp_get_param(vt, "rs_preset_name", b, sizeof b) > 0 && strcmp(b, "g") == 0);
    CHECK(fabsf(rs_get(vt, "hm_octaving") - 1.0f) < 1e-6f && fabsf(rs_get(vt, "hm_transform") - 0.0f) < 1.0f);
    CHECK(dsp_set_param(vt, "rs_preset", "-3") == 0 && (int)rs_get(vt, "rs_preset") == 0);
    /* the index is not saved: a state holds the knobs, not which preset they came from */
    {
        static char st[8192];
        CHECK(dsp_get_param(vt, "state", st, sizeof st) > 0 && strstr(st, "rs_preset") == NULL);
    }
    /* and it plays */
    {
        static float out[VT_SAMPLE_RATE / 2];
        int finite = 1;
        rs_run(vt, 220.0, 0.2f, VT_SAMPLE_RATE / 2, out);
        for (int i = 0; i < VT_SAMPLE_RATE / 2; i++) finite &= isfinite(out[i]) && fabsf(out[i]) <= 1.0f;
        CHECK(finite);
    }
    dsp_cleanup(vt);                                       /* frees the bank */

    /* one file's text, directly */
    {
        char nm[8], *st = NULL;
        CHECK(rsbank_parse("{\"name\":\"a long name\",\"state\":{\"engine\":\"Resynth\"}}", nm, sizeof nm, &st) == 0);
        CHECK(st && strcmp(nm, "a long ") == 0);           /* cut to fit */
        free(st);
        CHECK(rsbank_parse("{\"state\":{\"engine\":\"VoiceFX\"}}", nm, sizeof nm, &st) == -1 && st == NULL);
        CHECK(rsbank_parse("[1,2]", nm, sizeof nm, &st) == -1);
        CHECK(rsbank_parse("{\"state\":\"{\\\"engine\\\":\\\"Resynth\\\"}\",\"name\":\"s\"}", nm, sizeof nm, &st) == 0);
        CHECK(st && strcmp(st, "{\"engine\":\"Resynth\"}") == 0 && strcmp(nm, "s") == 0);
        free(st);
    }
}

/* Hold Scale: Key, Scale and Notes set by hand survive a preset (the Presets page, My Presets), so many
 * presets can be tried over one song; Off lets a preset bring its own */
static void test_resynth_hold_scale(void)
{
    char b[128];
    static char st[8192];
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    rsbank_t *bk = rsbank_load("tests/fixtures/rs_default", "tests/fixtures/rs_presets");     /* 1 = Bravo: Custom scale, notes mask 1; 0 = alpha: no scale */
    CHECK(bk && bk->count[1] == 4 && bk->count[0] == 4 && dsp_set_rs_bank(vt, bk) == 0);
    dsp_set_param(vt, "rs_bank", "Imported");
    CHECK(dsp_get_param(vt, "hm_scale_hold", b, sizeof b) > 0 && strcmp(b, "On") == 0);     /* on by default */
    dsp_set_param(vt, "hm_key", "A");
    dsp_set_param(vt, "hm_scale", "Minor");
    CHECK(dsp_set_param(vt, "rs_preset", "1") == 0);
    CHECK(fabsf(rs_get(vt, "hm_transform") - 0.7f) < 1e-6f);                                 /* the preset itself loaded... */
    CHECK(dsp_get_param(vt, "hm_key", b, sizeof b) > 0 && strcmp(b, "A") == 0);              /* ...Key, Scale and Notes did not */
    CHECK(dsp_get_param(vt, "hm_scale", b, sizeof b) > 0 && strcmp(b, "Minor") == 0);
    CHECK((int)rs_get(vt, "hm_mask") == 2741);
    CHECK(dsp_set_param(vt, "rs_preset", "0") == 0 && dsp_get_param(vt, "hm_scale", b, sizeof b) > 0 && strcmp(b, "Minor") == 0);
    /* a hand on them still works, and is what the next preset keeps */
    dsp_set_param(vt, "hm_key", "D");
    dsp_set_param(vt, "rs_preset", "2");
    CHECK(dsp_get_param(vt, "hm_key", b, sizeof b) > 0 && strcmp(b, "D") == 0);
    /* My Presets recalls a state through set_param("state"): after load it keeps them too */
    {
        static float sink[VT_SAMPLE_RATE + 4096];
        rs_run(vt, 220.0, 0.2f, VT_SAMPLE_RATE + 4096, sink);                                /* the module has been running */
        CHECK(dsp_set_param(vt, "state", "{\"engine\":\"Resynth\",\"hm_key\":\"F#\",\"hm_scale\":\"Custom\",\"hm_mask\":5,\"hm_robotify\":0.9}") == 0);
        CHECK(fabsf(rs_get(vt, "hm_robotify") - 0.9f) < 1e-6f);
        CHECK(dsp_get_param(vt, "hm_key", b, sizeof b) > 0 && strcmp(b, "D") == 0);
        CHECK(dsp_get_param(vt, "hm_scale", b, sizeof b) > 0 && strcmp(b, "Minor") == 0);
        /* a state that says Hold Scale Off applies in full, and turns it off */
        CHECK(dsp_set_param(vt, "state", "{\"hm_key\":\"F#\",\"hm_scale\":\"Custom\",\"hm_mask\":5,\"hm_scale_hold\":\"Off\"}") == 0);
        CHECK(dsp_get_param(vt, "hm_key", b, sizeof b) > 0 && strcmp(b, "F#") == 0 && (int)rs_get(vt, "hm_mask") == 5);
        CHECK(dsp_get_param(vt, "hm_scale_hold", b, sizeof b) > 0 && strcmp(b, "Off") == 0);
    }
    /* Off: the preset's own notes apply */
    CHECK(dsp_set_param(vt, "rs_preset", "1") == 0);
    CHECK(dsp_get_param(vt, "hm_scale", b, sizeof b) > 0 && strcmp(b, "Custom") == 0 && (int)rs_get(vt, "hm_mask") == 1);
    /* the restore at load is not a recall: a fresh instance takes the state in full, Hold On or not, and it saves */
    dsp_set_param(vt, "hm_scale_hold", "On");
    CHECK(dsp_get_param(vt, "state", st, sizeof st) > 0 && strstr(st, "\"hm_scale_hold\":\"On\"") != NULL);
    {
        vt_t *fresh = dsp_init(VT_SAMPLE_RATE);
        CHECK(dsp_set_param(fresh, "state", st) == 0);
        CHECK(dsp_get_param(fresh, "hm_scale", b, sizeof b) > 0 && strcmp(b, "Custom") == 0 && (int)rs_get(fresh, "hm_mask") == 1);
        dsp_cleanup(fresh);
    }
    dsp_cleanup(vt);
}

/* Resynth's Demo: five Default presets in turn (here the two the fixtures have), the Presets page following,
 * the player's own sound back at the end, and a hand on a knob pinning the tour */
static void test_resynth_demo(void)
{
    char b[64];
    static float sink[VT_SAMPLE_RATE];
    for (int pin = 0; pin < 3; pin++) {
        vt_t *vt = dsp_init(VT_SAMPLE_RATE);
        rsbank_t *bk = rsbank_load("tests/fixtures/rs_default", "tests/fixtures/rs_presets");
        CHECK(bk && dsp_set_rs_bank(vt, bk) == 0);
        dsp_set_bpm(vt, 400.0f);                                 /* a bar is 0.6 s: four bars 2.4 s */
        dsp_set_param(vt, "engine", "Resynth");
        dsp_set_param(vt, "hm_robotify", "0.123");
        dsp_set_param(vt, "hm_transform", "0.77");
        dsp_set_param(vt, "demo", "On");
        rs_run(vt, 220.0, 0.2f, VT_SAMPLE_RATE / 2, sink);       /* step 0: "Tune - Hard Snap" */
        CHECK(fabsf(rs_get(vt, "hm_robotify") - 0.35f) < 1e-6f && fabsf(rs_get(vt, "hm_transform") - 0.0f) < 1e-6f);
        CHECK(dsp_get_param(vt, "rs_preset_name", b, sizeof b) > 0 && strcmp(b, "Tune - Hard Snap") == 0);   /* the Presets page follows */
        if (pin == 1) { dsp_set_param(vt, "hm_robotify", "0.5"); }                       /* a hand on a knob */
        if (pin == 2) { dsp_set_param(vt, "rs_preset", "0"); }                           /* or on the Presets page */
        for (int k = 0; k < 11; k++) rs_run(vt, 220.0, 0.2f, VT_SAMPLE_RATE, sink);      /* past step 4: 4 * 2.4 s = 9.6 s */
        if (pin == 0) CHECK(fabsf(rs_get(vt, "hm_robotify") - 0.8f) < 1e-6f);            /* step 4: "Fx - Dark Choir" */
        else if (pin == 1) CHECK(fabsf(rs_get(vt, "hm_robotify") - 0.5f) < 1e-6f);       /* pinned on the knob's value: no step */
        else CHECK(fabsf(rs_get(vt, "hm_transform") - 0.2f) < 1e-6f);                    /* pinned on "Alpha shipped": no step */
        dsp_set_param(vt, "demo", "Off");
        rs_run(vt, 220.0, 0.2f, 1024, sink);
        if (pin == 0) {                                          /* the player's own sound is back */
            CHECK(fabsf(rs_get(vt, "hm_robotify") - 0.123f) < 1e-6f && fabsf(rs_get(vt, "hm_transform") - 0.77f) < 1e-6f);
            CHECK(dsp_get_param(vt, "hm_wave", b, sizeof b) > 0 && strcmp(b, "Saw") == 0);
        } else {                                                 /* pinned: what the player stopped on stays (not their old sound) */
            CHECK(fabsf(rs_get(vt, "hm_transform") - 0.77f) > 1e-6f);
            if (pin == 1) CHECK(fabsf(rs_get(vt, "hm_robotify") - 0.5f) < 1e-6f);
        }
        dsp_cleanup(vt);
    }
}

/* The Demo's recorded phrase (demo.c demo_pcm_*): the real clip loads, plays looped and interpolated, and the
 * Demo uses it in place of the synthetic voice; a missing or unusable file is simply no recording */
#include "demo.h"
static void test_demo_recording(void)
{
    CHECK(demo_pcm_load("tests/fixtures/no_such_file.raw") == NULL);
    CHECK(demo_pcm_load("tests/fixtures/rs_presets/e.txt") == NULL);                    /* too short to be a clip */
    demo_pcm_t *d = demo_pcm_load("src/demo/demo_voice.raw");
    CHECK(d != NULL);
    if (!d) return;
    CHECK(d->n > 44100 * 6 && d->n < 44100 * 9);
    double pk = 0.0, ma = 0.0;
    int act = 0;
    for (int i = 0; i < d->n; i++) { const double v = fabs(d->pcm[i] / 32768.0); if (v > pk) pk = v; }
    for (int i = 0; i < d->n; i++) { const double v = fabs(d->pcm[i] / 32768.0); if (v > 0.05 * pk) { ma += v; act++; } }
    CHECK(pk > 0.3 && pk < 0.96);
    CHECK(act > 44100 * 4 && fabs(ma / act - 0.0902) < 0.01);                             /* at the demo's level */
    double pos = 0.0;
    float first = demo_pcm_next(d, &pos, 1.0);
    CHECK(fabsf(first - d->pcm[0] / 32768.0f) < 1e-6f && fabs(pos - 1.0) < 1e-12);
    pos = (double)d->n - 0.5;                                                              /* the loop point: interpolates into the start */
    const float w = demo_pcm_next(d, &pos, 1.0);
    CHECK(fabsf(w - 0.5f * (d->pcm[d->n - 1] + d->pcm[0]) / 32768.0f) < 1e-5f);
    CHECK(pos > (double)d->n - 0.5 + 0.99 && pos < (double)d->n + 0.6);
    pos = 1e9; (void)demo_pcm_next(d, &pos, 1.0);                                          /* any position is safe */
    CHECK(pos >= 0.0);

    /* through the module: Demo On plays it (not the synthetic phrase) and the output is sound */
    static float a[VT_SAMPLE_RATE], b[VT_SAMPLE_RATE];
    vt_t *with = dsp_init(VT_SAMPLE_RATE), *without = dsp_init(VT_SAMPLE_RATE);
    CHECK(dsp_set_demo_pcm(with, d) == 0);
    for (int k = 0; k < 2; k++) { vt_t *v = k ? without : with; dsp_set_param(v, "mode", "Snap"); dsp_set_param(v, "demo", "On"); }
    float blk[256];
    double ea = 0.0, diff = 0.0;
    for (int n = 0; n < VT_SAMPLE_RATE; n += 128) {
        for (int k = 0; k < 2; k++) {
            for (int i = 0; i < 256; i++) blk[i] = 0.0f;
            dsp_process_block_f(k ? without : with, blk, 128);
            for (int i = 0; i < 128; i++) (k ? b : a)[n + i] = blk[2 * i];
        }
    }
    for (int i = 0; i < VT_SAMPLE_RATE; i++) { ea += (double)a[i] * a[i]; diff += fabs(a[i] - b[i]); }
    CHECK(ea > 1e-3 && diff > 1.0);
    {
        demo_pcm_t *again = demo_pcm_load("src/demo/demo_voice.raw");
        CHECK(again && dsp_set_demo_pcm(with, again) == -1);                               /* one handover */
        demo_pcm_free(again);
    }
    dsp_cleanup(with);                                                                     /* frees the clip */
    dsp_cleanup(without);
}
