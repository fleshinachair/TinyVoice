"""Render a Humanoid preset (.baby XML) through TinyVoice's Resynth engine (build/hm_render.exe).

The preset maps one to one: hm.c takes Humanoid's preset ids and units. The tuning tree gives
tuning_mode, tuning_duophonic and the 12-note mask; the wavetable goes as a float32 cycle."""
from __future__ import annotations
import base64, os, re, subprocess, tempfile
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "build", "hm_render.exe")
SRC = ["src/dsp/hm.c", "src/dsp/hmfx.c", "src/dsp/fft.c", "src/dsp/cher.c", "tools/hm_render.c"]


def build():
    """Rebuild the CLI if any source is newer."""
    srcs = [os.path.join(ROOT, s) for s in SRC] + [os.path.join(ROOT, "src/dsp/hm.h"), os.path.join(ROOT, "src/dsp/hmfx.h"), os.path.join(ROOT, "src/dsp/fft.h")]
    if os.path.exists(EXE) and os.path.getmtime(EXE) > max(os.path.getmtime(s) for s in srcs):
        return
    cmd = ["zig", "cc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Isrc/dsp"] + SRC + ["-o", EXE, "-lm"]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr)


def preset_to_params(xml: str, workdir: str) -> list[str]:
    lines = []
    for k, v in re.findall(r'<PARAM id="([^"]+)" value="([^"]*)"', xml):
        lines.append(f"{k} {float(v)!r}")
    tt = re.search(r"<tuning_tree([^>]*)>", xml, re.S)
    if tt:
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', tt.group(1)))
        lines.append(f"tuning_mode {attrs.get('tuning_mode', '0')}")
        lines.append(f"tuning_duophonic {attrs.get('tuning_duophonic', '0')}")
    an = re.search(r"<tuning_auto_notes([^>]*)/>", xml, re.S)
    if an:
        notes = dict(re.findall(r'_(\d+)="([^"]*)"', an.group(1)))
        mask = sum(1 << int(k) for k, v in notes.items() if v == "1")
        lines.append(f"mask {mask}")
    wt = re.search(r'<wavetable_tree[^>]*wavetable_base64="([^"]*)"', xml, re.S)
    if wt:
        data = np.frombuffer(base64.b64decode(wt.group(1)), dtype="<f4")
        p = os.path.join(workdir, "wt.f32")
        data.astype("<f4").tofile(p)
        lines.append(f"wavetable {p}")
    return lines


def render(xml: str, x: np.ndarray, midi=None) -> tuple[np.ndarray, int]:
    """Same call shape as hum_host.render: mono x at 44.1 kHz, midi [(note, vel, start, dur)].
    Returns ((2, n) float32, latency samples)."""
    build()
    x = np.asarray(x, dtype=np.float32)
    if x.ndim == 2:
        x = x.mean(axis=0)
    with tempfile.TemporaryDirectory() as d:
        lines = preset_to_params(xml, d)
        for (note, vel, t0, dur) in (midi or []):
            lines.append(f"note {int(note)} {max(float(t0), 0.1)} {max(float(t0), 0.1) + float(dur)}")
        pp = os.path.join(d, "p.txt")
        open(pp, "w").write("\n".join(lines) + "\n")
        xi, yo = os.path.join(d, "in.f32"), os.path.join(d, "out.f32")
        x.astype("<f4").tofile(xi)
        r = subprocess.run([EXE, pp, xi, yo], capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stderr)
        y = np.fromfile(yo, dtype="<f4").reshape(-1, 2).T.copy()
        return y, int(r.stdout.strip() or 0)
