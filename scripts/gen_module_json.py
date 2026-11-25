"""Regenerate TinyVoice's module.json AND src/dsp/module_meta.h from one table:
29 params, two personalities chosen by `engine` (the hierarchy's mode_param):
  Vocoder: three pages (VOICE, PITCH, MORE), plus three read-only
           readouts (pitch, target, cents) that carry the OLED scope on PITCH
           knobs 5-7. VOICE is the home page: Mode, five contextual knobs, Mix, Level.
  VoiceFX: one page of eight knobs (Vowel, Tract, Peak, Choir, Breath, Attack,
           Release, Mix/Drive), nothing else.

    python scripts/gen_module_json.py [module.json] [src/dsp/module_meta.h]

The header carries chain_params and ui_hierarchy as JSON strings for the DSP to
answer get_param("chain_params" / "ui_hierarchy") with. That matters: the chain
host's own fallback re-serialises chain_params from its parsed table and drops
`viz`, so a custom widget (canvas.js) only mounts when the module answers
itself, as tinyk, hank and monksynth do.

The served chain_params names char1 / char2 / mod / char4 / char5 per mode: the
header's copy has @1@ .. @5@ in their "name" fields, which dsp.c fills from its
k_macro_names table. module.json keeps the generic names.

Order of PARAMS must match vt_param_id / k_params in src/dsp/dsp.c; the
consistency test in tests/test_main.c checks every field against the C table.
"""
import copy, json, os, re, sys

# No "Follow": Schwung does not pass Move's project key to modules, so the key is set here.
KEYS = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

def enum(key, name, options, default):
    return {"key": key, "name": name, "type": "enum", "options": options, "default": default}

def flt(key, name, lo, hi, step, default, unit=None):
    p = {"key": key, "name": name, "type": "float", "min": lo, "max": hi, "step": step}
    if unit: p["unit"] = unit
    p["default"] = default
    return p

# The vocoder and carrier modes first, then the voice transformers; Morph and
# Double (the older PSOLA modes) after the roster. Same order as vt_mode in dsp.h.
MODES = ["Robot", "Snap", "Chords", "Prism", "Tape", "Nightcall",
         "Scream", "Dalek", "HardTune", "Harmony", "Walkie", "Morph", "Double"]

PARAMS = [
    enum("mode", "Mode", MODES, 1),
    enum("key", "Key", KEYS, 0),
    enum("scale", "Scale", ["Chromatic", "Major", "Minor", "Pentatonic", "Dorian"], 0),
    flt("retune", "Retune", 0, 500, 1, 0, "ms"),
    flt("char1", "Char 1", 0, 1, 0.01, 0.5, "%"),
    flt("char2", "Char 2", 0, 1, 0.01, 0.5, "%"),
    # Gate and Auto in one knob: Auto (the automatic front end), then a manual
    # threshold -60..0 dB in 3 dB steps, which is Auto Off. Nothing hides any more.
    enum("gate", "Gate", ["Auto"] + ["%d dB" % db for db in range(-60, 1, 3)], 0),   # 3 dB steps
    flt("mix", "Mix", 0, 1, 0.01, 1, "%"),
    flt("gain", "Level", -12, 12, 0.5, 0, "dB"),
    flt("spread", "Spread", 0, 1, 0.01, 0.25, "%"),
    flt("tone", "Tone", 0, 1, 0.01, 1, "%"),
    flt("glide", "Glide", 0, 500, 1, 0, "ms"),
    flt("mod", "Mod", 0, 1, 0.01, 0, "%"),
    enum("freeze_len", "Length", ["Free", "1 Bar", "2 Bars", "4 Bars"], 2),   # Freeze: a take is N bars (it ends and loops by itself)
    flt("beauty", "Beauty", 0, 1, 0.01, 0.5, "%"),          # per-mode polish: stereo, detune, air, warmth
    enum("latch", "Latch", ["Off", "On"], 0),               # pad chord holds after release (Robot, Chords, Prism, Nightcall, Harmony)
    flt("char4", "Char 4", 0, 1, 0.01, 0.5, "%"),           # macros 4 and 5: per mode a new control, or an alias of an old param
    flt("char5", "Char 5", 0, 1, 0.01, 0.5, "%"),
    # the personalities: the Mode selector page writes `engine`; the vf_* params are the Voice FX page,
    # the hm_* params (after vf_preset) the Resynth engine's
    enum("engine", "Engine", ["Vocoder", "Synth Voice", "Resynth", "Mangler"], 0),
    flt("vf_vowel", "Vowel", 0, 1, 0.01, 0.0, "%"),         # A E I O U sweep
    flt("vf_tract", "Tract", 0, 1, 0.01, 0.5, "%"),         # formant scale / gender
    flt("vf_peak", "Peak", 0, 1, 0.01, 0.5, "%"),           # formant Q
    flt("vf_choir", "Choir", 0, 1, 0.01, 0.35, "%"),        # L/R formant detune and width
    flt("vf_breath", "Breath", 0, 1, 0.01, 0.15, "%"),      # noise through the formants + consonant air
    flt("vf_attack", "Attack", 0, 1, 0.01, 0.12, "%"),      # onset swell
    flt("vf_release", "Release", 0, 1, 0.01, 0.3, "%"),     # tail
    flt("vf_mix", "Mix/Drv", 0, 1, 0.01, 0.75, "%"),        # wet/dry up to 70 %, then all wet with the drive climbing
    flt("vf_preset", "Preset", 0, 5, 1, 5),                 # the Voice FX bank (presets.c); the host's preset browser writes it
    flt("rs_bank", "Bank", 0, 25, 1, 0),                    # which bank the Presets page lists: the engine's list (Resynth: Default, Imported, the kinds; Mangler: Default, Imported), then yours; the DSP clamps to the list, the Bank page draws its name (canvas.js)
    flt("rs_preset", "Preset", 0, 1023, 1, 0),              # the Resynth bank (rsbank.c: the preset folder's Resynth presets); its browser writes it
] + []   # the Resynth block is appended below (it is read from src/dsp/hm_params.h)

