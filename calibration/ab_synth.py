"""A/B of the Synthesize panel: Octave, Note Shift, Synth Mix, Shape, Stretch, Warble."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import *  # noqa
import tv_host as T
def hz(m): return 440 * 2 ** ((m - 69) / 12)
def tv(xml, x, midi=None): return T.render(xml, x, midi)[0]
B = dict(robotify=0.0, sharpen=0.0, tuning_duophonic=0, min_f0=40.0)
f0 = hz(52); xa = S.voice(S.f0_const(f0, 2.0), "a")
def lines(y, f, n=6):
    h = A.harmonics(y, f, .5, 1.9, kmax=n)
    return float(10 * np.log10(np.sum(10 ** (h / 10))))
print("== octave: sub (odd multiples of f0/2), odd and even harmonics, total level")
for tr in (0.0, 1.0):
    for oc in (-1.0, -0.5, 0.5, 1.0, 1.5):
        row = []
        for who, f in (("HUM", hum), ("TV", tv)):
            y = f(init(**B, transform=tr, octaving=oc), xa)
            sub = 10 * np.log10(np.sum(10 ** (A.harmonics(y, f0 / 2, .5, 1.9, kmax=8)[0::2] / 10)))
            hh = A.harmonics(y, f0, .5, 1.9, kmax=8)
            row.append(f"{who} sub {sub:6.1f} odd {10*np.log10(np.sum(10**(hh[0::2]/10))):6.1f} even {10*np.log10(np.sum(10**(hh[1::2]/10))):6.1f} lvl {A.rms_db(A.mono(y)[22050:80000]):6.1f}")
        print(f"  tr {tr} oct {oc:+.1f}: " + " | ".join(row))
print("== note shift (+1 st keeps the lines apart): main, shift line, total")
for tr in (0.0, 1.0):
    for ns, sm in ((1.0, 1.0), (1.0, 0.5), (-1.0, 1.0)):
        row = []
        for who, f in (("HUM", hum), ("TV", tv)):
            y = f(init(**dict(B, transform=tr, pitch_shift=ns, synth_mix=sm, tuning_mode=1, pitch_mono=52.0)), xa)
            row.append(f"{who} main {lines(y, f0):6.1f} shift {lines(y, f0 * 2 ** (ns / 12)):6.1f} lvl {A.rms_db(A.mono(y)[22050:80000]):6.1f}")
        print(f"  tr {tr} ns {ns:+.0f} sm {sm}: " + " | ".join(row))
print("== note shift +7 in Scale mode (C major), voice on A3: which note does the line take?")
xv = S.voice(S.f0_const(hz(57), 2.0), "a")
for ns in (2.0, 4.0, 7.0):
    for who, f in (("HUM", hum), ("TV", tv)):
        y = f(init(**B, pitch_shift=ns, tuning_mode=0), xv)
        fr, db = A.spectrum_db(y, .5, 1.9)
        cands = []
        for m in range(57, 72):
            if m == 57: continue
            sel = (fr > hz(m) * .99) & (fr < hz(m) * 1.01)
            cands.append((db[sel].max(), m))
        top = max(cands)
        print(f"  shift {ns:+.0f} {who}: strongest other fundamental {note_name(top[1])} ({top[0]:.1f} dB)")
print("== synth mix 0.5 (transform 0, 1)")
for tr in (0.0, 1.0):
    print(f"  tr {tr}:", " | ".join(f"{who} lvl {A.rms_db(A.mono(f(init(**B, transform=tr, synth_mix=0.5), xa))[22050:80000]):6.1f}" for who, f in (("HUM", hum), ("TV", tv))))
print("== shape / stretch at transform 1 (harmonics re H1)")
for sh, st in ((0.5, 0.5), (1.0, 0.5), (0.0, 0.1), (0.0, 0.3), (0.0, 0.7), (0.0, 0.9)):
    for who, f in (("HUM", hum), ("TV ", tv)):
        h = A.harmonics(f(init(**B, transform=1.0, wavetable_shape=sh, wavetable_stretch=st), xa), f0, .5, 1.9, kmax=14)
        print(f"  shape {sh} stretch {st} {who}", np.round(h - h[0], 0).astype(int).tolist())
print("== warble")
for mode in (0.0, 1.0):
    for rate in (0.25, 0.5, 1.0, -0.5):
        row = []
        for who, f in (("HUM", hum), ("TV", tv)):
            y = f(init(**B, warble_depth=1.0, warble_rate=rate, warble_mode=mode), xa)
            t, ff, ap = A.f0_track(y, hop=128, win=1024); d, r = A.pitch_mod(ff[t > .5], 128 / 44100)
            fr, db = A.spectrum_db(y, .5, 1.9)
            row.append(f"{who} depth {d:6.1f} c rate {r:5.1f} Hz")
        print(f"  mode {mode} rate {rate:+.2f}: " + " | ".join(row))
for dep in (0.25, 0.5):
    print(f"  LFO depth {dep}:", " | ".join(f"{who} {A.pitch_mod(A.f0_track(f(init(**B, warble_depth=dep, warble_rate=0.5, warble_mode=1.0), xa), hop=128, win=1024)[1][20:], 128/44100)[0]:6.1f} c" for who, f in (("HUM", hum), ("TV", tv))))
