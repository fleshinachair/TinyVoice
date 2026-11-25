#!/usr/bin/env python3
"""Convert PitchMonster (Devious Machines) presets into TinyVoice Mangler presets, for your own use.

    python scripts/import_pitchmonster.py [presets_folder] [out_folder]

  presets_folder  PitchMonster's preset library (default: C:/ProgramData/DeviousMachines/PitchMonster/Library/presets
                  on Windows, or $PITCHMONSTER_PRESETS); every *.preset under it, folders and all.
  out_folder      default build/pitchmonster_presets (gitignored). Install on the Move with
                  scripts/deploy_pitchmonster_presets.sh: they appear in Mangler's Presets page, Imported bank.

A PitchMonster preset is XML: <PARAM id=".." value=".."/> in real units (semitones, Hz, dB, octaves) plus
<Chord_0 Note_0=".."/> .. <Chord_7/> for the eight chord slots. Each becomes one Schwung User Preset
(the My Presets file shape: {"name", "module", "version", "state"}) whose state is the Mangler's own controls
(src/dsp/dsp.c mg_*) and its chord slots ("mg_chords").

These are YOUR presets, converted from YOUR PitchMonster install: they stay on your machine and your Move.
Never commit or ship them (build/ is ignored; *.preset too).
"""
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_SRC = os.environ.get("PITCHMONSTER_PRESETS", r"C:\ProgramData\DeviousMachines\PitchMonster\Library\presets")

SHIFT = ["Granular", "Formant", "Vocode"]
NOTES = ["Normal", "Pads", "Chord"]          # PitchMonster's noteMode: 0 Normal, 1 MIDI (our Pads), 2 Chord
OFFON = ["Off", "On"]
SPREAD = ["Spread", "Jitter"]
MAX_NOTES = 8                                # per chord slot


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def num(v):
    r = round(v, 4)
    return int(r) if r == int(r) else r


def convert(xml):
    """(state dict, warnings) from one preset's XML text."""
    P = {k: float(v) for k, v in re.findall(r'PARAM id="(\w+)" value="([-\d.e+]+)"', xml)}
    warn = []
    g = lambda k, d: P.get(k, d)
    st = {"engine": "Mangler"}
    st["mg_shift"] = SHIFT[int(clamp(round(g("shiftMode", 0)), 0, 2))]
    st["mg_pitch"] = num(clamp(g("pitch", 0), -48, 24))
    st["mg_formant"] = num(clamp(g("formantShift", 0), -24, 24))
    st["mg_voices"] = int(clamp(round(g("unisonVoices", 1)), 1, 8))
    st["mg_detune"] = num(clamp(g("pitchSpreadAmount", 0), 0, 12))
    st["mg_time"] = num(clamp(g("timeSpreadAmount", 0), 0, 1000))
    st["mg_grain"] = num(clamp(g("grainRate", 10), 1, 100))
    st["mg_mix"] = num(clamp(g("dryWet", 1), 0, 1))
    st["mg_notes"] = NOTES[int(clamp(round(g("noteMode", 0)), 0, 2))]
    st["mg_chord"] = str(int(clamp(round(g("selectedChord", 0)), 0, 7)) + 1)
    st["mg_mono"] = OFFON[g("monoMode", 1) > 0.5]
    st["mg_fix"] = OFFON[g("formantFix", 1) > 0.5]
    st["mg_snap"] = OFFON[g("pitchSnapOn", 1) > 0.5]
    st["mg_scatter"] = SPREAD[g("pitchScatterOn", 0) > 0.5]
    st["mg_smear"] = SPREAD[g("timeScatterOn", 0) > 0.5]
    st["mg_wide"] = OFFON[g("wideModeOn", 0) > 0.5]
    st["mg_gjitter"] = OFFON[g("grainRateJitter", 1) > 0.5]
    st["mg_filter"] = OFFON[g("filterEnable", 0) > 0.5]
    st["mg_freq"] = num(clamp(g("filterFreq", 1000), 20, 20000))
    st["mg_width"] = num(clamp(g("filterBandwidth", 10), 0.1, 10))
    st["mg_volume"] = num(clamp(g("masterVolume", 0), -24, 12))
    chords = []
    for i in range(8):
        m = re.search(r'<Chord_%d([^/]*)/>' % i, xml)
        notes = [int(n) for n in re.findall(r'Note_\d+="(\d+)"', m.group(1))] if m else []
        if len(notes) > MAX_NOTES:
            warn.append("chord %d has %d notes (kept %d)" % (i + 1, len(notes), MAX_NOTES))
            notes = notes[:MAX_NOTES]
        chords.append(",".join(str(clamp(n, 0, 127)) for n in notes))
    st["mg_chords"] = ";".join(chords)
    if st["mg_voices"] * max(1, len([c for c in chords[int(st["mg_chord"]) - 1].split(",") if c])) > 16 and st["mg_notes"] == "Chord":
        warn.append("voices x chord notes > 16 (the engine thins the unison to fit)")
    return st, warn


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "pitchmonster_presets")
    if not os.path.isdir(src):
        sys.exit("no preset folder at %s (pass it as the first argument)" % src)
    files = []
    for d, _, fs in os.walk(src):
        for f in fs:
            if f.lower().endswith(".preset"):
                files.append(os.path.join(d, f))
    files.sort()
    if not files:
        sys.exit("no *.preset files under %s" % src)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):                      # a fresh set: no stale files from an earlier run
        if f.lower().endswith(".json"):
            os.remove(os.path.join(out, f))
    taken = {}
    n = 0
    for path in files:
        xml = open(path, encoding="utf-8", errors="replace").read()
        if "<PARAM" not in xml:
            print("skipped (not a PitchMonster preset):", path)
            continue
        st, warn = convert(xml)
        name = os.path.splitext(os.path.basename(path))[0]
        cat = os.path.basename(os.path.dirname(path))
        if name in taken:                          # the same name in two folders: say which
            name = "%s (%s)" % (name, cat)
        taken[name] = path
        doc = {"name": name, "module": "tinyvoice", "version": 1, "category": cat, "state": st}
        safe = re.sub(r'[^\w .()&+-]', "_", name).strip() or "preset"
        with open(os.path.join(out, safe + ".json"), "w", encoding="utf-8", newline="") as fh:
            fh.write(json.dumps(doc, separators=(",", ":"), ensure_ascii=False))
        n += 1
        for w in warn:
            print("  %s: %s" % (name, w))
    print("wrote %d presets to %s" % (n, out))


if __name__ == "__main__":
    main()
