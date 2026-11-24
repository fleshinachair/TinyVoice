"""A/B of the core behaviours: Humanoid (cached renders) vs TinyVoice Resynth on the same inputs."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import *  # noqa
import tv_host as T
def hz(m): return 440 * 2 ** ((m - 69) / 12)
def tv(xml, x, midi=None): return T.render(xml, x, midi)[0]
B = dict(robotify=0.0, sharpen=0.0, tuning_duophonic=0, min_f0=40.0)

print("== tuning: chromatic steps +30c, INIT mask (C major)")
STEP = 0.6
notes = [48 + i + 0.30 for i in range(12)]
x = S.voice(S.f0_steps([hz(m) for m in notes], STEP), "a")
for lab, xml in (("scale", init(tuning_mode=0, tuning_duophonic=0)), ("note", init(tuning_mode=1, tuning_duophonic=0))):
    for who, f in (("HUM", hum), ("TV ", tv)):
        y = f(xml, x)
        print(f"  {lab} {who}", " ".join(f"{note_name(A.midi(out_pitch(y, i*STEP+.25, (i+1)*STEP-.05))):>7s}" for i in range(12)))

print("== vibrato 40c pass-through (5.5 Hz)")
for q, rob in ((0.03, 0.0), (0.25, 0.0), (0.4, 0.5), (0.5, 0.5), (0.25, 1.0)):
    vib = S.voice(S.f0_vibrato(hz(57), 3.0, 5.5, 40), "a")
    row = []
    for who, f in (("HUM", hum), ("TV", tv)):
        y = f(init(quantize=q, robotify=rob, tuning_duophonic=0), vib)
        t, f0, ap = A.f0_track(y, hop=128, win=1024); d, r = A.pitch_mod(f0[t > 0.6], 128 / 44100)
        row.append(f"{who} {d/40*100:4.0f}%")
    print(f"  quantize {q} robotify {rob}:", "  ".join(row))

print("== level and harmonic profile (vowel a, 165 Hz), diff vs input per harmonic")
f0 = hz(52); xa = S.voice(S.f0_const(f0, 2.0), "a"); hx = A.harmonics(xa, f0, .5, 1.9, kmax=16)
for lab, kw in (("rob 0", {}), ("rob 1 emph 1", dict(robotify=1.0, robotify_emphasis=1.0)), ("INIT-ish sharpen 80", dict(sharpen=80.0, robotify=0.5)),
                ("formant +0.5", dict(formant_shift=0.5)), ("formant -0.5", dict(formant_shift=-0.5)), ("transform .5", dict(transform=0.5)), ("transform 1", dict(transform=1.0))):
    for who, f in (("HUM", hum), ("TV ", tv)):
        y = f(init(**dict(B, **kw)), xa); hy = A.harmonics(y, f0, .5, 1.9, kmax=16)
        print(f"  {lab:20s} {who} lvl {A.rms_db(A.mono(y)[22050:80000]) - A.rms_db(xa[22050:80000]):+5.1f}", np.round(hy - hx, 0).astype(int).tolist())

print("== breathy HNR")
xb = S.voice(S.f0_const(f0, 2.0), "a", breath_db=-12)
for lab, kw in (("rob 0", {}), ("rob .5", dict(robotify=0.5)), ("rob 1", dict(robotify=1.0)), ("rob 1 denoise 0", dict(robotify=1.0, robotify_res=0.0))):
    print(f"  {lab:16s}", "  ".join(f"{who} {A.hnr_db(f(init(**dict(B, **kw)), xb), f0, .5, 1.9):5.1f}" for who, f in (("HUM", hum), ("TV", tv))), f" (in {A.hnr_db(xb, f0, .5, 1.9):.1f})")

print("== latency (gap centres, 150 ms gaps)")
v = S.voice(S.f0_const(hz(57), 4.0), "a", fade=0.002); g = np.ones_like(v)
for c in (1.5, 3.0): g[int((c - .075) * 44100):int((c + .075) * 44100)] = 0
v = (v * g).astype(np.float32)
for la in (0, 1, 2):
    row = []
    for who, f in (("HUM", hum), ("TV", tv)):
        y = f(init(lookahead=la, tuning_duophonic=0), v)
        e = np.sqrt(np.convolve(A.mono(y) ** 2, np.ones(441) / 441, "same")); ref = np.median(e[int(.6*44100):int(1.2*44100)])
        a = int(1.3 * 44100); seg = e[a:a + int(.9 * 44100)]; low = np.where(seg < .3 * ref)[0]
        row.append(f"{who} {((a + low.mean()) / 44100 - 1.5) * 1000:6.1f} ms" if len(low) else f"{who} --")
    print(f"  latency {la}:", "  ".join(row))
