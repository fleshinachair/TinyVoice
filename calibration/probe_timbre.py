"""Timbre probes: Robotify (+Denoise/Metallic/Emphasis), Formant (+Bandwidth), Sharpen, level."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import *  # noqa
def hz(m): return 440 * 2 ** ((m - 69) / 12)
CLEAN = dict(robotify=0.0, sharpen=0.0, tuning_duophonic=0, smoothing=0.1)

def prof(y, f0, k=16):
    h = A.harmonics(y, f0, 0.5, 1.9, kmax=k)
    return h

for f0m, vw in ((45, "a"), (57, "a"), (64, "i")):   # A2 110, A3 220, E4 330
    f0 = hz(f0m)
    x = S.voice(S.f0_const(f0, 2.0), vw)
    hx = prof(x, f0)
    print(f"\n== {vw} at {f0:.0f} Hz   input harmonics rel H1:", np.round(hx - hx[0], 0).astype(int).tolist())
    for lab, kw in [("robotify 0 sharpen 0", {}), ("robotify .5", dict(robotify=0.5)), ("robotify 1", dict(robotify=1.0)),
                    ("robotify 1 emphasis 1", dict(robotify=1.0, robotify_emphasis=1.0)), ("robotify 1 emphasis 0", dict(robotify=1.0, robotify_emphasis=0.0)),
                    ("INIT (sharpen 80, rob .5)", dict(sharpen=80.0, robotify=0.5))]:
        p = dict(CLEAN); p.update(kw)
        y = hum(init(**p), x)
        hy = prof(y, f0)
        print(f"  {lab:26s} level {A.rms_db(A.mono(y)[22050:80000]) - A.rms_db(x[22050:80000]):+5.1f} dB  diff per harmonic:", np.round(hy - hx - (hy[0] - hx[0]), 0).astype(int).tolist(), f" H1 {hy[0]-hx[0]:+.1f}")

# noise handling: breathy voice
f0 = hz(52)
x = S.voice(S.f0_const(f0, 2.0), "a", breath_db=-12)
print(f"\nbreathy input HNR {A.hnr_db(x, f0, 0.5, 1.9):.1f} dB")
for lab, kw in [("rob 0", {}), ("rob .5", dict(robotify=0.5)), ("rob 1 denoise 1", dict(robotify=1.0)), ("rob 1 denoise 0", dict(robotify=1.0, robotify_res=0.0)),
                ("rob 1 metallic 0", dict(robotify=1.0, robotify_harm=0.0))]:
    p = dict(CLEAN); p.update(kw)
    y = hum(init(**p), x)
    print(f"  {lab:18s} HNR {A.hnr_db(y, f0, 0.5, 1.9):6.1f} dB")

# inharmonic input: stretched partials f_k = k f0 (1 + B k^2)
f0 = hz(48); B = 4e-4
t = np.arange(int(2.0 * 44100)) / 44100
x = sum(10 ** (S.env_db(k * f0, "a", f0=f0) / 20) * np.sin(2 * np.pi * k * f0 * (1 + B * k * k) * t + 0.3 * k * k) for k in range(1, 30))
x = (x * 10 ** (-18 / 20) / np.sqrt((x ** 2).mean())).astype(np.float32)
fr0, db0 = A.spectrum_db(x, 0.5, 1.9)
def partials(y):
    fr, db = A.spectrum_db(y, 0.5, 1.9)
    # measured partial k: the strongest peak within k*F +- 0.3F, F = median output f0
    F = out_pitch(y, 0.5, 1.9)
    out = []
    for k in (5, 10, 15, 20):
        m = (fr > (k - 0.3) * F) & (fr < (k + 0.3) * F)
        out.append(fr[m][np.argmax(db[m])] / (k * F))
    return F, out
print(f"\ninharmonic input (B={B}): partial/ideal ratio at k=5,10,15,20:", [round(1 + B * k * k, 4) for k in (5, 10, 15, 20)])
for lab, kw in [("rob 0", {}), ("rob 1 metallic 1", dict(robotify=1.0)), ("rob 1 metallic 0", dict(robotify=1.0, robotify_harm=0.0)), ("rob .5 metallic 1", dict(robotify=0.5))]:
    p = dict(CLEAN); p.update(kw)
    F, r = partials(hum(init(**p), x))
    print(f"  {lab:18s} f0 {F:.1f}:", [round(v, 4) for v in r])

# formant
f0 = hz(45); x = S.voice(S.f0_const(f0, 2.0), "a")
for w in (0.0, 0.125, 0.5):
    row = []
    for fs in (-1.0, -0.5, -0.25, 0.25, 0.5, 1.0):
        y = hum(init(**CLEAN, formant_shift=fs, formant_width=w), x)
        s, c = A.envelope_shift_st(x, y, 0.5, 1.9, f0, f0)
        row.append(f"{fs:+.2f}:{s:+5.2f}st")
    print(f"formant width {w}:", " ".join(row))

# sharpen: output/input envelope difference with pulse-train input (flat harmonics), robotify 0
f0 = 110.0; x = S.flat_harmonics(f0, 2.0)
base = A.harmonics(hum(init(**CLEAN), x), f0, 0.5, 1.9, kmax=150)
fk = f0 * np.arange(1, 151)
sel = [1, 3, 9, 18, 27, 45, 72, 99, 130]
print("\nsharpen response (dB rel sharpen 0) at Hz", [int(fk[i - 1]) for i in sel])
for sh, bias in ((40, 0), (80, 0), (100, 0), (80, -6), (80, 6)):
    h = A.harmonics(hum(init(**dict(CLEAN, sharpen=float(sh), sharpen_bias=float(bias))), x), f0, 0.5, 1.9, kmax=150)
    d = h - base
    print(f"  sharpen {sh:3d} bias {bias:+d}:", [round(float(d[i - 1]), 1) for i in sel])
