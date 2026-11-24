"""Measurements on rendered audio (44.1 kHz): pitch track, harmonic profile, noise, envelope shift,
latency, stereo, level. Shared by the Humanoid probes and the TinyVoice A/B."""
from __future__ import annotations
import numpy as np

SR = 44100


def rms_db(x):
    return 20 * np.log10(np.sqrt(np.mean(np.asarray(x, dtype=np.float64) ** 2)) + 1e-12)


def mono(y):
    y = np.asarray(y)
    return y.mean(axis=0) if y.ndim == 2 else y


def f0_track(x, fmin=60.0, fmax=1000.0, hop=256, win=2048, thresh=0.15):
    """YIN (cumulative-mean-normalised difference) per frame.
    Returns (times s, f0 Hz or nan when unvoiced, aperiodicity)."""
    x = np.asarray(mono(x), dtype=np.float64)
    W = win // 2
    tmin, tmax = max(2, int(SR / fmax)), min(int(SR / fmin), W - 2)
    n = (len(x) - win) // hop
    times, f0s, aps = [], [], []
    for i in range(max(n, 0)):
        fr = x[i * hop:i * hop + win]
        times.append((i * hop + win / 2) / SR)
        if np.max(np.abs(fr)) < 1e-4:
            f0s.append(np.nan); aps.append(1.0); continue
        a = fr[:W]
        r = np.fft.irfft(np.fft.rfft(fr, 2 * win) * np.conj(np.fft.rfft(a, 2 * win)))[:W]
        c = np.concatenate([[0.0], np.cumsum(fr ** 2)])
        tau = np.arange(W)
        d = c[W] + (c[tau + W] - c[tau]) - 2 * r
        d[0] = 0.0
        cm = np.cumsum(d[1:])
        dn = np.ones(W)
        dn[1:] = d[1:] * np.arange(1, W) / np.maximum(cm, 1e-20)
        seg = dn[tmin:tmax + 1]
        below = np.where(seg < thresh)[0]
        if len(below):
            j = below[0]
            while j + 1 < len(seg) and seg[j + 1] < seg[j]:
                j += 1
        else:
            j = int(np.argmin(seg))
        t = tmin + j
        ap = float(dn[t])
        a_, b_, c_ = dn[t - 1], dn[t], dn[t + 1]
        den = a_ - 2 * b_ + c_
        tf = t + (0.5 * (a_ - c_) / den if abs(den) > 1e-12 else 0.0)
        f0s.append(SR / tf if ap < 0.4 else np.nan)
        aps.append(ap)
    return np.array(times), np.array(f0s), np.array(aps)


def cents(f, ref):
    return 1200 * np.log2(np.asarray(f) / ref)


def midi(f):
    return 69 + 12 * np.log2(np.asarray(f) / 440.0)


def spectrum_db(x, t0=None, t1=None, n=16384):
    """Averaged Blackman-Harris magnitude spectrum (dB) over [t0, t1)."""
    x = np.asarray(mono(x), dtype=np.float64)
    a = 0 if t0 is None else int(t0 * SR)
    b = len(x) if t1 is None else int(t1 * SR)
    seg = x[a:b]
    w = np.blackman(n)
    hop = n // 4
    acc = None
    cnt = 0
    for i in range(0, max(len(seg) - n, 0) + 1, hop):
        fr = seg[i:i + n]
        if len(fr) < n:
            break
        p = np.abs(np.fft.rfft(fr * w)) ** 2
        acc = p if acc is None else acc + p
        cnt += 1
    if acc is None:
        raise ValueError("segment shorter than the FFT")
    acc /= cnt
    return np.fft.rfftfreq(n, 1 / SR), 10 * np.log10(acc / (np.sum(w) / 2) ** 2 + 1e-24)


def harmonics(x, f0, t0=None, t1=None, kmax=40, n=16384):
    """Peak level (dB, sine amplitude 1 = 0 dB) of harmonics 1..kmax of a steady tone at ~f0."""
    fr, db = spectrum_db(x, t0, t1, n)
    out = []
    df = fr[1]
    for k in range(1, kmax + 1):
        fk = k * f0
        if fk > 0.45 * SR:
            out.append(np.nan); continue
        lo, hi = int((fk - 0.25 * f0) / df), int((fk + 0.25 * f0) / df) + 1
        j = lo + int(np.argmax(db[lo:hi]))
        out.append(db[j])
    return np.array(out)


