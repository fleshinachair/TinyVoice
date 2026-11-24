"""Builds Resynth's Default bank: 30 original presets, written to src/presets/*.json (shipped in the module).

    calibration/.venv/Scripts/python.exe -I calibration/make_default_bank.py [--trim]

They are TinyVoice's own: inspired by the kinds of sound a hard-tune / resynthesis vocal engine makes,
not copies of anyone's presets. Wavetables (organ, glass, pulse, bell, vowel) are additive recipes
written here. Each preset is rendered through the real engine (build/tv_state_render.exe) with a sung
vocal at -30 dBFS and its level reported (Auto Level in the engine keeps them together; --trim also writes an Out Gain trim).

A preset names only what it changes: choosing one resets Resynth to its defaults first. None sets Key,
Scale or Notes (Hold Scale keeps yours).
"""
import base64, glob, json, math, os, struct, subprocess, sys, tempfile
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
OUT = ROOT + "/src/presets"
EXE = ROOT + "/build/tv_state_render.exe"
TARGET_DB = -21.0          # output level over the sung part, input at -30 dBFS


# ------------------------------------------------------------------ wavetables
def cycle(harm, n=512):
    """harm: {harmonic: (amp, phase)} -> one cycle, peak 1."""
    x = np.zeros(n)
    t = np.arange(n) / n
    for k, (a, ph) in harm.items():
        x += a * np.sin(2 * math.pi * k * t + ph)
    return x / max(1e-9, np.abs(x).max())


def pulse(duty, n=512):
    h = {k: (math.sin(math.pi * k * duty) / k, -math.pi * k * duty + math.pi / 2) for k in range(1, 60)}
    return cycle(h, n)


WT = {
    "organ": cycle({1: (1, 0), 2: (.8, 0), 3: (.55, 0), 4: (.5, 0), 6: (.35, 0), 8: (.3, 0), 10: (.12, 0)}),
    "glass": cycle({1: (1, 0), 3: (.5, 1.2), 5: (.42, 2.1), 7: (.3, .4), 9: (.22, 1.7), 13: (.12, 2.6), 17: (.08, 0.9)}),
    "pulse": pulse(0.125),
    "bell": cycle({1: (.55, 0), 2: (1, 0.7), 3: (.25, 1.4), 4: (.8, 2.1), 6: (.5, .3), 7: (.35, 1.1), 9: (.3, 2.4), 11: (.2, .6)}),
    "vowel": cycle({k: (math.exp(-((k - 5) ** 2) / 9) + .75 * math.exp(-((k - 11) ** 2) / 14) + .06 / k, 0) for k in range(1, 40)}),
    "hollow": cycle({1: (1, 0), 3: (.62, 0), 5: (.45, 0), 7: (.33, 0), 9: (.25, 0), 11: (.18, 0)}),
}


def b64(c):
    pcm = struct.pack("<512h", *[int(round(max(-1, min(1, v)) * 32767)) for v in c])
    return base64.b64encode(pcm).decode("ascii")


