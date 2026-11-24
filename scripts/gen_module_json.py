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
    enum("freeze_len", "Freeze Len", ["Free", "1 Bar", "2 Bars", "4 Bars"], 2),   # Rec stops itself after N bars
    flt("beauty", "Beauty", 0, 1, 0.01, 0.5, "%"),          # per-mode polish: stereo, detune, air, warmth
    enum("latch", "Latch", ["Off", "On"], 0),               # pad chord holds after release (Robot, Chords, Prism, Nightcall, Harmony)
    flt("char4", "Char 4", 0, 1, 0.01, 0.5, "%"),           # macros 4 and 5: per mode a new control, or an alias of an old param
    flt("char5", "Char 5", 0, 1, 0.01, 0.5, "%"),
    # the personalities: the Mode selector page writes `engine`; the vf_* params are the Voice FX page,
    # the hm_* params (after vf_preset) the Resynth engine's
    enum("engine", "Engine", ["Vocoder", "Synth Voice", "Resynth"], 0),
    flt("vf_vowel", "Vowel", 0, 1, 0.01, 0.0, "%"),         # A E I O U sweep
    flt("vf_tract", "Tract", 0, 1, 0.01, 0.5, "%"),         # formant scale / gender
    flt("vf_peak", "Peak", 0, 1, 0.01, 0.5, "%"),           # formant Q
    flt("vf_choir", "Choir", 0, 1, 0.01, 0.35, "%"),        # L/R formant detune and width
    flt("vf_breath", "Breath", 0, 1, 0.01, 0.15, "%"),      # noise through the formants + consonant air
    flt("vf_attack", "Attack", 0, 1, 0.01, 0.12, "%"),      # onset swell
    flt("vf_release", "Release", 0, 1, 0.01, 0.3, "%"),     # tail
    flt("vf_mix", "Mix/Drv", 0, 1, 0.01, 0.75, "%"),        # wet/dry up to 70 %, then all wet with the drive climbing
    flt("vf_preset", "Preset", 0, 5, 1, 5),                 # the Voice FX bank (presets.c); the host's preset browser writes it
    enum("rs_bank", "Bank", ["Default", "Imported"], 0),    # which bank the Presets page lists: the shipped presets, or yours (imported / saved)
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
    enum("hm_midi_idle", "No Pad", ["Tune", "Silent"], 0),                      # MIDI mode with no pad held: keep tuning to the scale, or go silent (Humanoid)
    enum("hm_wave", "Wave", ["Saw", "Square", "Triangle", "Sine", "Preset"], 0),  # Preset = the wavetable a preset carried (hm_wt)
]
PARAMS += HM + HM_EXTRA + [
    # performance toggles (knobs on MORE; not saved in "state")
    enum("freeze", "Freeze", ["Off", "Rec", "Loop"], 0),       # punch-in looper (src/dsp/freeze.h)
    enum("demo", "Demo", ["Off", "On"], 0),
]

# Read-only readouts the DSP serves (not params: never set, never saved).
# They sit under the OLED scope on PITCH knobs 5-7, so turning those knobs
# does nothing, and their names label the cells under the picture.
def readout(key, name, lo, hi, step, unit):
    r = flt(key, name, lo, hi, step, 0, unit)
    r["access"] = "read"
    return r

READOUTS = [
    readout("pitch", "Pitch", 0, 2000, 0.1, "Hz"),
    readout("target", "Target", 0, 2000, 0.1, "Hz"),
    readout("cents", "Cents", -50, 50, 1, "ct"),
]
for role, r in zip(["a", "b", "c"], READOUTS):
    r["viz"] = {"kind": "custom:tinyvoice_scope", "group": "scope", "role": role, "extra_keys": ["scope"]}

# The Freeze banner spans MORE knobs 3-4 (Freeze, Freeze Len): Off / ARMED /
# REC / LOOP, the take length, and whether a transport clock is seen. The knobs
# under it still turn; their values show in the header while touched.
for role, k in zip(["a", "b"], ["freeze", "freeze_len"]):
    p = next(x for x in PARAMS if x["key"] == k)
    p["viz"] = {"kind": "custom:tinyvoice_freeze", "group": "freeze", "role": role, "extra_keys": ["scope"]}

# Latch (MORE knob 6) draws its own value: On / Off in the modes it acts in
# (Robot, Chords, Prism, Nightcall, Harmony), a dimmed "---" in the others.
next(x for x in PARAMS if x["key"] == "latch")["viz"] = {"kind": "custom:tinyvoice_latch", "group": "latch", "role": "a", "extra_keys": ["mode"]}

# Knobs that do nothing in some modes draw their own value (a dimmed "N/A"
# where they are inert): Key, Scale, Retune, Glide, Beauty (all on PITCH). Same
# cell, same knob. The table of modes is vt_param_acts in dsp.h and GATED in
# src/ui/canvas.js.
for k in ["key", "scale", "retune", "glide", "beauty"]:
    next(x for x in PARAMS if x["key"] == k)["viz"] = {"kind": "custom:tinyvoice_" + k, "group": k, "role": "a", "extra_keys": ["mode"]}

