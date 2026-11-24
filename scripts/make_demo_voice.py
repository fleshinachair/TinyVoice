"""Cut the Demo's sung phrase from a vocadito recording (CC BY 4.0, see CREDITS.md).

    calibration/.venv/Scripts/python.exe -I scripts/make_demo_voice.py [wav] [start_s] [length_s]

default: vocadito_25.wav, 6.8 s in, 7 s long  ->  src/demo/demo_voice.raw

Raw signed 16-bit little-endian, mono, 44100 Hz (the module reads it at load; nothing else). The clip is
faded in and out, followed by a short breath of near-silence so the voice gate closes once a loop, and
scaled so its mean level over the sung part matches the synthetic demo voice (DEMO_LEVEL in demo.c): the
Auto stage downstream then treats it like any voice.
"""
import os
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WAV = sys.argv[1] if len(sys.argv) > 1 else ROOT + "/calibration/vocals/vocadito_25.wav"
START = float(sys.argv[2]) if len(sys.argv) > 2 else 6.8
LENGTH = float(sys.argv[3]) if len(sys.argv) > 3 else 7.0
OUT = ROOT + "/src/demo/demo_voice.raw"
SR = 44100
DEMO_LEVEL = 0.51 * 0.25 / 1.5707963 / 0.9          # mean |x| of the sung part, as demo.c holds the synth voice

sr, x = wavfile.read(WAV)
x = x.astype(np.float64)
if x.ndim > 1:
    x = x.mean(1)
if np.abs(x).max() > 1.5:
    x /= 32768.0
if sr != SR:
    x = resample_poly(x, SR, sr)
seg = x[int(START * SR): int((START + LENGTH) * SR)].copy()
fi, fo = int(0.03 * SR), int(0.12 * SR)
seg[:fi] *= np.linspace(0, 1, fi)
seg[-fo:] *= np.linspace(1, 0, fo)
rng = np.random.RandomState(7)
breath = 0.0006 * rng.randn(int(0.4 * SR))           # about -64 dBFS: a quiet room, so the gate sees a pause
seg = np.concatenate([seg, breath])
act = np.abs(seg) > 0.05 * np.abs(seg).max()
seg *= DEMO_LEVEL / np.mean(np.abs(seg[act]))
peak = np.abs(seg).max()
if peak > 0.95:
    seg *= 0.95 / peak
os.makedirs(os.path.dirname(OUT), exist_ok=True)
(np.clip(seg, -1, 1) * 32767).astype("<i2").tofile(OUT)
print("wrote %s: %.2f s, peak %.2f, mean |x| (sung) %.3f" % (OUT, len(seg) / SR, np.abs(seg).max(), np.mean(np.abs(seg[act]))))
