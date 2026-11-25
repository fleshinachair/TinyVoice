/*
 * TinyVoice engine: parameter table, MIDI voice tracking, mode dispatch,
 * delay-compensated dry/wet mix, Tone, Spread chorus, state snapshot,
 * visualizer feed.
 * Engines: daft.c (Robot, Nightcall), imogen.c (Chords), modes.c (Snap, Morph,
 * Prism, Double, Tape, Scream, HardTune), voice.c (Dalek, Harmony, Walkie); the
 * pitch modes run on the cher.c tracker.
 */
#include "dsp.h"
#include "cher.h"
#include "daft.h"
#include "imogen.h"
#include "modes.h"
#include "voice.h"
#include "freeze.h"
#include "loopsave.h"
#include "demo.h"
#include "vfx.h"
#include "groove.h"
#include "tour.h"
#include "presets.h"
#include "hm.h"
#include "mangle.h"
#include "rsbank.h"
#include "vt_floor.h"
#include "module_meta.h"   /* generated: chain_params / ui_hierarchy JSON */

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VT_DRY_DELAY 2048   /* power of two; cher latency tops out near 760 samples */
#define VT_CHORUS_LEN 2048  /* power of two; chorus taps reach ~12 ms */
#define CHORUS_BASE_MS 9.0f
#define CHORUS_DEPTH_MS 2.5f
#define CHORUS_RATE_HZ 0.4f
/* Chords with Auto Off: +1 dB on top of the harmony bus's +4.5 dB, +5.5 dB
 * net over the input, so the cluster stays present at a raw mic level (Auto
 * On levels it anyway). +9 dB net drove a hot voice into the output limiter. */
#define CHORDS_MANUAL_TRIM 1.1220f
#define LABEL_PULSE_SEC 0.6f /* is_loading "1" after a mode change: > the UI's ~8-tick poll */
#define ROBOT_REG_LO    48     /* Robot carrier register: held pads fold, as a block, */
#define ROBOT_REG_HI    72     /* so their mean sits in C3..B4 */
#define GATE_DEFAULT_DB (-39)  /* the manual level until one is chosen: above room noise */
#define MODE_FADE_SEC   0.005f /* a mode switch fades the old engine out and the new one in */
#define SNAP_INSTANT_BEAUTY 0.005f /* Beauty at (or within half a step of) 0: Snap retunes instantly */

/* Auto's input AGC (every mode): soft voices are lifted towards a working
 * level, loud ones eased down. It holds its gain in silence rather than
 * chasing the noise floor up. */
#define IN_AGC_TARGET  0.25f    /* peak-equivalent, about -12 dBFS */
#define IN_AGC_MAX     3.981f   /* +12 dB: never enough to turn a room into a rush */
#define IN_AGC_MIN     0.5f     /* -6 dB */
/* The voice gate: true RMS of the raw input (10 ms, before any gain) against
 * a fixed threshold, Auto's -42 dBFS or the Gate knob with Auto Off. One
 * decision for everything: below it (after a 50 ms hold) Robot outputs
 * exact zeros, and with Auto the AGC freezes at once and a downward
 * expander takes the input 40 dB down. (A peak-riding level read noise ~3 dB
 * hot and let -45 dBFS RMS mic hiss open it, playing a pad's carrier; an
 * adaptive room-noise threshold missed pauses on the device.) */
#define SG_THR         0.00794f /* -42 dBFS RMS */
#define SG_DEPTH       0.01f    /* closed: -40 dB */
#define SG_HOLD_SEC    0.05f    /* stays open this long below the threshold */
#define SG_ATTACK_SEC  0.002f
#define SG_RELEASE_SEC 0.06f

#ifdef VT_TEST_ALLOC
/* Tests substitute counting allocators to prove nothing leaks. */
void *vt_test_calloc(size_t n, size_t sz);
void  vt_test_free(void *p);
#define VT_CALLOC vt_test_calloc
#define VT_FREE   vt_test_free
#else
#define VT_CALLOC calloc
#define VT_FREE   free
#endif

/* ------------------------------------------------------------------ params */

static const char *const k_mode_opts[]  = {"Robot", "Snap", "Chords", "Prism", "Tape", "Nightcall", "Scream", "Dalek",
                                           "HardTune", "Harmony", "Walkie", "Morph", "Double"};
/* Gate: Auto, then the manual threshold in 3 dB steps (option i = -60 + 3 (i - 1) dB). */
static const char *const k_gate_opts[VT_GATE_OPTS] = {
    "Auto", "-60 dB", "-57 dB", "-54 dB", "-51 dB", "-48 dB", "-45 dB", "-42 dB", "-39 dB", "-36 dB", "-33 dB",
    "-30 dB", "-27 dB", "-24 dB", "-21 dB", "-18 dB", "-15 dB", "-12 dB", "-9 dB", "-6 dB", "-3 dB", "0 dB",
};
static const char *const k_onoff_opts[] = {"Off", "On"};
static const char *const k_engine_opts[] = {"Vocoder", "Synth Voice", "Resynth", "Mangler"};   /* "Synth Voice" was "VoiceFX": a saved set's old name still loads (dsp_set_param) */
/* Resynth's enum option lists, named after their hm_params.h macro (the table below pastes the name) */
static const char *const k_hm_HM_OPTS_OFFON[] = {HM_OPTS_OFFON};
static const char *const k_hm_HM_OPTS_LAT[]   = {HM_OPTS_LAT};
static const char *const k_hm_HM_OPTS_SYNC[]  = {HM_OPTS_SYNC};
static const char *const k_hm_HM_OPTS_WMODE[] = {HM_OPTS_WMODE};
static const char *const k_hm_HM_OPTS_PMODE[] = {HM_OPTS_PMODE};
static const char *const k_rs_bank_opts[] = {RSBANK_NAMES};    /* rsbank.h: Default, Imported, then Imported by kind */
static const char *const k_hm_idle_opts[] = {"Tune", "Silent"};
static const char *const k_hm_scale_opts[] = {"Major", "Minor", "Chromatic", "Custom"};
static const char *const k_hm_wave_opts[]  = {"Saw", "Square", "Triangle", "Sine", "Preset"};
#define HM_SCALE_CUSTOM 3
#define HM_WAVE_PRESET 4
#define VT_HM_WT_N 512                     /* hm_wt: one cycle, 512 int16 samples, base64 (684 x 2 chars) */
#define VT_HM_WT_STR (4 * ((2 * VT_HM_WT_N + 2) / 3) + 6)   /* + ":<len>" (the table's own length, up to 4096) */
/* Mangler's enum option lists */
static const char *const k_mg_shift_opts[]  = {"Granular", "Formant", "Vocode"};
static const char *const k_mg_notes_opts[]  = {"Normal", "Pads", "Chord"};
static const char *const k_mg_chord_opts[]  = {"1", "2", "3", "4", "5", "6", "7", "8"};
static const char *const k_mg_spread_opts[] = {"Spread", "Jitter"};
#define MG_CHORDS_STR 384                 /* "mg_chords" text: 8 slots x 8 notes x 4 chars + separators */
static const char *const k_freeze_rec_opts[] = {"Off", "Rec"};      /* Freeze's Rec control (freeze.h) */
static const char *const k_freeze_save_opts[] = {"Off", "Save", "Saved", "Failed"};   /* Looper Save: the DSP moves it to Saved / Failed, then back to Off */
static const char *const k_freeze_len_opts[] = {"Free", "1 Bar", "2 Bars", "4 Bars"};
static const int k_freeze_bars[] = { 0, 1, 2, 4 };
static const char *const k_root_opts[]  = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *const k_scale_opts[] = {"Chromatic", "Major", "Minor", "Pentatonic", "Dorian"};

#define ENUM_DEF(key, opts, def) {key, VT_PTYPE_ENUM, 0, (float)(sizeof(opts) / sizeof(opts[0]) - 1), def, opts, (int)(sizeof(opts) / sizeof(opts[0])), 0}
#define TOGGLE_DEF(key) {key, VT_PTYPE_ENUM, 0, 1, 0, k_onoff_opts, 2, 1}
#define TRANSIENT_ENUM_DEF(key, opts) {key, VT_PTYPE_ENUM, 0, (float)(sizeof(opts) / sizeof(opts[0]) - 1), 0, opts, (int)(sizeof(opts) / sizeof(opts[0])), 1}
#define FLOAT_DEF(key, lo, hi, def) {key, VT_PTYPE_FLOAT, lo, hi, def, NULL, 0, 0}
/* Resynth's block, from the X-macro list (key "hm_" + the Humanoid preset id) */
#define HM_DEF_F(pid, def, lo, hi, opts) {"hm_" pid, VT_PTYPE_FLOAT, (float)(lo), (float)(hi), (float)(def), NULL, 0, 0}
#define HM_DEF_E(pid, def, lo, hi, opts) {"hm_" pid, VT_PTYPE_ENUM, 0, (float)(sizeof(opts) / sizeof(opts[0]) - 1), (float)(def), opts, (int)(sizeof(opts) / sizeof(opts[0])), 0}
#define HM_DEF_T(pid, def, lo, hi, opts) {"hm_" pid, VT_PTYPE_ENUM, 0, (float)(sizeof(opts) / sizeof(opts[0]) - 1), (float)(def), opts, (int)(sizeof(opts) / sizeof(opts[0])), 1}
#define VT_X_HM(id, pid, label, kind, def, lo, hi, step, unit, ...) [VT_P_HM_FIRST + id] = HM_DEF_##kind(pid, def, lo, hi, k_hm_ ## __VA_ARGS__),

/* Must stay in sync with vt_param_id and module.json (see tests). */
static const vt_param_def k_params[VT_P_COUNT] = {
    [VT_P_MODE]     = ENUM_DEF("mode",  k_mode_opts,  VT_MODE_SNAP),
    [VT_P_KEY]       = ENUM_DEF("key",   k_root_opts,  0),             /* C */
    [VT_P_SCALE]     = ENUM_DEF("scale", k_scale_opts, VT_SCALE_CHROMATIC),
    [VT_P_RETUNE]    = FLOAT_DEF("retune",    0.0f,   500.0f, 0.0f),
    [VT_P_CHAR1]     = FLOAT_DEF("char1",     0.0f,   1.0f,   0.5f),
    [VT_P_CHAR2]     = FLOAT_DEF("char2",     0.0f,   1.0f,   0.5f),
    [VT_P_GATE]      = ENUM_DEF("gate",   k_gate_opts, 0),       /* Auto */
    [VT_P_MIX]      = FLOAT_DEF("mix",       0.0f,   1.0f,   1.0f),
    [VT_P_GAIN]      = FLOAT_DEF("gain",    -12.0f,  12.0f,   0.0f),
    [VT_P_SPREAD]    = FLOAT_DEF("spread",    0.0f,   1.0f,   0.25f),
    [VT_P_TONE]      = FLOAT_DEF("tone",      0.0f,   1.0f,   1.0f),
    [VT_P_GLIDE]     = FLOAT_DEF("glide",     0.0f,   500.0f, 0.0f),
    [VT_P_MOD]       = FLOAT_DEF("mod",       0.0f,   1.0f,   0.0f),
    [VT_P_FREEZE_LEN] = ENUM_DEF("freeze_len", k_freeze_len_opts, 2),  /* 2 Bars */
    [VT_P_BEAUTY]    = FLOAT_DEF("beauty",    0.0f,   1.0f,   0.5f),
    [VT_P_LATCH]     = ENUM_DEF("latch",      k_onoff_opts, 0),
    [VT_P_CHAR4]     = FLOAT_DEF("char4",     0.0f,   1.0f,   0.5f),
    [VT_P_CHAR5]     = FLOAT_DEF("char5",     0.0f,   1.0f,   0.5f),
    [VT_P_ENGINE]    = ENUM_DEF("engine",     k_engine_opts, VT_ENGINE_VOCODER),
    /* defaults = the "Vowel Morph Solo" preset (the test checks it) */
    [VT_P_VF_VOWEL]  = FLOAT_DEF("vf_vowel",   0.0f,  1.0f,   0.0f),
    [VT_P_VF_TRACT]  = FLOAT_DEF("vf_tract",   0.0f,  1.0f,   0.5f),
    [VT_P_VF_PEAK]   = FLOAT_DEF("vf_peak",    0.0f,  1.0f,   0.5f),
    [VT_P_VF_CHOIR]  = FLOAT_DEF("vf_choir",   0.0f,  1.0f,   0.35f),
    [VT_P_VF_BREATH] = FLOAT_DEF("vf_breath",  0.0f,  1.0f,   0.15f),
    [VT_P_VF_ATTACK] = FLOAT_DEF("vf_attack",  0.0f,  1.0f,   0.12f),
    [VT_P_VF_RELEASE] = FLOAT_DEF("vf_release", 0.0f, 1.0f,   0.3f),
    [VT_P_VF_MIX]    = FLOAT_DEF("vf_mix",     0.0f,  1.0f,   0.75f),
    [VT_P_VF_PRESET] = {"vf_preset", VT_PTYPE_FLOAT, 0, 5, 5, NULL, 0, 1},    /* 0..5 = the bank; transient; 5 = Vowel Morph Solo */
    [VT_P_RS_BANK]   = {"rs_bank", VT_PTYPE_FLOAT, 0, RSBANK_SLOTS - 1, 0, NULL, 0, 1},   /* which bank the Presets page lists: the engine's list, then yours */
    [VT_P_RS_PRESET] = {"rs_preset", VT_PTYPE_FLOAT, 0, RSBANK_MAX - 1, 0, NULL, 0, 1},   /* the Resynth bank (rsbank.c); transient */
    HM_PARAM_LIST(VT_X_HM)
    [VT_P_HM_KEY]    = ENUM_DEF("hm_key",   k_root_opts,     0),
    [VT_P_HM_SCALE]  = ENUM_DEF("hm_scale", k_hm_scale_opts, 0),       /* Major */
    [VT_P_HM_MASK]   = FLOAT_DEF("hm_mask",  0.0f, 4095.0f, 2741.0f),  /* C major */
    [VT_P_HM_HOLD]   = ENUM_DEF("hm_scale_hold", k_onoff_opts, 1),     /* On: a preset keeps Key / Scale / Notes */
    [VT_P_HM_LEVEL]  = ENUM_DEF("hm_autolevel", k_onoff_opts, 1),      /* On: the output leveller */
    [VT_P_HM_IDLE]   = ENUM_DEF("hm_midi_idle", k_hm_idle_opts, 0),    /* MIDI mode, no pad: Tune to the scale / Silent (the reference) */
    [VT_P_HM_VGATE]  = FLOAT_DEF("hm_mic_gate", -72.0f, -24.0f, -42.0f), /* the voice gate in Resynth, dBFS RMS of the raw input */
    [VT_P_HM_WAVE]   = ENUM_DEF("hm_wave",  k_hm_wave_opts,  0),       /* Saw */
    /* Mangler (the reference's units; defaults are the reference's own plain-voice start) */
    [VT_P_MG_SHIFT]   = ENUM_DEF("mg_shift",   k_mg_shift_opts,  MG_GRANULAR),
    [VT_P_MG_PITCH]   = FLOAT_DEF("mg_pitch",   -48.0f, 24.0f,   0.0f),
    [VT_P_MG_FORMANT] = FLOAT_DEF("mg_formant", -24.0f, 24.0f,   0.0f),
    [VT_P_MG_VOICES]  = FLOAT_DEF("mg_voices",    1.0f,  8.0f,   1.0f),
    [VT_P_MG_DETUNE]  = FLOAT_DEF("mg_detune",    0.0f, 12.0f,   0.0f),
    [VT_P_MG_TIME]    = FLOAT_DEF("mg_time",      0.0f, MG_MAX_TIME_MS, 0.0f),
    [VT_P_MG_GRAIN]   = FLOAT_DEF("mg_grain",     1.0f, 100.0f,  10.0f),
    [VT_P_MG_MIX]     = FLOAT_DEF("mg_mix",       0.0f,  1.0f,   1.0f),
    [VT_P_MG_NOTES]   = ENUM_DEF("mg_notes",   k_mg_notes_opts,  MG_NOTES_NORMAL),
    [VT_P_MG_CHORD]   = ENUM_DEF("mg_chord",   k_mg_chord_opts,  0),
    [VT_P_MG_MONO]    = ENUM_DEF("mg_mono",    k_onoff_opts,     1),
    [VT_P_MG_FIX]     = ENUM_DEF("mg_fix",     k_onoff_opts,     1),
    [VT_P_MG_SNAP]    = ENUM_DEF("mg_snap",    k_onoff_opts,     1),
    [VT_P_MG_SCATTER] = ENUM_DEF("mg_scatter", k_mg_spread_opts, 0),
    [VT_P_MG_SMEAR]   = ENUM_DEF("mg_smear",   k_mg_spread_opts, 0),
    [VT_P_MG_WIDE]    = ENUM_DEF("mg_wide",    k_onoff_opts,     0),
    [VT_P_MG_GJITTER] = ENUM_DEF("mg_gjitter", k_onoff_opts,     1),
    [VT_P_MG_FILTER]  = ENUM_DEF("mg_filter",  k_onoff_opts,     0),
    [VT_P_MG_FREQ]    = FLOAT_DEF("mg_freq",     20.0f, 20000.0f, 1000.0f),
    [VT_P_MG_WIDTH]   = FLOAT_DEF("mg_width",     0.1f,  10.0f,   10.0f),
    [VT_P_MG_VOLUME]  = FLOAT_DEF("mg_volume",  -24.0f,  12.0f,    0.0f),
    /* performance toggles: never saved in "state" */
    [VT_P_FREEZE]    = TOGGLE_DEF("freeze"),                             /* Loop: Off / On (plays the kept take) */
    [VT_P_FREEZE_REC] = TRANSIENT_ENUM_DEF("freeze_rec", k_freeze_rec_opts),   /* Rec: Off / Rec (records a new take) */
    [VT_P_FREEZE_SAVE] = TRANSIENT_ENUM_DEF("freeze_save", k_freeze_save_opts),
    [VT_P_DEMO]      = TOGGLE_DEF("demo"),
};

