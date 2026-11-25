/*
 * Looper Save (src/dsp/loopsave.c): one period of the processed loop, written as a WAV. Included by test_main.c after
 * bankmgr_tests.inc (it uses that file's bmt_rmrf / bmt_exists). Works in build/tests/ls_root (made and removed here).
 */
#define LST_ROOT "build/tests/ls_root"
#define LST_DIR  LST_ROOT "/Samples/TinyVoice"

/* processes the 440 Hz sine in blocks until the capture reaches `state` (or max_s seconds pass); the seconds it took */
static double lst_run_until(vt_t *vt, int state, double max_s, long *pos)
{
    static float blk[2 * 128];
    int blocks = 0;
    while (loopsave_state(dsp_loopsave(vt)) != state && blocks * 128.0 < max_s * VT_SAMPLE_RATE) {
        for (int i = 0; i < 128; i++, (*pos)++)
            blk[2 * i] = blk[2 * i + 1] = 0.5f * (float)sin(2.0 * M_PI * 440.0 * (double)(*pos) / VT_SAMPLE_RATE);
        dsp_process_block_f(vt, blk, 128);
        blocks++;
    }
    return blocks * 128.0 / VT_SAMPLE_RATE;
}

static void lst_run(vt_t *vt, double seconds, long *pos)
{
    int n;
    run_sine_cap(vt, 440.0, 0.5f, seconds, pos, NULL, &n);
}

/* a WAV's header fields and samples; returns the frame count, or -1 when it is not the file we write */
static int lst_read_wav(const char *path, int *rate, int *chan, int *bits, int16_t *l, int max)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char h[44];
    if (fread(h, 1, 44, f) != 44 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVEfmt ", 8) != 0 || memcmp(h + 36, "data", 4) != 0) { fclose(f); return -1; }
    *chan = h[22] | (h[23] << 8);
    *rate = (int)(h[24] | (h[25] << 8) | (h[26] << 16) | ((unsigned)h[27] << 24));
    *bits = h[34] | (h[35] << 8);
    const unsigned long bytes = h[40] | (h[41] << 8) | (h[42] << 16) | ((unsigned long)h[43] << 24);
    const unsigned long riff = h[4] | (h[5] << 8) | (h[6] << 16) | ((unsigned long)h[7] << 24);
    int frames = (int)(bytes / 4);
    if (riff != 36ul + bytes || frames > max) { fclose(f); return -1; }
    for (int i = 0; i < frames; i++) {
        unsigned char s[4];
        if (fread(s, 1, 4, f) != 4) { fclose(f); return -1; }
        l[i] = (int16_t)(s[0] | (s[1] << 8));
    }
    char extra;
    if (fread(&extra, 1, 1, f) != 0) frames = -1;                    /* nothing after the data */
    fclose(f);
    return frames;
}

/* the one .wav in the folder whose name starts with `prefix`; its path in out, 1 when found */
static int lst_find(const char *prefix, char *out, size_t n)
{
    DIR *d = opendir(LST_DIR);
    struct dirent *de;
    int found = 0;
    while (d && (de = readdir(d)) != NULL)
        if (strncmp(de->d_name, prefix, strlen(prefix)) == 0 && strstr(de->d_name, ".wav") && !strstr(de->d_name, ".part")) {
            snprintf(out, n, "%s/%s", LST_DIR, de->d_name);
            found++;
        }
    if (d) closedir(d);
    return found == 1;
}

static void lst_param(vt_t *vt, const char *key, const char *want)
{
    char v[32];
    dsp_get_param(vt, key, v, sizeof v);
    if (strcmp(v, want) != 0) printf("  %s is \"%s\", wanted \"%s\"\n", key, v, want);
    CHECK(strcmp(v, want) == 0);
}

