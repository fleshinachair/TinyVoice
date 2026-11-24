"""Render audio through the real Humanoid VST3 (DawDreamer), with a preset loaded from a .baby file.

Trial mode outputs 5 s of silence every 60 s of rendered audio per instance (60-65 s, 120-125 s, ...),
so every render runs on a fresh plugin instance and stays under 20 s. `check_dropout` adds a check
(a long run of near-silence while the input is active, retried on a fresh instance).

Python: the TinyHouse venv (dawdreamer 0.9, numpy, soundfile), run with -I:
    C:/Github/TinyHouse/.venv/Scripts/python.exe -I calibration/<script>.py
"""
from __future__ import annotations
import os, re, sys, tempfile
import numpy as np
import dawdreamer as daw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import juce_state as J  # noqa: E402

PLUGIN = r"C:\Program Files\Common Files\VST3\Humanoid.vst3"
PRESETS = r"C:\ProgramData\BABY Audio\presets\Humanoid"
SR = 44100
BS = 128
MAX_RENDER_SEC = 20.0

_template: tuple[str, bytes] | None = None   # (outer VST3 xml, component tail after the humanoid xml)


def pump(sec: float = 0.3) -> int:
    """Run the Windows message loop of this (the JUCE message) thread for `sec` seconds.

    Humanoid builds its wavetable (and maybe more) through JUCE async messages. DawDreamer never
    runs a message loop, so without this the synth oscillator stays silent: Transform 1 renders
    as digital silence (found 2026-11-24). Returns the number of messages dispatched."""
    import ctypes, ctypes.wintypes as wt, time
    user32 = ctypes.windll.user32
    msg = wt.MSG()
    n = 0
    end = time.time() + sec
    while time.time() < end:
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
            n += 1
        time.sleep(0.005)
    return n


def _new_engine():
    eng = daw.RenderEngine(SR, BS)
    p = eng.make_plugin_processor("hum", PLUGIN)
    pump(0.2)
    return eng, p


def _get_template():
    global _template
    if _template is None:
        eng, p = _new_engine()
        fd, path = tempfile.mkstemp(suffix=".bin"); os.close(fd)
        try:
            p.save_state(path)
            outer, comp = J.split_vst3(open(path, "rb").read())
        finally:
            os.remove(path)
        n = int.from_bytes(comp[4:8], "little")
        _template = (outer, comp[8 + n:])
    return _template


def preset_xml(path_or_xml: str) -> str:
    if path_or_xml.lstrip().startswith("<"):
        x = path_or_xml
    else:
        x = open(path_or_xml, "r", encoding="utf-8").read()
    x = re.sub(r"<\?xml[^>]*\?>", "", x).strip()
    return '<?xml version="1.0" encoding="UTF-8"?> ' + x


def _state_bytes(xml: str) -> bytes:
    outer, tail = _get_template()
    d = xml.encode("utf-8") + b"\0"
    comp = b"VC2!" + len(d).to_bytes(4, "little") + d + tail
    return J.replace_component(outer, comp)


def set_params_xml(xml: str, **params) -> str:
    """Return the preset xml with PARAM values replaced (adds missing ones)."""
    for k, v in params.items():
        pat = re.compile(r'(<PARAM id="%s" value=")[^"]*(")' % re.escape(k))
        if pat.search(xml):
            xml = pat.sub(lambda m: m.group(1) + repr(float(v)) + m.group(2), xml, count=1)
        else:
            xml = xml.replace("</Humanoid>", '<PARAM id="%s" value="%r"/></Humanoid>' % (k, float(v)), 1)
    return xml


def set_tuning_xml(xml: str, **attrs) -> str:
    for k, v in attrs.items():
        xml = re.sub(r'(<tuning_tree[^>]*\b%s=")[^"]*(")' % re.escape(k), lambda m: m.group(1) + str(v) + m.group(2), xml, count=1)
    return xml


def _dropout(x: np.ndarray, y: np.ndarray, min_sec=0.25) -> bool:
    """True if y holds >= min_sec of near-silence (-90 dBFS) where x is active (-50 dBFS)."""
    w = 1024
    n = min(x.shape[-1], y.shape[-1]) // w
    if n == 0:
        return False
    xr = np.sqrt((x[..., :n * w].reshape(-1, n, w) ** 2).mean(axis=(0, 2)))
    yr = np.sqrt((y[..., :n * w].reshape(-1, n, w) ** 2).mean(axis=(0, 2)))
    bad = (xr > 10 ** (-50 / 20)) & (yr < 10 ** (-90 / 20))
    run = best = 0
    for b in bad:
        run = run + 1 if b else 0
        best = max(best, run)
    return best * w / SR >= min_sec


def render(xml: str, x: np.ndarray, midi: list | None = None, tries: int = 3,
           params: dict | None = None, check_dropout: bool = False) -> np.ndarray:
    """x: mono (n,) or stereo (2, n) float32 at 44.1 kHz. midi: [(note, vel, start_s, dur_s)].
    params: VST parameter index -> normalised value, applied after the preset (rarely needed).
    Returns (2, n) float32."""
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 1:
        x = np.stack([x, x])
    if x.shape[1] / SR > MAX_RENDER_SEC:
        raise ValueError("render <= %.0f s per call (trial dropouts)" % MAX_RENDER_SEC)
    st = _state_bytes(xml)
    fd, path = tempfile.mkstemp(suffix=".bin"); os.close(fd)
    open(path, "wb").write(st)
    try:
        for _ in range(tries):
            eng, p = _new_engine()
            p.load_state(path)
            for i, v in (params or {}).items():
                p.set_parameter(int(i), float(v))
            pump(0.3)
            for (note, vel, t0, dur) in (midi or []):
                p.add_midi_note(int(note), int(vel), max(float(t0), 0.1), float(dur))  # a note in the first ~0.05 s is dropped
            pb = eng.make_playback_processor("in", x)
            eng.load_graph([(pb, []), (p, ["in"])])
            pump(0.2)
            eng.render(x.shape[1] / SR)
            y = eng.get_audio()[:, :x.shape[1]]
            # The trial's silence is counted in rendered samples (60-65 s, 120-125 s... of each
            # instance, measured 2026-11-24), so a fresh instance capped at 20 s never reaches it.
            # The check is for renders that might legitimately be silent only on request.
            if not check_dropout or not _dropout(x, y):
                return y.astype(np.float32)
        raise RuntimeError("dropout in every try")
    finally:
        os.remove(path)


def param_readback(xml: str) -> list[dict]:
    """Load a preset and read back every VST parameter (index, name, normalised value, display text)."""
    st = _state_bytes(xml)
    fd, path = tempfile.mkstemp(suffix=".bin"); os.close(fd)
    open(path, "wb").write(st)
    try:
        eng, p = _new_engine()
        p.load_state(path)
        pump(0.3)
        out = []
        for d in p.get_parameters_description():
            i = d["index"]
            if i >= 63:
                break
            out.append({"i": i, "name": d["name"], "v": p.get_parameter(i), "text": p.get_parameter_text(i)})
        return out
    finally:
        os.remove(path)


def init_xml() -> str:
    return preset_xml(os.path.join(PRESETS, "00 INIT.baby"))