# ------------------------------------------------------------------ the presets
# values are Humanoid-style ids (hm_ prefix added); wave: 'saw'|'square'|'triangle'|'sine'|<WT name>
SCALE, NOTE, MIDI = 0, 1, 2
P = [
    # --- Tune: the voice, corrected
    ("Tune - Natural Fix", dict(quantize=0.12, robotify=0, synth_mix=0.15, smoothing=0.3)),
    ("Tune - Studio Pop", dict(quantize=0.3, robotify=0.2, synth_mix=0.35, sharpen=60, deess=6)),
    ("Tune - Hard Snap", dict(quantize=0.5, robotify=0.35, synth_mix=0.4, tuner_attack=0.0, sharpen=70)),
    ("Tune - Robot Pop", dict(quantize=0.5, robotify=1, robotify_emphasis=0.3, synth_mix=1, sharpen=90)),
    ("Tune - Chrome Girl", dict(quantize=0.5, robotify=0.8, formant_shift=0.38, sharpen=95, synth_mix=0.9, widen=0.25)),
    ("Tune - Slow Slide", dict(quantize=0.2, tuner_attack=0.3, tuner_hold=40, robotify=0, synth_mix=0.3)),
    # --- Voco: the voice as a carrier
    ("Voco - Saw Choir", dict(wave="saw", transform=0.7, robotify=1, quantize=0.5, widen=0.45, sharpen=80)),
    ("Voco - Square Wave Talker", dict(wave="square", transform=0.8, robotify=1, quantize=0.5, octaving=0.35, formant_shift=-0.1)),
    ("Voco - Glass Pad", dict(wave="glass", transform=0.85, robotify=1, quantize=0.45, smoothing=0.6, widen=0.8, hpf_freq=48)),
    ("Voco - Vowel Organ", dict(wave="organ", transform=1, robotify=1, quantize=0.5, wavetable_shape=0.4, widen=0.3)),
    ("Voco - Talk Box", dict(wave="saw", transform=0.6, robotify=0.7, sharpen=100, filter_freq=82, filter_gain=10, filter_width=26)),
    ("Voco - Android", dict(wave="vowel", transform=0.9, robotify=1, robotify_harm=0.4, warble_depth=0.25, warble_rate=0.4, quantize=0.5)),
    # --- Harmony: lines around the voice
    ("Harm - Octave Down", dict(octaving=-1, synth_mix=0.85, quantize=0.4, robotify=0.3)),
    ("Harm - Octave Shimmer", dict(octaving=1.2, synth_mix=0.85, quantize=0.4, widen=0.5, lpf_freq=118)),
    ("Harm - Fifth Above", dict(pitch_shift=7, quantize=0.45, robotify=0.5, synth_mix=0.9, widen=0.3)),
    ("Harm - Third and Octave", dict(pitch_shift=4, octaving=-0.6, quantize=0.45, robotify=0.4, widen=0.4)),
    ("Harm - Drone Duo", dict(tuning_mode=NOTE, tuning_duophonic=1, pitch_mono=57, pitch_duo=64, transform=0.5, wave="saw", synth_mix=1, widen=0.5)),
    ("Harm - Play Me", dict(tuning_mode=MIDI, tuning_duophonic=1, transform=0.55, wave="triangle", quantize=0.5, synth_mix=1, widen=0.3)),
    # --- Synth: the voice drives an instrument
    ("Synth - Sub Voice", dict(wave="sine", octaving=-1, transform=0.7, lpf_freq=72, robotify=1, quantize=0.5, synth_mix=1)),
    ("Synth - Pluck Saw", dict(wave="saw", transform=1, robotify=1, quantize=0.5, smoothing=0.1, wavetable_stretch=0.3, wavetable_shape=0.3)),
    ("Synth - Pulse Lead", dict(wave="pulse", transform=1, robotify=1, quantize=0.5, filter_freq=96, filter_gain=7)),
    ("Synth - Bell Tone", dict(wave="bell", transform=0.9, robotify=1, quantize=0.5, wavetable_shape=0.5, widen=0.4)),
    ("Synth - Hollow Reed", dict(wave="hollow", transform=0.9, robotify=1, quantize=0.45, hpf_freq=44, warble_depth=0.15)),
    ("Synth - Wide Detune", dict(wave="saw", transform=0.8, robotify=1, quantize=0.5, widen=1, widen_rate=3, warble_depth=0.35, warble_rate=0.5, warble_mode=1)),
    # --- Fx
    ("Fx - Tape Warble", dict(quantize=0.15, robotify=0, synth_mix=0.5, warble_depth=0.7, warble_rate=0.35, warble_mode=0, lpf_freq=112)),
    ("Fx - Telephone", dict(quantize=0.25, synth_mix=0.5, hpf_freq=71, lpf_freq=96, filter_freq=84, filter_gain=8, robotify=0.3)),
    ("Fx - Dark Choir", dict(wave="vowel", transform=0.5, formant_shift=-0.28, lpf_freq=88, widen=0.7, robotify=0.8, quantize=0.5)),
    ("Fx - Bright Air", dict(quantize=0.3, synth_mix=0.6, sharpen=100, sharpen_bias=3, deess=8, filter_freq=112, filter_gain=7, filter_width=30)),
    ("Fx - Whisper Robot", dict(robotify=1, robotify_res=0.3, synth_mix=0.5, quantize=0.5, formant_shift=0.1, sharpen=40, transform=0.2)),
    ("Init - Resynth", dict()),
]
assert len(P) == 30 and len({n for n, _ in P}) == 30


