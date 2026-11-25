/* Mangler (src/dsp/mangle.c), the fourth personality. Included by test_main.c after resynth_tests.inc.
 * The sound is calibrated against PitchMonster offline (in TinyVoice-lab, outside this repo): these check the module. */

#include "mangle.h"

/* a harmonic tone at f Hz through the whole module, mono in, left / right out */
static void mg_run(vt_t *vt, double f, float a, int frames, float *out_l, float *out_r)
{
    float blk[2 * 128];
    static long ph = 0;
    for (int n = 0; n < frames; n += 128) {
        for (int i = 0; i < 128; i++) blk[2 * i] = blk[2 * i + 1] = a * tone(f, (int)(ph + i), 6, VT_SAMPLE_RATE);
        ph += 128;
        dsp_process_block_f(vt, blk, 128);
        for (int i = 0; i < 128 && n + i < frames; i++) {
            if (out_l) out_l[n + i] = blk[2 * i];
            if (out_r) out_r[n + i] = blk[2 * i + 1];
        }
    }
}

static void test_mangler_pitch(void)
{
    /* a 220 Hz voice: Granular +12 -> 440; Formant +7 -> 329.6; Vocode, Pitch 0 -> C4 (261.6), whatever is sung */
    static float out[VT_SAMPLE_RATE * 2];
    static const struct { const char *shift; const char *pitch; double want; } c[] = {
        { "Granular", "12", 440.0 }, { "Formant", "7", 329.63 }, { "Vocode", "0", 261.63 }, { "Vocode", "7", 392.0 },
    };
    for (size_t k = 0; k < sizeof c / sizeof c[0]; k++) {
        vt_t *vt = dsp_init(VT_SAMPLE_RATE);
        CHECK(dsp_set_param(vt, "engine", "Mangler") == 0);
        CHECK(dsp_set_param(vt, "mg_shift", c[k].shift) == 0 && dsp_set_param(vt, "mg_pitch", c[k].pitch) == 0);
        mg_run(vt, 220.0, 0.2f, VT_SAMPLE_RATE * 2, out, NULL);
        const double f = rs_pitch(out + VT_SAMPLE_RATE, 8192);
        const double ct = f > 0.0 ? 1200.0 * log2(f / c[k].want) : 9999.0;
        if (fabs(ct) > 30.0) printf("  %s %s: %.2f Hz (want %.2f)\n", c[k].shift, c[k].pitch, f, c[k].want);
        CHECK(fabs(ct) < 30.0);
        dsp_cleanup(vt);
    }
}

static void test_mangler_notes(void)
{
    /* Notes = Chord with the default slot 1 (C E G, offsets 0 4 7 from C4): a 220 Hz voice sounds 220, 277.2 and 329.6;
     * Pads with no pad is silent, with one pad held (G5, folded to G4: +7) it sounds 329.6 and not 220 */
    static float out[VT_SAMPLE_RATE * 2];
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    dsp_set_param(vt, "engine", "Mangler");
    dsp_set_param(vt, "mg_notes", "Chord");
    mg_run(vt, 220.0, 0.2f, VT_SAMPLE_RATE * 2, out, NULL);
    const double base = band_at(out + VT_SAMPLE_RATE, 16384, 250.0);                    /* between the notes */
    const double a = band_at(out + VT_SAMPLE_RATE, 16384, 220.0), e = band_at(out + VT_SAMPLE_RATE, 16384, 277.18),
                 g = band_at(out + VT_SAMPLE_RATE, 16384, 329.63);
    CHECK(a > 5.0 * base && e > 5.0 * base && g > 5.0 * base);
    CHECK(dsp_set_param(vt, "mg_chord", "2") == 0);                                      /* another slot: no hang, no click */
    mg_run(vt, 220.0, 0.2f, VT_SAMPLE_RATE, out, NULL);
    CHECK(rms_of(out + VT_SAMPLE_RATE / 2, VT_SAMPLE_RATE / 4) > 0.01);
    dsp_cleanup(vt);

    vt = dsp_init(VT_SAMPLE_RATE);
    dsp_set_param(vt, "engine", "Mangler");
    dsp_set_param(vt, "mg_notes", "Pads");
    mg_run(vt, 220.0, 0.2f, VT_SAMPLE_RATE, out, NULL);
    CHECK(rms_of(out + VT_SAMPLE_RATE / 2, VT_SAMPLE_RATE / 4) < 0.002);
    const uint8_t on[3] = { 0x90, 79, 100 };
    dsp_handle_midi(vt, on, 3);
    mg_run(vt, 220.0, 0.2f, VT_SAMPLE_RATE * 2, out, NULL);
    const double b = band_at(out + VT_SAMPLE_RATE, 16384, 250.0);
    CHECK(rms_of(out + VT_SAMPLE_RATE, 8192) > 0.02);
    CHECK(band_at(out + VT_SAMPLE_RATE, 16384, 329.63) > 5.0 * b);
    CHECK(band_at(out + VT_SAMPLE_RATE, 16384, 220.0) < 0.2 * band_at(out + VT_SAMPLE_RATE, 16384, 329.63));
    dsp_cleanup(vt);
}