/* The five contextual knobs, in the order of the macro names (k_macro_names). */
static const int k_macro_ids[VT_MACROS] = { VT_P_CHAR1, VT_P_CHAR2, VT_P_MOD, VT_P_CHAR4, VT_P_CHAR5 };

/* Gate option <-> manual level in dB (option 0 is Auto). */
static float gate_db_of(int opt) { return (float)(VT_GATE_MIN_DB + VT_GATE_STEP_DB * (opt - 1)); }
static int gate_opt_of(float db)                                    /* nearest 3 dB step */
{
    if (db < (float)VT_GATE_MIN_DB) db = (float)VT_GATE_MIN_DB;
    if (db > 0.0f) db = 0.0f;
    return 1 + (int)lroundf((db - (float)VT_GATE_MIN_DB) / (float)VT_GATE_STEP_DB);
}

const vt_param_def *dsp_param_def(int id)
{
    return (id >= 0 && id < VT_P_COUNT) ? &k_params[id] : NULL;
}

int dsp_param_index(const char *key)
{
    if (!key) return -1;
    for (int i = 0; i < VT_P_COUNT; i++)
        if (strcmp(k_params[i].key, key) == 0) return i;
    return -1;
}

/* -------------------------------------------------------------- instance */

typedef struct {
    int note;       /* -1 = free */
    int velocity;
    uint32_t age;   /* for oldest-voice stealing */
} vt_voice;

struct vt {
    int sample_rate;
    float p[VT_P_COUNT];

    vt_voice voices[VT_MAX_VOICES];
    uint32_t voice_clock;

    /* Viz state, written by the DSP and read by get_param("viz"). */
    float viz_f0, viz_target, viz_cents;
    float viz_bands[VT_NUM_BANDS];

    cher_t cher;                /* Cher engine, and the tracker/delay line Imogen runs on */
    daft_t daft;                /* Robot */
    daft_t nc;                  /* Nightcall: the same vocoder with the talker voicing */
    imogen_t imogen;
    fx_t fx;                    /* Snap, Morph, Prism, Double, Tape, Scream, HardTune */
    vox_t vox;                  /* Dalek, Harmony, Walkie */
    freeze_t freeze;            /* 16 s punch-in looper on the input */
    loopsave_t ls;              /* Looper Save: one loop period of the output, written to a WAV by the worker (loopsave.h) */
    int   save_asked;           /* the Save control's last value */
    int   save_hold;            /* samples left of showing Saved / Failed on the knob */
    demo_t demo;                /* built-in sung phrase */
    vfx_t vfx;                  /* Voice FX: the formant engine of the second personality */
    groove_t groove;            /* Voice FX's audition source: what Demo plays there instead of the sung phrase */
    hm_t hm;                    /* Resynth: the third personality (harmonic analysis / resynthesis) */
    mg_t mg;                    /* Mangler: the fourth (multi-voice pitch / formant shifter, modelled on the reference) */
    int   mg_fold;              /* Mangler Pads: octaves (x12 st) the held pads are moved by, to sit near C4 */
    int   demo_mg_snap, demo_mg_step;       /* Mangler's Demo: a snapshot is held / the step last loaded */
    float demo_mg_p[VT_P_MG_LAST - VT_P_MG_FIRST + 1];
    char  demo_mg_chords[MG_CHORDS_STR];
    _Atomic(rsbank_t *) mg_bank;  /* Mangler's presets (dsp_set_mg_bank) */
    float hm_wt[VT_HM_WT_N];    /* the cycle a preset carried (hm_wt), used when Wave is Preset */
    int   hm_wt_valid;
    int   hm_wt_len;            /* the the reference table's own length before padding (the ":<len>" suffix), 0 = a full cycle */
    char  hm_wt_str[VT_HM_WT_STR];   /* as received, for "state" */
    int   hm_wave_set;          /* the wave the engine holds (-1 = none yet) */
    int   hm_wt_gen;            /* bumped when hm_wt changes */
    int   hm_wt_gen_set;
    /* Resynth's Demo: it steps through five Default presets, and puts the player's own sound back at the end */
    int   demo_rs_snap, demo_rs_step;       /* a snapshot is held / the step of the tour last loaded (-1 none) */
    float demo_rs_p[VT_P_HM_WAVE - VT_P_HM_FIRST + 1];
    char  demo_rs_wt[VT_HM_WT_STR];
    int   demo_rs_wt_valid, demo_rs_bank, demo_rs_idx;
    /* The Presets page's place survives a reload: the bank and the name of the preset last loaded are saved in
     * "state" (rs_bank / rs_name; the index itself is transient, and the bank is read after the restore), and once the
     * bank has arrived the index is pointed back at that name, nothing loaded (the knobs came from the state). */
    char  rs_name[RSBANK_NAME];
    int   rs_find;                  /* rs_name is to be looked up in the bank (after a restore) */
    char  rs_bank_cur[RSBANK_NAME]; /* the name of the bank the Presets page is on (kept by rs_set_bank) */
    char  rs_bank_want[RSBANK_NAME];/* a bank to look up by name when a list is there (a restore; a list that changed; an engine switch) */
    _Atomic unsigned bank_gen;      /* bumped when a list is swapped (dsp_swap_*) */
    _Atomic int ren_state;          /* a rename hint (dsp_bank_renamed): 0 none, 1 being written, 2 ready */
    int   ren_mg;
    char  ren_old[RSBANK_NAME], ren_new[RSBANK_NAME];
    unsigned bank_seen;
    char  demo_rs_name[RSBANK_NAME];
    int   hold_skip;            /* inside a preset load with Hold Scale on: hm_key / hm_scale / hm_mask are left alone */
    int   hm_held[VT_MAX_VOICES], hm_nheld;   /* pads the engine holds (MIDI mode), oldest first */
    _Atomic(demo_pcm_t *) demo_pcm;   /* the recorded Demo phrase, published once by the loader thread */
    double demo_pos;            /* its playback position, source samples */
    _Atomic(rsbank_t *) rs_bank;  /* Resynth's presets, published once by the loader thread (dsp_set_rs_bank) */
    /* Ghost hands, as the screen sees them. While the Showcase Tour runs, get_param on Voice FX's knobs answers
     * `disp` (the tour's current values, snapshotted by the audio block: get_param and set_param run on the same
     * callback thread, so a read never races the write) and the host's UI, which re-reads every visible knob a few
     * times a second, shows them turning. `p` is never touched, and "state" (what the host saves and hashes) is
     * built from `p`, so the user's own settings are what is kept, and nothing is ever re-saved because of the tour. */
    float disp[VT_PRESET_VALUES];
    int   disp_on;
    double demo_time;           /* seconds since Demo was switched on */
    float in_env, agc_goal, agc_gain;   /* Auto input AGC */
    float k_agc_att, k_agc_rel, k_agc_gain;
    /* Auto silence gate (freezes the AGC and expands pauses down) */
    float sg_ms, sg_gain, k_sg_env_a, k_sg_env_r, k_sg_att, k_sg_rel;   /* sg_ms: mean square, 10 ms */
    int   sg_open, sg_hold, sg_hold_n;
    /* `diag` readout (get_param only): input and output levels, held over
     * ~1 s, and how often the voice gate was open. Updated per block. */
    float dg_in_pk, dg_in_min, dg_out_pk, dg_open, dg_ropen, dg_out_ms;
    unsigned midi_note_ons;     /* note-ons received, for the screen / diagnostics */
    int      midi_ch;           /* the slot's receive channel 0-15, -1 = omni */
    unsigned midi_foreign;      /* channel messages dropped for another channel (diag) */
    int      midi_ch_raw;       /* what the host answered for the slot's channel (diag) */
    int      midi_src;          /* source of the message being handled (diag) */
    int      midi_log[6][3];    /* last note-ons: status, note, source (diag) */
    int      midi_log_n;
    int   demo_active, demo_saved_mode;
    /* The five contextual knobs (char1, char2, mod, char4, char5) belong to a mode: HardTune's Amount is not
     * Harmony's Lead. p[] holds the running mode's five (so every engine reads them as before); the others wait
     * here, and a mode change swaps them (macros_follow_mode). */
    float macro_store[VT_MODE_COUNT][VT_MACROS];
    int   macro_mode;           /* the mode whose five are in p[] now */
    int   demo_pinned;            /* Mode turned during Demo: the tour stops on it */
    int   restoring;              /* inside apply_state: Beauty is set whatever the mode */
    float bpm;
    double beat;                /* host transport, beats at the next block's start; < 0 = stopped */

    /* Per-mode knob names: the served chain_params names char1/char2/mod for
     * the current mode. When the mode changes, is_loading reads "1" for a
     * moment; its fall back to "0" is what makes the knob pages re-read
     * chain_params (shadow_ui_param_pages.mjs, reloadIfChanged). */
    int label_mode;             /* mode the served names describe, -1 = not yet */
    int loading_blocks;         /* blocks left reading is_loading = "1" */
    int warm_samples;           /* samples processed, saturating at 1 s */

    /* Mode switch: the running engine's wet fades out, the new engine takes
     * over at the next block boundary and fades in, so switching (knob, state
     * or the Demo tour) never cuts one engine's waveform into another's. */
    int   run_mode;             /* engine actually running, -1 = none yet */
    int   robot_fold;           /* Robot carrier: octaves (x12 st) the held pads are moved by */
    float hp_a, hp_x[2], hp_y[2];   /* mic pre-conditioning: 1-pole high-pass, 120 Hz, per channel */
    /* Latch: the chord the poly modes (Robot, Chords, Prism) play while Latch
     * is On, independent of the pads' note-offs. */
    int   latch_note[VT_MAX_VOICES], latch_vel[VT_MAX_VOICES];
    int   latch_was;            /* Latch at the last block, to catch it being switched */
    int   gate_mem;             /* Gate option of the last manual level (what "auto Off" returns to) */
    float sw_g, k_sw;           /* wet switch gain 0..1, its per-sample step */

    /* Tone (2-pole lowpass on the wet signal) and Spread (stereo chorus). */
    float tone_z[2][2];
    float chorus[VT_CHORUS_LEN];
    int   ch_w;
    float ch_phase;
    float ch_amt_s;             /* the chorus amount, smoothed: it is 0 while a vocoder runs, and
                                 * switching on at once replayed the old engine's tail 9 ms late */

    /* Dry path delay line (per channel) so wet/dry blends stay phase-aligned
     * with the shifter's latency (Cher and Imogen). */
    float dry[2][VT_DRY_DELAY];
    int   dry_w;
    float dry_d;                /* current dry delay in samples */
};

vt_t *dsp_init(int sample_rate)
{
    vt_t *vt = (vt_t *)VT_CALLOC(1, sizeof(*vt));
    if (!vt) return NULL;

    vt->sample_rate = sample_rate > 0 ? sample_rate : VT_SAMPLE_RATE;
    for (int i = 0; i < VT_P_COUNT; i++) vt->p[i] = k_params[i].def;
    for (int m = 0; m < VT_MODE_COUNT; m++)
        for (int k = 0; k < VT_MACROS; k++) vt->macro_store[m][k] = k_params[k_macro_ids[k]].def;
    vt->macro_mode = (int)vt->p[VT_P_MODE];
    for (int i = 0; i < VT_MAX_VOICES; i++) vt->voices[i].note = -1;
    cher_init(&vt->cher, (float)vt->sample_rate);
    daft_init(&vt->daft, (float)vt->sample_rate);
    daft_init(&vt->nc, (float)vt->sample_rate);
    vox_init(&vt->vox, (float)vt->sample_rate);
    imogen_init(&vt->imogen, (float)vt->sample_rate);
    fx_init(&vt->fx, (float)vt->sample_rate);
    freeze_init(&vt->freeze, (float)vt->sample_rate);
    loopsave_init(&vt->ls, vt->sample_rate);
    demo_init(&vt->demo, (float)vt->sample_rate);
    vfx_init(&vt->vfx, (float)vt->sample_rate);
    groove_init(&vt->groove, (float)vt->sample_rate);
    hm_init(&vt->hm, (float)vt->sample_rate);
    mg_init(&vt->mg, (float)vt->sample_rate);
    mg_default_chords(&vt->mg);
    vt->demo_mg_step = -1;
    vt->hm_wave_set = -1;
    atomic_init(&vt->rs_bank, NULL);
    atomic_init(&vt->mg_bank, NULL);
    atomic_init(&vt->demo_pcm, NULL);
    atomic_init(&vt->bank_gen, 0u);
    atomic_init(&vt->ren_state, 0);
    vt->label_mode = -1;
    vt->run_mode = -1;
    vt->midi_ch = -1;                       /* omni until the host names the slot's channel */
    vt->midi_ch_raw = -99;                  /* never asked */
    vt->midi_src = -1;
    vt->gate_mem = gate_opt_of(GATE_DEFAULT_DB);
    vt->hp_a = expf(-2.0f * 3.14159265f * VT_INPUT_HPF_HZ / (float)vt->sample_rate);
    for (int i = 0; i < VT_MAX_VOICES; i++) vt->latch_note[i] = -1;
    vt->sw_g = 1.0f;
    vt->k_sw = 1.0f / (MODE_FADE_SEC * (float)vt->sample_rate);
    vt->bpm = 120.0f;
    vt->beat = -1.0;
    vt->agc_goal = vt->agc_gain = 1.0f;
    vt->k_agc_att  = 1.0f - expf(-1.0f / (0.010f * (float)vt->sample_rate));
    vt->k_agc_rel  = 1.0f - expf(-1.0f / (1.000f * (float)vt->sample_rate));
    vt->k_agc_gain = 1.0f - expf(-1.0f / (0.050f * (float)vt->sample_rate));
    /* the gate's level: a plain 10 ms average (a fast-attack follower rides
     * noise peaks ~6 dB over the noise's level and keeps reopening) */
    vt->k_sg_env_a = vt->k_sg_env_r = 1.0f - expf(-1.0f / (0.010f * (float)vt->sample_rate));
    vt->k_sg_att   = 1.0f - expf(-1.0f / (SG_ATTACK_SEC * (float)vt->sample_rate));
    vt->k_sg_rel   = 1.0f - expf(-1.0f / (SG_RELEASE_SEC * (float)vt->sample_rate));
    vt->sg_hold_n  = (int)(SG_HOLD_SEC * (float)vt->sample_rate);
    vt->sg_gain = 1.0f;
    vt->sg_open = 1;
    return vt;
}

void dsp_cleanup(vt_t *vt)
{
    if (!vt) return;
    rsbank_free(atomic_load_explicit(&vt->rs_bank, memory_order_acquire));
    rsbank_free(atomic_load_explicit(&vt->mg_bank, memory_order_acquire));
    demo_pcm_free(atomic_load_explicit(&vt->demo_pcm, memory_order_acquire));
    VT_FREE(vt);
}

int dsp_set_rs_bank(vt_t *vt, struct rsbank *bank)
{
    rsbank_t *none = NULL;
    if (!vt || !bank) return -1;
    return atomic_compare_exchange_strong_explicit(&vt->rs_bank, &none, bank, memory_order_release, memory_order_relaxed) ? 0 : -1;
}

int dsp_set_mg_bank(vt_t *vt, struct rsbank *bank)
{
    rsbank_t *none = NULL;
    if (!vt || !bank) return -1;
    return atomic_compare_exchange_strong_explicit(&vt->mg_bank, &none, bank, memory_order_release, memory_order_relaxed) ? 0 : -1;
}

struct loopsave *dsp_loopsave(vt_t *vt)
{
    return vt ? &vt->ls : NULL;
}

struct rsbank *dsp_swap_rs_bank(vt_t *vt, struct rsbank *bank)
{
    if (!vt) return NULL;
    rsbank_t *old = atomic_exchange_explicit(&vt->rs_bank, bank, memory_order_acq_rel);
    atomic_fetch_add_explicit(&vt->bank_gen, 1u, memory_order_release);
    return old;
}