# ---- Resynth (src/dsp/hm.c): one parameter per Humanoid control, key "hm_" + the .baby preset id,
# in hm_param order, read from the X-macro list both C files use; then the Move-side extras.
HERE = os.path.dirname(os.path.abspath(__file__))
_hp = open(os.path.join(HERE, "..", "src", "dsp", "hm_params.h"), encoding="utf-8").read()
_opts = {m.group(1): [o.strip().strip('"') for o in m.group(2).split(",")]
         for m in re.finditer(r'#define (HM_OPTS_\w+)\s+(.*)', _hp)}
HM = []
for m in re.finditer(r'X\((HM_\w+),\s*"(\w+)",\s*"([^"]*)",\s*([FET]),\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+),\s*"([^"]*)",\s*(\w*)\)', _hp):
    _id, pid, label, kind, d, lo, hi, step, unit, opt = m.groups()
    num = lambda v: int(float(v)) if float(v) == int(float(v)) else float(v)
    if kind in "ET":
        HM.append(enum("hm_" + pid, label, _opts[opt], int(float(d))))
    else:
        HM.append(flt("hm_" + pid, label, num(lo), num(hi), num(step), num(d), unit or None))
assert len(HM) == _hp.count("    X(HM_") and len(HM) > 60, len(HM)
HM_EXTRA = [
    enum("hm_key", "Key", KEYS, 0),
    enum("hm_scale", "Scale", ["Major", "Minor", "Chromatic", "Custom"], 0),   # Custom = the preset's own note mask (hm_mask)
    flt("hm_mask", "Notes", 0, 4095, 1, 2741),                                  # tuning_auto_notes, bit pc (2741 = C major)
    enum("hm_scale_hold", "Hold Scale", ["Off", "On"], 1),                      # On: a preset keeps your Key / Scale / Notes (set and forget)
    enum("hm_autolevel", "Auto Level", ["Off", "On"], 1),                       # On: the output leveller holds every preset at one loudness
    enum("hm_midi_idle", "No Pad", ["Tune", "Silent"], 0),                      # MIDI mode with no pad held: keep tuning to the scale, or go silent (the reference)
    flt("hm_mic_gate", "Mic Gate", -72, -24, 1, -42, "dB"),                    # the voice gate on the raw input (Resynth runs Auto): lower for a quiet voice
    enum("hm_wave", "Wave", ["Saw", "Square", "Triangle", "Sine", "Preset"], 0),  # Preset = the wavetable a preset carried (hm_wt)
]
# ---- Mangler (src/dsp/mangle.c, modelled on the reference): its controls in the reference's units.
OFFON = ["Off", "On"]
MG = [
    enum("mg_shift", "Shifter", ["Granular", "Formant", "Vocode"], 0),     # the core: grains / PSOLA with formants / a monotone at an absolute note
    flt("mg_pitch", "Pitch", -48, 24, 0.1, 0, "st"),
    flt("mg_formant", "Formant", -24, 24, 0.1, 0, "st"),                    # Formant and Vocode
    flt("mg_voices", "Voices", 1, 8, 1, 1),                                 # unison voices per note
    flt("mg_detune", "Detune", 0, 12, 0.1, 0, "st"),                        # Pitch Spread over the voices
    flt("mg_time", "Time", 0, 1000, 1, 0, "ms"),                            # Time Spread over the voices
    flt("mg_grain", "Grain", 1, 100, 1, 10, "Hz"),                          # Granular's grain rate
    flt("mg_mix", "Mix", 0, 1, 0.01, 1, "%"),
    enum("mg_notes", "Notes", ["Normal", "Pads", "Chord"], 0),              # where the voices' notes come from
    enum("mg_chord", "Chord", [str(i) for i in range(1, 9)], 0),            # the chord slot (Notes = Chord)
    enum("mg_mono", "Mono", OFFON, 1),                                      # Pads: the newest pad only
    enum("mg_fix", "Fix", OFFON, 1),                                        # Formant Fix: formants stay put as the pitch moves
    enum("mg_snap", "Snap", OFFON, 1),                                      # Pitch rounds to a semitone
    enum("mg_scatter", "Scatter", ["Spread", "Jitter"], 0),                 # Pitch Spread: fixed and even, or random per grain
    enum("mg_smear", "Smear", ["Spread", "Jitter"], 0),                     # Time Spread: fixed and even, or random per grain
    enum("mg_wide", "Wide", OFFON, 0),                                      # pan the voices across the field
    enum("mg_gjitter", "Jitter", OFFON, 1),                                 # Granular: random grain lengths
    enum("mg_filter", "Filter", OFFON, 0),
    flt("mg_freq", "Freq", 20, 20000, 1, 1000, "Hz"),
    flt("mg_width", "Width", 0.1, 10, 0.1, 10, "oct"),
    flt("mg_volume", "Volume", -24, 12, 0.5, 0, "dB"),
]
PARAMS += HM + HM_EXTRA + MG + [
    # performance toggles (not saved in "state")
    enum("freeze", "Loop", ["Off", "On"], 0),                  # Freeze (src/dsp/freeze.h): play the kept take; never records
    enum("freeze_rec", "Rec", ["Off", "Rec"], 0),              # record a new take (the old loop is kept until it completes); never plays
    enum("freeze_save", "Save", ["Off", "Save", "Saved", "Failed"], 0),   # write the processed loop to a WAV (src/dsp/loopsave.h); the DSP shows Saved / Failed for 2 s
    enum("demo", "Demo", ["Off", "On"], 0),
]