static void test_mangler_state(void)
{
    /* every Mangler param and the chord slots round-trip through "state"; limits clamp, junk is refused */
    static char st[16384], st2[16384];
    vt_t *a = dsp_init(VT_SAMPLE_RATE), *b = dsp_init(VT_SAMPLE_RATE);
    dsp_set_param(a, "engine", "Mangler");
    for (int id = VT_P_MG_FIRST; id <= VT_P_MG_LAST; id++) {
        const vt_param_def *d = dsp_param_def(id);
        char v[32];
        if (d->type == VT_PTYPE_ENUM) snprintf(v, sizeof v, "%s", d->options[((int)d->def + 1) % d->num_options]);
        else snprintf(v, sizeof v, "%g", (double)(d->min + 0.375f * (d->max - d->min)));
        CHECK(dsp_set_param(a, d->key, v) == 0);
    }
    CHECK(dsp_set_param(a, "mg_chords", "48,52,55;;50;;;;;72,76,79,83,86,88,91,93") == 0);
    CHECK(dsp_get_param(a, "state", st, sizeof st) > 0);
    CHECK(strstr(st, "\"mg_chords\":\"48,52,55;;50;;;;;72,76,79,83,86,88,91,93\"") != NULL);
    CHECK(dsp_set_param(b, "state", st) == 0);
    CHECK(dsp_get_param(b, "state", st2, sizeof st2) > 0 && strcmp(st, st2) == 0);
    for (int id = VT_P_MG_FIRST; id <= VT_P_MG_LAST; id++) {
        char x[32], y[32];
        dsp_get_param(a, dsp_param_def(id)->key, x, sizeof x);
        dsp_get_param(b, dsp_param_def(id)->key, y, sizeof y);
        CHECK(strcmp(x, y) == 0);
    }
    char v[32];
    CHECK(dsp_set_param(a, "mg_pitch", "-999") == 0);
    dsp_get_param(a, "mg_pitch", v, sizeof v);
    CHECK(atof(v) == -48.0);
    CHECK(dsp_set_param(a, "mg_voices", "99") == 0);
    dsp_get_param(a, "mg_voices", v, sizeof v);
    CHECK(atof(v) == 8.0);
    CHECK(dsp_set_param(a, "mg_shift", "Nonsense") == -1);
    (void)dsp_set_param(a, "mg_chords", "x,y;;;");                                       /* junk: no crash */
    dsp_cleanup(a);
    dsp_cleanup(b);
}

static void test_mangler_isolation(void)
{
    /* Mangler's controls do not move the other engines', and the other way round */
    static char after[16384];
    static float out[8192];
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    dsp_set_param(vt, "hm_transform", "0.6");
    dsp_set_param(vt, "mode", "Robot");
    dsp_set_param(vt, "engine", "Mangler");
    dsp_set_param(vt, "mg_pitch", "5");
    dsp_set_param(vt, "mg_voices", "4");
    dsp_set_param(vt, "engine", "Vocoder");
    dsp_get_param(vt, "state", after, sizeof after);
    CHECK(strstr(after, "\"hm_transform\":0.6") != NULL && strstr(after, "\"mode\":\"Robot\"") != NULL);
    CHECK(strstr(after, "\"mg_pitch\":5") != NULL && strstr(after, "\"mg_voices\":4") != NULL);
    mg_run(vt, 220.0, 0.2f, 8192, out, NULL);                                            /* the Vocoder still runs on its own */
    for (int i = 0; i < 8192; i++) CHECK(isfinite(out[i]));
    dsp_cleanup(vt);
}

