"""Cached Humanoid renders for the probes: a render is keyed by (preset xml, input, midi), stored in out/renders."""
from __future__ import annotations
import hashlib, json, os, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hum_host as H      # noqa: E402
import signals as S       # noqa: E402
import analysis as A      # noqa: E402

CACHE = os.path.join(HERE, "out", "renders")
os.makedirs(CACHE, exist_ok=True)


def hum(xml: str, x: np.ndarray, midi=None) -> np.ndarray:
    h = hashlib.sha1()
    h.update(xml.encode()); h.update(np.ascontiguousarray(x, dtype=np.float32).tobytes())
    h.update(json.dumps(midi or []).encode())
    path = os.path.join(CACHE, h.hexdigest()[:20] + ".npy")
    xin = np.asarray(x, dtype=np.float32)
    if os.path.exists(path):
        y = np.load(path)
        if not H._dropout(xin, y):
            return y
    # Humanoid sometimes starts silent or drops out (an asynchronous start-up race, not the trial timer:
    # the same preset renders fine on the next fresh instance), and such renders used to be cached for
    # good (four presets "silent in Humanoid" were this; up to ~50 % of renders for presets with a
    # reshaped wavetable; a warm-up render before the real one made it worse). Retry; keep a dropout only if it repeats.
    for _ in range(5):
        y = H.render(xml, x, midi)
        if not H._dropout(xin, y):
            break
    np.save(path, y)
    return y


def init(**params):
    """INIT preset with PARAMs replaced; tuning attributes via tuning_<name>=..."""
    xml = H.init_xml()
    tun = {k: v for k, v in params.items() if k.startswith("tuning_")}
    par = {k: v for k, v in params.items() if not k.startswith("tuning_")}
    if par:
        xml = H.set_params_xml(xml, **par)
    if tun:
        xml = H.set_tuning_xml(xml, **tun)
    return xml


def mask_xml(xml: str, notes) -> str:
    """Set tuning_auto_notes to exactly the given pitch classes."""
    import re
    for pc in range(12):
        xml = re.sub(r'(<tuning_auto_notes[^>]*\b_%d=")[^"]*(")' % pc, lambda m: m.group(1) + ("1" if pc in notes else "0") + m.group(2), xml, count=1)
    return xml


def out_pitch(y, t0, t1, fmin=60, fmax=1000):
    t, f, ap = A.f0_track(y, fmin, fmax)
    m = (t >= t0) & (t < t1)
    return float(np.nanmedian(f[m])) if np.any(~np.isnan(f[m])) else float("nan")


def note_name(m):
    if m != m:
        return "--"
    n = int(round(m))
    return ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"][n % 12] + str(n // 12 - 1) + ("%+d" % round((m - n) * 100))