def state_of(vals, trim=0.0):
    st = {"engine": "Resynth"}
    for k, v in vals.items():
        if k == "wave":
            if v in ("saw", "square", "triangle", "sine"):
                st["hm_wave"] = ["saw", "square", "triangle", "sine"].index(v)
            else:
                st["hm_wave"] = 4
                st["hm_wt"] = b64(WT[v])
        else:
            st["hm_" + k] = v
    if trim:
        st["hm_output_gain"] = round(trim, 2)
    return st


def write(name, vals, trim):
    os.makedirs(OUT, exist_ok=True)
    doc = {"name": name, "module": "tinyvoice", "version": 1, "state": state_of(vals, trim)}
    with open("%s/%s.json" % (OUT, name), "w", encoding="utf-8") as f:
        json.dump(doc, f, separators=(",", ":"))


# ------------------------------------------------------------------ measuring
def win_db(x, n=4410):
    m = len(x) // n
    return 10 * np.log10(np.mean(x[:m * n].reshape(m, n).astype(np.float64) ** 2, 1) + 1e-20)


def voice(level=-30.0):
    sr, x = wavfile.read(ROOT + "/calibration/vocals/vocadito_4.wav")
    x = x.astype(np.float32)
    if x.ndim > 1:
        x = x.mean(1)
    if abs(x).max() > 1.5:
        x = x / 32768
    x = resample_poly(x, 44100, sr).astype(np.float32)[: 10 * 44100]
    act = win_db(x) > -45
    x = (x * 10 ** ((level - np.mean(win_db(x)[act])) / 20)).astype(np.float32)
    x = np.concatenate([1e-5 * np.random.RandomState(1).randn(88200).astype(np.float32), x])
    return x, win_db(x) > -55


def measure(tmp, x, act, st):
    open(tmp + "/in.f32", "wb").write(x.tobytes())
    open(tmp + "/st.json", "w").write(json.dumps(st))
    subprocess.check_call([EXE, tmp + "/st.json", tmp + "/in.f32", tmp + "/out.f32"])
    o = np.fromfile(tmp + "/out.f32", np.float32).reshape(-1, 2).mean(1)
    ow = win_db(o)
    return float(np.mean(ow[act])), float(np.mean(ow[act] < -70)), float(np.abs(o).max())


def main():
    trim_on = "--trim" in sys.argv                 # off by default: the engine's Auto Level holds the loudness
    for f in glob.glob(OUT + "/*.json"):
        os.remove(f)
    x, act = voice(-30.0)
    tmp = tempfile.mkdtemp()
    print("%-28s %7s %7s %6s %6s" % ("preset", "before", "after", "trim", "drop%"))
    for name, vals in P:
        lev, drop, pk = measure(tmp, x, act, state_of(vals))
        trim = 0.0
        if trim_on and lev > -90:
            trim = max(-24.0, min(12.0, TARGET_DB - lev))
        lev2, drop2, pk2 = measure(tmp, x, act, state_of(vals, trim)) if trim else (lev, drop, pk)
        write(name, vals, trim)
        print("%-28s %7.1f %7.1f %+6.1f %6.0f%s" % (name, lev, lev2, trim, 100 * drop2, "  peak %.2f" % pk2 if pk2 > 0.95 else ""))


main()