void dsp_bank_renamed(vt_t *vt, int mangler, const char *old_name, const char *new_name)
{
    int none = 0;
    if (!vt || !old_name || !new_name) return;
    if (!atomic_compare_exchange_strong_explicit(&vt->ren_state, &none, 1, memory_order_acquire, memory_order_relaxed)) return;
    vt->ren_mg = mangler != 0;
    snprintf(vt->ren_old, sizeof vt->ren_old, "%s", old_name);
    snprintf(vt->ren_new, sizeof vt->ren_new, "%s", new_name);
    atomic_store_explicit(&vt->ren_state, 2, memory_order_release);
}

struct rsbank *dsp_swap_mg_bank(vt_t *vt, struct rsbank *bank)
{
    if (!vt) return NULL;
    rsbank_t *old = atomic_exchange_explicit(&vt->mg_bank, bank, memory_order_acq_rel);
    atomic_fetch_add_explicit(&vt->bank_gen, 1u, memory_order_release);
    return old;
}

int dsp_set_demo_pcm(vt_t *vt, struct demo_pcm *pcm)
{
    demo_pcm_t *none = NULL;
    if (!vt || !pcm) return -1;
    return atomic_compare_exchange_strong_explicit(&vt->demo_pcm, &none, pcm, memory_order_release, memory_order_relaxed) ? 0 : -1;
}

/* The preset list the Presets page shows: Mangler's while the engine is Mangler, else Resynth's. */
static const rsbank_t *rs_bank_of(vt_t *vt)
{
    return atomic_load_explicit((int)vt->p[VT_P_ENGINE] == VT_ENGINE_MANGLER ? &vt->mg_bank : &vt->rs_bank, memory_order_acquire);
}

static int ieq(const char *a, const char *b);

/* the name of bank bn in this engine's list (before the list is in: the fixed names) */
static const char *rs_bank_name_at(const rsbank_t *bk, int bn)
{
    if (bn < 0 || bn >= RSBANK_SLOTS) return k_rs_bank_opts[0];
    if (bk) return bn < bk->nbanks ? bk->name[bn] : k_rs_bank_opts[0];
    return bn < RSBANK_BANKS ? k_rs_bank_opts[bn] : k_rs_bank_opts[0];
}

/* the Presets page is on bank bn: the index, and its name (what a restore, a changed list and the other engine look it up by) */
static void rs_set_bank(vt_t *vt, const rsbank_t *bk, int bn)
{
    vt->p[VT_P_RS_BANK] = (float)bn;
    snprintf(vt->rs_bank_cur, sizeof vt->rs_bank_cur, "%s", rs_bank_name_at(bk, bn));
    vt->rs_bank_want[0] = '\0';          /* (a state applied just now may have named a bank: this is where the page is) */
}

/* the index of rs_name in bank bn, -1 if it isn't there */
static int rs_find_name(vt_t *vt, int bn)
{
    const rsbank_t *bk = rs_bank_of(vt);
    if (!bk || !vt->rs_name[0] || bn < 0 || bn >= RSBANK_SLOTS) return -1;
    for (int i = 0; i < bk->count[bn]; i++)
        if (strcmp(bk->e[bn][i].name, vt->rs_name) == 0) return i;
    return -1;
}

/* after a restore: point the Presets page at the saved preset once the bank is there (nothing is loaded) */
static void rs_resolve(vt_t *vt)
{
    const rsbank_t *bk = rs_bank_of(vt);
    if (!bk) return;
    const unsigned g = atomic_load_explicit(&vt->bank_gen, memory_order_acquire);
    if (g != vt->bank_seen) {                  /* a list was swapped (a bank made, renamed, removed): keep the place by its name */
        vt->bank_seen = g;
        if (atomic_load_explicit(&vt->ren_state, memory_order_acquire) == 2) {      /* a rename: the page follows its bank */
            if (vt->ren_mg == ((int)vt->p[VT_P_ENGINE] == VT_ENGINE_MANGLER) && ieq(vt->rs_bank_cur, vt->ren_old))
                snprintf(vt->rs_bank_cur, sizeof vt->rs_bank_cur, "%s", vt->ren_new);
            atomic_store_explicit(&vt->ren_state, 0, memory_order_release);
        }
        if (!vt->rs_bank_want[0]) snprintf(vt->rs_bank_want, sizeof vt->rs_bank_want, "%s", vt->rs_bank_cur);
        vt->rs_find = 1;
    }
    if (!vt->rs_find) return;
    vt->rs_find = 0;
    int bn = (int)vt->p[VT_P_RS_BANK];
    if (vt->rs_bank_want[0]) {                 /* by name: this engine's list may not have it (then Default) */
        int f = -1;
        for (int k = 0; k < bk->nbanks && f < 0; k++) if (ieq(bk->name[k], vt->rs_bank_want)) f = k;
        bn = f >= 0 ? f : 0;
        vt->rs_bank_want[0] = '\0';
    } else if (bn < 0 || bn >= bk->nbanks) {
        bn = 0;
    }
    rs_set_bank(vt, bk, bn);
    const int i = rs_find_name(vt, bn);
    vt->p[VT_P_RS_PRESET] = i >= 0 ? (float)i : 0.0f;
}

/* ---------------------------------------------------------------- params */

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int apply_state(vt_t *vt, const char *json);

/* ---------------------------------------------------------------- Resynth wavetable (hm_wt) */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* hm_wt: base64 of VT_HM_WT_N little-endian int16 samples (one cycle), optionally followed by ":<len>", the length
 * of the the reference table it was made from (2..4096) before it was zero-padded and resampled: the reference plays the
 * harmonics from that length up flat (hm_set_wavetable_len). "" clears it. */