static void test_loopsave(void)
{
    static int16_t wav[16 * 48000];
    char path[800], st[256];
    int rate, chan, bits;
    bmt_rmrf(LST_ROOT);

    /* ---- a Free take of 2 s: Save keeps exactly one round of the looping output ---- */
    vt_t *vt = dsp_init(VT_SAMPLE_RATE);
    loopsave_t *ls = dsp_loopsave(vt);
    loopsave_set_dir(ls, LST_DIR);
    dsp_set_param(vt, "mode", "Robot");
    dsp_set_param(vt, "mix", "0");                                  /* a clean window onto the loop */
    dsp_set_param(vt, "freeze_len", "Free");
    long pos = 0;
    lst_run(vt, 0.3, &pos);

    /* nothing is looping yet: it fails at once, and says so */
    CHECK(dsp_set_param(vt, "freeze_save", "Save") == 0);
    lst_run(vt, 0.05, &pos);
    lst_param(vt, "freeze_save", "Failed");
    loopsave_status(ls, st, sizeof st);
    CHECK(strstr(st, "\"ok\":0") && strstr(st, "nothing to save"));
    lst_run(vt, 2.1, &pos);                                         /* Failed shows for 2 s, then Off */
    lst_param(vt, "freeze_save", "Off");

    CHECK(dsp_set_param(vt, "freeze_rec", "Rec") == 0);
    lst_run(vt, 2.0, &pos);
    CHECK(dsp_set_param(vt, "freeze_rec", "Off") == 0);
    lst_run(vt, 0.3, &pos);
    lst_param(vt, "freeze", "On");
    const int period = 2 * VT_SAMPLE_RATE - (int)(FREEZE_XF_SEC * VT_SAMPLE_RATE);   /* the take less the wrap fade */

    CHECK(dsp_set_param(vt, "freeze_save", "Save") == 0);
    lst_run_until(vt, LS_READY, 6.0, &pos);
    CHECK(loopsave_state(ls) == LS_READY);                          /* captured: the worker's turn */
    if (!(ls->want > period - 130 && ls->want < period + 130)) printf("  capture %d samples, the take less its fade is %d\n", ls->want, period);
    CHECK(ls->want > period - 130 && ls->want < period + 130);          /* the loop period, give or take the take's own rounding */
    lst_param(vt, "freeze_save", "Save");
    CHECK(!lst_find("TinyVoice Loop", path, sizeof path));          /* nothing written until the worker runs */
    CHECK(loopsave_pump(ls) == 1);
    lst_run(vt, 0.05, &pos);
    lst_param(vt, "freeze_save", "Saved");
    loopsave_status(ls, st, sizeof st);
    CHECK(strstr(st, "\"ok\":1") && strstr(st, "saved TinyVoice Loop 001"));
    CHECK(lst_find("TinyVoice Loop 001 - ", path, sizeof path));
    if (!strstr(path, " s.wav")) printf("  named %s\n", path);
    CHECK(strstr(path, " s.wav") != NULL);                          /* a Free take is named by its length */
    const int n = lst_read_wav(path, &rate, &chan, &bits, wav, 16 * 48000);
    CHECK(n == ls->want && rate == VT_SAMPLE_RATE && chan == 2 && bits == 16);
    {   /* the 440 Hz the loop carries is in the file; the file is not silent or clipped */
        static float f[16 * 48000];
        int peak = 0;
        for (int i = 0; i < n; i++) { f[i] = (float)wav[i] / 32768.0f; if (abs(wav[i]) > peak) peak = abs(wav[i]); }
        CHECK(band_at(f, n, 440.0) > 0.01);
        CHECK(peak > 3000 && peak < 32767);
    }
    lst_run(vt, 2.1, &pos);
    lst_param(vt, "freeze_save", "Off");
    CHECK(loopsave_state(ls) == LS_IDLE);

    /* a second save is numbered on, and the one in the way is never overwritten */
    CHECK(dsp_set_param(vt, "freeze_save", "Save") == 0);
    lst_run_until(vt, LS_READY, 6.0, &pos);
    CHECK(loopsave_pump(ls) == 1);
    lst_run(vt, 0.05, &pos);
    CHECK(lst_find("TinyVoice Loop 002 - ", path, sizeof path));
    CHECK(lst_find("TinyVoice Loop 001 - ", path, sizeof path));
    lst_run(vt, 2.1, &pos);

    /* Save turned off before the capture begins: cancelled, nothing written */
    CHECK(dsp_set_param(vt, "freeze_save", "Save") == 0);
    lst_run(vt, 0.01, &pos);
    CHECK(loopsave_state(ls) == LS_ARMED || loopsave_state(ls) == LS_CAPTURING);
    CHECK(dsp_set_param(vt, "freeze_save", "Off") == 0);
    lst_run(vt, 0.05, &pos);
    CHECK(loopsave_state(ls) == LS_IDLE);
    CHECK(loopsave_pump(ls) == 0);
    CHECK(!lst_find("TinyVoice Loop 003 - ", path, sizeof path));

    /* the loop stopped under a capture: nothing is kept */
    CHECK(dsp_set_param(vt, "freeze_save", "Save") == 0);
    lst_run(vt, 0.01, &pos);
    CHECK(dsp_set_param(vt, "freeze", "Off") == 0);
    lst_run(vt, 0.1, &pos);
    lst_param(vt, "freeze_save", "Failed");
    loopsave_status(ls, st, sizeof st);
    CHECK(strstr(st, "\"ok\":0") && strstr(st, "changed or stopped"));
    CHECK(loopsave_pump(ls) == 0);
    CHECK(!lst_find("TinyVoice Loop 003 - ", path, sizeof path));
    dsp_cleanup(vt);

    /* ---- a bar take: 2 bars at 120 bpm is exactly 4 s, and the file is named for it ---- */
    vt = dsp_init(VT_SAMPLE_RATE);
    ls = dsp_loopsave(vt);
    loopsave_set_dir(ls, LST_DIR);
    dsp_set_param(vt, "mode", "Robot");
    dsp_set_param(vt, "mix", "0");
    dsp_set_bpm(vt, 120.0f);
    dsp_set_param(vt, "freeze_len", "2 Bars");
    pos = 0;
    lst_run(vt, 0.2, &pos);
    CHECK(dsp_set_param(vt, "freeze_rec", "Rec") == 0);            /* the transport is stopped: the take starts at once */
    lst_run(vt, 4.2, &pos);
    lst_param(vt, "freeze", "On");
    CHECK(dsp_set_param(vt, "freeze_save", "Save") == 0);
    lst_run_until(vt, LS_READY, 8.0, &pos);
    CHECK(loopsave_state(ls) == LS_READY);
    CHECK(ls->want == 4 * VT_SAMPLE_RATE);                          /* exactly two bars, to the sample */
    CHECK(loopsave_pump(ls) == 1);
    CHECK(lst_find("TinyVoice Loop 003 - 2 bars 120 bpm.wav", path, sizeof path));
    CHECK(lst_read_wav(path, &rate, &chan, &bits, wav, 16 * 48000) == 4 * VT_SAMPLE_RATE);
    loopsave_status(ls, st, sizeof st);
    CHECK(strstr(st, "2 bars 120 bpm") != NULL);
    dsp_cleanup(vt);

    /* the folder cannot be made (a file is in its way): Failed, with the system's reason, and no half file */
    vt = dsp_init(VT_SAMPLE_RATE);
    ls = dsp_loopsave(vt);
    {
        FILE *blocker = fopen(LST_ROOT "/blocker", "wb");
        CHECK(blocker != NULL);
        if (blocker) fclose(blocker);
    }
    loopsave_set_dir(ls, LST_ROOT "/blocker/TinyVoice");
    dsp_set_param(vt, "mode", "Robot");
    dsp_set_param(vt, "freeze_len", "Free");
    pos = 0;
    lst_run(vt, 0.2, &pos);
    dsp_set_param(vt, "freeze_rec", "Rec");
    lst_run(vt, 1.5, &pos);
    dsp_set_param(vt, "freeze_rec", "Off");
    lst_run(vt, 0.2, &pos);
    dsp_set_param(vt, "freeze_save", "Save");
    lst_run_until(vt, LS_READY, 6.0, &pos);
    CHECK(loopsave_pump(ls) == 1);
    lst_run(vt, 0.05, &pos);
    lst_param(vt, "freeze_save", "Failed");
    loopsave_status(ls, st, sizeof st);
    CHECK(strstr(st, "\"ok\":0") && strstr(st, "could not write"));
    dsp_cleanup(vt);

    /* Save is a performance control: not in the saved state */
    vt = dsp_init(VT_SAMPLE_RATE);
    {
        static char state[16384];
        dsp_set_param(vt, "freeze_save", "Save");
        dsp_get_param(vt, "state", state, sizeof state);
        CHECK(strstr(state, "freeze_save") == NULL);
    }
    dsp_cleanup(vt);
    bmt_rmrf(LST_ROOT);
}
