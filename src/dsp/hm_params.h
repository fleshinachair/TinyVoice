/* Resynth's parameters, ONE list for hm.c (the engine's table), dsp.c (TinyVoice's
 * param table: key "hm_" + id) and scripts/gen_module_json.py (which parses this
 * file). Order = hm_param in hm.h.
 *
 *   X(enum id, preset id, Move label, kind, default, min, max, step, unit, options)
 *
 * kind F = float, E = enum (options = a NULL-free string list, min 0, max n-1),
 * T = an enum that is a performance toggle (never saved in a state).
 * The preset id and the units are Humanoid's (.baby): frequencies are MIDI note
 * numbers, gains dB, times seconds. Labels are short enough for a Move cell. */
#ifndef TINYVOICE_HM_PARAMS_H
#define TINYVOICE_HM_PARAMS_H

#define HM_OPTS_OFFON   "Off", "On"
#define HM_OPTS_LAT     "Lo", "Mid", "Hi"
#define HM_OPTS_SYNC    "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1/1"
#define HM_OPTS_WMODE   "FM", "LFO"
#define HM_OPTS_PMODE   "Scale", "Note", "MIDI"

#define HM_PARAM_LIST(X) \
    X(HM_PITCH_MONO,     "pitch_mono",          "Note",      F, 60,     24,    96,   1,     "",   ) \
    X(HM_PITCH_DUO,      "pitch_duo",           "Duo Note",  F, 67,     24,    96,   1,     "",   ) \
    X(HM_SCALE_BYPASS,   "quantize_enabled",    "Bypass",    E, 0,      0,     1,    1,     "",   HM_OPTS_OFFON) \
    X(HM_TUNER_ATTACK,   "tuner_attack",        "Q Attack",  F, 0.2,    0,     0.4,  0.01,  "s",  ) \
    X(HM_NOTE_SHIFT,     "pitch_shift",         "Shift",     F, 0,      -12,   12,   1,     "st", ) \
    X(HM_GATE,           "noisegate_threshold", "Gate",      F, -80,    -80,   -6,   1,     "dB", ) \
    X(HM_GATE_ATTACK,    "noisegate_attack",    "G Attack",  F, 0.05,   0.001, 0.2,  0.001, "s",  ) \
    X(HM_GATE_HOLD,      "noisegate_hold",      "G Hold",    F, 0.05,   0.001, 0.4,  0.001, "s",  ) \
    X(HM_GATE_RELEASE,   "noisegate_release",   "G Release", F, 0.01,   0.001, 0.4,  0.001, "s",  ) \
    X(HM_QUANTIZE,       "quantize",            "Quantize",  F, 0.25,   0.03,  0.5,  0.01,  "",   ) \
    X(HM_POLARITY,       "tuner_polarity",      "Polarity",  E, 0,      0,     1,    1,     "",   HM_OPTS_OFFON) \
    X(HM_TUNER_HOLD,     "tuner_hold",          "Q Hold",    F, 0,      0,     100,  1,     "",   ) \
    X(HM_TRANSFORM,      "transform",           "Transform", F, 0,      0,     1,    0.01,  "",   ) \
    X(HM_STRETCH,        "wavetable_stretch",   "Stretch",   F, 0.5,    0.1,   0.9,  0.01,  "",   ) \
    X(HM_SHAPE,          "wavetable_shape",     "Shape",     F, 0,      0,     1,    0.01,  "",   ) \
    X(HM_CHEB0,          "chebyshev_0",         "Odd 1",     F, 1.0,    -2,    2,    0.01,  "",   ) \
    X(HM_CHEB1,          "chebyshev_1",         "Even 1",    F, -0.5,   -2,    2,    0.01,  "",   ) \
    X(HM_CHEB2,          "chebyshev_2",         "Odd 2",     F, -0.3,   -2,    2,    0.01,  "",   ) \
    X(HM_CHEB3,          "chebyshev_3",         "Even 2",    F, 0.4,    -2,    2,    0.01,  "",   ) \
    X(HM_CHEB4,          "chebyshev_4",         "Odd 3",     F, 0.6,    -2,    2,    0.01,  "",   ) \
    X(HM_CHEB5,          "chebyshev_5",         "Even 3",    F, -0.8,   -2,    2,    0.01,  "",   ) \
    X(HM_CHEB6,          "chebyshev_6",         "Odd 4",     F, -0.4,   -2,    2,    0.01,  "",   ) \
    X(HM_LPF,            "lpf_freq",            "LPF",       F, 138,    57,    138,  0.5,   "",   ) \
    X(HM_LPF_RES,        "lpf_res",             "LPF Res",   F, 0,      -80,   24,   0.5,   "dB", ) \
    X(HM_HPF,            "hpf_freq",            "HPF",       F, 10,     10,    107,  0.5,   "",   ) \
    X(HM_HPF_RES,        "hpf_res",             "HPF Res",   F, 0,      -80,   24,   0.5,   "dB", ) \
    X(HM_MID_FREQ,       "filter_freq",         "Mid Freq",  F, 98.29,  28,    127,  0.5,   "",   ) \
    X(HM_MID_GAIN,       "filter_gain",         "Mid Gain",  F, 0,      -60,   24,   0.5,   "dB", ) \
    X(HM_MID_WIDTH,      "filter_width",        "Mid Width", F, 34,     8,     60,   1,     "",   ) \
    X(HM_LATENCY,        "lookahead",           "Latency",   E, 0,      0,     2,    1,     "",   HM_OPTS_LAT) \
    X(HM_RANGE_LO,       "min_f0",              "Range Lo",  F, 48,     40,    60,   1,     "",   ) \
    X(HM_RANGE_HI,       "max_f0",              "Range Hi",  F, 79,     69,    84,   1,     "",   ) \
    X(HM_INPUT_GAIN,     "input_gain",          "In Gain",   F, 0,      -16,   16,   0.1,   "dB", ) \
    X(HM_SYNTH_MIX,      "synth_mix",           "Synth Mix", F, 1,      0,     1,    0.01,  "",   ) \
    X(HM_FORMANT,        "formant_shift",       "Formant",   F, 0,      -1,    1,    0.01,  "",   ) \
    X(HM_FORMANT_WIDTH,  "formant_width",       "Fmt Width", F, 0.125,  0,     0.5,  0.005, "",   ) \
    X(HM_ROBOTIFY,       "robotify",            "Robotify",  F, 0.5,    0,     1,    0.01,  "",   ) \
    X(HM_DENOISE,        "robotify_res",        "Denoise",   F, 1,      0,     1,    0.01,  "",   ) \
    X(HM_METALLIC,       "robotify_harm",       "Metallic",  F, 1,      0,     1,    0.01,  "",   ) \
    X(HM_FLATTEN,        "robotify_flatten",    "Flatten",   F, 1,      0,     1,    0.01,  "",   ) \
    X(HM_EMPHASIS,       "robotify_emphasis",   "Emphasis",  F, 0.1,    0,     1,    0.01,  "",   ) \
    X(HM_OCTAVE,         "octaving",            "Octave",    F, 0,      -1,    1.5,  0.01,  "",   ) \
    X(HM_SMOOTHING,      "smoothing",           "Smoothing", F, 0.1,    0.1,   0.9,  0.01,  "",   ) \
    X(HM_SMOOTH_PITCH,   "smoothing_pitch",     "Sm Pitch",  F, 1,      0,     1,    0.01,  "",   ) \
    X(HM_SMOOTH_TIMBRE,  "smoothing_timbre",    "Sm Timbre", F, 1,      0,     1,    0.01,  "",   ) \
    X(HM_DEESS,          "deess",               "De-ess",    F, 0,      0,     24,   0.1,   "dB", ) \
    X(HM_DEESS_SENS,     "deess_sensitivity",   "DS Sens",   F, 0.4,    0.1,   0.6,  0.01,  "",   ) \
    X(HM_DEESS_ATTACK,   "deess_attack",        "DS Attack", F, 0.02,   0.005, 0.05, 0.001, "s",  ) \
    X(HM_SHARPEN,        "sharpen",             "Sharpen",   F, 80,     0,     100,  1,     "",   ) \
    X(HM_SHARPEN_BIAS,   "sharpen_bias",        "Bias",      F, 0,      -6,    6,    0.1,   "",   ) \
    X(HM_DRY_WET,        "dry_wet",             "Dry/Wet",   F, 1,      0,     1,    0.01,  "",   ) \
    X(HM_WIDEN,          "widen",               "Widen",     F, 0,      0,     1,    0.01,  "",   ) \
    X(HM_WIDEN_CUTOFF,   "widen_cutoff",        "Wd Cutoff", F, 0.9,    0.8,   1,    0.005, "",   ) \
    X(HM_WIDEN_RATE,     "widen_rate",          "Wd Rate",   F, 7,      0.3,   10,   0.1,   "",   ) \
    X(HM_FREEZE,         "freeze",              "Glitch",    T, 0,      0,     1,    1,     "",   HM_OPTS_OFFON) \
    X(HM_FREEZE_MS,      "freeze_length",       "Glitch ms", F, 101,    1,     1000, 1,     "ms", ) \
    X(HM_FREEZE_SYNC,    "freeze_sync",         "Glt Sync",  E, 0,      0,     1,    1,     "",   HM_OPTS_OFFON) \
    X(HM_FREEZE_SYNCLEN, "freeze_synclength",   "Glt Div",   E, 3,      0,     6,    1,     "",   HM_OPTS_SYNC) \
    X(HM_WARBLE_RATE,    "warble_rate",         "Wb Rate",   F, 0.5,    -1,    1,    0.01,  "",   ) \
    X(HM_WARBLE,         "warble_depth",        "Warble",    F, 0,      0,     1,    0.01,  "",   ) \
    X(HM_WARBLE_MODE,    "warble_mode",         "Wb Mode",   E, 0,      0,     1,    1,     "",   HM_OPTS_WMODE) \
    X(HM_OUTPUT_GAIN,    "output_gain",         "Out Gain",  F, 0,      -24,   12,   0.1,   "dB", ) \
    X(HM_TUNING_MODE,    "tuning_mode",         "Pitch",     E, 0,      0,     2,    1,     "",   HM_OPTS_PMODE) \
    X(HM_DUOPHONIC,      "tuning_duophonic",    "Duo",       E, 0,      0,     1,    1,     "",   HM_OPTS_OFFON)

#endif