static int set_hm_wt(vt_t *vt, const char *s, size_t n)
{
    unsigned char raw[2 * VT_HM_WT_N];
    size_t got = 0, nb = n;
    unsigned acc = 0;
    int bits = 0, len = 0;
    if (n == 0) { vt->hm_wt_valid = 0; vt->hm_wt_len = 0; vt->hm_wt_str[0] = '\0'; vt->hm_wt_gen++; return 0; }
    if (n >= VT_HM_WT_STR) return -1;
    for (size_t i = 0; i < n; i++)
        if (s[i] == ':') {
            nb = i;
            if (i + 1 >= n || n - i - 1 > 4) return -1;
            for (size_t j = i + 1; j < n; j++) {
                if (s[j] < '0' || s[j] > '9') return -1;
                len = 10 * len + (s[j] - '0');
            }
            if (len < 2 || len > 4096) return -1;
            break;
        }
    for (size_t i = 0; i < nb; i++) {
        if (s[i] == '=') break;
        int v = b64_val(s[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (got < sizeof raw) raw[got++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    if (got != sizeof raw) return -1;
    for (int i = 0; i < VT_HM_WT_N; i++) {
        int16_t v = (int16_t)(uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
        vt->hm_wt[i] = (float)v / 32767.0f;
    }
    memcpy(vt->hm_wt_str, s, n);
    vt->hm_wt_str[n] = '\0';
    vt->hm_wt_valid = 1;
    vt->hm_wt_len = len;
    vt->hm_wt_gen++;
    return 0;
}

/* the 12-note mask the Resynth tuner uses: Key + Scale, or the preset's own (Custom) */
static unsigned hm_mask_of(const vt_t *vt)
{
    const int sc = (int)vt->p[VT_P_HM_SCALE], key = (int)vt->p[VT_P_HM_KEY];
    if (sc == HM_SCALE_CUSTOM) return (unsigned)lrintf(vt->p[VT_P_HM_MASK]) & 0xFFFu;
    const unsigned base = sc == 0 ? 0xAB5u : sc == 1 ? 0x5ADu : 0xFFFu;   /* major, natural minor, chromatic */
    return ((base << key) | (base >> (12 - key))) & 0xFFFu;
}

static int latch_on(const vt_t *vt);

/* Push Resynth's parameters, wave, scale, tempo and pads into the engine (once per block). */
static void hm_sync(vt_t *vt)
{
    hm_t *h = &vt->hm;
    for (int i = 0; i < VT_HM_COUNT; i++) h->p[i] = vt->p[VT_P_HM_FIRST + i];
    h->autolevel = (int)vt->p[VT_P_HM_LEVEL];
    h->midi_idle_tune = (int)vt->p[VT_P_HM_IDLE] == 0;
    hm_set_mask(h, hm_mask_of(vt));
    hm_set_bpm(h, vt->bpm);
    const int wave = (int)vt->p[VT_P_HM_WAVE];
    if (wave != vt->hm_wave_set || (wave == HM_WAVE_PRESET && vt->hm_wt_gen != vt->hm_wt_gen_set)) {
        float cyc[VT_HM_WT_N];
        for (int i = 0; i < VT_HM_WT_N; i++) {
            const float ph = (float)i / (float)VT_HM_WT_N;
            switch (wave) {
            case 1:  cyc[i] = ph < 0.5f ? 1.0f : -1.0f; break;                                        /* square */
            case 2:  cyc[i] = ph < 0.5f ? -1.0f + 4.0f * ph : 3.0f - 4.0f * ph; break;               /* triangle */
            case 3:  cyc[i] = sinf(6.2831853f * ph); break;                                           /* sine */
            case HM_WAVE_PRESET: cyc[i] = vt->hm_wt_valid ? vt->hm_wt[i] : -1.0f + 2.0f * ph; break;
            default: cyc[i] = -1.0f + 2.0f * ph; break;                                               /* saw (the reference's INIT) */
            }
        }
        hm_set_wavetable(h, cyc, VT_HM_WT_N);
        hm_set_wavetable_len(h, wave == HM_WAVE_PRESET && vt->hm_wt_valid ? vt->hm_wt_len : 0);
        vt->hm_wave_set = wave;
        vt->hm_wt_gen_set = vt->hm_wt_gen;
    }
    /* pads, oldest first, so the engine's newest two are the newest held (MIDI mode) */
    int held[VT_MAX_VOICES], n = 0;
    if (latch_on(vt)) {                                          /* Latch: the buffer holds the notes after the pads are let go */
        for (int i = 0; i < VT_MAX_VOICES; i++) if (vt->latch_note[i] >= 0) held[n++] = vt->latch_note[i];
    } else
    for (uint32_t age = 0;;) {
        int pick = -1;
        for (int i = 0; i < VT_MAX_VOICES; i++)
            if (vt->voices[i].note >= 0 && vt->voices[i].age > age && (pick < 0 || vt->voices[i].age < vt->voices[pick].age)) pick = i;
        if (pick < 0) break;
        held[n++] = vt->voices[pick].note;
        age = vt->voices[pick].age;
    }
    int same = n == vt->hm_nheld;
    for (int i = 0; same && i < n; i++) same = held[i] == vt->hm_held[i];
    if (!same) {
        hm_all_notes_off(h);
        for (int i = 0; i < n; i++) hm_note_on(h, held[i]);
        memcpy(vt->hm_held, held, sizeof held);
        vt->hm_nheld = n;
    }
    hm_begin(h);
}

/* Case-insensitive equality: Schwung's Mode page hands back a mode's name in the
 * spelling it matched (level names are lower case), which must still select it. */
static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        const int x = (unsigned char)*a, y = (unsigned char)*b;
        if ((x >= 'A' && x <= 'Z' ? x + 32 : x) != (y >= 'A' && y <= 'Z' ? y + 32 : y)) return 0;
    }
    return *a == *b;
}

/*
 * The OLED widget's feed (canvas.js reads it as an extra_key, ~5 Hz):
 *   "S,<mode>,<freeze>,<loop phase 0..1>,<loop s>,<f0 Hz>,<target Hz>,<cents>,<b0>,...,<b15>,
 *    (freeze: 0 live, 1 looping, 2 recording, 3 armed for the downbeat)
 *    <auto gain dB>,<held pad note or -1>,<note-on count mod 1000>,<transport beat or -1>,<bar s>"
 * Pitch modes fill f0/target/cents (Tape: target = the dropped output pitch);
 * Robot fills the 16 analysis bands (0..255 each). Built only when asked: nothing extra runs
 * per sample for it.
 */
static int newest_held_note(const vt_t *vt);
static int shown_note(const vt_t *vt);

/* Which engine family a mode runs on. */
static int has_bands(int mode) { return mode == VT_MODE_ROBOT || mode == VT_MODE_NIGHTCALL; }   /* the vocoders */
static int is_vox(int mode) { return mode == VT_MODE_DALEK || mode == VT_MODE_HARMONY || mode == VT_MODE_WALKIE; }

static int format_scope(vt_t *vt, char *buf, int buf_len)
{
    const int mode = (int)vt->p[VT_P_MODE];
    const float sr = (float)vt->sample_rate;
    float f0 = 0.0f, tgt = 0.0f, cents = 0.0f;
    int bands[VT_NUM_BANDS] = {0};

    if (has_bands(mode)) {
        for (int i = 0; i < VT_NUM_BANDS; i++) bands[i] = (int)clampf(vt->viz_bands[i] * 255.0f, 0.0f, 255.0f);
    } else if (is_vox(mode) && mode != VT_MODE_HARMONY) {
        /* no tracker running: nothing pitched to show */
    } else {
        f0 = vt->cher.f0;
        tgt = vt->cher.target_hz;
        cents = vt->cher.cents_err;
        if (mode == VT_MODE_TAPE) {
            float drop = -(1.0f - vt->p[VT_P_CHAR1]) * 12.0f;
            tgt = f0 > 0.0f ? f0 * exp2f(drop / 12.0f) : 0.0f;
            cents = drop * 100.0f;
        }
    }

    int n = snprintf(buf, (size_t)buf_len, "S,%d,%d,%.3f,%.3f,%.1f,%.1f,%.0f", mode,
                     freeze_recording(&vt->freeze) ? 2 : freeze_armed(&vt->freeze) ? 3 :
                     freeze_looping(&vt->freeze) ? 1 : 0,
                     freeze_phase(&vt->freeze),
                     freeze_loop_seconds(&vt->freeze, sr), f0, tgt, cents);
    for (int i = 0; i < VT_NUM_BANDS && n > 0 && n < buf_len; i++)
        n += snprintf(buf + n, (size_t)(buf_len - n), ",%d", bands[i]);
    /* Auto's input boost in dB, the newest held pad (-1 none) and how many
     * note-ons have arrived: tells you on screen whether pads reach the FX. */
    if (n > 0 && n < buf_len)
        n += snprintf(buf + n, (size_t)(buf_len - n), ",%.1f,%d,%u",
                      20.0f * log10f(vt->agc_gain), shown_note(vt), vt->midi_note_ons % 1000u);
    /* the host transport, beats (-1 = stopped / no clock): Freeze's banner
     * counts down to the downbeat with it, and shows when there is no clock */
    if (n > 0 && n < buf_len)
        n += snprintf(buf + n, (size_t)(buf_len - n), ",%.2f", vt->beat >= 0.0 ? vt->beat : -1.0);
    /* one bar (4 beats) at the host tempo, seconds: the banner numbers the bars of a take */
    if (n > 0 && n < buf_len)
        n += snprintf(buf + n, (size_t)(buf_len - n), ",%.4f", 240.0f / vt->bpm);
    return n;
}


/* What char1 / char2 / mod / char4 / char5 do in each mode: the knob names the
 * UI shows (8 characters or fewer, to fit the OLED cell). */
static const char *const k_macro_names[VT_MODE_COUNT][VT_MACROS] = {
    [VT_MODE_ROBOT]     = { "Shift",   "Noise",    "Ensemble", "Beauty",  "Glide"   },
    [VT_MODE_SNAP]      = { "Formant", "Sens",     "Vibrato",  "Retune",  "Beauty"  },
    [VT_MODE_CHORDS]    = { "Level",   "Voicing",  "Humanize", "Retune",  "Beauty"  },
    [VT_MODE_PRISM]     = { "Voicing", "Warmth",   "Humanize", "Refract", "Shimmer" },
    [VT_MODE_TAPE]      = { "Drop",    "Flutter",  "Saturate", "Tone",    "Hiss"    },
    [VT_MODE_NIGHTCALL] = { "Formant", "Breath",   "Rasp",     "Duty",    "Cavity"  },
    [VT_MODE_SCREAM]    = { "Pitch",   "Throat",   "Grit",     "Narrow",  "Line"    },
    [VT_MODE_DALEK]     = { "Shift",   "Ring",     "Metal",    "Drive",   "Width"   },
    [VT_MODE_HARDTUNE]  = { "Amount",  "Formant",  "Vibrato",  "Glide",   "Sens"    },
    [VT_MODE_HARMONY]   = { "Lead",    "Stack",    "Detune",   "Grain",   "Micro"   },
    [VT_MODE_WALKIE]    = { "Peak",    "Drive",    "Static",   "Squelch", "Band"    },
    [VT_MODE_MORPH]     = { "Throat",  "Drive",    "Wobble",   "Retune",  "Glide"   },
    [VT_MODE_DOUBLE]    = { "Detune",  "Width",    "Drift",    "Retune",  "Glide"   },
};

/* char4 / char5 in the older modes are not new controls: they are the param of
 * the same name, scaled to 0..1 (the knob on the VOICE page and the one on its
 * own page move together: one value). -1 = the macro's own storage. The
 * aliased param always acts in that mode (vt_param_acts), checked by a test. */
#define SLOT_OWN (-1)
static const int k_slot_alias[VT_MODE_COUNT][2] = {
    [VT_MODE_ROBOT]     = { VT_P_BEAUTY, VT_P_GLIDE },
    [VT_MODE_SNAP]      = { VT_P_RETUNE, VT_P_BEAUTY },
    [VT_MODE_CHORDS]    = { VT_P_RETUNE, VT_P_BEAUTY },
    [VT_MODE_PRISM]     = { SLOT_OWN,    SLOT_OWN },
    [VT_MODE_TAPE]      = { VT_P_TONE,   SLOT_OWN },
    [VT_MODE_NIGHTCALL] = { SLOT_OWN,    SLOT_OWN },
    [VT_MODE_SCREAM]    = { SLOT_OWN,    SLOT_OWN },
    [VT_MODE_DALEK]     = { SLOT_OWN,    SLOT_OWN },
    [VT_MODE_HARDTUNE]  = { VT_P_GLIDE,  SLOT_OWN },
    [VT_MODE_HARMONY]   = { SLOT_OWN,    SLOT_OWN },
    [VT_MODE_WALKIE]    = { SLOT_OWN,    SLOT_OWN },
    [VT_MODE_MORPH]     = { VT_P_RETUNE, VT_P_GLIDE },
    [VT_MODE_DOUBLE]    = { VT_P_RETUNE, VT_P_GLIDE },
};

/* The param char4 (slot 0) / char5 (slot 1) is an alias of in `mode`, or SLOT_OWN. */
static int slot_alias(int mode, int id)
{
    if (mode < 0 || mode >= VT_MODE_COUNT) return SLOT_OWN;
    return k_slot_alias[mode][id == VT_P_CHAR4 ? 0 : 1];
}

static float slot_norm(const vt_t *vt, int alias)
{
    const vt_param_def *d = &k_params[alias];
    return (vt->p[alias] - d->min) / (d->max - d->min);
}

/* The effective 0..1 value of char4 / char5 for the engines. */
static float slot_value(const vt_t *vt, int id)
{
    const int a = slot_alias((int)vt->p[VT_P_MODE], id);
    return a >= 0 ? slot_norm(vt, a) : vt->p[id];
}

const char *dsp_macro_name(int mode, int which)
{
    if (mode < 0 || mode >= VT_MODE_COUNT || which < 0 || which >= VT_MACROS) return NULL;
    return k_macro_names[mode][which];
}

/* chain_params for the current mode: the generated JSON with its @1@..@5@
 * name placeholders (char1, char2, mod, char4, char5) filled in. Returns bytes written, or
 * -1 if it doesn't fit. */
static int format_chain_params(const vt_t *vt, char *buf, int buf_len)
{
    const int mode = (int)vt->p[VT_P_MODE];
    int n = 0;
    for (size_t part = 0; part < sizeof VT_CHAIN_PARAMS_PARTS / sizeof VT_CHAIN_PARAMS_PARTS[0]; part++) {
        for (const char *t = VT_CHAIN_PARAMS_PARTS[part]; *t; t++) {
            const char *sub = NULL;
            if (t[0] == '@' && t[1] >= '1' && t[1] <= '5' && t[2] == '@') {
                sub = k_macro_names[mode][t[1] - '1'];
                t += 2;
            }
            if (sub) {
                int k = (int)strlen(sub);
                if (n + k >= buf_len) return -1;
                memcpy(buf + n, sub, (size_t)k);
                n += k;
            } else {
                if (n + 1 >= buf_len) return -1;
                buf[n++] = *t;
            }
        }
    }
    buf[n] = '\0';
    return n;
}

/* A mode change brings that mode's own five knobs: the running mode's are filed away, the new mode's (its defaults
 * the first time) are loaded into p[]. Called wherever the mode can change: the Mode knob, a state restore, and at
 * the start of an audio block (the Demo tour steps the mode itself). */
static void macros_follow_mode(vt_t *vt)
{
    const int m = (int)vt->p[VT_P_MODE];
    if (m == vt->macro_mode || m < 0 || m >= VT_MODE_COUNT) return;
    if (vt->macro_mode >= 0 && vt->macro_mode < VT_MODE_COUNT)
        for (int k = 0; k < VT_MACROS; k++) vt->macro_store[vt->macro_mode][k] = vt->p[k_macro_ids[k]];
    for (int k = 0; k < VT_MACROS; k++) vt->p[k_macro_ids[k]] = vt->macro_store[m][k];
    vt->macro_mode = m;
}

/* "HardTune.char1": one mode's own knob, as a saved state names the ones that are not the running mode's. */
static int macro_key(const char *key, int *mode, int *which)
{
    static const char *const names[VT_MACROS] = { "char1", "char2", "mod", "char4", "char5" };
    const char *dot = strchr(key, '.');
    if (!dot) return 0;
    for (int m = 0; m < VT_MODE_COUNT; m++) {
        const size_t n = strlen(k_mode_opts[m]);
        if ((size_t)(dot - key) != n || strncmp(key, k_mode_opts[m], n) != 0) continue;
        for (int k = 0; k < VT_MACROS; k++) if (strcmp(dot + 1, names[k]) == 0) { *mode = m; *which = k; return 1; }
    }
    return 0;
}

/* The knobs the Showcase Tour turns (Vowel, Tract, Peak, Choir, Mix/Drv), and the preset browser, which would
 * load values over it: a write to any of them while the tour runs is the player's hand. */
static int tour_moves(int id)
{
    return id == VT_P_VF_VOWEL || id == VT_P_VF_TRACT || id == VT_P_VF_PEAK || id == VT_P_VF_CHOIR ||
           id == VT_P_VF_MIX || id == VT_P_VF_PRESET;
}

/* Resynth's Demo: the Default-bank presets it shows off, in order (a range: tuner, vocoder, harmony, synth, fx) */
#define RS_DEMO_N 5
static const char *const k_rs_demo[RS_DEMO_N] = { "Tune - Hard Snap", "Voco - Saw Choir", "Harm - Fifth Above", "Synth - Bell Tone", "Fx - Dark Choir" };

/* Load preset `i` of bank `bn` over Resynth's defaults (the Presets page, and the Demo tour). Hold Scale keeps
 * Key, Scale and Notes; Auto Level, Hold Scale, No Pad and Mic Gate are the player's and stay. */
static void rs_load(vt_t *vt, const rsbank_t *bk, int bn, int i)
{
    const int hold = (int)vt->p[VT_P_HM_HOLD] == 1;
    if ((int)vt->p[VT_P_ENGINE] == VT_ENGINE_MANGLER) {                /* Mangler: nothing of the last preset carries over either */
        for (int k = VT_P_MG_FIRST; k <= VT_P_MG_LAST; k++) vt->p[k] = k_params[k].def;
        mg_default_chords(&vt->mg);
        apply_state(vt, bk->e[bn][i].state);
        rs_set_bank(vt, bk, bn);
        snprintf(vt->rs_name, sizeof vt->rs_name, "%s", bk->e[bn][i].name);
        vt->rs_find = 0;
        return;
    }
    for (int k = VT_P_HM_FIRST; k <= VT_P_HM_WAVE; k++) {            /* nothing of the last preset carries over */
        if (k_params[k].transient || k == VT_P_HM_HOLD || k == VT_P_HM_LEVEL || k == VT_P_HM_IDLE || k == VT_P_HM_VGATE) continue;
        if (hold && (k == VT_P_HM_KEY || k == VT_P_HM_SCALE || k == VT_P_HM_MASK)) continue;
        vt->p[k] = k_params[k].def;
    }
    set_hm_wt(vt, "", 0);
    vt->hold_skip = hold && !strstr(bk->e[bn][i].state, "\"hm_scale_hold\":\"Off\"");
    apply_state(vt, bk->e[bn][i].state);
    vt->hold_skip = 0;
    rs_set_bank(vt, bk, bn);             /* (after the state: a preset saved from My Presets carries an rs_bank / rs_name of its own) */
    snprintf(vt->rs_name, sizeof vt->rs_name, "%s", bk->e[bn][i].name);
    vt->rs_find = 0;
}

int dsp_set_param(vt_t *vt, const char *key, const char *val)
{
    if (!vt || !key || !val) return -1;
    if (strcmp(key, "state") == 0) {
        /* Hold Scale: a state set after the module has been running is a preset recalled by hand (My Presets), and
         * keeps the Key / Scale / Notes in force. The restore at load (autosave, config) is not: it sets them. A state
         * that itself says Hold Scale Off applies in full. */
        vt->hold_skip = (int)vt->p[VT_P_HM_HOLD] == 1 && vt->warm_samples >= vt->sample_rate && !strstr(val, "\"hm_scale_hold\":\"Off\"");
        const int r = apply_state(vt, val);
        vt->hold_skip = 0;
        return r;
    }
    if (strcmp(key, "hm_wt") == 0) return set_hm_wt(vt, val, strlen(val));
    if (strcmp(key, "mg_chords") == 0) return mg_chords_parse(&vt->mg, val);
    if (strcmp(key, "auto") == 0) {
        /* The old Auto switch (sets saved before Gate took it in, tests):
         * On = Gate Auto, Off = back to the last manual level. */
        int on;
        if (strcmp(val, "On") == 0 || strcmp(val, "1") == 0) on = 1;
        else if (strcmp(val, "Off") == 0 || strcmp(val, "0") == 0) on = 0;
        else return -1;
        const int cur = (int)vt->p[VT_P_GATE];
        if (on && cur) { vt->gate_mem = cur; vt->p[VT_P_GATE] = 0.0f; }
        if (!on && !cur) vt->p[VT_P_GATE] = (float)vt->gate_mem;
        return 0;
    }
    if (strcmp(key, "gate") == 0) {
        /* Option names ("Auto", "-38 dB") and option indices (0 = Auto) as
         * any enum; a negative or fractional number is a level in dB (sets
         * saved when Gate was a dB knob, and tests). */
        int opt = -1;
        for (int i = 0; i < VT_GATE_OPTS; i++) if (strcmp(k_gate_opts[i], val) == 0) opt = i;
        if (opt < 0) {
            char *end;
            float f = strtof(val, &end);
            if (end == val || !isfinite(f)) return -1;
            while (*end == ' ') end++;
            if (*end != '\0' && strcmp(end, "dB") != 0) return -1;
            const int is_db = f < 0.0f || strchr(val, '.') != NULL || strstr(val, "dB") != NULL;
            opt = is_db ? gate_opt_of(f) : (int)clampf(f, 0.0f, (float)(VT_GATE_OPTS - 1));
        }
        vt->p[VT_P_GATE] = (float)opt;
        if (opt) vt->gate_mem = opt;
        return 0;
    }
    {   /* a saved state also names the knobs of the modes that were not running ("Snap.char1") */
        int mm, mk;
        if (macro_key(key, &mm, &mk)) {
            char *end;
            const float f = strtof(val, &end);
            if (end == val || !isfinite(f)) return -1;
            const float v = clampf(f, k_params[k_macro_ids[mk]].min, k_params[k_macro_ids[mk]].max);
            if (mm == vt->macro_mode) vt->p[k_macro_ids[mk]] = v; else vt->macro_store[mm][mk] = v;
            return 0;
        }
    }
    int id = dsp_param_index(key);
    if (id < 0) return -1;
    const vt_param_def *d = &k_params[id];

    /* A knob that does nothing in the current mode (vt_param_acts: Key, Scale,
     * Retune, Glide, Beauty) ignores turns, so its stored value waits unchanged
     * for the next mode that uses it. A restored state or config still sets it
     * (it may come before "mode" in the object), and so does any write in the
     * first second after load, in case the host restores a set key by key. */
    if (!vt->restoring && vt->warm_samples >= vt->sample_rate && !vt_param_acts(id, (int)vt->p[VT_P_MODE])) return 0;

    /* The screen shows the tour's values only while it runs: switching Demo or the engine ends that at once, so the
     * very next read of a knob is the user's own. */
    if (id == VT_P_DEMO || id == VT_P_ENGINE) vt->disp_on = 0;
    if (id == VT_P_ENGINE) {                     /* the Presets page lists the new engine's banks: its place by name (Default if this engine lacks that bank) */
        vt->rs_find = 1;
        if (!vt->restoring && !vt->rs_bank_want[0]) snprintf(vt->rs_bank_want, sizeof vt->rs_bank_want, "%s", vt->rs_bank_cur);
    }
    /* A hand on a knob the tour is turning (the host writes the knob it was turned from the value it was SHOWING, so
     * the player takes over from where the dial was) or on the preset browser: the tour steps aside. Demo goes Off,
     * the value written is the user's, and every other knob is back on the user's own value. A state restore is not
     * a hand; neither is a knob the tour leaves alone (Breath, Attack, Release). */
    /* A hand on Resynth (a knob, or the Presets page) pins its Demo tour on the preset it is playing. */
    if (!vt->restoring && vt->demo_active && (id == VT_P_RS_PRESET || id == VT_P_RS_BANK || (id >= VT_P_HM_FIRST && id <= VT_P_HM_WAVE) || (id >= VT_P_MG_FIRST && id <= VT_P_MG_LAST)))
        vt->demo_pinned = 1;
    if (!vt->restoring && vt->disp_on && vt->demo_active && tour_moves(id)) {
        vt->p[VT_P_DEMO] = 0.0f;
        vt->disp_on = 0;
    }

    if (id == VT_P_RS_BANK) {     /* a new list starts at the preset you have loaded if it is in it, else at its first (nothing is loaded) */
        const rsbank_t *bk = rs_bank_of(vt);
        const int nb = bk ? (bk->nbanks > 0 ? bk->nbanks : 1) : RSBANK_BANKS;
        int b = -1;
        for (int i = 0; i < nb && b < 0; i++) if (ieq(rs_bank_name_at(bk, i), val)) b = i;
        if (b < 0) {
            char *end;
            const long idx = strtol(val, &end, 10);
            if (end == val || *end != '\0') return -1;
            b = idx < 0 ? 0 : idx >= nb ? nb - 1 : (int)idx;
        }
        rs_set_bank(vt, bk, b);
        vt->rs_bank_want[0] = '\0';
        const int i = rs_find_name(vt, b);
        vt->p[VT_P_RS_PRESET] = i >= 0 ? (float)i : 0.0f;
        return 0;
    }
    if (id == VT_P_ENGINE && ieq(val, "VoiceFX")) val = "Synth Voice";   /* the old name, in sets and presets saved before the rename */
    if (id == VT_P_FREEZE) {      /* the old three-way Freeze (Off / Rec / Loop): its names still work, on the new pair of controls */
        if (strcmp(val, "Loop") == 0) val = "On";
        else if (strcmp(val, "Rec") == 0) { vt->p[VT_P_FREEZE_REC] = 1.0f; return 0; }
    }
    if (d->type == VT_PTYPE_ENUM) {
        for (int i = 0; i < d->num_options; i++) {
            if (strcmp(d->options[i], val) == 0) {
                vt->p[id] = (float)i;
                if (id == VT_P_MODE) { if (vt->demo_active) vt->demo_pinned = 1; macros_follow_mode(vt); }   /* stop the tour here */
                return 0;
            }
        }
        for (int i = 0; i < d->num_options; i++) {                    /* the host's own spelling of an option (case aside) */
            if (ieq(d->options[i], val)) {
                vt->p[id] = (float)i;
                if (id == VT_P_MODE) { if (vt->demo_active) vt->demo_pinned = 1; macros_follow_mode(vt); }
                return 0;
            }
        }
        char *end;
        long idx = strtol(val, &end, 10);
        if (end == val || *end != '\0') return -1;
        if (idx < 0) idx = 0;
        if (idx >= d->num_options) idx = d->num_options - 1;
        vt->p[id] = (float)idx;
        if (id == VT_P_MODE) { if (vt->demo_active) vt->demo_pinned = 1; macros_follow_mode(vt); }
        return 0;
    }

    char *end;
    float f = strtof(val, &end);
    if (end == val || !isfinite(f)) return -1;
    if (id == VT_P_RS_PRESET) {           /* Resynth's browser writes an index: that preset, over Resynth's defaults */
        const rsbank_t *bk = rs_bank_of(vt);
        const int bn = (int)vt->p[VT_P_RS_BANK];
        const int n = bk ? bk->count[bn] : 0;
        const int i = (int)lroundf(clampf(f, 0.0f, (float)(n > 0 ? n - 1 : 0)));
        if (n > 0) rs_load(vt, bk, bn, i);
        vt->p[VT_P_RS_PRESET] = (float)i;
        return 0;
    }
    if (id == VT_P_VF_PRESET) {                                   /* the browser writes an index: load that preset into the eight knobs */
        const vt_preset *ps = vt_preset_get(VT_ENGINE_VOICEFX, (int)lroundf(clampf(f, d->min, d->max)));
        if (!ps) return -1;
        vt->p[VT_P_VF_PRESET] = (float)(int)lroundf(clampf(f, d->min, d->max));
        for (int k = 0; k < VT_PRESET_VALUES; k++) vt->p[VT_P_VF_VOWEL + k] = ps->v[k];
        return 0;
    }
    vt->p[id] = clampf(f, d->min, d->max);
    if ((id == VT_P_CHAR4 || id == VT_P_CHAR5) && !vt->restoring) {     /* an alias moves the param it stands for */
        const int a = slot_alias((int)vt->p[VT_P_MODE], id);
        if (a >= 0) vt->p[a] = k_params[a].min + vt->p[id] * (k_params[a].max - k_params[a].min);
    }
    return 0;
}

int dsp_get_param(vt_t *vt, const char *key, char *buf, int buf_len)
{
    if (!vt || !key || !buf || buf_len <= 0) return -1;
    rs_resolve(vt);                     /* the browser's first reads already see the restored place */
    int n;

    if (strcmp(key, "state") == 0) {
        n = snprintf(buf, (size_t)buf_len, "{");
        for (int i = 0; i < VT_P_COUNT && n >= 0 && n < buf_len; i++) {
            const vt_param_def *d = &k_params[i];
            if (d->transient) continue;                         /* performance toggles aren't saved */
            int w = d->type == VT_PTYPE_ENUM
                  ? snprintf(buf + n, (size_t)(buf_len - n), "%s\"%s\":\"%s\"", i ? "," : "", d->key, d->options[(int)vt->p[i]])
                  : snprintf(buf + n, (size_t)(buf_len - n), "%s\"%s\":%g", i ? "," : "", d->key, (double)vt->p[i]);
            if (w < 0) return -1;
            n += w;
        }
        /* the knobs of the other modes, only where they are not at their defaults (a set stays short) */
        {
            static const char *const names[VT_MACROS] = { "char1", "char2", "mod", "char4", "char5" };
            for (int m = 0; m < VT_MODE_COUNT && n >= 0 && n < buf_len; m++) {
                if (m == vt->macro_mode) continue;
                for (int k = 0; k < VT_MACROS && n >= 0 && n < buf_len; k++) {
                    const float v = vt->macro_store[m][k];
                    if (fabsf(v - k_params[k_macro_ids[k]].def) < 1e-6f) continue;
                    n += snprintf(buf + n, (size_t)(buf_len - n), ",\"%s.%s\":%g", k_mode_opts[m], names[k], (double)v);
                }
            }
        }
        if (vt->hm_wt_valid && n >= 0 && n < buf_len)                /* a preset's wavetable */
            n += snprintf(buf + n, (size_t)(buf_len - n), ",\"hm_wt\":\"%s\"", vt->hm_wt_str);
        if (n >= 0 && n < buf_len - MG_CHORDS_STR - 16) {            /* Mangler's chord slots */
            char cs[MG_CHORDS_STR];
            if (mg_chords_format(&vt->mg, cs, sizeof cs) >= 0) n += snprintf(buf + n, (size_t)(buf_len - n), ",\"mg_chords\":\"%s\"", cs);
        }
        if (vt->rs_name[0] && n >= 0 && n < buf_len) {               /* the Presets page's place (rs_resolve) */
            n += snprintf(buf + n, (size_t)(buf_len - n), ",\"rs_bank\":\"%s\",\"rs_name\":\"",
                          vt->rs_bank_cur[0] ? vt->rs_bank_cur : rs_bank_name_at(rs_bank_of(vt), (int)vt->p[VT_P_RS_BANK]));
            for (const char *c = vt->rs_name; *c && n >= 0 && n < buf_len - 2; c++) {
                if (*c == '"' || *c == '\\') buf[n++] = '\\';
                if ((unsigned char)*c >= 0x20) buf[n++] = *c;
            }
            if (n >= 0 && n < buf_len) n += snprintf(buf + n, (size_t)(buf_len - n), "\"");
        }
        if (n >= 0 && n < buf_len) n += snprintf(buf + n, (size_t)(buf_len - n), "}");
    } else if (strcmp(key, "hm_wt") == 0) {
        n = snprintf(buf, (size_t)buf_len, "%s", vt->hm_wt_str);
    } else if (strcmp(key, "mg_chords") == 0) {
        n = mg_chords_format(&vt->mg, buf, buf_len);
    } else if (strcmp(key, "diag") == 0) {
        /* For checking a real mic on the device: input RMS (10 ms, max over
         * ~1 s), voice gate open share, Robot's own gate / AGC / leveller,
         * output RMS (max over ~1 s), held notes, the MIDI channel listened
         * to (1-16, 0 = omni) and how many other-channel messages were dropped. */
        const daft_t *d = &vt->daft;
        n = snprintf(buf, (size_t)buf_len,
                     "in %.1f / floor %.1f dBFS, vgate %.0f%%, robot gate %.0f%%, agc %+.1f dB, lev %.2f, out %.1f dBFS, notes %d %d %d %d, latch %d %d %d %d, mode %d, midi ch %d, foreign %u",
                     10.0 * log10(vt->dg_in_pk + 1e-12), 10.0 * log10(vt->dg_in_min + 1e-12), 100.0 * vt->dg_open, 100.0 * vt->dg_ropen,
                     20.0 * log10(d->agc_gain + 1e-9), d->lev_gain, 10.0 * log10(vt->dg_out_pk + 1e-12),
                     vt->voices[0].note, vt->voices[1].note, vt->voices[2].note, vt->voices[3].note,
                     vt->latch_note[0], vt->latch_note[1], vt->latch_note[2], vt->latch_note[3], vt->run_mode,
                     vt->midi_ch + 1, vt->midi_foreign);
        /* the host's raw channel answer and the last note-ons (status/note/source) */
        if (n >= 0 && n < buf_len) n += snprintf(buf + n, (size_t)(buf_len - n), ", recv %d, last", vt->midi_ch_raw);
        for (int k = 0; k < 6 && k < vt->midi_log_n && n >= 0 && n < buf_len; k++) {
            const int *e = vt->midi_log[(vt->midi_log_n - 1 - k) % 6];
            n += snprintf(buf + n, (size_t)(buf_len - n), " %02x/%d/s%d", e[0], e[1], e[2]);
        }
    } else if (strcmp(key, "auto") == 0) {                        /* the old switch, read-only view of Gate */
        n = snprintf(buf, (size_t)buf_len, "%s", (int)vt->p[VT_P_GATE] == 0 ? "On" : "Off");
    } else if (strcmp(key, "vf_preset_count") == 0) {              /* the Voice FX level's preset browser (list_param / count_param / name_param) */
        n = snprintf(buf, (size_t)buf_len, "%d", vt_preset_count(VT_ENGINE_VOICEFX));
    } else if (strcmp(key, "vf_preset_name") == 0) {
        const vt_preset *ps = vt_preset_get(VT_ENGINE_VOICEFX, (int)vt->p[VT_P_VF_PRESET]);
        n = snprintf(buf, (size_t)buf_len, "%s", ps ? ps->name : "");
    } else if (strcmp(key, "rs_bank_name") == 0) {                 /* the Bank page's widget (canvas.js) */
        n = snprintf(buf, (size_t)buf_len, "%s", rs_bank_name_at(rs_bank_of(vt), (int)vt->p[VT_P_RS_BANK]));
    } else if (strcmp(key, "rs_bank_info") == 0) {                 /* the Remote UI's bank list: this engine's, yours from first_user */
        const rsbank_t *bk = rs_bank_of(vt);
        const int nb = bk ? bk->nbanks : RSBANK_BANKS;
        n = snprintf(buf, (size_t)buf_len, "{\"banks\":[");
        for (int i = 0; i < nb && n >= 0 && n < buf_len - 80; i++)
            n += snprintf(buf + n, (size_t)(buf_len - n), "%s\"%s\"", i ? "," : "", rs_bank_name_at(bk, i));
        if (n >= 0 && n < buf_len)
            n += snprintf(buf + n, (size_t)(buf_len - n), "],\"first_user\":%d,\"cur\":%d,\"max\":%d}",
                          bk ? bk->first_user : RSBANK_BANKS, (int)vt->p[VT_P_RS_BANK], RSBANK_USER_MAX);
    } else if (strcmp(key, "rs_preset_count") == 0) {              /* the Resynth level's preset browser */
        const rsbank_t *bk = rs_bank_of(vt);
        n = snprintf(buf, (size_t)buf_len, "%d", bk ? bk->count[(int)vt->p[VT_P_RS_BANK]] : 0);
    } else if (strcmp(key, "rs_preset_name") == 0 ||
               /* the header's name: on the drawn Presets page (a canvas preset_browser) the host runs its knob rotation as
                * well as the browser's, and that one reads `preset_name`; unanswered it cleared the name the browser had just
                * set, and the header flickered between the preset and the module. So, in Resynth, the very same string. */
               (strcmp(key, "preset_name") == 0 && ((int)vt->p[VT_P_ENGINE] == VT_ENGINE_RESYNTH || (int)vt->p[VT_P_ENGINE] == VT_ENGINE_MANGLER))) {
        const rsbank_t *bk = rs_bank_of(vt);
        const int i = (int)vt->p[VT_P_RS_PRESET];
        const int bn = (int)vt->p[VT_P_RS_BANK];
        n = snprintf(buf, (size_t)buf_len, "%s", !bk ? "(loading)" : bk->count[bn] == 0 ? "(no presets)" : i < bk->count[bn] ? bk->e[bn][i].name : "");
    } else if (strcmp(key, "voices") == 0) {
        n = snprintf(buf, (size_t)buf_len, "%d", dsp_active_voices(vt));
    } else if (strcmp(key, "name") == 0) {
        /* The chain editor polls this every frame; unanswered it logged ~70 read errors a second. */
        n = snprintf(buf, (size_t)buf_len, "TinyVoice");
    } else if (strcmp(key, "chain_params") == 0) {
        /* Answering this ourselves is what carries `viz` to the UI (the chain
         * host's fallback drops it), so the canvas.js widget can mount. */
        n = format_chain_params(vt, buf, buf_len);
    } else if (strcmp(key, "ui_hierarchy") == 0) {
        n = 0;
        for (size_t part = 0; part < sizeof VT_UI_HIERARCHY_PARTS / sizeof VT_UI_HIERARCHY_PARTS[0]; part++) {
            const int k = (int)strlen(VT_UI_HIERARCHY_PARTS[part]);
            if (n + k >= buf_len) { n = -1; break; }
            memcpy(buf + n, VT_UI_HIERARCHY_PARTS[part], (size_t)k);
            n += k;
        }
        if (n >= 0) buf[n] = '\0';
    } else if (strcmp(key, "is_loading") == 0) {
        n = snprintf(buf, (size_t)buf_len, "%s", vt->loading_blocks > 0 ? "1" : "0");
    } else if (strcmp(key, "pitch") == 0) {                       /* read-only readouts under the scope */
        n = snprintf(buf, (size_t)buf_len, "%.1f", (double)vt->viz_f0);
    } else if (strcmp(key, "target") == 0) {
        n = snprintf(buf, (size_t)buf_len, "%.1f", (double)vt->viz_target);
    } else if (strcmp(key, "cents") == 0) {
        n = snprintf(buf, (size_t)buf_len, "%.0f", (double)vt->viz_cents);
    } else if (strcmp(key, "scope") == 0) {
        n = format_scope(vt, buf, buf_len);
    } else if (strcmp(key, "viz") == 0) {
        if (!has_bands((int)vt->p[VT_P_MODE])) {                  /* pitch modes: tuning view */
            n = snprintf(buf, (size_t)buf_len, "c,%.2f,%.2f,%.1f",
                         vt->viz_f0, vt->viz_target, vt->viz_cents);
        } else {
            int off = snprintf(buf, (size_t)buf_len, "d");
            for (int i = 0; i < VT_NUM_BANDS && off > 0 && off < buf_len; i++) {
                int v = (int)clampf(vt->viz_bands[i] * 255.0f, 0.0f, 255.0f);
                off += snprintf(buf + off, (size_t)(buf_len - off), ",%d", v);
            }
            n = off;
        }
    } else {
        int id = dsp_param_index(key);
        if (id < 0) return -1;
        const vt_param_def *d = &k_params[id];
        if (d->type == VT_PTYPE_ENUM)
            n = snprintf(buf, (size_t)buf_len, "%s", d->options[(int)vt->p[id]]);
        else if (id == VT_P_CHAR4 || id == VT_P_CHAR5)
            n = snprintf(buf, (size_t)buf_len, "%g", (double)slot_value(vt, id));
        else if (vt->disp_on && (id == VT_P_VF_VOWEL || id == VT_P_VF_TRACT || id == VT_P_VF_PEAK || id == VT_P_VF_CHOIR || id == VT_P_VF_MIX))
            n = snprintf(buf, (size_t)buf_len, "%g", (double)vt->disp[id - VT_P_VF_VOWEL]);     /* the tour's value, for the screen (Breath, Attack and Release are not its to turn) */
        else
            n = snprintf(buf, (size_t)buf_len, "%g", (double)vt->p[id]);
    }
    return (n < 0 || n >= buf_len) ? -1 : n;
}

/* Flat JSON object -> dsp_set_param per member. Strings and bare numbers are
 * applied; nested objects/arrays are skipped. Returns 0 if the text was an
 * object, -1 otherwise. No allocation. */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *skip_string(const char *p)    /* p at opening quote */
{
    for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
    return *p ? p + 1 : p;
}

/* Resynth's Key, Scale and Notes: what Hold Scale keeps across presets */
static int is_scale_key(const char *key)
{
    return strcmp(key, "hm_key") == 0 || strcmp(key, "hm_scale") == 0 || strcmp(key, "hm_mask") == 0;
}

static int is_transient(const char *key)
{
    int id = dsp_param_index(key);
    return id >= 0 && k_params[id].transient;
}

/* A set saved before Gate took in Auto has "auto" and a dB "gate". Its Auto
 * is applied after everything else, so the manual level is remembered either
 * way and Auto On wins. */
static int apply_members(vt_t *vt, const char *json);

static int apply_state(vt_t *vt, const char *json)
{
    vt->restoring = 1;
    const int r = apply_members(vt, json);
    vt->restoring = 0;
    return r;
}

static int apply_members(vt_t *vt, const char *json)
{
    const char *p = skip_ws(json);
    if (*p != '{') return -1;
    p++;
    char legacy_auto[8] = "";
    for (;;) {
        char key[32], val[32];
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}' || *p == '\0') {
            if (legacy_auto[0]) dsp_set_param(vt, "auto", legacy_auto);
            return 0;
        }
        if (*p != '"') return -1;
        const char *ks = p + 1;
        p = skip_string(p);
        size_t kn = (size_t)(p - ks) - 1;
        if (kn >= sizeof key) kn = sizeof key - 1;
        memcpy(key, ks, kn); key[kn] = '\0';
        p = skip_ws(p);
        if (*p != ':') return -1;
        p = skip_ws(p + 1);

        if (*p == '"') {
            const char *vs = p + 1;
            p = skip_string(p);
            size_t vn = (size_t)(p - vs) - 1;
            if (strcmp(key, "hm_wt") == 0) { set_hm_wt(vt, vs, vn); continue; }   /* the one long value */
            if (strcmp(key, "mg_chords") == 0) {                     /* Mangler's chord slots: the other long one */
                char cs[MG_CHORDS_STR];
                const size_t cn = vn < sizeof cs ? vn : sizeof cs - 1;
                memcpy(cs, vs, cn); cs[cn] = '\0';
                mg_chords_parse(&vt->mg, cs);
                continue;
            }
            if (strcmp(key, "rs_name") == 0) {                       /* the Presets page's place: looked up when the bank is in */
                size_t o = 0;
                for (size_t j = 0; j < vn && o + 1 < sizeof vt->rs_name; j++) {
                    if (vs[j] == '\\' && j + 1 < vn) j++;
                    vt->rs_name[o++] = vs[j];
                }
                vt->rs_name[o] = '\0';
                vt->rs_find = 1;
                continue;
            }
            if (strcmp(key, "rs_bank") == 0) {                       /* by name: a fixed one now, any (yours too) when the list is in */
                char b[RSBANK_NAME];
                const size_t bn = vn < sizeof b ? vn : sizeof b - 1;
                memcpy(b, vs, bn); b[bn] = '\0';
                snprintf(vt->rs_bank_want, sizeof vt->rs_bank_want, "%s", b);
                snprintf(vt->rs_bank_cur, sizeof vt->rs_bank_cur, "%s", b);
                for (int j = 0; j < RSBANK_BANKS; j++) if (ieq(k_rs_bank_opts[j], b)) vt->p[VT_P_RS_BANK] = (float)j;
                vt->rs_find = 1;
                continue;
            }
            if (vn >= sizeof val) vn = sizeof val - 1;
            memcpy(val, vs, vn); val[vn] = '\0';
            if (strcmp(key, "auto") == 0) snprintf(legacy_auto, sizeof legacy_auto, "%s", val);
            else if (!is_transient(key) && !(vt->hold_skip && is_scale_key(key))) dsp_set_param(vt, key, val);
        } else if (*p == '{' || *p == '[') {
            int depth = 0;
            do {
                if (*p == '"') { p = skip_string(p); continue; }
                if (*p == '{' || *p == '[') depth++;
                else if (*p == '}' || *p == ']') depth--;
                p++;
            } while (*p && depth > 0);
        } else {
            size_t vn = 0;
            while (*p && *p != ',' && *p != '}' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') {
                if (vn < sizeof val - 1) val[vn++] = *p;
                p++;
            }
            val[vn] = '\0';
            if (strcmp(key, "auto") == 0) snprintf(legacy_auto, sizeof legacy_auto, "%s", val);
            else if (!is_transient(key) && !(vt->hold_skip && is_scale_key(key))) dsp_set_param(vt, key, val);
        }
    }
}

/* ------------------------------------------------------------------ MIDI */

int dsp_active_voices(const vt_t *vt)
{
    int n = 0;
    if (!vt) return 0;
    for (int i = 0; i < VT_MAX_VOICES; i++) n += vt->voices[i].note >= 0;
    return n;
}

/* Latch: the chord buffer, oldest first. A press with no pad physically
 * held starts a new chord (the old one is replaced at once); presses while
 * others are held add to it, the oldest dropping out past four notes. */
static void latch_clear(vt_t *vt)
{
    for (int i = 0; i < VT_MAX_VOICES; i++) vt->latch_note[i] = -1;
}

static void latch_add(vt_t *vt, int note, int vel)
{
    for (int i = 0; i < VT_MAX_VOICES; i++)
        if (vt->latch_note[i] == note) { vt->latch_vel[i] = vel; return; }
    int n = 0;
    while (n < VT_MAX_VOICES && vt->latch_note[n] >= 0) n++;
    if (n == VT_MAX_VOICES) {                                      /* full: drop the oldest */
        for (int i = 1; i < VT_MAX_VOICES; i++) { vt->latch_note[i - 1] = vt->latch_note[i]; vt->latch_vel[i - 1] = vt->latch_vel[i]; }
        n = VT_MAX_VOICES - 1;
    }
    vt->latch_note[n] = note;
    vt->latch_vel[n] = vel;
}

static int latch_on(const vt_t *vt) { return vt->p[VT_P_LATCH] >= 0.5f; }

/* The modes Latch applies to: the ones the pads play as chords. */
static int latch_mode(int mode)
{
    return mode == VT_MODE_ROBOT || mode == VT_MODE_CHORDS || mode == VT_MODE_PRISM ||
           mode == VT_MODE_NIGHTCALL || mode == VT_MODE_HARMONY || mode == VT_MODE_RESYNTH || mode == VT_MODE_MANGLER;   /* Resynth: MIDI mode, Mangler: Pads keep the pads */
}

static void note_on(vt_t *vt, int note, int vel)
{
    if (latch_on(vt)) {
        if (dsp_active_voices(vt) == 0) latch_clear(vt);           /* a new chord */
        latch_add(vt, note, vel);
    }
    int slot = 0;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < VT_MAX_VOICES; i++) {
        if (vt->voices[i].note == note) { slot = i; goto assign; }   /* retrigger */
    }
    for (int i = 0; i < VT_MAX_VOICES; i++) {
        if (vt->voices[i].note < 0) { slot = i; goto assign; }       /* free slot */
        if (vt->voices[i].age < oldest) { oldest = vt->voices[i].age; slot = i; }
    }
assign:
    vt->voices[slot].note = note;
    vt->voices[slot].velocity = vel;
    vt->voices[slot].age = ++vt->voice_clock;
}