# Read-only readouts the DSP serves (not params: never set, never saved).
# They sit under the OLED scope on PITCH knobs 5-7, so turning those knobs
# does nothing, and their names label the cells under the picture.
def readout(key, name, lo, hi, step, unit):
    r = flt(key, name, lo, hi, step, 0, unit)
    r["access"] = "read"
    return r

# Freeze's own page, the same on the Vocoder and Resynth: Rec, Loop, Length, Save.
LOOPER = ["freeze_rec", "freeze", "freeze_len", "freeze_save"]

READOUTS = [
    readout("pitch", "Pitch", 0, 2000, 0.1, "Hz"),
    readout("target", "Target", 0, 2000, 0.1, "Hz"),
    readout("cents", "Cents", -50, 50, 1, "ct"),
]
for role, r in zip(["a", "b", "c"], READOUTS):
    r["viz"] = {"kind": "custom:tinyvoice_scope", "group": "scope", "role": role, "extra_keys": ["scope"]}

# The Freeze banner spans the Looper page's three knobs (Rec, Loop, Length): their values when idle; ARMED / REC n/N
# / LOOP n/N (the bar it is on) when busy. The knobs under it still turn; their values show in the header while touched.
# loop_status (Looper Save's result) rides on the first one: the Remote UI only sees keys a widget names (the manager pumps viz.extra_keys).
for role, k in zip(["a", "b", "c"], LOOPER):
    p = next(x for x in PARAMS if x["key"] == k)
    p["viz"] = {"kind": "custom:tinyvoice_freeze", "group": "freeze", "role": role, "extra_keys": ["scope", "loop_status"] if role == "a" else ["scope"]}

