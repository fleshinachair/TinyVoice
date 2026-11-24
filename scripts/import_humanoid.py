"""Convert your own Baby Audio Humanoid presets (.baby) into TinyVoice Resynth presets for Schwung.

    python scripts/import_humanoid.py [presets dir] [out dir]

defaults: C:/ProgramData/BABY Audio/presets/Humanoid  ->  build/humanoid_presets

Each .baby becomes a Schwung User Preset, <out>/<name>.json (one flat folder: My Presets lists no subfolders):
    {"name": ..., "module": "tinyvoice", "version": 1, "state": {TinyVoice state object}}
The state switches TinyVoice to Resynth and sets every hm_* control (Resynth takes Humanoid's ids
and units one to one), the preset's own note mask (Scale = Custom) and its wavetable (Wave =
Preset, hm_wt: one cycle resampled to 512 int16 samples, base64).

The presets and wavetables are Baby Audio's and their sound designers'. This converts the copy on
your own machine for your own Move; never commit or publish the output (build/ is git-ignored).
Install with scripts/deploy_humanoid_presets.sh.
"""
from __future__ import annotations
import base64, glob, json, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = sys.argv[1] if len(sys.argv) > 1 else r"C:/ProgramData/BABY Audio/presets/Humanoid"
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "humanoid_presets")
WT_N = 512

# the parameter list (ids, kinds, ranges) the module uses
_hp = open(os.path.join(ROOT, "src", "dsp", "hm_params.h"), encoding="utf-8").read()
PAR = {}
for m in re.finditer(r'X\((HM_\w+),\s*"(\w+)",\s*"[^"]*",\s*([FET]),\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+)', _hp):
    PAR[m.group(2)] = (m.group(3), float(m.group(5)), float(m.group(6)))


def wavetable_b64(xml: str) -> str | None:
    m = re.search(r'<wavetable_tree[^>]*wavetable_base64="([^"]*)"', xml, re.S)
    if not m:
        return None
    raw = base64.b64decode(m.group(1))
    n = len(raw) // 4
    if n < 2:
        return None
    src = struct.unpack("<%df" % n, raw[:4 * n])
    # Humanoid plays the table zero-padded to max(256, next power of two) samples as one cycle (measured)
    pad = 256
    while pad < n:
        pad *= 2
    src = src + (0.0,) * (pad - n)
    n = pad
    out = []
    for i in range(WT_N):                         # periodic linear resampling to 512 points
        pos = i * n / WT_N
        a = int(pos)
        t = pos - a
        out.append(src[a % n] * (1 - t) + src[(a + 1) % n] * t)
    peak = max(1e-9, max(abs(v) for v in out))
    pcm = struct.pack("<%dh" % WT_N, *[int(round(max(-1.0, min(1.0, v / peak)) * 32767)) for v in out])
    return base64.b64encode(pcm).decode("ascii")


def convert(path: str) -> dict:
    xml = open(path, encoding="utf-8").read()
    st = {"engine": "Resynth"}
    for pid, val in re.findall(r'<PARAM id="([^"]+)" value="([^"]*)"', xml):
        if pid not in PAR or pid == "freeze":       # Glitch is a performance toggle, never loaded on
            continue
        kind, lo, hi = PAR[pid]
        v = min(hi, max(lo, float(val)))
        st["hm_" + pid] = int(round(v)) if kind in "ET" else round(v, 6)
    tt = re.search(r"<tuning_tree([^>]*)>", xml, re.S)
    if tt:
        a = dict(re.findall(r'(\w+)="([^"]*)"', tt.group(1)))
        st["hm_tuning_mode"] = int(a.get("tuning_mode", 0))
        st["hm_tuning_duophonic"] = int(a.get("tuning_duophonic", 0))
    an = re.search(r"<tuning_auto_notes([^>]*)/>", xml, re.S)
    if an:
        notes = dict(re.findall(r'_(\d+)="([^"]*)"', an.group(1)))
        st["hm_mask"] = sum(1 << int(k) for k, v in notes.items() if v == "1")
        st["hm_scale"] = "Custom"
    wt = wavetable_b64(xml)
    if wt:
        st["hm_wave"] = "Preset"
        st["hm_wt"] = wt
    return st


FACTORY_KINDS = ("Tuner", "Voco", "Harmony", "Synth", "Dryish", "Instrument")


def category(path: str, name: str) -> str:
    """The Bank a preset is listed in on the Move (rsbank.c reads it): the Factory names' own category
    ("1. Tuner - ..."), Artists for the anniversary pack ("Artist - Name"), Init for the INIT preset."""
    m = re.match(r"\s*\d+\.\s*([A-Za-z]+)", name)
    if m and m.group(1).capitalize() in FACTORY_KINDS:
        return m.group(1).capitalize()
    rel = os.path.relpath(path, SRC).replace("\\", "/")
    return "Artists" if "/" in rel and not rel.lower().startswith("factory/") else "Init"


def main():
    files = sorted(f for f in glob.glob(os.path.join(SRC, "**", "*.baby"), recursive=True)
                   if not f.endswith("wavetables.baby"))
    if not files:
        sys.exit("no .baby presets under " + SRC)
    # One flat folder: the host's My Presets lists only presets/tinyvoice/*.json (no subfolders), sorted
    # by name. The Factory names already sort by category ("1. Tuner - ..."); the anniversary pack's are
    # "Artist - Name". `state` is stored as the parsed object, as the host's own saves are.
    n = 0
    os.makedirs(OUT, exist_ok=True)
    for old in glob.glob(os.path.join(OUT, "**", "*.json"), recursive=True):
        os.remove(old)
    used = set()
    for f in files:
        name = os.path.splitext(os.path.basename(f))[0]
        name = re.sub(r'[<>:"/\\|?*]', "_", name).strip()
        base, k = name, 2
        while name.lower() in used:
            name = "%s %d" % (base, k)
            k += 1
        used.add(name.lower())
        doc = {"name": name, "module": "tinyvoice", "version": 1, "category": category(f, name), "state": convert(f)}
        json.dump(doc, open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8"), ensure_ascii=False, separators=(",", ":"))
        n += 1
    print(f"converted {n} presets into {OUT}")


if __name__ == "__main__":
    main()