static void note_off(vt_t *vt, int note)
{
    for (int i = 0; i < VT_MAX_VOICES; i++)
        if (vt->voices[i].note == note) vt->voices[i].note = -1;
}

void dsp_handle_midi(vt_t *vt, const uint8_t *msg, int len)
{
    if (!vt || !msg || len < 1) return;
    /* The chain hands over 3-byte messages; a 4-byte USB-MIDI packet
     * ([cable|CIN], status, d1, d2) is accepted too, by skipping its header. */
    if (len >= 4 && !(msg[0] & 0x80) && (msg[1] & 0x80)) { msg++; len--; }
    int status = msg[0] & 0xF0;
    int d1 = len > 1 ? msg[1] & 0x7F : 0;
    int d2 = len > 2 ? msg[2] & 0x7F : 0;

    const int src = vt->midi_src;
    vt->midi_src = -1;                                               /* this message only */
    if (status == 0x90 && d2 > 0) {                                  /* diag: who sent it */
        int *e = vt->midi_log[vt->midi_log_n++ % 6];
        e[0] = msg[0]; e[1] = d1; e[2] = src;
    }

    /* Only the slot's own MIDI. The chain host also broadcasts every track's
     * MIDI to every audio FX (Move's sequencer sends notes on muted tracks
     * too, with MIDI Out on): taken, those played Robot's carrier and
     * Chords/Prism voices. The slot's own notes come on their own path as
     * well, so broadcasts are dropped whole; and where the host names the
     * slot's channel (slot_recv_channel; it answers -2 for an FX on the
     * device), other channels are too. Notes, CCs (panic included), pitch
     * bend: all filtered. */
    if (status >= 0x80 && status <= 0xE0 &&
        (src == VT_MIDI_SOURCE_BROADCAST || (vt->midi_ch >= 0 && (msg[0] & 0x0F) != vt->midi_ch))) {
        vt->midi_foreign++;
        return;
    }

    if (status == 0x90 && d2 > 0) { note_on(vt, d1, d2); vt->midi_note_ons++; }
    else if (status == 0x80 || status == 0x90) note_off(vt, d1);
    else if (status == 0xB0 && (d1 == 120 || d1 == 123)) {            /* all sound/notes off */
        for (int i = 0; i < VT_MAX_VOICES; i++) vt->voices[i].note = -1;
        latch_clear(vt);                                              /* a panic releases a latch too */
    }
}