# Looper Save is a TRIGGER (access "write", the Schwung convention for a one-shot action: a click on the knob fires it by writing option 1,
# "Save", through the enum's own wire; it is never scrubbed). Its cell draws where the save is (custom:tinyvoice_save).
_sv = next(x for x in PARAMS if x["key"] == "freeze_save")
_sv["access"] = "write"
_sv["viz"] = {"kind": "custom:tinyvoice_save", "group": "save", "role": "a", "extra_keys": []}

# Latch (MORE knob 6) draws its own value: On / Off in the modes it acts in
# (Robot, Chords, Prism, Nightcall, Harmony), a dimmed "---" in the others.
next(x for x in PARAMS if x["key"] == "latch")["viz"] = {"kind": "custom:tinyvoice_latch", "group": "latch", "role": "a", "extra_keys": ["mode", "engine", "hm_tuning_mode", "mg_notes"]}   # Resynth: MIDI mode; Mangler: Pads

# Knobs that do nothing in some modes draw their own value (a dimmed "N/A"
# where they are inert): Key, Scale, Retune, Glide, Beauty (all on PITCH). Same
# cell, same knob. The table of modes is vt_param_acts in dsp.h and GATED in
# src/ui/canvas.js.
for k in ["key", "scale", "retune", "glide", "beauty"]:
    next(x for x in PARAMS if x["key"] == k)["viz"] = {"kind": "custom:tinyvoice_" + k, "group": k, "role": "a", "extra_keys": ["mode"]}

# Resynth's knobs that do nothing in some settings draw their own value, or a dimmed "N/A" (custom:tinyvoice_rsna,
# the group names the knob; the rules are RS_ACTS in src/ui/canvas.js, extra_keys the keys each one reads: RS_DEPS
# there, checked by tests/canvas_test.mjs). The DSP still takes turns of them.
RS_DEPS = {
    "hm_transform": ["hm_synth_mix"], "hm_pitch_shift": ["hm_synth_mix"], "hm_octaving": ["hm_synth_mix"],
    "hm_wave": ["hm_synth_mix", "hm_transform"], "hm_wavetable_shape": ["hm_synth_mix", "hm_transform"],
    "hm_wavetable_stretch": ["hm_synth_mix", "hm_transform"],
    "hm_key": ["hm_tuning_mode", "hm_midi_idle", "hm_quantize_enabled", "hm_pitch_shift", "hm_synth_mix"],
    "hm_scale": ["hm_tuning_mode", "hm_midi_idle", "hm_quantize_enabled", "hm_pitch_shift", "hm_synth_mix"],
    "hm_tuning_duophonic": ["hm_tuning_mode"], "hm_pitch_mono": ["hm_tuning_mode"], "hm_pitch_duo": ["hm_tuning_mode", "hm_tuning_duophonic"],
    "hm_midi_idle": ["hm_tuning_mode"],
    "hm_warble_rate": ["hm_warble_depth"], "hm_warble_mode": ["hm_warble_depth", "hm_warble_rate"],
    "hm_filter_freq": ["hm_filter_gain"], "hm_output_gain": ["hm_autolevel"], "hm_freeze_synclength": ["hm_freeze_sync"],
}
for k, deps in RS_DEPS.items():
    next(x for x in PARAMS if x["key"] == k)["viz"] = {"kind": "custom:tinyvoice_rsna", "group": k, "role": "a", "extra_keys": deps}

# The Bank page's knob draws the bank's NAME (the list is the engine's own and yours, so it is not a fixed enum): custom:tinyvoice_bank
# reads the get-only key rs_bank_name.
next(x for x in PARAMS if x["key"] == "rs_bank")["viz"] = {"kind": "custom:tinyvoice_bank", "group": "bank", "role": "a", "extra_keys": ["rs_bank_name", "rs_bank_info", "bank_status"]}

