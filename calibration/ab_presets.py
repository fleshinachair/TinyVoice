"""Preset A/B on real vocals: Humanoid vs TinyVoice Resynth, per preset and clip.

Metrics (TinyVoice minus Humanoid):
  lvl   output level difference, dB
  spec  long-term 1/3-octave spectrum distance after level alignment: mean |dB| over 100 Hz - 10 kHz
  pitch median |cents| between the two outputs' pitch tracks (latency-aligned), frames voiced in both
  corr  L/R correlation (Humanoid / TinyVoice)

    python -I ab_presets.py [--n 40] [--seed 1] [--match substring] [--out out/ab_presets.json]
"""
from __future__ import annotations
import argparse, glob, json, os, random, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from probe_lib import hum, H, S, A  # noqa
import tv_host as T

CLIPS = [("vocadito_4.wav", 1.0, 8.0), ("vocadito_19.wav", 1.0, 8.0), ("vocadito_2.wav", 1.0, 8.0)]
SR = 44100


def third_oct(y):
    f, db = A.spectrum_db(A.mono(y), 0.3, None, n=8192)
    p = 10 ** (db / 10)
    cs = 100 * 2 ** (np.arange(0, 6.65, 1 / 3))
    out = []
    for c in cs:
        m = (f >= c * 2 ** (-1 / 6)) & (f < c * 2 ** (1 / 6))
        out.append(10 * np.log10(p[m].mean() + 1e-20))
    return np.array(out)


def lag_between(a, b, maxlag=0.25):
    """Delay of b relative to a from their energy envelopes (seconds)."""
    hop = 64
    ea = np.sqrt(np.convolve(A.mono(a) ** 2, np.ones(hop) / hop, "same")[::hop])
    eb = np.sqrt(np.convolve(A.mono(b) ** 2, np.ones(hop) / hop, "same")[::hop])
    ea -= ea.mean(); eb -= eb.mean()
    m = int(maxlag * SR / hop)
    c = [np.dot(ea[max(0, -k):len(ea) - max(0, k)], eb[max(0, k):len(eb) - max(0, -k)]) for k in range(-m, m + 1)]
    return (int(np.argmax(c)) - m) * hop / SR


def compare(yh, yt):
    lvl = A.rms_db(A.mono(yt)) - A.rms_db(A.mono(yh))
    sh, st = third_oct(yh), third_oct(yt)
    spec = float(np.mean(np.abs((st - st.mean()) - (sh - sh.mean()))))
    lag = lag_between(yh, yt)
    th, fh, _ = A.f0_track(yh)
    tt, ft, _ = A.f0_track(yt)
    ft_al = np.interp(th + lag, tt, ft, left=np.nan, right=np.nan)
    m = ~np.isnan(fh) & ~np.isnan(ft_al)
    if m.sum() > 10:
        c = A.cents(ft_al[m], fh[m])
        folded = (c + 600) % 1200 - 600            # octave-folded: lines an octave apart are not a tuning error
        pitch = float(np.median(np.abs(folded)))
        octs = float(np.mean(np.abs(c - folded) > 600))
    else:
        pitch, octs = float("nan"), float("nan")
    ch, _ = A.stereo(yh) if np.std(yh[1]) > 0 else (1.0, 0)
    ct, _ = A.stereo(yt) if np.std(yt[1]) > 0 else (1.0, 0)
    return dict(lvl=round(float(lvl), 2), spec=round(spec, 2), pitch=round(pitch, 1), oct=round(octs, 2), corr_h=round(ch, 3), corr_t=round(ct, 3),
                lag_ms=round(lag * 1000, 1), band_diff=np.round((st - st.mean()) - (sh - sh.mean()), 1).tolist())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=40)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--match", default="")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "out", "ab_presets.json"))
    a = ap.parse_args()
    files = sorted(f for f in glob.glob(os.path.join(H.PRESETS, "**", "*.baby"), recursive=True) if not f.endswith("wavetables.baby"))
    if a.match:
        files = [f for f in files if a.match.lower() in f.lower()]
    random.Random(a.seed).shuffle(files)
    files = files[:a.n]
    clips = {c: S.load_vocal(c, s, d) for c, s, d in CLIPS}
    rows = []
    t0 = time.time()
    for i, f in enumerate(files):
        xml = H.preset_xml(f)
        name = os.path.relpath(f, H.PRESETS)
        # MIDI-mode presets get a held note pattern (the pads)
        midi = None
        if 'tuning_mode="2"' in xml:
            midi = [(57, 100, 0.1, 3.9), (60, 100, 4.0, 3.9)]
        for c, x in clips.items():
            yh = hum(xml, x, midi)
            yt, lat = T.render(xml, x, midi)
            r = compare(yh, yt)
            r.update(preset=name, clip=c)
            rows.append(r)
        print(f"{i+1}/{len(files)} {time.time()-t0:5.0f}s {name}: " + "  ".join(f"{r['clip'][9:-4]} lvl {r['lvl']:+5.1f} spec {r['spec']:4.1f} pitch {r['pitch']:5.1f}" for r in rows[-len(clips):]), flush=True)
    json.dump(rows, open(a.out, "w"), indent=0)
    spec = np.array([r["spec"] for r in rows]); lv = np.array([r["lvl"] for r in rows]); pc = np.array([r["pitch"] for r in rows])
    print(f"\nall: spec mean {spec.mean():.2f} median {np.median(spec):.2f}; |lvl| mean {np.abs(lv).mean():.2f}; pitch median {np.nanmedian(pc):.1f} c")


if __name__ == "__main__":
    main()