void dsp_set_midi_channel(vt_t *vt, int channel)
{
    if (!vt) return;
    vt->midi_ch_raw = channel;
    vt->midi_ch = channel >= 0 && channel <= 15 ? channel : -1;
}

void dsp_set_midi_source(vt_t *vt, int source)
{
    if (vt) vt->midi_src = source;
}

void dsp_set_bpm(vt_t *vt, float bpm)
{
    if (vt && bpm >= 20.0f && bpm <= 400.0f) vt->bpm = bpm;
}

void dsp_set_transport(vt_t *vt, double beat)
{
    if (vt) vt->beat = beat;
}

/* Most recently pressed note that is still held, or -1. */
static int newest_held_note(const vt_t *vt)
{
    int best = -1;
    uint32_t age = 0;
    for (int i = 0; i < VT_MAX_VOICES; i++)
        if (vt->voices[i].note >= 0 && vt->voices[i].age >= age) { age = vt->voices[i].age; best = vt->voices[i].note; }
    return best;
}

/* The pad the screen shows: the newest latched note while Latch holds the
 * chord in this mode, else the newest pad held. */
static int shown_note(const vt_t *vt)
{
    if (latch_on(vt) && latch_mode(vt->run_mode)) {
        for (int i = VT_MAX_VOICES - 1; i >= 0; i--) if (vt->latch_note[i] >= 0) return vt->latch_note[i];
        return -1;
    }
    return newest_held_note(vt);
}

/* ----------------------------------------------------------------- audio */

/* Transparent below 0.9; above, eases into a 1.0 ceiling (Gain goes to +12 dB). */
static inline float soft_limit(float x)
{
    float a = fabsf(x);
    if (a <= 0.9f) return x;
    return copysignf(0.9f + 0.1f * tanhf((a - 0.9f) * 10.0f), x);
}

/* Linear-interpolated read d samples behind write index w in a power-of-two ring. */
static float ring_read(const float *line, int mask, int w, float d)
{
    float pos = (float)w - d;
    float fl = floorf(pos);
    float frac = pos - fl;
    int i0 = (int)fl & mask;
    int i1 = (i0 + 1) & mask;
    return line[i0] + frac * (line[i1] - line[i0]);
}

/* The dry delay tracks the engine's latency exactly, except across a jump
 * (the shifter's latency steps up when the pitch falls): there it glides over
 * ~3 ms, so the dry signal doesn't step (a click whenever Mix < 100%). */
static inline void dry_follow(vt_t *vt, float target)
{
    float d = target - vt->dry_d;
    if (d > 1.0f || d < -1.0f) vt->dry_d += d * (1.0f - expf(-1.0f / (0.003f * (float)vt->sample_rate)));
    else vt->dry_d = target;
}

/* Held pads -> a vocoder's carrier voices. Move's pads sit around E5 and up,
 * where a carrier is above most of the voice's formants (no words come
 * through, just a high tone). Held pads keep their names and voicing but move
 * by whole octaves, together, until their mean is in C3..B4; the last fold is
 * kept while the mean stays within 2 st of that range. */
static void assign_carriers(vt_t *vt, daft_t *d, const int held[VT_MAX_VOICES], const int vel[VT_MAX_VOICES])
{
    int nh = 0, sum = 0;
    for (int s = 0; s < VT_MAX_VOICES; s++) if (held[s] >= 0) { sum += held[s]; nh++; }
    if (nh) {
        const float mean = (float)sum / (float)nh;
        const float m = mean + (float)vt->robot_fold;
        if (m < (float)(ROBOT_REG_LO - 2) || m >= (float)(ROBOT_REG_HI + 2)) {
            int f = 0;
            while (mean + (float)f >= (float)ROBOT_REG_HI) f -= 12;
            while (mean + (float)f < (float)ROBOT_REG_LO) f += 12;
            vt->robot_fold = f;
        }
    }
    for (int s = 0; s < VT_MAX_VOICES; s++)
        daft_set_voice(d, s, held[s] >= 0 ? held[s] + vt->robot_fold : -1, vel[s]);
}

/* The headroom stage of the new modes: linear up to 0.8 of the ceiling, then
 * eased in so the output never reaches it. */
static inline float headroom(float x)
{
    const float knee = 0.8f * TINYVOICE_OUTPUT_HEADROOM, span = TINYVOICE_OUTPUT_HEADROOM - knee;
    const float a = fabsf(x);
    if (a <= knee) return x;
    return copysignf(knee + span * tanhf((a - knee) / span), x);
}

/* Mangler's pads: the notes held (the Demo's chord, the latched chord or the pads), oldest first, moved by whole octaves
 * together until their mean is in C4's neighbourhood (Move's pads sit high; the reference's notes are offsets from C4). */
static int mg_pad_list(vt_t *vt, int demo_on, const int *demo_notes, int latched, int out[VT_MAX_VOICES])
{
    int n = 0;
    if (demo_on) {
        for (int s = 0; s < VT_MAX_VOICES; s++) if (demo_notes[s] >= 0) out[n++] = demo_notes[s];
    } else if (latched) {
        for (int s = 0; s < VT_MAX_VOICES; s++) if (vt->latch_note[s] >= 0) out[n++] = vt->latch_note[s];
    } else {
        for (uint32_t age = 0;;) {
            int pick = -1;
            for (int i = 0; i < VT_MAX_VOICES; i++)
                if (vt->voices[i].note >= 0 && vt->voices[i].age > age && (pick < 0 || vt->voices[i].age < vt->voices[pick].age)) pick = i;
            if (pick < 0) break;
            out[n++] = vt->voices[pick].note;
            age = vt->voices[pick].age;
        }
    }
    if (n) {
        int sum = 0;
        for (int i = 0; i < n; i++) sum += out[i];
        const float mean = (float)sum / (float)n;
        if (mean + (float)vt->mg_fold < 50.0f || mean + (float)vt->mg_fold >= 70.0f) {
            int f = 0;
            while (mean + (float)f >= 66.0f) f -= 12;
            while (mean + (float)f < 54.0f) f += 12;
            vt->mg_fold = f;
        }
        for (int i = 0; i < n; i++) out[i] += vt->mg_fold;
    }
    return n;
}

/* Push Mangler's controls, chord slot and pads into the engine (once per block), and set the tracker it runs on. */
static void mg_sync(vt_t *vt, const int *pads, int npads)
{
    mg_params q;
    memset(&q, 0, sizeof q);
    q.engine = (int)vt->p[VT_P_MG_SHIFT];
    q.pitch = vt->p[VT_P_MG_PITCH];
    q.snap = (int)vt->p[VT_P_MG_SNAP];
    q.unison = (int)lroundf(vt->p[VT_P_MG_VOICES]);
    q.detune = vt->p[VT_P_MG_DETUNE];
    q.detune_jitter = (int)vt->p[VT_P_MG_SCATTER];
    q.time_ms = vt->p[VT_P_MG_TIME];
    q.time_jitter = (int)vt->p[VT_P_MG_SMEAR];
    q.grain_hz = vt->p[VT_P_MG_GRAIN];
    q.grain_jitter = (int)vt->p[VT_P_MG_GJITTER];
    q.wide = (int)vt->p[VT_P_MG_WIDE];
    q.formant = vt->p[VT_P_MG_FORMANT];
    q.fix = (int)vt->p[VT_P_MG_FIX];
    q.filter_on = (int)vt->p[VT_P_MG_FILTER];
    q.filter_hz = vt->p[VT_P_MG_FREQ];
    q.filter_oct = vt->p[VT_P_MG_WIDTH];
    q.mix = vt->p[VT_P_MG_MIX];
    q.volume_db = vt->p[VT_P_MG_VOLUME];
    q.note_mode = (int)vt->p[VT_P_MG_NOTES];
    q.mono = (int)vt->p[VT_P_MG_MONO];
    mg_set_pads(&vt->mg, pads, npads);
    mg_select_chord(&vt->mg, (int)vt->p[VT_P_MG_CHORD]);
    mg_set(&vt->mg, &q);
    cher_set_params(&vt->cher, 0, 0, 0.0f, -60.0f, -1, 0.0f);
    cher_set_auto(&vt->cher, 1);
    cher_set_clarity(&vt->cher, 0.65f);
}