# Pages. VOICE is the home page (and the root's knobs): Mode, the five
# contextual knobs, Mix, Level. PITCH: the tuning knobs and the scope on 5-7.
# MORE: Spread, Tone, Gate, Latch / Beauty, Demo. Freeze has a page of its own (LOOPER, above; it sat on MORE
# knobs 3-4 as Off / Rec / Loop on one knob, where turning between Off and Loop recorded over the loop).
# No delay or reverb: removed on 2026-10-11, the chain has its own.
VOICE = ["mode", "char1", "char2", "mod", "char4", "char5", "mix", "gain"]
PITCH = ["key", "scale", "retune", "glide", "pitch", "target", "cents"]
MORE  = ["spread", "tone", "gate", "latch", "beauty", "demo"]
# Voice FX: its whole UI. No Mode, Key, Scale, Gate, Level or Latch: the engine runs with the automatic front end.
AUDITION = ["demo"]
RS_BANK = ["rs_bank"]
VOICEFX = ["vf_vowel", "vf_tract", "vf_peak", "vf_choir", "vf_breath", "vf_attack", "vf_release", "vf_mix"]
# Resynth: the main controls on the Move, four more pages; everything else is on the Remote UI (web_ui.html).
RS_MAIN  = ["hm_transform", "hm_robotify", "hm_formant_shift", "hm_synth_mix", "hm_pitch_shift", "hm_octaving", "hm_quantize", "hm_dry_wet"]
RS_PITCH = ["hm_tuning_mode", "hm_key", "hm_scale", "hm_scale_hold", "hm_tuner_attack", "hm_min_f0", "hm_max_f0", "hm_smoothing"]
# Transform is on Main (the headline knob) and again here, next to the wave it moves toward (Wave, Shape, Stretch act only through it)
RS_SYNTH = ["hm_transform", "hm_wave", "hm_wavetable_shape", "hm_wavetable_stretch", "hm_warble_depth", "hm_warble_rate", "hm_warble_mode", "hm_widen"]
RS_TONE  = ["hm_hpf_freq", "hm_lpf_freq", "hm_filter_freq", "hm_filter_gain", "hm_noisegate_threshold", "hm_deess", "hm_sharpen", "hm_output_gain"]
RS_PLAY  = ["hm_freeze", "hm_lookahead", "hm_freeze_sync", "hm_freeze_synclength", "hm_input_gain", "hm_mic_gate", "demo"]
RS_NOTES = ["hm_tuning_duophonic", "hm_pitch_mono", "hm_pitch_duo", "latch", "hm_midi_idle"]      # Duo and Note mode's two notes, and the pads (MIDI mode)
# Mangler: Main (Shifter, Pitch, Formant, Voices, Detune, Time, Grain, Mix), More (where the notes come from, Fix, Snap, how the
# spreads move), Filter (the band-pass, the grain jitter, Volume, and the live-play knobs), Looper, and the presets.
MG_MAIN   = ["mg_shift", "mg_pitch", "mg_formant", "mg_voices", "mg_detune", "mg_time", "mg_grain", "mg_mix"]
MG_MORE   = ["mg_notes", "mg_chord", "mg_mono", "mg_fix", "mg_snap", "mg_scatter", "mg_smear", "mg_wide"]
MG_FILTER = ["mg_filter", "mg_freq", "mg_width", "mg_gjitter", "mg_volume", "hm_mic_gate", "latch", "demo"]
MENU_ONLY = {}

# Resynth's Presets page, drawn by canvas.js (drawPage) instead of the host's stock browser, so a long imported name
# wraps over up to three lines (the stock page shows one line in a big face and cut most of them). `preset_browser`
# makes it THE level's preset page: the host still browses (wheel, click, knobs on the level) and hands the name,
# index and count to the drawing. Not a param: never set or saved, the DSP doesn't know the key.
PAGES = [{"key": "rs_presets_page", "name": "Presets", "type": "canvas", "as_page": True, "preset_browser": True,
          "canvas_script": "canvas.js"}]

CHAIN = PARAMS + READOUTS + PAGES

def entry(k):
    return k

def line(o): return json.dumps(o, separators=(", ", ": "))

