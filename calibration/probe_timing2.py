"""Quantize as a fraction or a filter; latency from gaps in a steady voice."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import *  # noqa
def hz(m): return 440 * 2 ** ((m - 69) / 12)

# static offset: A3+30c held
st = S.voice(S.f0_const(hz(57.3), 2.0), "a")
for q in (0.03, 0.1, 0.25, 0.4, 0.5):
    y = hum(init(quantize=q, tuning_duophonic=0), st)
    print(f"static +30c, quantize {q:4.2f}: out {(A.midi(out_pitch(y, 0.8, 1.9)) - 57)*100:+6.1f} c")
# vibrato depth pass-through vs rate
for q in (0.03, 0.25, 0.5):
    row = []
    for rate in (1.0, 3.0, 5.5, 10.0):
        vib = S.voice(S.f0_vibrato(hz(57), 3.0, rate, 40), "a")
        y = hum(init(quantize=q, tuning_duophonic=0), vib)
        t, f, ap = A.f0_track(y, hop=128, win=1024)
        d, r = A.pitch_mod(f[t > 0.6], 128 / 44100)
        row.append(f"{rate:4.1f}Hz:{d/40*100:5.0f}%")
    print(f"vibrato 40c pass-through, quantize {q:4.2f}:", " ".join(row))
for pol in (1,):
    for q in (0.03, 0.5):
        vib = S.voice(S.f0_vibrato(hz(57), 3.0, 5.5, 40), "a")
        y = hum(init(quantize=q, tuner_polarity=pol, tuning_duophonic=0), vib)
        t, f, ap = A.f0_track(y, hop=128, win=1024); d, r = A.pitch_mod(f[t > 0.6], 128 / 44100)
        print(f"polarity 1 quantize {q}: {d/40*100:.0f}%")
for rel in (-1.0, -0.25, 0.1):
    vib = S.voice(S.f0_vibrato(hz(57), 3.0, 5.5, 40), "a")
    y = hum(init(quantize=0.25, relax=rel, tuning_duophonic=0), vib)
    t, f, ap = A.f0_track(y, hop=128, win=1024); d, r = A.pitch_mod(f[t > 0.6], 128 / 44100)
    print(f"relax {rel}: {d/40*100:.0f}%")

# latency from two 40 ms gaps in a steady A3
f0 = S.f0_const(hz(57), 3.0)
v = S.voice(f0, "a", fade=0.002)
g = np.ones_like(v)
for c in (1.0, 2.0):
    a, b = int((c - 0.02) * 44100), int((c + 0.02) * 44100)
    g[a:b] = 0
v = (v * g).astype(np.float32)
def gap_centres(y):
    e = np.sqrt(np.convolve(A.mono(y) ** 2, np.ones(441) / 441, "same"))
    out = []
    for c in (1.0, 2.0):
        a, b = int((c - 0.1) * 44100), int((c + 0.5) * 44100)
        seg = e[a:b]
        thr = 0.3 * np.median(e[int(0.5 * 44100):int(0.9 * 44100)])
        low = np.where(seg < thr)[0]
        out.append(((a + low.mean()) / 44100 - c) * 1000 if len(low) else np.nan)
    return out
for la in (0, 1, 2):
    for gate in (-80.0,):
        y = hum(init(lookahead=la, tuning_duophonic=0, noisegate_threshold=gate), v)
        print(f"lookahead {la}: gap centre delay (ms)", np.round(gap_centres(y), 1))
