"""The two render paths (Synth Mix 0 = the tuner path, 1 = resynthesis) and MIDI / Duo."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import *  # noqa
def hz(m): return 440 * 2 ** ((m - 69) / 12)
B = dict(robotify=0.0, sharpen=0.0, tuning_duophonic=0, min_f0=40.0)
f0 = hz(52); xa = S.voice(S.f0_const(f0, 2.0), "a", breath_db=-12)
ref0 = hum(init(**B, synth_mix=0.0), xa); ref1 = hum(init(**B), xa)
print(f"breathy a: HNR in {A.hnr_db(xa, f0, .5, 1.9):.1f}; synth_mix 0 {A.hnr_db(ref0, f0, .5, 1.9):.1f}; synth_mix 1 {A.hnr_db(ref1, f0, .5, 1.9):.1f}")
for sm in (0.0, 1.0):
    y = hum(init(**dict(B, robotify=1.0), synth_mix=sm), xa)
    print(f"  synth_mix {sm} robotify 1: HNR {A.hnr_db(y, f0, .5, 1.9):.1f}")
    for fs in (-0.5, 0.5):
        y = hum(init(**B, synth_mix=sm, formant_shift=fs), xa)
        print(f"  synth_mix {sm} formant {fs:+}: envelope shift {A.envelope_shift_st(ref0 if sm == 0 else ref1, y, .5, 1.9, f0, f0)[0]:+.2f} st")
    for oc in (-1.0, 1.5):
        y = hum(init(**B, synth_mix=sm, octaving=oc), xa)
        lo = A.harmonics(y, f0 / 2, .5, 1.9, kmax=8)[0::2]
        print(f"  synth_mix {sm} octave {oc:+}: sub-octave {10*np.log10(np.sum(10**(lo/10))):.1f} dB")
# vibrato through the two paths with robotify 1 flatten 1
vib = S.voice(S.f0_vibrato(hz(57), 3.0, 5.5, 40), "a")
for sm in (0.0, 1.0):
    y = hum(init(**dict(B, robotify=1.0), synth_mix=sm), vib)
    t, f, ap = A.f0_track(y, hop=128, win=1024); d, r = A.pitch_mod(f[t > 0.6], 128 / 44100)
    print(f"vibrato 40c, robotify 1 flatten 1, synth_mix {sm}: out {d:.1f} c")
# latency per path
burst = S.cat(S.silence(0.5), S.voice(S.f0_const(hz(57), 1.0), "a", fade=0.002), S.silence(0.3))
for sm in (0.0, 1.0):
    y = hum(init(**B, synth_mix=sm), burst)
    print(f"synth_mix {sm}: onset {1000*A.latency_sec(burst, y):.1f} ms")

# MIDI mode
x = S.voice(S.f0_const(hz(55), 3.0), "a")
for lab, notes, duo in [("C4", [(60, 100, 0.1, 2.8)], 0), ("C4 then E4 at 1.5 s", [(60, 100, 0.1, 1.4), (64, 100, 1.5, 1.4)], 0),
                        ("C4+G4 duo 0", [(60, 100, 0.1, 2.8), (67, 100, 0.1, 2.8)], 0), ("C4+G4 duo 1", [(60, 100, 0.1, 2.8), (67, 100, 0.1, 2.8)], 1)]:
    y = hum(init(**dict(B, tuning_duophonic=duo), tuning_mode=2), x, notes)
    fr, db = A.spectrum_db(y, 0.5, 1.3)
    peaks = []
    for m in range(48, 80):
        f = hz(m); sel = (fr > f * 0.985) & (fr < f * 1.015)
        peaks.append((db[sel].max(), m))
    top = sorted(peaks, reverse=True)[:3]
    p2 = A.midi(out_pitch(y, 1.8, 2.8))
    print(f"MIDI {lab}: lvl {A.rms_db(A.mono(y)[22050:]):6.1f}, strongest fundamentals 0.5-1.3 s {[(note_name(m), round(d,1)) for d, m in top]}, pitch 1.8-2.8 s {note_name(p2)}")
# Note mode duo
for duo in (0, 1):
    y = hum(init(**dict(B, tuning_duophonic=duo), tuning_mode=1), x)
    fr, db = A.spectrum_db(y, 0.5, 2.5)
    lv = {nm: round(float(db[(fr > hz(m) * .985) & (fr < hz(m) * 1.015)].max()), 1) for nm, m in (("C4", 60), ("G4", 67))}
    print(f"Note mode (mono C4, duo G4) duophonic {duo}: fundamentals {lv}")
