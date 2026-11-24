"""Output-effect probes: filter responses, de-ess, widen, output/input gain, dry/wet."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import *  # noqa
B = dict(robotify=0.0, sharpen=0.0, tuning_duophonic=0, min_f0=40.0, quantize_enabled=1.0)
f0 = 55.0 * 2   # 110 Hz, many harmonics
x = S.flat_harmonics(f0, 2.0)
K = 150
ref = A.harmonics(hum(init(**B), x), f0, .5, 1.9, kmax=K)
fk = f0 * np.arange(1, K + 1)
sel = [1, 2, 3, 5, 8, 12, 18, 27, 40, 60, 90, 130]
print("freq Hz", [int(fk[i - 1]) for i in sel])
for lab, kw in [("hpf 60 res 0", dict(hpf_freq=60.0)), ("hpf 60 res -80", dict(hpf_freq=60.0, hpf_res=-80.0)), ("hpf 60 res 24", dict(hpf_freq=60.0, hpf_res=24.0)),
                ("hpf 72 res 12", dict(hpf_freq=72.0, hpf_res=12.0)),
                ("lpf 100 res 0", dict(lpf_freq=100.0)), ("lpf 100 res -80", dict(lpf_freq=100.0, lpf_res=-80.0)), ("lpf 100 res 24", dict(lpf_freq=100.0, lpf_res=24.0)),
                ("lpf 90 res 12", dict(lpf_freq=90.0, lpf_res=12.0)),
                ("mid 80 +12 w 34", dict(filter_freq=80.0, filter_gain=12.0)), ("mid 80 -12 w 34", dict(filter_freq=80.0, filter_gain=-12.0)),
                ("mid 80 +12 w 12", dict(filter_freq=80.0, filter_gain=12.0, filter_width=12.0)), ("mid 80 +12 w 60", dict(filter_freq=80.0, filter_gain=12.0, filter_width=60.0)),
                ("mid 98 +12 w 24", dict(filter_freq=98.0, filter_gain=12.0, filter_width=24.0)), ("mid 98 -60 w 24", dict(filter_freq=98.0, filter_gain=-60.0, filter_width=24.0))]:
    y = hum(init(**dict(B, **kw)), x)
    h = A.harmonics(y, f0, .5, 1.9, kmax=K) - ref
    print(f"{lab:18s} lvl {A.rms_db(A.mono(y)[22050:]) - A.rms_db(A.mono(hum(init(**B), x))[22050:]):+5.1f}", [round(float(h[i - 1]), 1) for i in sel])
for og in (-12.0, 6.0):
    print(f"output_gain {og}: {A.rms_db(A.mono(hum(init(**dict(B, output_gain=og)), x))[22050:]) - A.rms_db(A.mono(hum(init(**B), x))[22050:]):+.2f} dB")
for ig in (-12.0, 12.0):
    print(f"input_gain {ig}: {A.rms_db(A.mono(hum(init(**dict(B, input_gain=ig)), x))[22050:]) - A.rms_db(A.mono(hum(init(**B), x))[22050:]):+.2f} dB")
v = S.voice(S.f0_const(165, 2.0), "a")
for dw in (0.0, 0.5):
    y = hum(init(**dict(B, dry_wet=dw)), v)
    print(f"dry_wet {dw}: lvl {A.rms_db(A.mono(y)[22050:]) - A.rms_db(v[22050:]):+.2f} dB")
# de-ess on speech with sibilants
sp = S.load_vocal("speech_libri1.wav", 0.0, 8.0)
base = hum(init(**dict(B, quantize_enabled=0.0)), sp)
fb, db0 = A.spectrum_db(base, 0.3, None, 4096)
for ds, sens, att in ((12.0, 0.4, 0.02), (24.0, 0.4, 0.02), (24.0, 0.2, 0.02), (24.0, 0.6, 0.02)):
    y = hum(init(**dict(B, quantize_enabled=0.0, deess=ds, deess_sensitivity=sens, deess_attack=att)), sp)
    f, d = A.spectrum_db(y, 0.3, None, 4096)
    bands = [(2000, 4000), (4000, 6000), (6000, 8000)]
    print(f"deess {ds} sens {sens}: lvl {A.rms_db(A.mono(y)) - A.rms_db(A.mono(base)):+.2f}", [round(float(10*np.log10(np.mean(10**(d[(f>=a)&(f<b)]/10)) / np.mean(10**(db0[(fb>=a)&(fb<b)]/10)))), 1) for a, b in bands])
