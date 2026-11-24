"""Synthesize-panel probes: Transform, Synth Mix, Note Shift, Octave, Shape/Stretch, Warble, Widen."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import *  # noqa
def hz(m): return 440 * 2 ** ((m - 69) / 12)
CLEAN = dict(robotify=0.0, sharpen=0.0, tuning_duophonic=0, min_f0=40.0)
f0 = hz(52)  # E3 164.8 Hz
xa = S.voice(S.f0_const(f0, 2.0), "a"); xi = S.voice(S.f0_const(f0, 2.0), "i")
saw = -20 * np.log10(np.arange(1, 21))
def H(y, k=20, f=f0): return A.harmonics(y, f, 0.5, 1.9, kmax=k)
ha, hi = H(xa), H(xi)
print("saw 1/k rel H1:          ", np.round(saw, 0).astype(int).tolist())
print("vowel a in rel H1:       ", np.round(ha - ha[0], 0).astype(int).tolist())
print("vowel i in rel H1:       ", np.round(hi - hi[0], 0).astype(int).tolist())
for tr in (0.0, 0.25, 0.5, 0.75, 1.0):
    ya = hum(init(**CLEAN, transform=tr), xa); yi = hum(init(**CLEAN, transform=tr), xi)
    a, i = H(ya), H(yi)
    print(f"transform {tr:4.2f} a: lvl {A.rms_db(A.mono(ya)[22050:80000]):6.1f}", np.round(a - a[0], 0).astype(int).tolist())
    print(f"               i: lvl {A.rms_db(A.mono(yi)[22050:80000]):6.1f}", np.round(i - i[0], 0).astype(int).tolist())
print("input level", round(A.rms_db(xa[22050:80000]), 1))
for sm in (0.0, 0.5):
    y = hum(init(**CLEAN, transform=1.0, synth_mix=sm), xa); a = H(y)
    print(f"transform 1 synth_mix {sm}: lvl {A.rms_db(A.mono(y)[22050:80000]):6.1f}", np.round(a - a[0], 0).astype(int).tolist())

# Note shift / octave: measure energy at the extra lines (harmonics of f0*2^(s/12)) vs the main
def line_level(y, f):
    h = A.harmonics(y, f, 0.5, 1.9, kmax=6)
    return float(10 * np.log10(np.sum(10 ** (h / 10))))
for tr in (0.0, 1.0):
    for ns in (0.0, 7.0, -12.0, 12.0):
        for sm in (1.0, 0.5):
            y = hum(init(**CLEAN, transform=tr, pitch_shift=ns, synth_mix=sm), xa)
            print(f"transform {tr} note shift {ns:+5.1f} synth_mix {sm}: main {line_level(y, f0):6.1f} dB, line at {ns:+.0f} st {line_level(y, f0 * 2 ** (ns / 12)) if ns else float('nan'):6.1f} dB, lvl {A.rms_db(A.mono(y)[22050:80000]):6.1f}")
for tr in (0.0, 1.0):
    for oc in (-1.0, -0.5, 0.0, 0.5, 1.0, 1.5):
        y = hum(init(**CLEAN, transform=tr, octaving=oc), xa)
        lo = A.harmonics(y, f0 / 2, 0.5, 1.9, kmax=8)[0::2]   # odd multiples of f0/2 = sub octave only
        hi_ = A.harmonics(y, f0, 0.5, 1.9, kmax=8)
        sub = 10 * np.log10(np.sum(10 ** (lo / 10)))
        ev = 10 * np.log10(np.sum(10 ** (hi_[1::2] / 10)))
        od = 10 * np.log10(np.sum(10 ** (hi_[0::2] / 10)))
        print(f"transform {tr} octave {oc:+.1f}: sub-octave {sub:6.1f} dB, odd harmonics {od:6.1f}, even {ev:6.1f}, lvl {A.rms_db(A.mono(y)[22050:80000]):6.1f}")

# shape / stretch at transform 1
for sh, stc in ((0.0, 0.5), (0.5, 0.5), (1.0, 0.5), (0.0, 0.1), (0.0, 0.9)):
    y = hum(init(**CLEAN, transform=1.0, wavetable_shape=sh, wavetable_stretch=stc), xa); a = H(y)
    print(f"transform 1 shape {sh} stretch {stc}:", np.round(a - a[0], 0).astype(int).tolist())

# warble
for mode in (0.0, 1.0):
    for rate in (0.0, 0.5, 1.0):
        y = hum(init(**CLEAN, warble_depth=1.0, warble_rate=rate, warble_mode=mode), xa)
        t, f, ap = A.f0_track(y, hop=128, win=1024)
        d, r = A.pitch_mod(f[t > 0.5], 128 / 44100)
        fr, db = A.spectrum_db(y, 0.5, 1.9)
        print(f"warble mode {mode} rate {rate}: pitch mod depth {d:6.1f} c rate {r:5.1f} Hz, voiced {np.mean(~np.isnan(f[t>0.5])):.2f}")
# widen
for wd, cut, rate in ((0.0, 0.9, 7.0), (0.5, 0.9, 7.0), (1.0, 0.9, 7.0), (1.0, 0.8, 7.0), (1.0, 1.0, 7.0), (1.0, 0.9, 0.3)):
    y = hum(init(**CLEAN, widen=wd, widen_cutoff=cut, widen_rate=rate), xa)
    c, sm_ = A.stereo(y[:, 22050:])
    print(f"widen {wd} cutoff {cut} rate {rate}: L/R corr {c:+.3f}, side/mid {sm_:6.1f} dB, lvl {A.rms_db(A.mono(y)[22050:80000]):6.1f}")
