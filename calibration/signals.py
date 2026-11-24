"""Test inputs for the Humanoid measurements (44.1 kHz mono float32).

Synthetic voices are additive: every harmonic of a glottal source follows the f0 contour and takes
its amplitude from a formant envelope evaluated at its own frequency, so pitch, vibrato, jitter,
breath and the vowel are all known exactly. Real recordings come from calibration/vocals/.
"""
from __future__ import annotations
import glob, os
import numpy as np

SR = 44100
HERE = os.path.dirname(os.path.abspath(__file__))
VOCALS = os.path.join(HERE, "vocals")

# (F, bandwidth) pairs, adult male-ish; scaled for other voices with `tract`
VOWELS = {
    "a": [(730, 90), (1090, 110), (2440, 160), (3400, 250), (4500, 300)],
    "e": [(530, 70), (1840, 100), (2480, 160), (3500, 250), (4500, 300)],
    "i": [(270, 60), (2290, 100), (3010, 160), (3700, 250), (4600, 300)],
    "o": [(570, 80), (840, 90), (2410, 160), (3400, 250), (4500, 300)],
    "u": [(300, 60), (870, 90), (2240, 160), (3400, 250), (4500, 300)],
}


def env_db(f, vowel="a", tract=1.0, tilt_db_oct=-12.0, f0=110.0):
    """Formant envelope (dB) at frequencies f: glottal tilt + resonances."""
    f = np.maximum(np.asarray(f, dtype=np.float64), 1.0)
    db = tilt_db_oct * np.log2(np.maximum(f / f0, 1.0))
    mag = np.zeros_like(f)
    for (F, B) in VOWELS[vowel]:
        F *= tract
        B *= tract
        # magnitude of a 2-pole resonance, peak normalised to 1
        mag = mag + 1.0 / np.sqrt(1 + ((f ** 2 - F ** 2) / (f * B)) ** 2)
    return db + 20 * np.log10(mag + 1e-3)


def f0_const(hz, dur):
    return np.full(int(dur * SR), float(hz))


def f0_vibrato(hz, dur, rate=5.5, cents=40.0):
    t = np.arange(int(dur * SR)) / SR
    return hz * 2 ** (cents / 1200 * np.sin(2 * np.pi * rate * t))


def f0_steps(notes_hz, step_sec):
    return np.concatenate([np.full(int(step_sec * SR), float(h)) for h in notes_hz])


def f0_glide(h0, h1, dur):
    n = int(dur * SR)
    return h0 * (h1 / h0) ** (np.arange(n) / n)


def voice(f0, vowel="a", tract=1.0, jitter_cents=0.0, breath_db=-60.0, level_db=-18.0,
          fade=0.02, seed=1, kmax=200):
    """Additive voice following the per-sample f0 contour `f0` (Hz)."""
    rng = np.random.default_rng(seed)
    f0 = np.asarray(f0, dtype=np.float64).copy()
    n = len(f0)
    if jitter_cents > 0:
        # band-limited random pitch wander (~30 Hz smooth)
        w = rng.standard_normal(n // 512 + 2)
        j = np.interp(np.arange(n) / 512, np.arange(len(w)), w)
        f0 = f0 * 2 ** (jitter_cents / 1200 * j)
    ph = 2 * np.pi * np.cumsum(f0) / SR
    y = np.zeros(n)
    fm = float(np.median(f0))
    for k in range(1, kmax + 1):
        fk = k * f0
        if fm * k > 0.45 * SR:
            break
        a = 10 ** (env_db(fk, vowel, tract, f0=fm) / 20)
        a[fk > 0.45 * SR] = 0
        y += a * np.sin(k * ph + 0.3 * k * k)   # a fixed phase spread keeps the crest factor voice-like
    if breath_db > -59:
        nz = rng.standard_normal(n)
        spec = np.fft.rfft(nz)
        fr = np.fft.rfftfreq(n, 1 / SR)
        spec *= 10 ** (env_db(fr, vowel, tract, tilt_db_oct=-3, f0=fm) / 20)
        nz = np.fft.irfft(spec, n)
        nz *= np.sqrt((y ** 2).mean() / max((nz ** 2).mean(), 1e-20)) * 10 ** (breath_db / 20)
        y += nz
    y *= 10 ** (level_db / 20) / max(np.sqrt((y ** 2).mean()), 1e-12)
    m = int(fade * SR)
    if m:
        r = 0.5 - 0.5 * np.cos(np.pi * np.arange(m) / m)
        y[:m] *= r
        y[-m:] *= r[::-1]
    return y.astype(np.float32)


def flat_harmonics(f0_hz, dur, level_db=-18.0, kmax=400):
    """Every harmonic at equal amplitude (a pulse train), for transfer-function probes."""
    n = int(dur * SR)
    t = np.arange(n) / SR
    y = np.zeros(n)
    for k in range(1, kmax + 1):
        if k * f0_hz > 0.45 * SR:
            break
        y += np.sin(2 * np.pi * k * f0_hz * t + 0.3 * k * k)
    y *= 10 ** (level_db / 20) / np.sqrt((y ** 2).mean())
    return y.astype(np.float32)


def silence(dur):
    return np.zeros(int(dur * SR), dtype=np.float32)


def cat(*xs):
    return np.concatenate(xs).astype(np.float32)


def load_vocal(name, start=0.0, dur=None, level_db=-18.0):
    """A recording from calibration/vocals/ (mono, resampled to 44.1 kHz, RMS-normalised over voiced parts)."""
    import soundfile as sf
    from scipy.signal import resample_poly
    path = name if os.path.isabs(name) else os.path.join(VOCALS, name)
    x, sr = sf.read(path, always_2d=True)
    x = x.mean(axis=1)
    if sr != SR:
        g = np.gcd(sr, SR)
        x = resample_poly(x, SR // g, sr // g)
    i0 = int(start * SR)
    x = x[i0:] if dur is None else x[i0:i0 + int(dur * SR)]
    w = 1024
    r = np.sqrt((x[:len(x) // w * w].reshape(-1, w) ** 2).mean(axis=1))
    act = r[r > r.max() * 0.1]
    rms = np.sqrt((act ** 2).mean()) if len(act) else 1.0
    return (x * 10 ** (level_db / 20) / rms).astype(np.float32)


def vocal_files():
    return sorted(glob.glob(os.path.join(VOCALS, "*.wav")))