static void test_mangler_numeric_safety(void)
{
    /* the extremes of every control, every shifter, a hot input and silence: finite, and under the -6.1 dBFS ceiling */
    static float out[4096], outr[4096];
    static const char *const shifts[3] = { "Granular", "Formant", "Vocode" };
    for (int s = 0; s < 3; s++) {
        for (int ext = 0; ext < 2; ext++) {
            vt_t *vt = dsp_init(VT_SAMPLE_RATE);
            dsp_set_param(vt, "engine", "Mangler");
            dsp_set_param(vt, "mg_shift", shifts[s]);
            dsp_set_param(vt, "mg_pitch", ext ? "-48" : "24");
            dsp_set_param(vt, "mg_formant", ext ? "24" : "-24");
            dsp_set_param(vt, "mg_voices", "8"); dsp_set_param(vt, "mg_detune", "12"); dsp_set_param(vt, "mg_time", "1000");
            dsp_set_param(vt, "mg_grain", ext ? "1" : "100"); dsp_set_param(vt, "mg_scatter", "Jitter"); dsp_set_param(vt, "mg_smear", "Jitter");
            dsp_set_param(vt, "mg_wide", "On"); dsp_set_param(vt, "mg_filter", "On"); dsp_set_param(vt, "mg_freq", ext ? "20" : "20000");
            dsp_set_param(vt, "mg_width", "0.1"); dsp_set_param(vt, "mg_volume", "12"); dsp_set_param(vt, "mg_notes", "Chord");
            dsp_set_param(vt, "mg_chords", "36,40,43,47,50,53,57,60;;;;;;;");
            for (int rep = 0; rep < 3; rep++) {
                mg_run(vt, 130.0 + 90.0 * rep, 0.9f, 4096, out, outr);                   /* hot */
                for (int i = 0; i < 4096; i++) {
                    CHECK(isfinite(out[i]) && isfinite(outr[i]));
                    CHECK(fabsf(out[i]) <= TINYVOICE_OUTPUT_HEADROOM + 1e-4f && fabsf(outr[i]) <= TINYVOICE_OUTPUT_HEADROOM + 1e-4f);
                }
            }
            float z[2 * 128] = {0};
            for (int n = 0; n < 40; n++) { dsp_process_block_f(vt, z, 128); for (int i = 0; i < 256; i++) CHECK(isfinite(z[i])); }
            dsp_cleanup(vt);
        }
    }
}

static void test_mangler_cpu(void)
{
    /* the heaviest setting: 8 unison x a 2-note chord (16 voices), Formant core, Jitter, Wide, Filter */
    static float blk[2 * 128];
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    dsp_set_param(vt, "engine", "Mangler"); dsp_set_param(vt, "mg_shift", "Formant"); dsp_set_param(vt, "mg_voices", "8");
    dsp_set_param(vt, "mg_detune", "6"); dsp_set_param(vt, "mg_time", "300"); dsp_set_param(vt, "mg_scatter", "Jitter");
    dsp_set_param(vt, "mg_smear", "Jitter"); dsp_set_param(vt, "mg_wide", "On"); dsp_set_param(vt, "mg_filter", "On");
    dsp_set_param(vt, "mg_notes", "Chord"); dsp_set_param(vt, "mg_chords", "60,67;;;;;;;");
    const int nb = 2000;
    clock_t t0 = clock();
    for (int bn = 0; bn < nb; bn++) {
        for (int i = 0; i < 128; i++) blk[2 * i] = blk[2 * i + 1] = 0.3f * tone(150.0, bn * 128 + i, 6, VT_SAMPLE_RATE);
        dsp_process_block_f(vt, blk, 128);
    }
    const double us = 1e6 * (double)(clock() - t0) / CLOCKS_PER_SEC / nb;
    printf("  Mangler, 16 voices Formant: %.0f us per 128-frame block (native)\n", us);
    CHECK(us < 600.0);
    dsp_cleanup(vt);
}