out = []
a = out.append
a("{")
for k, v in [("id", "tinyvoice"), ("name", "TinyVoice"), ("abbrev", "TVO"), ("version", "1.5.0"),
             ("description", "Four engines for the voice: a 13-mode vocoder and voice transformer (Robot, Snap, Chords, Prism, Tape, Nightcall, Scream, Dalek, HardTune, Harmony, Walkie, Morph, Double), Synth Voice (gives a synth a voice with vowel formants; one page, presets) Resynth (hard tuning and harmonic resynthesis with a wavetable synth; imports the reference presets) and Mangler (a multi-voice pitch and formant shifter: unison, spread, chords from the pads, a filter)"),
             ("author", "Mat Taylor"), ("dsp", "tinyvoice.so"), ("api_version", 2), ("component_type", "audio_fx")]:
    a("    %s: %s," % (json.dumps(k), json.dumps(v)))
a('    "capabilities": {')
a('        "chainable": true,')
a('        "component_type": "audio_fx",')
a('        "audio_in": true,')
a('        "midi_in": true,')
a('        "ui_hierarchy": {')
# `modes` + `mode_param`: the host adds its own Mode page (writes `engine`), and the
# active mode's level is the walk root. Level names are the modes, lower case.
a('            "modes": ["vocoder", "synth voice", "resynth", "mangler"],')
a('            "mode_param": "engine",')
a('            "levels": {')
a('                "vocoder": {')
a('                    "name": "TinyVoice",')
a('                    "knobs": %s,' % line(VOICE))
a('                    "params": [')
a('                        {"level": "VOICE", "label": "Voice"},')
a('                        {"level": "PITCH", "label": "Pitch"},')
a('                        {"level": "MORE", "label": "More"},')
a('                        {"level": "LOOPER", "label": "Looper"}')
a('                    ]')
a('                },')
for name, label, keys, last in [("VOICE", "Voice", VOICE, False), ("PITCH", "Pitch", PITCH, False), ("MORE", "More", MORE, False),
                                ("LOOPER", "Looper", LOOPER, False)]:
    a('                "%s": {' % name)
    a('                    "label": "%s",' % label)
    a('                    "knobs": %s,' % line(keys))
    a('                    "params": %s' % line([entry(k) for k in keys + MENU_ONLY.get(name, [])]))
    a('                }%s' % ("" if last else ","))
a('                "synth voice": {')
a('                    "name": "Synth Voice",')
a('                    "list_param": "vf_preset",')
a('                    "count_param": "vf_preset_count",')
a('                    "name_param": "vf_preset_name",')
a('                    "knobs": %s,' % line(VOICEFX))
# Audition: Demo, on a page of its own after the eight knobs (the Mode page and the preset browser are
# drawn by the host, so nothing of ours can sit on them): in Voice FX Demo plays the funk groove.
a('                    "params": %s' % line(VOICEFX + [{"level": "audition", "label": "Audition"}]))
a('                },')
a('                "audition": {')
a('                    "label": "Audition",')
a('                    "knobs": %s,' % line(AUDITION))
a('                    "params": %s' % line(AUDITION))
a('                },')
a('                "resynth": {')
a('                    "name": "Resynth",')
# Presets first (the host draws the browser as the level's first page): the Resynth presets in Schwung's
# preset folder, read into memory when TinyVoice loads (rsbank.c)
a('                    "list_param": "rs_preset",')
a('                    "count_param": "rs_preset_count",')
a('                    "name_param": "rs_preset_name",')
a('                    "knobs": %s,' % line(RS_MAIN))
a('                    "params": %s' % line(RS_MAIN + ["rs_presets_page", {"level": "rs_pitch", "label": "Pitch"}, {"level": "rs_synth", "label": "Synth"},
                                                      {"level": "rs_tone", "label": "Tone"}, {"level": "rs_play", "label": "Play"},
                                                      {"level": "rs_notes", "label": "Notes"}, {"level": "rs_looper", "label": "Looper"},
                                                      {"level": "rs_bank", "label": "Bank"}]))
a('                },')
for name, label, keys, last in [("rs_pitch", "Pitch", RS_PITCH, False), ("rs_synth", "Synth", RS_SYNTH, False),
                                ("rs_tone", "Tone", RS_TONE, False), ("rs_play", "Play", RS_PLAY, False),
                                ("rs_notes", "Notes", RS_NOTES, False), ("rs_looper", "Looper", LOOPER, False),
                                ("rs_bank", "Bank", RS_BANK, False)]:
    a('                "%s": {' % name)
    a('                    "label": "%s",' % label)
    a('                    "knobs": %s,' % line(keys))
    a('                    "params": %s' % line(keys))
    a('                }%s' % ("" if last else ","))