def hnr_db(x, f0, t0=None, t1=None, n=16384, fmax=8000.0):
    """Harmonic-to-noise ratio: power within +-f0/8 of the harmonics vs the power between them."""
    fr, db = spectrum_db(x, t0, t1, n)
    p = 10 ** (db / 10)
    m = (fr > f0 * 0.5) & (fr < fmax)
    pos = (fr / f0) % 1.0
    near = (pos < 0.125) | (pos > 0.875)
    h = p[m & near].sum()
    nz = p[m & ~near].sum() * (near[m].sum() / max((~near[m]).sum(), 1))
    return 10 * np.log10(h / max(nz, 1e-30))


def envelope_db(x, f0=None, t0=None, t1=None, kmax=80):
    """Spectral envelope on a log-frequency grid 100 Hz..8 kHz (1/24 octave), interpolated through the
    harmonic peaks of a steady tone at ~f0 (estimated with the tracker when not given)."""
    if f0 is None:
        a = 0 if t0 is None else int(t0 * SR)
        b = None if t1 is None else int(t1 * SR)
        f0 = float(np.nanmedian(f0_track(np.asarray(mono(x))[a:b])[1]))
    h = harmonics(x, f0, t0, t1, kmax=kmax)
    fk = f0 * np.arange(1, kmax + 1)
    m = ~np.isnan(h)
    grid = 100 * 2 ** (np.arange(0, np.log2(8000 / 100), 1 / 24))
    return grid, np.interp(np.log2(grid), np.log2(fk[m]), h[m])


def envelope_shift_st(x_ref, x, t0=None, t1=None, f0_ref=None, f0=None):
    """Shift (semitones) that best maps x_ref's spectral envelope onto x's (positive = x is higher),
    and the correlation at that shift."""
    g, a = envelope_db(x_ref, f0_ref, t0, t1)
    _, b = envelope_db(x, f0, t0, t1)
    xs = np.arange(len(a))
    # the glottal tilt does not move with the formants: remove each envelope's straight-line trend
    a = a - np.polyval(np.polyfit(xs, a, 1), xs)
    b = b - np.polyval(np.polyfit(xs, b, 1), xs)
    best, bs = -1e9, 0.0
    for s in np.arange(-24, 24.01, 0.25):
        ai = np.interp(xs - 2 * s, xs, a, left=np.nan, right=np.nan)   # 2 grid steps per semitone
        m = ~np.isnan(ai)
        if m.sum() < len(a) // 2:
            continue
        c = np.corrcoef(ai[m], b[m])[0, 1]
        if c > best:
            best, bs = c, s
    return float(bs), float(best)


def onset_time(x, thresh_db=-40.0, hop=64):
    x = np.asarray(mono(x), dtype=np.float64)
    n = len(x) // hop
    r = np.sqrt((x[:n * hop].reshape(n, hop) ** 2).mean(axis=1))
    ref = r.max()
    i = np.where(20 * np.log10(r / ref + 1e-12) > thresh_db)[0]
    return i[0] * hop / SR if len(i) else np.nan


def latency_sec(x, y):
    """Onset difference of a burst (y's onset minus x's), from a -30 dB-relative threshold."""
    return onset_time(y, -30) - onset_time(x, -30)


def stereo(y):
    """(L/R correlation, side/mid ratio dB)."""
    l, r = np.asarray(y[0], np.float64), np.asarray(y[1], np.float64)
    c = np.dot(l, r) / np.sqrt(np.dot(l, l) * np.dot(r, r) + 1e-30)
    m, s = (l + r) / 2, (l - r) / 2
    return float(c), rms_db(s) - rms_db(m)


def pitch_mod(f0s, frame_hop_s=256 / SR):
    """Depth (cents, peak) and rate (Hz) of the dominant periodic pitch modulation in a track."""
    f = f0s[~np.isnan(f0s)]
    if len(f) < 16:
        return np.nan, np.nan
    c = cents(f, np.median(f))
    c = c - c.mean()
    sp = np.abs(np.fft.rfft(c * np.hanning(len(c))))
    fr = np.fft.rfftfreq(len(c), frame_hop_s)
    j = 1 + int(np.argmax(sp[1:]))
    return float(np.sqrt(2) * c.std()), float(fr[j])
