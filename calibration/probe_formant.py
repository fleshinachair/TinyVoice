import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import *  # noqa
def hz(m): return 440 * 2 ** ((m - 69) / 12)
CLEAN = dict(robotify=0.0, sharpen=0.0, tuning_duophonic=0, min_f0=40.0)
for f0m, vw in ((45, "a"), (52, "o"), (57, "e")):
    f0 = hz(f0m); x = S.voice(S.f0_const(f0, 2.0), vw)
    hx = A.harmonics(x, f0, 0.5, 1.9, kmax=12)
    y0 = hum(init(**CLEAN), x)
    h0 = A.harmonics(y0, f0, 0.5, 1.9, kmax=12)
    print(f"{vw} {f0:.0f} Hz rob 0 per-harmonic diff:", np.round(h0 - hx, 1).tolist())
    for w in (0.0, 0.125, 0.5):
        row = []
        for fs in (-1.0, -0.5, -0.25, 0.25, 0.5, 1.0):
            y = hum(init(**CLEAN, formant_shift=fs, formant_width=w), x)
            s, c = A.envelope_shift_st(y0, y, 0.5, 1.9, f0, f0)
            row.append(f"{fs:+.2f}:{s:+5.2f}({c:.2f})")
        print(f"  width {w:5.3f}:", " ".join(row))