static void test_mangler_bank(void)
{
    /* the Mangler bank holds the presets that run Mangler (the Resynth ones are left out), the Presets page lists it while
     * the engine is Mangler, and choosing one loads it over Mangler's defaults, chord slots included */
    char b[256];
    rsbank_t *mb = rsbank_load_engine("mangler", "tests/fixtures/mg_default", "tests/fixtures/rs_presets");
    CHECK(mb && mb->count[0] == 2 && mb->count[1] == 0);
    if (!mb || mb->count[0] != 2) { rsbank_free(mb); return; }
    CHECK(strcmp(mb->e[0][0].name, "Alpha Mangler") == 0 && strcmp(mb->e[0][1].name, "beta Mangler") == 0);
    rsbank_t *rb = rsbank_load("tests/fixtures/mg_default", "tests/fixtures/rs_presets");
    CHECK(rb && rb->count[0] == 1 && strcmp(rb->e[0][0].name, "A Resynth one") == 0);
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    CHECK(dsp_set_rs_bank(vt, rb) == 0 && dsp_set_mg_bank(vt, mb) == 0);
    {
        rsbank_t *again = rsbank_load_engine("mangler", "tests/fixtures/mg_default", NULL);
        CHECK(dsp_set_mg_bank(vt, again) == -1);                                         /* one handover; the caller keeps the second */
        rsbank_free(again);
    }
    dsp_set_param(vt, "engine", "Mangler");
    CHECK(dsp_get_param(vt, "rs_preset_count", b, sizeof b) > 0 && strcmp(b, "2") == 0);
    CHECK(dsp_set_param(vt, "rs_preset", "0") == 0);
    CHECK(dsp_get_param(vt, "mg_shift", b, sizeof b) > 0 && strcmp(b, "Formant") == 0);
    CHECK(dsp_get_param(vt, "mg_notes", b, sizeof b) > 0 && strcmp(b, "Chord") == 0);
    CHECK(dsp_get_param(vt, "mg_chord", b, sizeof b) > 0 && strcmp(b, "2") == 0);
    CHECK(dsp_get_param(vt, "rs_preset_name", b, sizeof b) > 0 && strcmp(b, "Alpha Mangler") == 0);
    CHECK(dsp_get_param(vt, "preset_name", b, sizeof b) > 0 && strcmp(b, "Alpha Mangler") == 0);
    CHECK(dsp_get_param(vt, "mg_chords", b, sizeof b) > 0 && strncmp(b, "60,64,67;62,65,69;;", 19) == 0);
    CHECK(dsp_set_param(vt, "rs_preset", "1") == 0);                                     /* nothing of the last preset stays */
    CHECK(dsp_get_param(vt, "mg_shift", b, sizeof b) > 0 && strcmp(b, "Vocode") == 0);
    CHECK(dsp_get_param(vt, "mg_notes", b, sizeof b) > 0 && strcmp(b, "Normal") == 0);
    CHECK(dsp_get_param(vt, "mg_voices", b, sizeof b) > 0 && atoi(b) == 1);
    CHECK(dsp_get_param(vt, "mg_chords", b, sizeof b) > 0 && strncmp(b, "60,64,67;62,65,69;64,67,71;", 27) == 0);   /* the plain chords are back */
    dsp_set_param(vt, "engine", "Resynth");                                              /* the other engine lists its own bank */
    CHECK(dsp_get_param(vt, "rs_preset_count", b, sizeof b) > 0 && strcmp(b, "1") == 0);
    CHECK(dsp_get_param(vt, "preset_name", b, sizeof b) > 0 && strcmp(b, "A Resynth one") == 0);
    dsp_cleanup(vt);                                                                     /* frees both banks */
}

static void test_mangler_demo(void)
{
    /* Demo in Mangler steps through the first Default presets and gives the player's own sound back */
    rsbank_t *mb = rsbank_load_engine("mangler", "tests/fixtures/mg_default", NULL);
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    CHECK(dsp_set_mg_bank(vt, mb) == 0);
    dsp_set_param(vt, "engine", "Mangler");
    dsp_set_param(vt, "mg_pitch", "3");
    dsp_set_bpm(vt, 240.0f);                                                             /* a 4-bar step is 4 s */
    CHECK(dsp_set_param(vt, "demo", "On") == 0);
    float blk[2 * 128];
    char b[64];
    int saw_formant = 0, saw_vocode = 0;
    for (int bn = 0; bn < 2500; bn++) {
        for (int i = 0; i < 256; i++) blk[i] = 0.0f;
        dsp_process_block_f(vt, blk, 128);
        dsp_get_param(vt, "mg_shift", b, sizeof b);
        saw_formant |= strcmp(b, "Formant") == 0;
        saw_vocode |= strcmp(b, "Vocode") == 0;
        for (int i = 0; i < 256; i++) CHECK(isfinite(blk[i]));
    }
    CHECK(saw_formant && saw_vocode);
    CHECK(dsp_set_param(vt, "demo", "Off") == 0);
    dsp_process_block_f(vt, blk, 128);
    dsp_get_param(vt, "mg_pitch", b, sizeof b);
    CHECK(atof(b) == 3.0);
    dsp_get_param(vt, "mg_shift", b, sizeof b);
    CHECK(strcmp(b, "Granular") == 0);
    dsp_cleanup(vt);
}