# Pages. VOICE is the home page (and the root's knobs): Mode, the five
# contextual knobs, Mix, Level. PITCH: the tuning knobs and the scope on 5-7.
# MORE: Spread, Tone, Freeze + Freeze Len (the banner spans knobs 3-4) / Gate, Latch,
# Beauty, Demo: eight knobs, Demo bottom right (the grid is 4 x 2 and a page cannot
# have a gap, so Beauty, a per-mode polish control, moved here from PITCH to fill cell 7;
# Gate and Latch used to sit alone on a page of their own).
# No delay or reverb: removed on 2026-10-11, the chain has its own.
VOICE = ["mode", "char1", "char2", "mod", "char4", "char5", "mix", "gain"]
PITCH = ["key", "scale", "retune", "glide", "pitch", "target", "cents"]
MORE  = ["spread", "tone", "freeze", "freeze_len", "gate", "latch", "beauty", "demo"]
# Voice FX: its whole UI. No Mode, Key, Scale, Gate, Level or Latch: the engine runs with the automatic front end.
AUDITION = ["demo"]
RS_BANK = ["rs_bank"]
VOICEFX = ["vf_vowel", "vf_tract", "vf_peak", "vf_choir", "vf_breath", "vf_attack", "vf_release", "vf_mix"]
# Resynth: the main controls on the Move, four more pages; everything else is on the Remote UI (web_ui.html).
RS_MAIN  = ["hm_transform", "hm_robotify", "hm_formant_shift", "hm_synth_mix", "hm_pitch_shift", "hm_octaving", "hm_quantize", "hm_dry_wet"]
RS_PITCH = ["hm_tuning_mode", "hm_key", "hm_scale", "hm_scale_hold", "hm_tuner_attack", "hm_min_f0", "hm_max_f0", "hm_tuning_duophonic"]
RS_SYNTH = ["hm_wave", "hm_wavetable_shape", "hm_wavetable_stretch", "hm_warble_depth", "hm_warble_rate", "hm_warble_mode", "hm_widen", "hm_smoothing"]
RS_TONE  = ["hm_hpf_freq", "hm_lpf_freq", "hm_filter_freq", "hm_filter_gain", "hm_noisegate_threshold", "hm_deess", "hm_sharpen", "hm_output_gain"]
RS_PLAY  = ["hm_freeze", "hm_lookahead", "hm_freeze_sync", "hm_freeze_synclength", "hm_input_gain", "demo"]
RS_NOTES = ["hm_pitch_mono", "hm_pitch_duo", "latch", "hm_midi_idle"]      # Note mode's two notes, and the pads (MIDI mode)
MENU_ONLY = {}

CHAIN = PARAMS + READOUTS

def entry(k):
    return k

def line(o): return json.dumps(o, separators=(", ", ": "))

out = []
a = out.append
a("{")
for k, v in [("id", "tinyvoice"), ("name", "TinyVoice"), ("abbrev", "TVO"), ("version", "1.0.1"),
             ("description", "Three engines for the voice: a 13-mode vocoder and voice transformer (Robot, Snap, Chords, Prism, Tape, Nightcall, Scream, Dalek, HardTune, Harmony, Walkie, Morph, Double), Synth Voice (a one-page talking formant filter with presets) and Resynth (hard tuning and harmonic resynthesis with a wavetable synth; imports Humanoid presets)"),
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
a('            "modes": ["vocoder", "synth voice", "resynth"],')
a('            "mode_param": "engine",')
a('            "levels": {')
a('                "vocoder": {')
a('                    "name": "TinyVoice",')
a('                    "knobs": %s,' % line(VOICE))
a('                    "params": [')
a('                        {"level": "VOICE", "label": "Voice"},')
a('                        {"level": "PITCH", "label": "Pitch"},')
a('                        {"level": "MORE", "label": "More"}')
a('                    ]')
a('                },')
for name, label, keys, last in [("VOICE", "Voice", VOICE, False), ("PITCH", "Pitch", PITCH, False), ("MORE", "More", MORE, False)]:
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
a('                    "params": %s' % line(RS_MAIN + [{"level": "rs_pitch", "label": "Pitch"}, {"level": "rs_synth", "label": "Synth"},
                                                      {"level": "rs_tone", "label": "Tone"}, {"level": "rs_play", "label": "Play"},
                                                      {"level": "rs_notes", "label": "Notes"}, {"level": "rs_bank", "label": "Bank"}]))
a('                },')
for name, label, keys, last in [("rs_pitch", "Pitch", RS_PITCH, False), ("rs_synth", "Synth", RS_SYNTH, False),
                                ("rs_tone", "Tone", RS_TONE, False), ("rs_play", "Play", RS_PLAY, False),
                                ("rs_notes", "Notes", RS_NOTES, False), ("rs_bank", "Bank", RS_BANK, True)]:
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
open(sys.argv[1] if len(sys.argv) > 1 else "module.json", "w").write(text)

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
with open(hdr, "w") as f:
    f.write("/* GENERATED by scripts/gen_module_json.py from the same table as module.json. Do not edit. */\n")
    f.write("#ifndef TINYVOICE_MODULE_META_H\n#define TINYVOICE_MODULE_META_H\n\n")
    f.write("/* chain_params with @1@ .. @5@ for the char1 / char2 / mod / char4 / char5 names (filled per mode by dsp.c) */\n")
    # One string per parameter: the whole array is over the 4095 characters a
    # pedantic C compiler must accept in one literal. No part splits a @n@ name placeholder.
    parts = ["[" + json.dumps(served[0], separators=(",", ":"))] + [("," + json.dumps(p, separators=(",", ":"))) for p in served[1:]] + ["]"]
    assert all(len(x) < 4000 for x in parts) and "".join(parts) == cp
    f.write("static const char *const VT_CHAIN_PARAMS_PARTS[] = {\n" + (",\n".join("    " + c_string(x) for x in parts)) + "\n};\n\n")
    f.write("static const char VT_UI_HIERARCHY_JSON[] = " + c_string(uh) + ";\n\n#endif\n")
print("wrote", len(PARAMS), "params +", len(READOUTS), "readouts;", hdr, "with", len(cp), "+", len(uh), "bytes of JSON")