void dsp_process_block_f(vt_t *vt, float *lr, int frames)
{
    if (vt) rs_resolve(vt);
    if (!vt || !lr || frames <= 0) return;

    const float sr = (float)vt->sample_rate;

    /* Demo: on switch-on remember the user's mode; while on, step through
     * every mode, 4 bars each, unless the Mode knob was turned, which pins the
     * tour on that mode; on switch-off put the user's mode back (or keep the
     * pinned one: that was a choice). */
    const int demo_on = (int)vt->p[VT_P_DEMO];
    if (demo_on && !vt->demo_active) {
        vt->demo_active = 1;
        vt->demo_pinned = 0;
        vt->demo_saved_mode = (int)vt->p[VT_P_MODE];
        vt->demo_time = 0.0;
        demo_reset(&vt->demo);
        groove_reset(&vt->groove);
        vt->demo_pos = 0.0;
        vt->demo_rs_snap = 0;
        vt->demo_rs_step = -1;
        if ((int)vt->p[VT_P_ENGINE] == VT_ENGINE_RESYNTH) {          /* the player's own Resynth sound, to come back to */
            memcpy(vt->demo_rs_p, &vt->p[VT_P_HM_FIRST], sizeof vt->demo_rs_p);
            memcpy(vt->demo_rs_wt, vt->hm_wt_str, sizeof vt->demo_rs_wt);
            vt->demo_rs_wt_valid = vt->hm_wt_valid;
            vt->demo_rs_bank = (int)vt->p[VT_P_RS_BANK];
            vt->demo_rs_idx = (int)vt->p[VT_P_RS_PRESET];
            memcpy(vt->demo_rs_name, vt->rs_name, sizeof vt->demo_rs_name);
            vt->demo_rs_snap = 1;
        }
        vt->demo_mg_snap = 0;
        vt->demo_mg_step = -1;
        if ((int)vt->p[VT_P_ENGINE] == VT_ENGINE_MANGLER) {          /* the player's own Mangler sound, to come back to */
            memcpy(vt->demo_mg_p, &vt->p[VT_P_MG_FIRST], sizeof vt->demo_mg_p);
            mg_chords_format(&vt->mg, vt->demo_mg_chords, sizeof vt->demo_mg_chords);
            memcpy(vt->demo_rs_name, vt->rs_name, sizeof vt->demo_rs_name);
            vt->demo_rs_bank = (int)vt->p[VT_P_RS_BANK];
            vt->demo_rs_idx = (int)vt->p[VT_P_RS_PRESET];
            vt->demo_mg_snap = 1;
        }
    } else if (!demo_on && vt->demo_active) {
        vt->demo_active = 0;
        if (!vt->demo_pinned) vt->p[VT_P_MODE] = (float)vt->demo_saved_mode;
        if (vt->demo_rs_snap && !vt->demo_pinned) {                  /* (pinned: the preset or knob the player stopped on is theirs) */
            memcpy(&vt->p[VT_P_HM_FIRST], vt->demo_rs_p, sizeof vt->demo_rs_p);
            if (vt->demo_rs_wt_valid) set_hm_wt(vt, vt->demo_rs_wt, strlen(vt->demo_rs_wt)); else set_hm_wt(vt, "", 0);
            rs_set_bank(vt, rs_bank_of(vt), vt->demo_rs_bank);
            vt->p[VT_P_RS_PRESET] = (float)vt->demo_rs_idx;
            memcpy(vt->rs_name, vt->demo_rs_name, sizeof vt->rs_name);
        }
        vt->demo_rs_snap = 0;
        if (vt->demo_mg_snap && !vt->demo_pinned) {
            memcpy(&vt->p[VT_P_MG_FIRST], vt->demo_mg_p, sizeof vt->demo_mg_p);
            mg_chords_parse(&vt->mg, vt->demo_mg_chords);
            rs_set_bank(vt, rs_bank_of(vt), vt->demo_rs_bank);
            vt->p[VT_P_RS_PRESET] = (float)vt->demo_rs_idx;
            memcpy(vt->rs_name, vt->demo_rs_name, sizeof vt->rs_name);
        }
        vt->demo_mg_snap = 0;
        vt->demo_pinned = 0;
    }
    int demo_chord_on = 0, demo_notes[VT_MAX_VOICES];
    if (vt->demo_active) {
        const double bar = 240.0 / (double)vt->bpm;                 /* 4 beats */
        if (!vt->demo_pinned && (int)vt->p[VT_P_ENGINE] == VT_ENGINE_VOCODER)
            vt->p[VT_P_MODE] = (float)((int)(vt->demo_time / (4.0 * bar)) % VT_MODE_COUNT);
        if (!vt->demo_pinned && vt->demo_rs_snap && (int)vt->p[VT_P_ENGINE] == VT_ENGINE_RESYNTH) {
            /* Resynth: five Default presets, four bars each, the Presets page following along */
            const rsbank_t *bk = rs_bank_of(vt);
            const int step = (int)(vt->demo_time / (4.0 * bar)) % RS_DEMO_N;
            if (bk && step != vt->demo_rs_step) {
                vt->demo_rs_step = step;
                for (int i = 0; i < bk->count[0]; i++)
                    if (strcmp(bk->e[0][i].name, k_rs_demo[step]) == 0) {
                        rs_load(vt, bk, 0, i);
                        rs_set_bank(vt, bk, 0);
                        vt->p[VT_P_RS_PRESET] = (float)i;
                        break;
                    }
            }
        }
        if (!vt->demo_pinned && vt->demo_mg_snap && (int)vt->p[VT_P_ENGINE] == VT_ENGINE_MANGLER) {
            /* Mangler: the first presets of the Default bank, four bars each */
            const rsbank_t *bk = rs_bank_of(vt);
            const int cnt = bk ? (bk->count[0] < 6 ? bk->count[0] : 6) : 0;
            const int step = cnt ? (int)(vt->demo_time / (4.0 * bar)) % cnt : 0;
            if (cnt && step != vt->demo_mg_step) {
                vt->demo_mg_step = step;
                rs_load(vt, bk, 0, step);
                rs_set_bank(vt, bk, 0);
                vt->p[VT_P_RS_PRESET] = (float)step;
            }
        }
        int m = (int)vt->p[VT_P_MODE];
        if (latch_mode(m) || ((int)vt->p[VT_P_ENGINE] == VT_ENGINE_MANGLER && (int)vt->p[VT_P_MG_NOTES] == MG_NOTES_PADS)) {
            demo_chord((int)(vt->demo_time / bar), demo_notes);      /* virtual pads, one chord a bar */
            demo_chord_on = 1;
        }
        vt->demo_time += (double)frames / sr;
    }

    macros_follow_mode(vt);                                          /* the tour (or its end) may have moved the mode: its own knobs come with it */
    /* A mode change (knob, state or Demo) renames char1/char2/mod: pulse
     * is_loading so the UI re-reads them. Not in the first second, when a set
     * is being restored (the host's restore waits while is_loading says "1"). */
    const int eng = (int)vt->p[VT_P_ENGINE];
    const int want_mode = eng == VT_ENGINE_VOICEFX ? VT_MODE_VOICEFX : eng == VT_ENGINE_RESYNTH ? VT_MODE_RESYNTH :
                          eng == VT_ENGINE_MANGLER ? VT_MODE_MANGLER : (int)vt->p[VT_P_MODE];
    if (want_mode != vt->label_mode) {
        if (vt->label_mode >= 0 && vt->warm_samples >= vt->sample_rate)
            vt->loading_blocks = (int)(LABEL_PULSE_SEC * sr / (float)frames);
        vt->label_mode = want_mode;
    } else if (vt->loading_blocks > 0) {
        vt->loading_blocks--;
    }
    if (vt->warm_samples < vt->sample_rate) vt->warm_samples += frames;

    /* The engine that runs this block: the old one until its fade-out has
     * reached silence, then the chosen one (the very first block takes it at
     * once). A knob swept across several modes only lands on the last. */
    if (vt->run_mode < 0 || (vt->run_mode != want_mode && vt->sw_g <= 0.0f)) vt->run_mode = want_mode;
    const int mode = vt->run_mode;
    /* Voice FX has its own Mix/Drive knob, a fixed Level, and runs with the
     * automatic front end (Auto's input AGC and voice gate), Tone off and its
     * own Choir for the stereo chorus: nothing on the pages it hides can leave
     * it in a state its one page cannot show. */
    const int vf = mode == VT_MODE_VOICEFX;
    /* Resynth runs the reference's own front end (Input gain, Gate), Dry/Wet, Widen and Output: TinyVoice's
     * Auto, Tone, Spread, Mix and Level stay out of it (its pages do not show them). */
    const int rs = mode == VT_MODE_RESYNTH;
    const int mg = mode == VT_MODE_MANGLER;     /* Mangler: its own Mix and Volume, with the same automatic front end */
    /* Voice FX's effective knobs. They are the stored ones, except under the Showcase Tour (Demo On in Voice FX:
     * Demo is the tour), when the ghost hands move Tract, Peak, Choir and Mix/Drv and pick the vowel along the
     * phrase's bars. Only these effective values change: nothing is ever written to vt->p, so what you set
     * is what you get back when the tour stops. */
    float vfp[VT_PRESET_VALUES];
    for (int k = 0; k < VT_PRESET_VALUES; k++) vfp[k] = vt->p[VT_P_VF_VOWEL + k];
    const int tour_on = vf && vt->demo_active;                      /* Demo in Voice FX is the tour */
    tour_t tour;
    if (tour_on) {
        tour_eval(groove_bar(&vt->groove), &tour);
        vfp[1] = tour.tract; vfp[2] = tour.peak; vfp[3] = tour.choir; vfp[7] = tour.mix;
        /* what the screen shows: the effective knobs, and the vowel as a position on the A E I O U line */
        for (int k = 0; k < VT_PRESET_VALUES; k++) vt->disp[k] = vfp[k];
        vt->disp[0] = ((float)tour.v0 + (float)(tour.v1 - tour.v0) * tour.vt) * 0.25f;
        vt->disp_on = 1;
    } else {
        vt->disp_on = 0;
    }
    groove_set_bpm(&vt->groove, vt->bpm);                           /* the audition loop follows the host tempo */
    const float mix  = vf ? clampf(vfp[7] / VFX_MIX_KNEE, 0.0f, 1.0f) : (rs || mg) ? 1.0f : vt->p[VT_P_MIX];   /* Voice FX: wet/dry up to the knee, drive above */
    const float dry_g = mix >= 0.999f ? 0.0f : 1.0f - mix;          /* 100% wet: no dry at all */
    const float gain = (vf || rs || mg) ? 1.0f : powf(10.0f, vt->p[VT_P_GAIN] / 20.0f);
    const float sw_goal = mode == want_mode ? 1.0f : 0.0f;
    const int freeze_rec = (int)vt->p[VT_P_FREEZE_REC], freeze_loop = (int)vt->p[VT_P_FREEZE];   /* Rec, Loop (freeze.h) */
    {   /* Looper Save: turned to Save it waits for the loop's start and keeps one period of the output (loopsave.h) */
        const int sv = (int)vt->p[VT_P_FREEZE_SAVE];
        if (sv != vt->save_asked) {
            vt->save_asked = sv;
            if (sv == 1) { vt->save_hold = 0; loopsave_arm(&vt->ls, !freeze_looping(&vt->freeze)); }
            else if (sv == 0) loopsave_cancel(&vt->ls);
        }
    }
    int ls_state = loopsave_state(&vt->ls);
    int ls_bars = 0;
    {   /* what the capture would be called: whole bars at the host tempo, else seconds */
        const float bar_n = 240.0f / vt->bpm * sr;
        const float r = (float)(vt->freeze.loop_len - vt->freeze.xf) / bar_n;
        if (vt->freeze.loop_len > 0 && r >= 0.98f && fabsf(r - roundf(r)) * bar_n < 4.0f) ls_bars = (int)roundf(r);   /* a bar take is exact to the sample; a Free take that is near a bar by chance is named in seconds */
    }
    {   /* Freeze Len: bars at the host tempo (4 beats a bar), 0 = free punch-out */
        const int bars = k_freeze_bars[(int)vt->p[VT_P_FREEZE_LEN]];
        freeze_set_target(&vt->freeze, bars ? (int)((float)bars * (240.0f / vt->bpm) * sr) : 0);
        /* samples to the next downbeat (bar = 4 beats), for an armed take */
        int to_db = -1;
        if (vt->beat >= 0.0) {
            double next = ceil(vt->beat / 4.0 - 1e-9) * 4.0;
            to_db = (int)lround((next - vt->beat) * 60.0 / (double)vt->bpm * (double)sr);
        }
        freeze_set_clock(&vt->freeze, to_db);
    }
    const int auto_on = (int)vt->p[VT_P_GATE] == 0 || vf || rs || mg;     /* Gate = Auto (always, in Voice FX and Resynth: the presets' gates and tracker expect a DAW-level voice) */
    const float gate_manual_db = gate_db_of((int)vt->p[VT_P_GATE] ? (int)vt->p[VT_P_GATE] : vt->gate_mem);
    const int root = (int)vt->p[VT_P_KEY];        /* 0 = C */
    const float c1 = vt->p[VT_P_CHAR1], c2 = vt->p[VT_P_CHAR2], md = vt->p[VT_P_MOD];
    const float beauty = vt->p[VT_P_BEAUTY];
    const int scale = (int)vt->p[VT_P_SCALE];
    /* Auto runs its own gate; the manual Gate knob is hidden then and ignored. */
    const float gate_db = auto_on ? -60.0f : gate_manual_db;

    /* Tone: 2 x 1-pole lowpass, 400 Hz (0) .. 20 kHz (1); fully open = bypassed. */
    const int tone_on = vt->p[VT_P_TONE] < 0.999f && !vf && !rs && !mg;
    const float tone_a = 1.0f - expf(-2.0f * 3.14159265f * 400.0f * powf(50.0f, vt->p[VT_P_TONE]) / sr);

    /* Spread: two modulated taps (quadrature LFO) widen the wet signal. In
     * Robot, Spread pans the vocoder's odd/even bands instead. */
    const float spread = vt->p[VT_P_SPREAD];
    const float ch_amt = (has_bands(mode) || rs || mg) ? 0.0f : (vf ? vfp[3] : spread) * 0.7f;
    const float ch_base = CHORUS_BASE_MS * 0.001f * sr, ch_depth = CHORUS_DEPTH_MS * 0.001f * sr;
    const float ch_inc = CHORUS_RATE_HZ / sr;
    const float k_ch = 1.0f - expf(-1.0f / (0.010f * sr));      /* 10 ms: the amount never steps */

    /* Latch switched: On takes the pads held right now as the chord; Off
     * empties it, which releases the latched voices at once. */
    if (latch_on(vt) && !vt->latch_was) {
        latch_clear(vt);
        for (uint32_t age = 0;;) {                                 /* oldest first */
            int pick = -1;
            for (int i = 0; i < VT_MAX_VOICES; i++)
                if (vt->voices[i].note >= 0 && vt->voices[i].age > age && (pick < 0 || vt->voices[i].age < vt->voices[pick].age)) pick = i;
            if (pick < 0) break;
            latch_add(vt, vt->voices[pick].note, vt->voices[pick].velocity);
            age = vt->voices[pick].age;
        }
    } else if (!latch_on(vt) && vt->latch_was) {
        latch_clear(vt);
    }
    vt->latch_was = latch_on(vt);
    const int latched = !demo_chord_on && latch_on(vt) && latch_mode(mode);
    int held[VT_MAX_VOICES], vel[VT_MAX_VOICES];
    for (int s = 0; s < VT_MAX_VOICES; s++) {
        held[s] = demo_chord_on ? demo_notes[s] : latched ? vt->latch_note[s] : vt->voices[s].note;
        vel[s] = demo_chord_on ? 100 : latched ? vt->latch_vel[s] : vt->voices[s].velocity;
    }

    const float c4 = slot_value(vt, VT_P_CHAR4), c5 = slot_value(vt, VT_P_CHAR5);
    if (mode == VT_MODE_ROBOT) {
        /* char1 formant (+-12 st, moves the synthesis bank), char2 sibilance,
         * mod VP-330 ensemble, spread odd/even band panning. Saw carrier with
         * a fixed 8-cent detune; band width at its middle setting. */
        /* Beauty: carrier detune 0..+-7.5 ct (0.5 ~ the old fixed +-4), plus
         * the vocoder's own polish (daft_set_beauty) and up to +35% band spread */
        daft_set_params(&vt->daft, 0, 15.0f * beauty, vt->p[VT_P_GLIDE],
                        0.5f, 0.0f, c2, (c1 - 0.5f) * 24.0f, gate_db);
        daft_set_beauty(&vt->daft, beauty);
        daft_set_auto(&vt->daft, auto_on);
        daft_set_drone_root(&vt->daft, root);                 /* the drone plays in the Key */
        daft_set_stereo(&vt->daft, fminf(1.0f, spread + 0.35f * beauty), md);
        assign_carriers(vt, &vt->daft, held, vel);
    } else if (mode == VT_MODE_NIGHTCALL) {
        /* DigiTech Talker style: one narrow pulse (duty 12..28%, 20% at 0.5) with
         * ~10% white noise, the pads or the drone as the carrier, the vocal
         * cavity EQ, and some of the unvoiced breath bled in. char1 formant,
         * char2 breath bleed (0..35%, 17.5% at 0.5), mod rasp (10..30%), char4
         * duty, char5 cavity peak (0..+12 dB at 1.2 kHz). Spread pans odd/even bands. */
        daft_set_params(&vt->nc, 1, 0.0f, vt->p[VT_P_GLIDE], 0.5f, 0.0f, 0.4f, (c1 - 0.5f) * 24.0f, gate_db);
        daft_set_beauty(&vt->nc, 0.0f);
        daft_set_auto(&vt->nc, auto_on);
        daft_set_drone_root(&vt->nc, root);
        daft_set_stereo(&vt->nc, spread, 0.0f);
        daft_set_talker(&vt->nc, 1, 0.12f + 0.16f * c4, 0.10f + 0.20f * md, 0.35f * c2, 12.0f * c5);
        assign_carriers(vt, &vt->nc, held, vel);
    } else if (rs) {
        hm_sync(vt);
    } else if (mg) {
        int pads[VT_MAX_VOICES];
        const int np = mg_pad_list(vt, demo_chord_on, demo_notes, latched, pads);
        mg_sync(vt, pads, np);
    } else if (vf) {
        if (tour_on) vfx_vowel_pair(&vt->vfx, tour.v0, tour.v1, tour.vt);
        else vfx_vowel_knob(&vt->vfx);
        vfx_set(&vt->vfx, vfp[0], vfp[1], vfp[2], vfp[3], vfp[4], vfp[5], vfp[6], vfp[7]);
    } else if (is_vox(mode)) {
        if (mode == VT_MODE_DALEK) vox_set_dalek(&vt->vox, c1, c2, md, c4, c5);
        else if (mode == VT_MODE_HARMONY) {
            /* the tracker runs for Harmony (cher_push below): the grain is set to whole periods of the voice */
            cher_set_params(&vt->cher, root, scale, 0.0f, gate_db, -1, 0.0f);
            cher_set_auto(&vt->cher, auto_on);
            cher_set_clarity(&vt->cher, 0.65f);
            vox_set_harmony(&vt->vox, c1, c2, md, c4, c5, spread, held, vt->cher.voiced ? vt->cher.f0 : 0.0f);
        }
        else vox_set_walkie(&vt->vox, c1, c2, md, c4, c5);
    } else {
        /* Every other mode runs on the tracker. The lead modes take a held pad
         * as the target note; Chords/Prism use the pads as harmony instead.
         * Prism is a 0 ms hard snap whatever Retune says. Snap at Beauty 0
         * is instant (no slew at all, Retune and Glide aside): a sliding
         * voice steps from semitone to semitone. HardTune is always instant. */
        const int lead_override = (mode == VT_MODE_SNAP || mode == VT_MODE_MORPH || mode == VT_MODE_DOUBLE || mode == VT_MODE_HARDTUNE);
        const int snap_instant = mode == VT_MODE_SNAP && beauty <= SNAP_INSTANT_BEAUTY;
        const float retune = (snap_instant || mode == VT_MODE_HARDTUNE) ? -1.0f : mode == VT_MODE_PRISM ? 0.0f : vt->p[VT_P_RETUNE];
        cher_set_params(&vt->cher, root, scale, retune, gate_db,
                        lead_override ? newest_held_note(vt) : -1, snap_instant ? 0.0f : vt->p[VT_P_GLIDE]);
        cher_set_auto(&vt->cher, auto_on);
        /* Snap's char2 / HardTune's char5 is tracking sensitivity: 0.5 = the default 0.65 clarity. */
        cher_set_clarity(&vt->cher, mode == VT_MODE_SNAP ? 0.9f - 0.5f * c2 : mode == VT_MODE_HARDTUNE ? 0.9f - 0.5f * c5 : 0.65f);
        if (mode == VT_MODE_CHORDS) {
            imogen_set_beauty(&vt->imogen, beauty);
            imogen_set_targets(&vt->imogen, &vt->cher, held, auto_on, root, scale,
                               retune, vt->p[VT_P_GLIDE], fminf(1.0f, spread + 0.4f * beauty),   /* Beauty: choir spread */
                               c2 < 0.5f ? VT_VOICING_STANDARD : VT_VOICING_WIDE);
            imogen_set_character(&vt->imogen, 2.0f * c1, md);
        }
    }
    if (!has_bands(mode) && !is_vox(mode) && !vf && !rs && !mg && mode != VT_MODE_CHORDS) {
        fx_set(&vt->fx, &vt->cher, mode, c1, c2, md, spread, vt->p[VT_P_GLIDE], auto_on, root, scale, held);
        fx_set_extra(&vt->fx, c4, c5);
        fx_set_beauty(&vt->fx, beauty);
    }
    const int head = vt_mode_headroom(mode);
    /* the voice gate's threshold (RMS): Resynth's Mic Gate, Auto's, or the Gate knob. Resynth always runs Auto (its
     * presets expect a DAW-level voice) but a quiet singer under -42 dBFS was cut with no knob to fix it. */
    const float vg_thr = (rs || mg) ? powf(10.0f, vt->p[VT_P_HM_VGATE] / 20.0f) : auto_on ? SG_THR : powf(10.0f, gate_manual_db / 20.0f);

    for (int i = 0; i < frames; i++) {
        float in_l = lr[2 * i], in_r = lr[2 * i + 1];

        /* Input stage: the demo phrase replaces the mic; Freeze then records
         * or loops whichever it is. Both the engine and the dry path see it. */
        /* Demo: Voice FX is auditioned with the funk groove (no track routed in), every other mode with the sung phrase. */
        if (vt->demo_active) {
            const demo_pcm_t *dp = vf ? NULL : atomic_load_explicit(&vt->demo_pcm, memory_order_acquire);
            in_l = in_r = vf ? groove_process(&vt->groove) : dp ? demo_pcm_next(dp, &vt->demo_pos, 44100.0 / (double)vt->sample_rate) : demo_process(&vt->demo);
        }   /* every mode, Snap included: Key/Scale act on it */
        /* Mic pre-conditioning: a 1-pole high-pass at 120 Hz on everything
         * (engines, gates, dry path, Freeze takes), so pad taps and chassis
         * thumps on the built-in mic don't open the voice gate, ride the AGC,
         * fill the vocoder's low analysis bands, or sit in the dry blend. */
        {
            const float yl = vt->hp_a * (vt->hp_y[0] + in_l - vt->hp_x[0]);
            const float yr = vt->hp_a * (vt->hp_y[1] + in_r - vt->hp_x[1]);
            vt->hp_x[0] = in_l; vt->hp_x[1] = in_r;
            vt->hp_y[0] = fabsf(yl) < 1e-15f ? 0.0f : yl;
            vt->hp_y[1] = fabsf(yr) < 1e-15f ? 0.0f : yr;
            in_l = vt->hp_y[0];
            in_r = vt->hp_y[1];
        }
        if (ls_state == LS_ARMED && freeze_looping(&vt->freeze) && vt->freeze.pos == 0 && vt->freeze.mix > 0.999f) {
            loopsave_begin(&vt->ls, vt->freeze.loop_len - vt->freeze.xf, vt->freeze.loop_len, ls_bars, (int)lroundf(vt->bpm * 10.0f));
            ls_state = loopsave_state(&vt->ls);          /* the loop is at its start: this sample is the first */
        }
        float fmix;
        float loop = freeze_process(&vt->freeze, 0.5f * (in_l + in_r), freeze_rec, freeze_loop, &fmix);
        if (fmix > 0.0f) {
            in_l += (loop - in_l) * fmix;
            in_r += (loop - in_r) * fmix;
        }
        const float mhp = 0.5f * (in_l + in_r);
        /* Auto: input AGC, so soft singing lands in the engines' sweet spot,
         * and the silence gate, so pauses stay quiet instead of the AGC
         * lifting the room into hiss. */
        {
            float a = fabsf(mhp) * 1.5707963f;
            vt->in_env += (a - vt->in_env) * (a > vt->in_env ? vt->k_agc_att : vt->k_agc_rel);
            if (vt->in_env < 1e-12f) vt->in_env = 0.0f;
            vt->sg_ms += (mhp * mhp - vt->sg_ms) * vt->k_sg_env_a;
            if (vt->sg_ms < 1e-20f) vt->sg_ms = 0.0f;
            const int voice = vt->sg_ms >= vg_thr * vg_thr;              /* RMS >= threshold */
            if (voice) {
                vt->sg_open = 1;
                vt->sg_hold = vt->sg_hold_n;
            } else if (vt->sg_hold > 0) {
                vt->sg_hold--;
            } else {
                vt->sg_open = 0;
            }
            if (auto_on) {
                if (voice)                                              /* frozen the moment it drops */
                    vt->agc_goal = clampf(IN_AGC_TARGET / fmaxf(vt->in_env, 1e-6f), IN_AGC_MIN, IN_AGC_MAX);
            } else {
                vt->agc_goal = 1.0f;
            }
            vt->agc_gain += (vt->agc_goal - vt->agc_gain) * vt->k_agc_gain;
            const float sg_goal = (!auto_on || vt->sg_open) ? 1.0f : SG_DEPTH;   /* the expander: Auto only */
            vt->sg_gain += (sg_goal - vt->sg_gain) * (sg_goal > vt->sg_gain ? vt->k_sg_att : vt->k_sg_rel);
            in_l *= vt->agc_gain * vt->sg_gain;
            in_r *= vt->agc_gain * vt->sg_gain;
        }
        const float mono = 0.5f * (in_l + in_r);

        float wet_l, wet_r;
        if (mode != VT_MODE_HARMONY) vox_listen(&vt->vox, mono);   /* Harmony's ring stays current (its own process writes it while it runs) */
        if (has_bands(mode)) {
            daft_t *dd = mode == VT_MODE_NIGHTCALL ? &vt->nc : &vt->daft;
            daft_set_voice_present(dd, vt->sg_open);         /* the voice gate shuts the vocoder */
            daft_process_lr(dd, mono, &wet_l, &wet_r);
            /* keep the tracker's delay line and analysis window current
             * (no pitch analysis: that doubled Robot's CPU), so a switch to a
             * pitch mode reads the live voice, not audio from before Robot */
            cher_listen(&vt->cher, mono);
            vt->dry_d *= 0.998f;                             /* vocoder is ~zero latency */
            if (vt->dry_d < 1e-6f) vt->dry_d = 0.0f;
        } else if (rs) {
            hm_tick(&vt->hm, mono, &wet_l, &wet_r);
            cher_listen(&vt->cher, mono);                    /* a switch back to a pitch mode finds the live voice */
            dry_follow(vt, 0.0f);                            /* Resynth mixes its own dry (Dry/Wet) */
        } else if (mg) {
            cher_push(&vt->cher, mono);                      /* the period it follows */
            mg_tick(&vt->mg, mono, vt->cher.f0 > 0.0f ? sr / vt->cher.f0 : 0.0f, &wet_l, &wet_r);
            dry_follow(vt, 0.0f);                            /* Mangler mixes its own dry (Mix) */
        } else if (vf) {
            vfx_process(&vt->vfx, mono, &wet_l, &wet_r);
            cher_listen(&vt->cher, mono);                    /* a switch back to a pitch mode finds the live voice */
            dry_follow(vt, 0.0f);
        } else if (is_vox(mode)) {
            vox_process(&vt->vox, mode, mono, vt->sg_open, &wet_l, &wet_r);
            if (mode == VT_MODE_HARMONY) cher_push(&vt->cher, mono);   /* its grain follows the voice's pitch */
            else cher_listen(&vt->cher, mono);               /* as for the vocoders: a switch to a pitch mode finds the live voice */
            dry_follow(vt, vox_latency(&vt->vox, mode));
        } else if (mode == VT_MODE_CHORDS) {
            imogen_process(&vt->imogen, &vt->cher, mono, &wet_l, &wet_r);
            if (!auto_on) { wet_l *= CHORDS_MANUAL_TRIM; wet_r *= CHORDS_MANUAL_TRIM; }
            dry_follow(vt, imogen_latency(&vt->imogen));      /* the voices' PSOLA latency */
        } else {
            fx_process(&vt->fx, &vt->cher, mono, &wet_l, &wet_r);
            dry_follow(vt, fx_latency(&vt->fx, &vt->cher));
        }
        if (vt->sw_g != sw_goal)                             /* mode switch fade */
            vt->sw_g = sw_goal > vt->sw_g ? fminf(1.0f, vt->sw_g + vt->k_sw) : fmaxf(0.0f, vt->sw_g - vt->k_sw);
        wet_l *= vt->sw_g;
        wet_r *= vt->sw_g;

        if (tone_on) {
            vt->tone_z[0][0] += (wet_l - vt->tone_z[0][0]) * tone_a;
            vt->tone_z[0][1] += (vt->tone_z[0][0] - vt->tone_z[0][1]) * tone_a;
            vt->tone_z[1][0] += (wet_r - vt->tone_z[1][0]) * tone_a;
            vt->tone_z[1][1] += (vt->tone_z[1][0] - vt->tone_z[1][1]) * tone_a;
            for (int c = 0; c < 2; c++)
                for (int k = 0; k < 2; k++)
                    if (fabsf(vt->tone_z[c][k]) < 1e-15f) vt->tone_z[c][k] = 0.0f;
            wet_l = vt->tone_z[0][1];
            wet_r = vt->tone_z[1][1];
        }

        vt->chorus[vt->ch_w] = 0.5f * (wet_l + wet_r);
        vt->ch_w = (vt->ch_w + 1) & (VT_CHORUS_LEN - 1);
        vt->ch_amt_s += (ch_amt - vt->ch_amt_s) * k_ch;
        if (vt->ch_amt_s < 1e-5f && ch_amt == 0.0f) vt->ch_amt_s = 0.0f;
        if (vt->ch_amt_s > 0.0f) {
            vt->ch_phase += ch_inc;
            if (vt->ch_phase >= 1.0f) vt->ch_phase -= 1.0f;
            float ph = 2.0f * 3.14159265f * vt->ch_phase;
            wet_l += vt->ch_amt_s * ring_read(vt->chorus, VT_CHORUS_LEN - 1, vt->ch_w, ch_base + ch_depth * sinf(ph));
            wet_r += vt->ch_amt_s * ring_read(vt->chorus, VT_CHORUS_LEN - 1, vt->ch_w, ch_base + ch_depth * cosf(ph));
        }

        vt->dry[0][vt->dry_w] = in_l;
        vt->dry[1][vt->dry_w] = in_r;
        vt->dry_w = (vt->dry_w + 1) & (VT_DRY_DELAY - 1);
        float dry_l = ring_read(vt->dry[0], VT_DRY_DELAY - 1, vt->dry_w, vt->dry_d + 1.0f);
        float dry_r = ring_read(vt->dry[1], VT_DRY_DELAY - 1, vt->dry_w, vt->dry_d + 1.0f);

        float out_l = dry_l * dry_g + wet_l * mix;
        float out_r = dry_r * dry_g + wet_r * mix;
        if (head) { out_l = headroom(out_l); out_r = headroom(out_r); }   /* new modes: nothing past -6.1 dBFS at Level 0 */

        vt->dg_out_ms += (out_l * out_l - vt->dg_out_ms) * vt->k_sg_env_a;
        if (vt->dg_out_ms < 1e-20f) vt->dg_out_ms = 0.0f;
        if (vt->dg_out_ms > vt->dg_out_pk) vt->dg_out_pk = vt->dg_out_ms;
        if (vt->sg_ms > vt->dg_in_pk) vt->dg_in_pk = vt->sg_ms;
        if (vt->sg_ms < vt->dg_in_min) vt->dg_in_min = vt->sg_ms;     /* ... falls at once */
        lr[2 * i]     = soft_limit(out_l * gain);
        lr[2 * i + 1] = soft_limit(out_r * gain);
        if (ls_state == LS_CAPTURING) {
            loopsave_feed(&vt->ls, lr[2 * i], lr[2 * i + 1]);
            if (vt->ls.n >= vt->ls.want) ls_state = LS_READY;
        }
    }
    {   /* diag: holds decay over ~1 s, gate-open share averaged over ~1 s */
        const float k = 1.0f - expf(-(float)frames / sr);
        vt->dg_in_pk -= vt->dg_in_pk * k;
        vt->dg_out_pk -= vt->dg_out_pk * k;
        vt->dg_open += ((vt->sg_open ? 1.0f : 0.0f) - vt->dg_open) * k;
        vt->dg_ropen += ((vt->daft.gate_open ? 1.0f : 0.0f) - vt->dg_ropen) * k;
        vt->dg_in_min += (vt->sg_ms - vt->dg_in_min) * k;            /* rises slowly ... */
        if (vt->dg_in_pk < 1e-20f) vt->dg_in_pk = 0.0f;
        if (vt->dg_out_pk < 1e-20f) vt->dg_out_pk = 0.0f;
    }
    if (ls_state == LS_ARMED || ls_state == LS_CAPTURING) {      /* the loop stopped, or a new take replaced it: nothing to keep */
        if (!freeze_looping(&vt->freeze) || vt->freeze.done || (ls_state == LS_CAPTURING && vt->freeze.loop_len != vt->ls.loop_len))
            loopsave_abort(&vt->ls, LS_WHY_CHANGED);
    }
    {   /* a finished save shows on the knob for 2 s: Saved or Failed, then Off */
        const int res = loopsave_result(&vt->ls);
        if (res) {
            vt->p[VT_P_FREEZE_SAVE] = res == 1 ? 2.0f : 3.0f;
            vt->save_asked = (int)vt->p[VT_P_FREEZE_SAVE];
            vt->save_hold = 2 * (int)sr;
        } else if (vt->save_hold > 0) {
            vt->save_hold -= frames;
            if (vt->save_hold <= 0) {
                vt->save_hold = 0;
                if ((int)vt->p[VT_P_FREEZE_SAVE] >= 2) { vt->p[VT_P_FREEZE_SAVE] = 0.0f; vt->save_asked = 0; }
            }
        }
    }
    if (freeze_take_done(&vt->freeze)) {           /* a take was kept and plays: the knobs follow (Rec back to Off, Loop On) */
        vt->p[VT_P_FREEZE_REC] = 0.0f;
        vt->p[VT_P_FREEZE] = 1.0f;
    }

    if (has_bands(mode)) {
        for (int b = 0; b < VT_NUM_BANDS; b++) vt->viz_bands[b] = daft_band_level(mode == VT_MODE_NIGHTCALL ? &vt->nc : &vt->daft, b);
    } else if (rs) {
        vt->viz_f0 = hm_f0(&vt->hm);
        vt->viz_target = hm_target(&vt->hm);
        vt->viz_cents = vt->viz_f0 > 0.0f && vt->viz_target > 0.0f ? 1200.0f * log2f(vt->viz_f0 / vt->viz_target) : 0.0f;
    } else if (mg) {
        vt->viz_f0 = vt->cher.f0;
        vt->viz_target = vt->cher.f0 > 0.0f && (int)vt->p[VT_P_MG_SHIFT] != MG_VOCODE ? vt->cher.f0 * exp2f(vt->p[VT_P_MG_PITCH] / 12.0f) : 0.0f;
        vt->viz_cents = 0.0f;
    } else if (is_vox(mode) && mode != VT_MODE_HARMONY) {
        vt->viz_f0 = vt->viz_target = vt->viz_cents = 0.0f;           /* nothing is tracked */
    } else {
        vt->viz_f0 = vt->cher.f0;
        vt->viz_target = vt->cher.target_hz;
        vt->viz_cents = vt->cher.cents_err;
    }
}

void dsp_process_block(vt_t *vt, int16_t *audio, int frames)
{
    if (!vt || !audio || frames <= 0) return;

    float tmp[2 * VT_MAX_BLOCK];            /* stack: no allocation on the audio thread */
    while (frames > 0) {
        int n = frames > VT_MAX_BLOCK ? VT_MAX_BLOCK : frames;
        for (int i = 0; i < 2 * n; i++) tmp[i] = (float)audio[i] * (1.0f / 32768.0f);
        dsp_process_block_f(vt, tmp, n);
        for (int i = 0; i < 2 * n; i++) {
            float s = clampf(tmp[i], -1.0f, 32767.0f / 32768.0f) * 32768.0f;
            audio[i] = (int16_t)lrintf(s);
        }
        audio += 2 * n;
        frames -= n;
    }
}
