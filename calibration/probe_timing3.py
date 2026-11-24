import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import *  # noqa
def hz(m): return 440 * 2 ** ((m - 69) / 12)
for q in (0.03, 0.1, 0.25, 0.4, 0.5):
    row = []
    for depth in (10, 25, 40, 60, 90):
        vib = S.voice(S.f0_vibrato(hz(57), 3.0, 5.5, depth), "a")
        y = hum(init(quantize=q, tuning_duophonic=0), vib)
        t, f, ap = A.f0_track(y, hop=128, win=1024)
        d, r = A.pitch_mod(f[t > 0.6], 128 / 44100)
        row.append(f"{depth:3d}c:{d:5.1f}")
    print(f"quantize {q:4.2f} 5.5 Hz vibrato in->out:", " ".join(row))
# slow linear glide across a semitone: where does the output switch, and is it stepped?
gl = S.voice(np.concatenate([S.f0_const(hz(57), 0.5), S.f0_glide(hz(57), hz(59), 2.0), S.f0_const(hz(59), 0.5)]), "a")
for q in (0.03, 0.25, 0.5):
    y = hum(init(quantize=q, tuning_duophonic=0, tuning_mode=0), mask_xml(init(quantize=q, tuning_duophonic=0), set(range(12))) and gl)
for q in (0.03, 0.25, 0.5):
    xml = mask_xml(init(quantize=q, tuning_duophonic=0), set(range(12)))
    y = hum(xml, gl)
    t, f, ap = A.f0_track(y, hop=128, win=1024)
    tin = np.arange(len(gl)) / 44100
    m = (t > 0.6) & (t < 2.4)
    dev = (A.midi(f[m]) - np.round(A.midi(f[m]))) * 100
    print(f"chromatic glide A3->B3, quantize {q}: output deviation from nearest note p95 {np.nanpercentile(np.abs(dev),95):5.1f} c, mean |dev| {np.nanmean(np.abs(dev)):5.1f} c")
# Hi latency with 150 ms gaps
v = S.voice(S.f0_const(hz(57), 4.0), "a", fade=0.002)
g = np.ones_like(v)
for c in (1.5, 3.0):
    g[int((c - 0.075) * 44100):int((c + 0.075) * 44100)] = 0
v = (v * g).astype(np.float32)
for la in (0, 1, 2):
    y = hum(init(lookahead=la, tuning_duophonic=0), v)
    e = np.sqrt(np.convolve(A.mono(y) ** 2, np.ones(441) / 441, "same"))
    ref = np.median(e[int(0.6 * 44100):int(1.2 * 44100)])
    out = []
    for c in (1.5, 3.0):
        a = int((c - 0.2) * 44100); seg = e[a:a + int(0.9 * 44100)]
        low = np.where(seg < 0.3 * ref)[0]
        out.append(round(((a + low.mean()) / 44100 - c) * 1000, 1) if len(low) else None)
    print(f"lookahead {la}: 150 ms gap centre delay (ms) {out}")
