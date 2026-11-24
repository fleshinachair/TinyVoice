"""Timing probes: latency per Latency setting, retune speed (Quantize, Attack), vibrato pass-through."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import *  # noqa

def hz(m): return 440 * 2 ** ((m - 69) / 12)

# --- latency: 0.5 s silence then a voice; per lookahead, wet and (dry_wet 0) dry
burst = S.cat(S.silence(0.5), S.voice(S.f0_const(hz(57), 1.0), "a", fade=0.002), S.silence(0.3))
for la in (0, 1, 2):
    y = hum(init(lookahead=la, tuning_duophonic=0), burst)
    yd = hum(init(lookahead=la, tuning_duophonic=0, dry_wet=0.0), burst)
    # dry latency by cross-correlation with the input
    c = np.correlate(A.mono(yd)[:30000], burst[:30000], "full"); lag = np.argmax(np.abs(c)) - 29999
    print(f"latency setting {la}: wet onset {1000*A.latency_sec(burst, y):6.1f} ms, dry lag {1000*lag/44100:6.1f} ms ({lag} smp)")

# --- retune speed: G3 for 1 s then A3+40c for 1.5 s (C major: target A3)
step = S.voice(np.concatenate([S.f0_const(hz(55), 1.0), S.f0_const(hz(57.4), 1.5)]), "a")
def traj(y, t0=1.0, t1=1.6):
    t, f, ap = A.f0_track(y, hop=64, win=1024)
    m = (t > t0 - 0.05) & (t < t1)
    return t[m] - t0, A.midi(f[m]) - 57
for q in (0.03, 0.1, 0.25, 0.5):
    for att in (0.0, 0.1, 0.2, 0.4):
        y = hum(init(quantize=q, tuner_attack=att, tuning_duophonic=0), step)
        tt, c = traj(y)
        # time from the step until within 10 cents of A3 and staying there
        ok = np.abs(c) < 0.10
        t_in = np.nan
        for i in range(len(ok)):
            if ok[i:].all() and tt[i] > 0:
                t_in = tt[i]; break
        early = c[(tt > 0.02) & (tt < 0.06)]
        print(f"quantize {q:4.2f} attack {att:3.1f}: on A3 within 10c after {1000*t_in:6.1f} ms; 20-60 ms after step: {np.nanmean(early)*100:+6.1f} c; at 0.5 s: {np.nanmean(c[(tt>0.45)&(tt<0.55)])*100:+5.1f} c")

# --- vibrato pass-through: A3 with 5.5 Hz +-60 c vibrato
vib = S.voice(S.f0_vibrato(hz(57), 3.0, 5.5, 60), "a")
for q in (0.03, 0.25, 0.5):
    for fl in (0.0, 1.0):
        for rob in (0.0, 1.0):
            y = hum(init(quantize=q, robotify=rob, robotify_flatten=fl, tuning_duophonic=0), vib)
            t, f, ap = A.f0_track(y)
            m = t > 0.5
            d, r = A.pitch_mod(f[m])
            print(f"vibrato in 60c: quantize {q:4.2f} robotify {rob} flatten {fl}: out depth {d:5.1f} c rate {r:4.1f} Hz, median {A.midi(np.nanmedian(f[m]))-57:+.2f} st")