a('                "mangler": {')
a('                    "name": "Mangler",')
a('                    "list_param": "rs_preset",')           # the Presets page: the Mangler bank while the engine is Mangler (dsp.c rs_bank_of)
a('                    "count_param": "rs_preset_count",')
a('                    "name_param": "rs_preset_name",')
a('                    "knobs": %s,' % line(MG_MAIN))
a('                    "params": %s' % line(MG_MAIN + ["rs_presets_page", {"level": "mg_more", "label": "More"}, {"level": "mg_filter", "label": "Filter"},
                                                      {"level": "mg_looper", "label": "Looper"}, {"level": "mg_bank", "label": "Bank"}]))
a('                },')
for name, label, keys, last in [("mg_more", "More", MG_MORE, False), ("mg_filter", "Filter", MG_FILTER, False),
                                ("mg_looper", "Looper", LOOPER, False), ("mg_bank", "Bank", RS_BANK, True)]:
    a('                "%s": {' % name)
    a('                    "label": "%s",' % label)
    a('                    "knobs": %s,' % line(keys))
    a('                    "params": %s' % line(keys))
    a('                }%s' % ("" if last else ","))
a('            }')
a('        },')
a('        "chain_params": [')
for j, p in enumerate(CHAIN):
    a('            %s%s' % (line(p), "," if j < len(CHAIN) - 1 else ""))
a('        ]')
a('    }')
a('}')
text = "\n".join(out) + "\n"
doc = json.loads(text)
open(sys.argv[1] if len(sys.argv) > 1 else "module.json", "w", newline="\n").write(text)

BS, DQ = chr(92), chr(34)
def c_string(js):
    return DQ + js.replace(BS, BS + BS).replace(DQ, BS + DQ) + DQ

# The served copy: per-mode name placeholders for the three macros.
served = copy.deepcopy(doc["capabilities"]["chain_params"])
for p in served:
    slot = {"char1": "@1@", "char2": "@2@", "mod": "@3@", "char4": "@4@", "char5": "@5@"}.get(p["key"])
    if slot: p["name"] = slot
cp = json.dumps(served, separators=(",", ":"))
assert "@" not in json.dumps(doc["capabilities"]["chain_params"]), "an @ in the table would be read as a placeholder"
uh = json.dumps(doc["capabilities"]["ui_hierarchy"], separators=(",", ":"))
hdr = sys.argv[2] if len(sys.argv) > 2 else "src/dsp/module_meta.h"
with open(hdr, "w", newline="\n") as f:
    f.write("/* GENERATED by scripts/gen_module_json.py from the same table as module.json. Do not edit. */\n")
    f.write("#ifndef TINYVOICE_MODULE_META_H\n#define TINYVOICE_MODULE_META_H\n\n")
    f.write("/* chain_params with @1@ .. @5@ for the char1 / char2 / mod / char4 / char5 names (filled per mode by dsp.c) */\n")
    # One string per parameter: the whole array is over the 4095 characters a
    # pedantic C compiler must accept in one literal. No part splits a @n@ name placeholder.
    parts = ["[" + json.dumps(served[0], separators=(",", ":"))] + [("," + json.dumps(p, separators=(",", ":"))) for p in served[1:]] + ["]"]
    assert all(len(x) < 4000 for x in parts) and "".join(parts) == cp
    f.write("static const char *const VT_CHAIN_PARAMS_PARTS[] = {\n" + (",\n".join("    " + c_string(x) for x in parts)) + "\n};\n\n")
    # the hierarchy is over 4095 characters too (a pedantic compiler's limit for one literal): in parts, joined by dsp.c
    uparts = [uh[i:i + 3000] for i in range(0, len(uh), 3000)]
    assert "".join(uparts) == uh
    f.write("static const char *const VT_UI_HIERARCHY_PARTS[] = {\n" + (",\n".join("    " + c_string(x) for x in uparts)) + "\n};\n\n#endif\n")
print("wrote", len(PARAMS), "params +", len(READOUTS), "readouts +", len(PAGES), "page;", hdr, "with", len(cp), "+", len(uh), "bytes of JSON")
